// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 The Pantheon authors

#pragma once

#include "PluginProcessor.h"
#include "ThemisUi.h"

#include <themis/HistoryChecks.h>
#include <themis/Spectrogram.h>

#include <array>
#include <functional>
#include <vector>

namespace ui
{
// "50 ", "1k ", "2.5k " (a trailing space: the caller adds the unit).
inline juce::String frequencyText (double hz)
{
    if (hz < 995.0)
        return juce::String (juce::roundToInt (hz)) + " ";
    const double k = hz / 1000.0;
    return (std::abs (k - std::round (k)) < 0.05 ? juce::String (juce::roundToInt (k)) : juce::String (k, 1)) + "k ";
}

// A scrolling graph of the last minute of analysed frames (themis::FrameHistory) made to answer a question, not just to
// plot numbers. A panel holds several tabs (views of the same history); each tab has
//   * a header row with the tabs and, where it applies, a chip per frequency range to pick the range it looks at,
//   * a legend that doubles as the readout (latest values, or those under the cursor); click an entry to hide its line,
//   * a verdict line: a traffic-light summary of the window drawn from themis::history (the limits are rules of thumb),
//   * the plot, with shaded zones that name what each region of the axis means.
// A tab can instead be a spectrogram (themis::SpectrumHistory): frequency up the axis, time across, level as colour.
class HistoryPanel : public juce::Component, public juce::TooltipClient
{
public:
    using Value = std::function<float (const themis::FrameStats&, int column)>; // a value without themis::hasLevel() means "no value"

    struct Series
    {
        const char* name;
        juce::Colour colour;
        Value value;
        bool dashed = false;
    };
    struct Axis
    {
        float top, bottom, step; // gridlines every `step` from the top
        std::function<juce::String (float)> tick;
        std::vector<float> marks = {}; // gridlines at these values instead (a frequency axis is not evenly marked)
    };
    struct Zone // a labelled band of the axis
    {
        float from, to;
        juce::Colour colour;
        juce::String label;
    };
    struct Verdict
    {
        juce::String text; // empty: nothing to say yet
        themis::Level level = themis::Level::good;
        bool lamp = true;  // false: a plain reading with no verdict lamp
    };
    struct Tab
    {
        Tab (const char* tabName, juce::String tabTooltip, Axis tabAxis) : name (tabName), tooltip (std::move (tabTooltip)), axis (std::move (tabAxis)) {}

        const char* name;
        juce::String tooltip;
        Axis axis;
        std::vector<Zone> zones;
        std::function<std::vector<Zone>()> liveZones; // zones that follow a setting (the true-peak ceiling): read when drawn, instead of `zones`
        std::vector<Series> series;
        bool chooseRange = false;   // shows the range chips; `column` is passed to the series and the verdict
        bool marksClips = false;    // flags the frames in which the audio clipped
        std::function<juce::String (float)> format;                                 // a legend value; default is dB
        std::function<Verdict (const themis::FrameStats*, int n, int column)> verdict; // the frames are the window shown, oldest first
        std::function<juce::String (const themis::FrameStats&)> detail;             // extra text for the frame shown
        bool spectrogram = false;   // drawn from themis::SpectrumHistory instead of the frames; `spectrumVerdict` replaces `verdict`
        std::function<Verdict (const themis::BandLevels*, int n, double periodSeconds, double usableMaxHz)> spectrumVerdict;
    };

    // Pull fresh frames (message thread, ~30 Hz). Repaints only when the analysis has produced something new.
    void update()
    {
        const auto& t = tabs[static_cast<size_t> (tab)];
        if (t.spectrogram)
        {
            const auto& history = proc.spectrumHistory();
            const auto sequence = history.sequence();
            if (sequence == lastSpectrumSequence && !dirty)
                return;
            if (sequence != lastSpectrumSequence)
            {
                lastSpectrumSequence = sequence;
                period = std::max (history.framePeriodSeconds(), 1.0e-3);
                const int wanted = std::min (themis::SpectrumHistory::kCapacity, static_cast<int> (std::ceil (kSpanSeconds / period)) + 1);
                numFrames = history.copyRecent (bandFrames.data(), wanted);
                spectrumStale = true;
            }
            dirty = false;
            if (spectrumStale)
                rebuildSpectrogram();
            verdict = t.spectrumVerdict && numFrames > 0 ? t.spectrumVerdict (bandFrames.data(), numFrames, period, themis::BandGrid::usableMaxHz (proc.getSampleRate())) : Verdict {};
            repaint();
            return;
        }
        const auto& history = proc.frameHistory();
        const auto sequence = history.sequence();
        if (sequence == lastSequence && !dirty)
            return;
        if (sequence != lastSequence)
        {
            lastSequence = sequence;
            period = std::max (history.framePeriodSeconds(), 1.0e-3);
            const int wanted = std::min (themis::FrameHistory::kCapacity, static_cast<int> (std::ceil (kSpanSeconds / period)) + 1);
            numFrames = history.copyRecent (frames.data(), wanted);
        }
        dirty = false;
        verdict = t.verdict && numFrames > 0 ? t.verdict (frames.data(), numFrames, column) : Verdict {};
        repaint();
    }

    // A setting the verdict or the zones depend on has changed: read the frames and judge them again at the next update().
    void refresh() { dirty = true; }

    void selectTab (int index)
    {
        tab = juce::jlimit (0, static_cast<int> (tabs.size()) - 1, index);
        hoverFrame = -1;
        hoverBand = -1;
        lastSequence = lastSpectrumSequence = ~uint64_t (0); // the two kinds of tab keep different frames: read again
        dirty = true;
        layout();
        repaint();
    }
    int selectedTab() const { return tab; }
    int selectedColumn() const { return column; }
    void selectColumn (int c)
    {
        column = juce::jlimit (0, themis::kAllRanges, c);
        dirty = true;
        repaint();
    }

    void resized() override { layout(); }

    void paint (juce::Graphics& g) override
    {
        ui::hw::drawModule (g, getLocalBounds().toFloat());
        ui::hw::drawGlass (g, verdictRow.reduced (0.0f, 1.0f), 3.0f, colours::amber);
        ui::hw::drawGlass (g, screen);

        const auto& t = tabs[static_cast<size_t> (tab)];
        if (t.spectrogram && specImage.isValid())
            g.drawImageAt (specImage, juce::roundToInt (plot.getX()), juce::roundToInt (plot.getY()));
        drawZones (g, t);
        drawGrid (g, t);
        if (t.marksClips)
            drawClipMarks (g);
        for (size_t i = 0; i < t.series.size(); ++i)
            if (!isHidden (i))
                drawSeries (g, t, t.series[i]);
        ui::hw::drawGlassOverlay (g, screen);
        ui::hw::drawGlassOverlay (g, verdictRow.reduced (0.0f, 1.0f), 3.0f, false);
        drawHeader (g, t);
        if (numFrames == 0)
        {
            g.setColour (colours::dim);
            g.setFont (font (13.0f));
            g.drawText ("Waiting for audio", plot.toNearestInt(), juce::Justification::centred);
        }
        else if (hoverFrame >= 0)
        {
            g.setColour (colours::text.withAlpha (0.35f));
            g.drawVerticalLine (juce::roundToInt (xFor (hoverFrame)), plot.getY(), plot.getBottom());
        }
    }

    void mouseMove (const juce::MouseEvent& e) override
    {
        setMouseCursor (hitAt (e.position) >= 0 ? juce::MouseCursor::PointingHandCursor : juce::MouseCursor::CrosshairCursor);
        int frame = -1, band = -1;
        if (numFrames > 0 && plot.contains (e.position))
        {
            band = juce::jlimit (0, themis::BandGrid::kNum - 1, static_cast<int> (std::lround ((plot.getBottom() - e.position.y) / plot.getHeight() * (themis::BandGrid::kNum - 1))));
            const double secondsAgo = (plot.getRight() - e.position.x) / plot.getWidth() * kSpanSeconds;
            frame = juce::jlimit (0, numFrames - 1, numFrames - 1 - static_cast<int> (std::lround (secondsAgo / period)));
        }
        if (frame != hoverFrame || band != hoverBand)
        {
            hoverFrame = frame;
            hoverBand = band;
            repaint();
        }
    }
    void mouseExit (const juce::MouseEvent&) override
    {
        hoverFrame = -1;
        hoverBand = -1;
        repaint();
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        const int hit = hitAt (e.position);
        if (hit < 0)
            return;
        const auto& h = hits[static_cast<size_t> (hit)];
        if (h.kind == Hit::tab)
            selectTab (h.index);
        else if (h.kind == Hit::range)
            selectColumn (h.index);
        else
        {
            auto& hide = hidden[static_cast<size_t> (tab)];
            hide[static_cast<size_t> (h.index)] = !hide[static_cast<size_t> (h.index)];
            repaint();
        }
    }

    juce::String getTooltip() override
    {
        const auto& t = tabs[static_cast<size_t> (tab)];
        return t.tooltip + (t.series.size() > 1 ? "\n\nClick an entry in the legend to hide or show its line." : juce::String());
    }

protected:
    HistoryPanel (ThemisProcessor& p, juce::String titleText, std::vector<Tab> t) : proc (p), title (std::move (titleText)), tabs (std::move (t))
    {
        setMouseCursor (juce::MouseCursor::CrosshairCursor);
        for (const auto& tabDef : tabs)
            hidden.emplace_back (tabDef.series.size(), char (0));
    }

    ThemisProcessor& proc;

private:
    static constexpr double kSpanSeconds = 60.0;
    static constexpr float kLegendWidth = 142.0f;
    static constexpr float kTitleRow = 20.0f, kLegendRow = 16.0f, kVerdictRow = 32.0f, kAxisLabels = 14.0f;

    struct Hit
    {
        enum Kind { tab, range, legend } kind;
        juce::Rectangle<float> area;
        int index;
    };

    static float textWidth (const juce::Font& f, const juce::String& text)
    {
        juce::GlyphArrangement glyphs;
        glyphs.addLineOfText (f, text, 0.0f, 0.0f);
        return glyphs.getBoundingBox (0, -1, true).getWidth();
    }

    bool isHidden (size_t series) const { return hidden[static_cast<size_t> (tab)][series] != 0; }

    // Places the header's clickable parts and the plot for the current size and tab.
    void layout()
    {
        hits.clear();
        auto area = getLocalBounds().toFloat().reduced (10.0f, 8.0f);
        auto row1 = area.removeFromTop (kTitleRow);
        legendRow = area.removeFromTop (kLegendRow);
        verdictRow = area.removeFromTop (kVerdictRow);
        area.removeFromTop (3.0f);
        screen = area;
        plot = screen.withTrimmedLeft (40.0f).withTrimmedTop (8.0f).withTrimmedBottom (kAxisLabels).withTrimmedRight (26.0f);

        titleArea = row1.removeFromLeft (textWidth (labelFont(), title.toUpperCase()) + 14.0f);
        const auto chipFont = labelFont (9.5f);
        for (int i = 0; i < static_cast<int> (tabs.size()); ++i)
        {
            const auto cell = row1.removeFromLeft (textWidth (chipFont, juce::String (tabs[static_cast<size_t> (i)].name).toUpperCase()) + 16.0f);
            hits.push_back ({ Hit::tab, cell.reduced (0.0f, 1.0f), i });
            row1.removeFromLeft (3.0f);
        }
        const auto& t = tabs[static_cast<size_t> (tab)];
        if (t.chooseRange)
            for (const int c : { 3, 2, 1, 0, themis::kAllRanges }) // right to left, so ALL ends up first
            {
                const auto cell = row1.removeFromRight (textWidth (chipFont, juce::String (themis::energyColumnName (c)).toUpperCase()) + 12.0f);
                hits.push_back ({ Hit::range, cell.reduced (0.0f, 1.0f), c });
                row1.removeFromRight (2.0f);
            }
        rangeLabelArea = row1;
        spectrumStale = true;
        dirty = true;
        auto legend = legendRow;
        for (int i = 0; i < static_cast<int> (t.series.size()); ++i)
            hits.push_back ({ Hit::legend, legend.removeFromLeft (kLegendWidth), i });
    }

    int hitAt (juce::Point<float> p) const
    {
        for (size_t i = 0; i < hits.size(); ++i)
            if (hits[i].area.contains (p))
                return static_cast<int> (i);
        return -1;
    }

    float xFor (int frame) const
    {
        const double secondsAgo = static_cast<double> (numFrames - 1 - frame) * period;
        return plot.getRight() - static_cast<float> (secondsAgo / kSpanSeconds) * plot.getWidth();
    }
    float yFor (const Axis& a, float value) const
    {
        return plot.getY() + plot.getHeight() * (a.top - juce::jlimit (a.bottom, a.top, value)) / (a.top - a.bottom);
    }
    float yFor (float value) const { return yFor (tabs[static_cast<size_t> (tab)].axis, value); }

    void drawZones (juce::Graphics& g, const Tab& t) const
    {
        g.setFont (font (10.5f));
        const auto zones = t.liveZones ? t.liveZones() : t.zones;
        for (const auto& z : zones)
        {
            const float y0 = yFor (t.axis, z.to), y1 = yFor (t.axis, z.from);
            g.setColour (z.colour.withAlpha (0.10f));
            g.fillRect (plot.getX(), y0, plot.getWidth(), y1 - y0);
            if (y1 - y0 >= 12.0f)
            {
                g.setColour (z.colour.withAlpha (0.75f));
                g.drawText (z.label, juce::Rectangle<float> (plot.getX() + 5.0f, y0 + 1.0f, 140.0f, 12.0f).toNearestInt(), juce::Justification::left);
            }
        }
    }

    void drawGrid (juce::Graphics& g, const Tab& t) const
    {
        g.setFont (font (11.0f));
        const auto gridline = [&] (float v) {
            const float y = yFor (t.axis, v);
            g.setColour (t.spectrogram ? colours::text.withAlpha (0.12f) : colours::glassGrid);
            g.drawHorizontalLine (juce::roundToInt (y), plot.getX(), plot.getRight());
            g.setColour (colours::dim);
            g.drawText (t.axis.tick ? t.axis.tick (v) : juce::String (juce::roundToInt (v)),
                        juce::Rectangle<float> (plot.getX() - 34.0f, y - 7.0f, 30.0f, 14.0f).toNearestInt(), juce::Justification::centredRight);
        };
        if (t.axis.marks.empty())
            for (float v = t.axis.top; v >= t.axis.bottom - 0.01f; v -= t.axis.step)
                gridline (v);
        else
            for (const float v : t.axis.marks)
                gridline (v);
        for (int s = 0; s <= static_cast<int> (kSpanSeconds); s += 20)
        {
            const float x = plot.getRight() - plot.getWidth() * static_cast<float> (s) / static_cast<float> (kSpanSeconds);
            g.setColour (colours::glassGrid);
            g.drawVerticalLine (juce::roundToInt (x), plot.getY(), plot.getBottom());
            g.setColour (colours::dim);
            g.drawText (s == 0 ? juce::String ("now") : "-" + juce::String (s) + " s",
                        juce::Rectangle<float> (x - 22.0f, plot.getBottom() + 3.0f, 44.0f, 12.0f).toNearestInt(), juce::Justification::centred);
        }
    }

    // A red column through the plot at every frame in which the audio clipped.
    void drawClipMarks (juce::Graphics& g) const
    {
        const float width = std::max (2.0f, plot.getWidth() * static_cast<float> (period / kSpanSeconds));
        g.setColour (colours::bad.withAlpha (0.45f));
        for (int i = 0; i < numFrames; ++i)
            if (frames[static_cast<size_t> (i)].clipped())
                g.fillRect (xFor (i) - width * 0.5f, plot.getY(), width, plot.getHeight());
    }

    // Lines break wherever a series has no value (silence; no fundamental found).
    void drawSeries (juce::Graphics& g, const Tab& t, const Series& s) const
    {
        juce::Path p;
        bool pen = false;
        for (int i = 0; i < numFrames; ++i)
        {
            const float v = s.value (frames[static_cast<size_t> (i)], column);
            if (!themis::hasLevel (v))
            {
                pen = false;
                continue;
            }
            const float x = xFor (i), y = yFor (t.axis, v);
            pen ? p.lineTo (x, y) : p.startNewSubPath (x, y);
            pen = true;
        }
        if (!s.dashed)
        {
            g.setColour (s.colour.withAlpha (0.14f)); // phosphor glow
            g.strokePath (p, juce::PathStrokeType (5.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        }
        g.setColour (s.colour);
        if (s.dashed)
        {
            juce::Path dashed;
            const float dashes[] = { 5.0f, 4.0f };
            juce::PathStrokeType (1.6f).createDashedStroke (dashed, p, dashes, 2);
            g.fillPath (dashed);
        }
        else
            g.strokePath (p, juce::PathStrokeType (2.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    }

    static juce::String defaultFormat (float v) { return juce::String (v, 1) + " dB"; }

    void drawChip (juce::Graphics& g, juce::Rectangle<float> r, const juce::String& text, bool active) const
    {
        ui::hw::drawPushButton (g, r, active, colours::amber);
        g.setFont (labelFont (9.5f));
        g.setColour (active ? colours::lampInk : colours::text);
        g.drawText (text.toUpperCase(), r.toNearestInt(), juce::Justification::centred);
    }

    void drawHeader (juce::Graphics& g, const Tab& t) const
    {
        hw::drawLabel (g, title, titleArea.toNearestInt());

        for (const auto& h : hits)
        {
            if (h.kind == Hit::tab)
                drawChip (g, h.area, tabs[static_cast<size_t> (h.index)].name, h.index == tab);
            else if (h.kind == Hit::range)
                drawChip (g, h.area, themis::energyColumnName (h.index), h.index == column);
        }
        if (t.chooseRange)
        {
            hw::drawLabel (g, "RANGE", rangeLabelArea.toNearestInt(), juce::Justification::right, colours::dim, 9.0f);
        }

        if (t.spectrogram)
            drawSpectrogramKey (g);
        const themis::FrameStats* shown = numFrames > 0 && !t.spectrogram ? &frames[static_cast<size_t> (hoverFrame >= 0 ? hoverFrame : numFrames - 1)] : nullptr;
        g.setFont (font (11.0f));
        const auto format = t.format ? t.format : std::function<juce::String (float)> (defaultFormat);
        for (const auto& h : hits)
        {
            if (h.kind != Hit::legend)
                continue;
            const auto& s = t.series[static_cast<size_t> (h.index)];
            const bool off = isHidden (static_cast<size_t> (h.index));
            const float fade = off ? 0.35f : 1.0f;
            g.setColour (s.colour.withMultipliedAlpha (fade));
            g.fillRoundedRectangle (h.area.getX(), h.area.getCentreY() - 1.0f, 12.0f, 2.0f, 1.0f); // a line sample, like the series itself
            const float v = shown != nullptr && !off ? s.value (*shown, column) : themis::kHistoryFloorDb;
            g.setColour (colours::text.withMultipliedAlpha (fade)); // text stays in the text colour; the swatch carries the identity
            g.drawText (juce::String (s.name) + "  " + (themis::hasLevel (v) ? format (v) : juce::String ("-")),
                        h.area.withTrimmedLeft (16.0f).toNearestInt(), juce::Justification::left);
        }

        // Verdict (left) and what is under the cursor (right).
        auto row = verdictRow;
        juce::String right;
        if (shown != nullptr)
        {
            if (hoverFrame >= 0)
                right = juce::String (static_cast<double> (numFrames - 1 - hoverFrame) * period, 1) + " s ago   ";
            if (t.detail)
                right += t.detail (*shown);
        }
        if (t.spectrogram && hoverFrame >= 0 && hoverBand >= 0)
        {
            const double hz = themis::BandGrid::centreHz (hoverBand);
            const float level = bandFrames[static_cast<size_t> (hoverFrame)][static_cast<size_t> (hoverBand)];
            right = juce::String (static_cast<double> (numFrames - 1 - hoverFrame) * period, 1) + " s ago   " + frequencyText (hz) + "Hz   "
                    + (themis::hasLevel (level) ? juce::String (level, 1) + " dB" : juce::String ("-"));
        }
        const auto detailArea = right.isEmpty() ? juce::Rectangle<float>() : row.removeFromRight (std::min (190.0f, row.getWidth() * 0.4f));
        if (verdict.text.isNotEmpty())
        {
            if (verdict.lamp)
                hw::drawLamp (g, { row.getX() + 9.0f, row.getCentreY() }, 3.6f, colours::forLevel (verdict.level));
            g.setColour (colours::text);
            g.setFont (font (11.5f));
            g.drawFittedText (verdict.text, row.withTrimmedLeft (verdict.lamp ? 20.0f : 8.0f).reduced (0.0f, 2.0f).toNearestInt(), juce::Justification::centredLeft, 2, 1.0f);
        }
        else if (numFrames > 0)
        {
            g.setColour (colours::dim);
            g.setFont (lcdFont (11.5f));
            g.drawText ("Collecting...", row.withTrimmedLeft (8.0f).toNearestInt(), juce::Justification::centredLeft);
        }
        if (right.isNotEmpty())
        {
            g.setColour (colours::dim);
            g.setFont (font (11.0f));
            g.drawText (right, detailArea.toNearestInt(), juce::Justification::topRight);
        }
    }

    // The spectrogram's colours: nothing, then teal phosphor, amber and the bright cream of the read-outs.
    static juce::Colour ramp (float b)
    {
        struct Stop { float at; juce::Colour colour; };
        static const Stop stops[] = { { 0.0f, colours::glass }, { 0.22f, juce::Colour (0xff0f3a40) }, { 0.5f, colours::accent.darker (0.2f) },
                                      { 0.78f, colours::amber }, { 1.0f, colours::text } };
        for (size_t i = 1; i < std::size (stops); ++i)
            if (b <= stops[i].at)
                return stops[i - 1].colour.interpolatedWith (stops[i].colour, (b - stops[i - 1].at) / (stops[i].at - stops[i - 1].at));
        return stops[std::size (stops) - 1].colour;
    }

    void drawSpectrogramKey (juce::Graphics& g) const
    {
        constexpr float kBarWidth = 100.0f;
        auto row = legendRow;
        g.setColour (colours::dim);
        g.setFont (font (10.5f));
        g.drawText ("quiet", row.removeFromLeft (34.0f).toNearestInt(), juce::Justification::centredLeft);
        const auto bar = row.removeFromLeft (kBarWidth).withSizeKeepingCentre (kBarWidth, 6.0f);
        for (int i = 0; i < static_cast<int> (kBarWidth); ++i)
        {
            g.setColour (ramp (static_cast<float> (i) / (kBarWidth - 1.0f)));
            g.fillRect (bar.getX() + static_cast<float> (i), bar.getY(), 1.0f, bar.getHeight());
        }
        g.setColour (colours::dim);
        g.drawText ("loud", row.removeFromLeft (34.0f).toNearestInt(), juce::Justification::centredRight);
        g.drawText ("levels tilted +" + juce::String (themis::spectro::kTiltDbPerOctave, 1) + " dB / octave around 1 kHz, as in the other plugins' displays",
                    row.withTrimmedLeft (10.0f).toNearestInt(), juce::Justification::centredLeft);
    }

    // One pixel column per moment, one row per position up the frequency axis (between two bands the colour is blended).
    void rebuildSpectrogram()
    {
        spectrumStale = false;
        const int w = juce::roundToInt (plot.getWidth()), h = juce::roundToInt (plot.getHeight());
        if (w < 2 || h < 2)
        {
            specImage = {};
            return;
        }
        specImage = juce::Image (juce::Image::ARGB, w, h, true);
        juce::Image::BitmapData bits (specImage, juce::Image::BitmapData::writeOnly);
        std::array<float, themis::BandGrid::kNum> shade {};
        for (int x = 0; x < w; ++x)
        {
            const double secondsAgo = static_cast<double> (w - 1 - x) / static_cast<double> (w) * kSpanSeconds;
            const int frame = numFrames - 1 - static_cast<int> (std::lround (secondsAgo / period));
            if (frame < 0 || frame >= numFrames)
                continue;
            for (int b = 0; b < themis::BandGrid::kNum; ++b)
                shade[static_cast<size_t> (b)] = themis::spectro::brightness (bandFrames[static_cast<size_t> (frame)][static_cast<size_t> (b)], themis::BandGrid::centreHz (b));
            for (int y = 0; y < h; ++y)
            {
                const float pos = static_cast<float> (h - 1 - y) / static_cast<float> (h - 1) * static_cast<float> (themis::BandGrid::kNum - 1);
                const int lo = std::min (static_cast<int> (pos), themis::BandGrid::kNum - 2);
                const float f = pos - static_cast<float> (lo);
                bits.setPixelColour (x, y, ramp (shade[static_cast<size_t> (lo)] * (1.0f - f) + shade[static_cast<size_t> (lo) + 1] * f));
            }
        }
    }

    juce::String title;
    std::vector<Tab> tabs;
    std::vector<std::vector<char>> hidden; // per tab, per series: hidden by a click on the legend
    std::vector<Hit> hits;
    std::array<themis::FrameStats, themis::FrameHistory::kCapacity> frames {};
    Verdict verdict;
    int numFrames = 0;
    int hoverFrame = -1, hoverBand = -1;
    int tab = 0;
    int column = themis::kAllRanges;
    bool dirty = true;
    uint64_t lastSequence = ~uint64_t (0), lastSpectrumSequence = ~uint64_t (0);
    std::vector<themis::BandLevels> bandFrames = std::vector<themis::BandLevels> (themis::SpectrumHistory::kCapacity);
    juce::Image specImage;
    bool spectrumStale = true;
    double period = 0.1;
    juce::Rectangle<float> plot, screen, titleArea, legendRow, verdictRow, rangeLabelArea;
};

// Categorical series colours: the first four slots, in order, of a dark-surface palette (blue, orange, aqua, yellow).
namespace palette
{
inline const juce::Colour blue { 0xff3987e5 }, orange { 0xffd95926 }, aqua { 0xff199e70 }, yellow { 0xffc98500 };
}

inline juce::String noteName (double hz)
{
    static const char* names[] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
    const int midi = juce::roundToInt (69.0 + 12.0 * std::log2 (hz / 440.0));
    return juce::String (names[((midi % 12) + 12) % 12]) + juce::String (midi / 12 - 1);
}

inline float midiOf (double hz) { return static_cast<float> (69.0 + 12.0 * std::log2 (hz / 440.0)); }
inline double hzOfMidi (float midi) { return 440.0 * std::pow (2.0, (static_cast<double> (midi) - 69.0) / 12.0); }
inline juce::String percentText (float share) { return juce::String (juce::roundToInt (share * 100.0f)) + " %"; }

// Where the audio's energy and peaks sit over time: per frequency range, per mid / side, per left / right, and the peaks
// with the frames that clipped.
class LevelView : public HistoryPanel
{
public:
    explicit LevelView (ThemisProcessor& p) : HistoryPanel (p, "LEVEL", { bands(), midSide(), leftRight(), peaks (p), spectrogram() }) {}

private:
    using Verdict = HistoryPanel::Verdict;
    static HistoryPanel::Axis dbAxis() { return { 0.0f, -72.0f, 12.0f, {} }; }

    static juce::String rangeText (int column) { return column == themis::kAllRanges ? juce::String ("The whole spectrum") : juce::String (themis::energyColumnName (column)); }

    static Tab bands()
    {
        const auto range = [] (int r, juce::Colour c) -> Series {
            return { themis::kEnergyRanges[r].name, c, [r] (const themis::FrameStats& f, int) { return f.energy (themis::Signal::stereo, r); } };
        };
        Tab t { "BANDS",
                "Energy of the stereo signal (mid + side) in each frequency range over the last minute, in dB re full scale; the dashed line "
                "is the whole spectrum. Unlike the tonal curve this is absolute: it follows the loudness of the programme.\n\n"
                "The verdict names the range whose level moves most between sections (10th to 90th percentile). Past about 6 dB the "
                "range is not holding steady, which matters most for the low end. That limit is a rule of thumb, not a target: a "
                "master can be dynamic on purpose.",
                dbAxis() };
        t.series = { range (0, palette::blue), range (1, palette::orange), range (2, palette::aqua), range (3, palette::yellow),
                     { "ALL", colours::text.withAlpha (0.8f), [] (const themis::FrameStats& f, int) { return f.energy (themis::Signal::stereo, themis::kAllRanges); }, true } };
        t.verdict = [] (const themis::FrameStats* f, int n, int) -> Verdict {
            const auto r = themis::history::analyseSwing (f, n);
            if (!r.have)
                return Verdict {};
            return Verdict { juce::String (themis::kEnergyRanges[r.widest].name) + " moves most: " + juce::String (r.swingDb, 1) + " dB between sections; "
                                 + themis::kEnergyRanges[r.steadiest].name + " is steadiest at " + juce::String (r.steadiestDb, 1) + " dB.",
                             r.level };
        };
        return t;
    }

    static Tab midSide()
    {
        Tab t { "MID / SIDE",
                "Energy of the mid signal ((L+R)/2: what is centred) and the side signal ((L-R)/2: what differs between the channels) over the "
                "last minute, in dB re full scale. The gap between them is the stereo width.\n\n"
                "Pick a RANGE to look at it on its own. The low range should be close to mono, with side far under mid, so the bass "
                "survives a mono sum and cuts cleanly. The verdict is the power a mono sum would lose, the same measure and limits as the "
                "MONO FOLD readouts (rules of thumb).",
                dbAxis() };
        t.chooseRange = true;
        t.series = { { "MID", palette::blue, [] (const themis::FrameStats& f, int c) { return f.energy (themis::Signal::mid, c); } },
                     { "SIDE", palette::orange, [] (const themis::FrameStats& f, int c) { return f.energy (themis::Signal::side, c); } } };
        t.verdict = [] (const themis::FrameStats* f, int n, int c) -> Verdict {
            const auto r = themis::history::analyseWidth (f, n, c);
            if (!r.have)
                return Verdict {};
            juce::String text = rangeText (c) + ": side is " + juce::String (std::abs (r.sideMinusMidDb), 1) + (r.sideMinusMidDb <= 0.0f ? " dB under" : " dB over")
                                + " mid; a mono sum loses " + juce::String (r.monoLossDb, 1) + " dB.";
            if (c == 0 && r.level != themis::Level::good)
                text += " Bass is best kept near mono.";
            return Verdict { text, r.level };
        };
        return t;
    }

    static Tab leftRight()
    {
        Tab t { "LEFT / RIGHT",
                "Energy of the left and right channels over the last minute, in dB re full scale. Pick a RANGE to compare them in one part "
                "of the spectrum.\n\n"
                "The verdict is the level difference over the window. A steady offset points to a balance or pan problem; "
                "more than about 0.5 dB is worth a look, beyond 1.5 dB it will be heard. Rules of thumb.",
                dbAxis() };
        t.chooseRange = true;
        t.series = { { "LEFT", palette::blue, [] (const themis::FrameStats& f, int c) { return f.energy (themis::Signal::left, c); } },
                     { "RIGHT", palette::orange, [] (const themis::FrameStats& f, int c) { return f.energy (themis::Signal::right, c); } } };
        t.verdict = [] (const themis::FrameStats* f, int n, int c) -> Verdict {
            const auto r = themis::history::analyseBalance (f, n, c);
            if (!r.have)
                return Verdict {};
            const float d = std::abs (r.leftMinusRightDb);
            const juce::String text = d < 0.05f ? juce::String (": left and right match.")
                                                : ": " + juce::String (r.leftMinusRightDb > 0.0f ? "left" : "right") + " is " + juce::String (d, 1) + " dB louder.";
            return Verdict { rangeText (c) + text, r.level };
        };
        return t;
    }

    static Tab spectrogram()
    {
        std::vector<float> marks;
        for (const double hz : { 50.0, 100.0, 200.0, 500.0, 1000.0, 2000.0, 5000.0, 10000.0, 20000.0 })
            marks.push_back (static_cast<float> (themis::BandGrid::indexForHz (hz)));
        Tab t { "SPECTROGRAM",
                "The spectrum over the last minute: frequency up the side (20 Hz to 20 kHz, a logarithmic axis), time across, and the level of "
                "every 1/6-octave band of the stereo signal as colour, from dark (quiet) through teal and amber to cream (loud). Levels are absolute "
                "and tilted by 4.5 dB per octave around 1 kHz, like the spectrum displays of the other plugins, so a typical programme looks about "
                "even instead of sloping down towards the highs.\n\n"
                "Use it to find when a problem happens: a harsh burst in the high mids, a sibilant spike, a ringing note that stays lit as a "
                "horizontal line, a section where the low end drops out. The verdict names the sharpest burst: the band and the moment where "
                "the level rose furthest above that band's usual (median) level. Drums make big bursts by nature, so it points at where to "
                "look and is never a fault. Hover for the band, the level and how long ago.",
                { 60.0f, 0.0f, 6.0f, [] (float v) { return frequencyText (themis::BandGrid::kF0 * std::pow (2.0, static_cast<double> (v) / themis::BandGrid::kPerOctave)).trimEnd(); }, marks } };
        t.spectrogram = true;
        t.spectrumVerdict = [] (const themis::BandLevels* f, int n, double period, double maxHz) -> Verdict {
            const auto r = themis::spectro::analyseBursts (f, n, maxHz);
            if (!r.have)
                return Verdict {};
            return Verdict { "Sharpest burst: " + frequencyText (themis::BandGrid::centreHz (r.band)) + "Hz, " + juce::String (r.excessDb, 1) + " dB above that band's usual level, "
                                 + juce::String (static_cast<double> (r.framesAgo) * period, 1) + " s ago. Where to look, not a fault.",
                             themis::Level::good, false };
        };
        return t;
    }

    static Tab peaks (ThemisProcessor& proc)
    {
        namespace clip = themis::history::clip;
        Tab t { "PEAKS",
                "The highest sample peak and true peak (inter-sample peak after 4x oversampling, in dBTP) in each analysed frame. A red column marks "
                "a frame in which the audio clipped.\n\n"
                "CLIPPED means a flat top: at least 4 equal samples in a row above -6 dBFS that the waveform ran into, which a hard clipper "
                "or an overdriven limiter leaves at any ceiling. OVER means a sample at or above 0 dBFS. A synthesised square wave or a held "
                "level looks like a flat top too, and a clip that was smoothed afterwards leaves none to find.\n\n"
                "The verdict is bad for any clip or over, a warning above the true-peak ceiling (the knob in the header, -1 dBTP unless you set another; "
                "lossy encoding can push those peaks over) and bad above 0 dBTP. Rules of thumb.",
                { 6.0f, -18.0f, 6.0f, [] (float v) { return juce::String (juce::roundToInt (v)); } } };
        t.liveZones = [&proc] {
            const float ceiling = std::min (proc.paramReader.truePeakCeilingDb(), clip::kTruePeakBadDb);
            return std::vector<Zone> { { 0.0f, 6.0f, colours::bad, "over 0 dB" }, { ceiling, 0.0f, colours::warn, juce::String (ceiling, 1) + " to 0 dBTP" } };
        };
        t.marksClips = true;
        // The true peak is never below the sample peak, so it goes first and the sample peak is drawn over it.
        t.series = { { "TRUE PEAK", palette::orange, [] (const themis::FrameStats& f, int) { return f.truePeakDb; } },
                     { "SAMPLE PEAK", palette::blue, [] (const themis::FrameStats& f, int) { return f.samplePeakDb; } } };
        t.verdict = [&proc] (const themis::FrameStats* f, int n, int) -> Verdict {
            const auto r = themis::history::analysePeaks (f, n, proc.paramReader.truePeakCeilingDb());
            if (!r.have)
                return Verdict {};
            const auto peak = "True peak " + juce::String (r.truePeakDb, 1) + " dBTP";
            if (r.clipEvents > 0 || r.overSamples > 0)
                return Verdict { juce::String ("CLIPPING: ") + juce::String (static_cast<juce::int64> (r.clipSamples)) + " flat-topped samples in "
                                     + juce::String (static_cast<juce::int64> (r.clipEvents)) + " clips, " + juce::String (static_cast<juce::int64> (r.overSamples))
                                     + " samples at 0 dBFS or over. " + peak + ".",
                                 r.level };
            return Verdict { peak + (r.level == themis::Level::good ? ": no clipping and room for lossy encoding."
                                     : r.level == themis::Level::warn ? ": no clipping, but over the ceiling, and lossy encoding can push this over 0 dB."
                                                                       : ": inter-sample peaks go over 0 dB.") ,
                             r.level };
        };
        t.detail = [] (const themis::FrameStats& f) {
            return f.clipped() ? "CLIPPED " + juce::String (juce::roundToInt (f.clipSamples)) + " samples" + (f.overSamples > 0.0f ? ", over 0 dBFS" : "") : juce::String();
        };
        return t;
    }
};

// The bass fundamental against its harmonics, and where it sits.
class FundamentalView : public HistoryPanel
{
public:
    explicit FundamentalView (ThemisProcessor& p) : HistoryPanel (p, "FUNDAMENTAL", { balance(), levels(), pitch() }) {}

private:
    using Verdict = HistoryPanel::Verdict;
    static juce::String pitchText (const themis::FrameStats& f) { return f.hasFundamental() ? juce::String (juce::roundToInt (f.f0Hz)) + " Hz  " + noteName (f.f0Hz) : juce::String ("no tonal bass"); }

    // What the BALANCE and LEVELS tabs conclude: where the fundamental sits against its harmonics most of the time.
    static Verdict balanceVerdict (const themis::FrameStats* f, int n, int)
    {
        using themis::fundamental::Balance;
        const auto r = themis::history::analyseBass (f, n);
        if (!r.have)
            return n >= themis::history::kMinFrames ? Verdict { "No tonal bass found in the last minute.", themis::Level::good } : Verdict {};
        juce::String text;
        std::array<int, 3> order { 0, 1, 2 };
        std::sort (order.begin(), order.end(), [&] (int a, int b) { return r.share[static_cast<size_t> (a)] > r.share[static_cast<size_t> (b)]; });
        for (const int i : order)
            if (r.share[static_cast<size_t> (i)] >= 0.05f)
                text += (text.isEmpty() ? juce::String() : juce::String (", ")) + themis::fundamental::name (static_cast<Balance> (i)) + " " + percentText (r.share[static_cast<size_t> (i)]);
        text += " of the time.";
        if (r.dominant == Balance::subHeavy)
            text += " Little above the fundamental for small speakers to play.";
        else if (r.dominant == Balance::harmonicRich)
            text += " The bass is carried by its harmonics.";
        return { text, r.balanceLevel };
    }

    static Tab balance()
    {
        namespace fund = themis::fundamental;
        Tab t { "BALANCE",
                "How far the bass fundamental (the lowest tonal peak between 30 and 250 Hz of the mid signal) sits above the summed level of "
                "its harmonics 2-8, over the last minute. The line breaks where no tonal bass is found.\n\n"
                "Above +15 dB the bass is sub-heavy: there is little above the fundamental for phones, laptops and small speakers to "
                "play, so it can vanish there. Below -6 dB it is harmonic-rich: carried by its upper partials, it translates "
                "everywhere but has less weight on full-range systems. Those limits are rules of thumb tuned on synthetic tones, not measured data.",
                { fund::kMaxTiltDb, -fund::kMaxTiltDb, 12.0f, [] (float v) { return (v > 0.0f ? "+" : "") + juce::String (juce::roundToInt (v)); } } };
        t.zones = { { fund::kSubHeavyDb, fund::kMaxTiltDb, colours::warn, "sub-heavy" },
                    { fund::kHarmonicRichDb, fund::kSubHeavyDb, colours::good, "balanced" },
                    { -fund::kMaxTiltDb, fund::kHarmonicRichDb, palette::blue, "harmonic-rich" } };
        t.series = { { "FUND vs HARM", colours::text, [] (const themis::FrameStats& f, int) { return fund::tiltDb (f); } } };
        t.format = [] (float v) { return v >= fund::kMaxTiltDb ? juce::String ("no harmonics") : (v > 0.0f ? "+" : "") + juce::String (v, 1) + " dB"; };
        t.verdict = balanceVerdict;
        t.detail = pitchText;
        return t;
    }

    static Tab levels()
    {
        Tab t { "LEVELS",
                "The level of the bass fundamental and the summed level of its harmonics 2-8, mean-square dBFS, measured on the mid signal over the "
                "last minute. The lines break where no tonal bass is found. The BALANCE tab plots the difference of the two.",
                { 0.0f, -72.0f, 12.0f, {} } };
        t.series = { { "FUND", palette::blue, [] (const themis::FrameStats& f, int) { return f.fundamentalLevelDb(); } },
                     { "HARMONICS", palette::orange, [] (const themis::FrameStats& f, int) { return f.harmonicsLevelDb(); } } };
        t.verdict = balanceVerdict;
        t.detail = pitchText;
        return t;
    }

    static Tab pitch()
    {
        Tab t { "PITCH",
                "The pitch of the bass fundamental over the last minute, on a note axis (a gridline per C). The shaded region is under 40 Hz, "
                "which phones, laptops and small monitors cannot reproduce: a master whose bass lives there for more than a quarter of the time "
                "depends on systems that can. A rule of thumb, not a measurement of any device.",
                { 60.0f, 20.0f, 12.0f, [] (float v) { return noteName (hzOfMidi (v)); } } };
        t.zones = { { 20.0f, midiOf (themis::history::kSubFloorHz), colours::warn, "under 40 Hz" } };
        t.series = { { "PITCH", palette::aqua, [] (const themis::FrameStats& f, int) { return f.hasFundamental() ? midiOf (f.f0Hz) : themis::kHistoryFloorDb; } } };
        t.format = [] (float v) { return juce::String (juce::roundToInt (hzOfMidi (v))) + " Hz  " + noteName (hzOfMidi (v)); };
        t.verdict = [] (const themis::FrameStats* f, int n, int) -> Verdict {
            const auto r = themis::history::analyseBass (f, n);
            if (!r.have)
                return n >= themis::history::kMinFrames ? Verdict { "No tonal bass found in the last minute.", themis::Level::good } : Verdict {};
            juce::String text = "Bass sits mostly between " + juce::String (juce::roundToInt (r.lowHz)) + " Hz (" + noteName (r.lowHz) + ") and "
                                + juce::String (juce::roundToInt (r.highHz)) + " Hz (" + noteName (r.highHz) + "), median " + juce::String (juce::roundToInt (r.medianHz))
                                + " Hz. " + percentText (r.belowFloorShare) + " of it is under 40 Hz";
            text += r.floorLevel == themis::Level::good ? "." : ": small speakers will not play that.";
            return { text, r.floorLevel };
        };
        t.detail = pitchText;
        return t;
    }
};
} // namespace ui
