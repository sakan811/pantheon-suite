// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 The Pantheon authors

#pragma once

#include "../DSP/Engine.h"
#include "../Parameters.h"
#include "HephaestusUi.h"
#include <juce_gui_basics/juce_gui_basics.h>
#include <pantheon/dsp/SpectrumAnalyzer.h>
#include <pantheon/ui/FreqAxis.h>

namespace hep::gui
{
// The logarithmic frequency axis shared by everything the view draws (the kit's).
using FreqAxis = ui::FreqAxis;

// Input (dim) vs output (cream) spectrum, with the user's bands drawn over it. Bands are made and
// edited here: double-click to add one, drag an edge to resize it, drag its body to move it,
// right-click for its mode and Remove. Saturate bands are amber, harmonic fill bands teal.
//
// Analysis and defaults follow the analyzer of a typical parametric EQ: 8192-point FFT, fast attack, selectable
// release, a 90 dB range and a 4.5 dB/oct tilt around 1 kHz. Range, speed and tilt are chosen from
// the right-click menu on empty space and saved with the project.
class SpectrumView : public juce::Component, public juce::SettableTooltipClient, private juce::Timer
{
public:
    SpectrumView (Engine& e, juce::AudioProcessorValueTreeState& s, const double& sampleRateRef)
        : engine (e), apvts (s), sampleRate (sampleRateRef)
    {
        range.load (apvts.state);
        speed.load (apvts.state);
        tilt.load (apvts.state);
        applyAnalyzerSettings();

        setTooltip ("Double-click to add a band. Drag a band's edges to resize it, its body to move it. "
                    "Right-click for the band menu, or on empty space for analyzer range, speed and tilt.");
        startTimerHz (30);
    }

    int getSelectedBand() const noexcept { return selected; }
    std::function<void (int)> onSelectionChanged;

    void setSelectedBand (int band)
    {
        if (band == selected)
            return;
        selected = band;
        repaint();
    }

    void paint (juce::Graphics& g) override
    {
        const auto window = getLocalBounds().toFloat();
        ui::hw::drawGlass (g, window, 4.0f, colours::amber);

        const auto plot = plotArea();
        g.saveState();
        g.reduceClipRegion (plot.toNearestInt());
        drawGrid (g, plot);
        drawBands (g, plot, false);
        drawTrace (g, plot, inTrace, colours::dim.withAlpha (0.7f), colours::dim.withAlpha (0.14f));
        drawTrace (g, plot, outTrace, colours::text, colours::text.withAlpha (0.16f));
        drawBands (g, plot, true);
        g.restoreState();

        drawAxisLabels (g, plot);
        ui::hw::drawGlassOverlay (g, window, 4.0f, true);
    }

    void mouseMove (const juce::MouseEvent& e) override
    {
        const auto hit = findHit (e.position);
        setMouseCursor (hit.part == Part::none ? juce::MouseCursor::NormalCursor
                        : hit.part == Part::body ? juce::MouseCursor::DraggingHandCursor
                                                 : juce::MouseCursor::LeftRightResizeCursor);
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        const auto hit = findHit (e.position);
        if (hit.part != Part::none)
            select (hit.band);

        if (e.mods.isPopupMenu())
        {
            if (hit.part != Part::none) showBandMenu (hit.band);
            else                        showSettingsMenu (e.position);
            return;
        }

        drag = {};
        if (hit.part == Part::none)
            return;

        const auto& ids = bandIds (hit.band);
        drag.hit = hit;
        drag.range = bandRange (apvts, hit.band);
        drag.startNormalised = xNormalised (e.position.x);
        drag.lo = apvts.getParameter (ids.lo);
        drag.hi = apvts.getParameter (ids.hi);
        drag.lo->beginChangeGesture();
        drag.hi->beginChangeGesture();
        drag.active = true;
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (! drag.active)
            return;

        const auto mode = bandMode (apvts, drag.hit.band);
        const double hz = FreqAxis::toHz (xNormalised (e.position.x));
        Range r = drag.range;

        if (drag.hit.part == Part::lowEdge)
            r.lo = std::clamp (hz, (double) FreqAxis::minHz, std::max ((double) FreqAxis::minHz, r.hi / minRangeRatio));
        else if (drag.hit.part == Part::highEdge)
            r.hi = std::clamp (hz, std::min (r.lo * minRangeRatio, (double) FreqAxis::maxHz), (double) FreqAxis::maxHz);
        else
        {
            // Slide both edges by the same number of octaves, stopping at the ends of the axis.
            const double octaves = (xNormalised (e.position.x) - drag.startNormalised) * std::log2 ((double) FreqAxis::maxHz / FreqAxis::minHz);
            const double limited = std::clamp (octaves, std::log2 (FreqAxis::minHz / drag.range.lo),
                                               std::log2 (FreqAxis::maxHz / drag.range.hi));
            r = { drag.range.lo * std::pow (2.0, limited), drag.range.hi * std::pow (2.0, limited) };
        }

        r = effectiveRange (mode, r.lo, r.hi);
        for (auto [param, value] : { std::pair<juce::RangedAudioParameter*, double> { drag.lo, r.lo }, { drag.hi, r.hi } })
            param->setValueNotifyingHost (param->convertTo0to1 ((float) value));
    }

    void mouseUp (const juce::MouseEvent&) override
    {
        if (! drag.active)
            return;
        drag.lo->endChangeGesture();
        drag.hi->endChangeGesture();
        drag.active = false;
    }

    void mouseDoubleClick (const juce::MouseEvent& e) override
    {
        if (findHit (e.position).part == Part::none)
            showAddMenu (e.position);
    }

private:
    static constexpr float topDb = -10.0f;

    // One selectable analyzer setting, persisted as a property on the plugin state.
    struct Setting
    {
        struct Option { const char* label; float value; };

        const char* title;
        const char* property;
        std::vector<Option> options;
        int selected;

        void load (const juce::ValueTree& state)
        {
            const float stored = state.getProperty (property, options[(size_t) selected].value);
            for (size_t i = 0; i < options.size(); ++i)
                if (std::fabs (options[i].value - stored) < 1.0e-4f)
                    selected = (int) i;
        }

        float value() const { return options[(size_t) selected].value; }
    };

    float rangeDb() const { return range.value(); }
    juce::Rectangle<float> plotArea() const { return getLocalBounds().toFloat().reduced (6.0f, 6.0f); }

    float xOf (float hz, juce::Rectangle<float> r) const { return r.getX() + FreqAxis::toNormalised (hz) * r.getWidth(); }

    float xNormalised (float x) const
    {
        const auto r = plotArea();
        return juce::jlimit (0.0f, 1.0f, (x - r.getX()) / r.getWidth());
    }

    enum class Part { none, lowEdge, highEdge, body };
    struct Hit { int band = -1; Part part = Part::none; };

    // Edges win over bodies; among bodies the narrowest wins, so a band inside another stays reachable.
    Hit findHit (juce::Point<float> at) const
    {
        constexpr float grab = 5.0f;
        const auto plot = plotArea();
        Hit best;
        float bestWidth = 1.0e9f, bestEdge = grab + 1.0f;

        for (int i = 0; i < maxBands; ++i)
        {
            if (! isBandOn (apvts, i))
                continue;

            const auto extent = bandRange (apvts, i);
            const float x0 = xOf ((float) extent.lo, plot), x1 = xOf ((float) extent.hi, plot);
            for (auto [x, part] : { std::pair<float, Part> { x0, Part::lowEdge }, { x1, Part::highEdge } })
                if (std::fabs (at.x - x) <= grab && std::fabs (at.x - x) < bestEdge)
                {
                    best = { i, part };
                    bestEdge = std::fabs (at.x - x);
                }

            if (bestEdge > grab && at.x > x0 && at.x < x1 && x1 - x0 < bestWidth)
            {
                best = { i, Part::body };
                bestWidth = x1 - x0;
            }
        }
        return best;
    }

    float yOf (float db, juce::Rectangle<float> r) const
    {
        const float t = (juce::jlimit (topDb - rangeDb(), topDb, db) - (topDb - rangeDb())) / rangeDb();
        return r.getY() + (1.0f - t) * r.getHeight();
    }

    void drawGrid (juce::Graphics& g, juce::Rectangle<float> plot) const
    {
        g.setColour (colours::glassGrid);
        for (float f : { 50.f, 100.f, 200.f, 500.f, 1000.f, 2000.f, 5000.f, 10000.f })
            g.drawVerticalLine ((int) xOf (f, plot), plot.getY(), plot.getBottom());

        const float step = rangeDb() > 100.0f ? 20.0f : 10.0f;
        g.setColour (colours::glassGrid);
        for (float db = topDb - step; db > topDb - rangeDb(); db -= step)
            g.drawHorizontalLine ((int) yOf (db, plot), plot.getX(), plot.getRight());
    }

    bool showsFill (int band) const
    {
        return bandMode (apvts, band) == BandMode::fill && apvts.getRawParameterValue (bandIds (band).amount)->load() > fillOffDb + 0.05f;
    }

    juce::String tabLabel (int band) const
    {
        juce::String label (band + 1);
        if (showsFill (band))
        {
            label << "  FILL " << juce::roundToInt (100.0f * engine.bandActivity[band].load (std::memory_order_relaxed)) << "%";
            const float reach = engine.bandReachDb[band].load (std::memory_order_relaxed);
            label << "  ABOVE " << (reach <= (float) FundamentalEngine::kReachFloorDb + 0.5f ? juce::String ("--")
                                                                                       : juce::String (juce::roundToInt (reach)))
                  << " dB";
        }
        return label;
    }

    // Each band's number tab sits on its lower edge. Tabs that would land on top of each other (bands that
    // share an edge, or sit close together) go to the first row where they fit, so every one stays readable.
    std::array<juce::Rectangle<float>, maxBands> layoutTabs (juce::Rectangle<float> plot) const
    {
        constexpr float rowHeight = 17.0f;
        std::array<juce::Rectangle<float>, maxBands> tabs {};

        for (int i = 0; i < maxBands; ++i)
        {
            if (! isBandOn (apvts, i))
                continue;

            const auto extent = bandRange (apvts, i);
            const float x0 = xOf ((float) extent.lo, plot), x1 = xOf ((float) extent.hi, plot);
            // A fill band's tab carries two read-outs (FILL and ABOVE), so it may stick out past a narrow band; the rows keep it clear.
            const float width = showsFill (i) ? 148.0f : std::min (20.0f, std::max (20.0f, x1 - x0 - 6.0f));

            for (int row = 0;; ++row)
            {
                const auto candidate = juce::Rectangle<float> (x0 + 3.0f, plot.getY() + 3.0f + rowHeight * (float) row, width, 15.0f);
                const bool clear = std::none_of (tabs.begin(), tabs.begin() + i, [&] (const auto& other)
                                                 { return other.expanded (2.0f, 1.0f).intersects (candidate); });
                if (clear || row == maxBands)
                {
                    tabs[(size_t) i] = candidate;
                    break;
                }
            }
        }
        return tabs;
    }

    void drawBands (juce::Graphics& g, juce::Rectangle<float> plot, bool selectedPass) const
    {
        bool any = false;
        const auto tabs = layoutTabs (plot);

        for (int i = 0; i < maxBands; ++i)
        {
            if (! isBandOn (apvts, i))
                continue;
            any = true;
            if ((i == selected) != selectedPass)
                continue;

            const auto mode = bandMode (apvts, i);
            const auto extent = bandRange (apvts, i);
            const auto tint = forMode (mode);
            const bool isSelected = i == selected;
            const float x0 = xOf ((float) extent.lo, plot), x1 = xOf ((float) extent.hi, plot);

            // Behind the traces, so the spectrum stays readable; the selected band is drawn on top as an outline only.
            g.setColour (tint.withAlpha (selectedPass ? 0.09f : 0.05f));
            g.fillRect (x0, plot.getY(), x1 - x0, plot.getHeight());

            const bool fillIsOff = mode == BandMode::fill && apvts.getRawParameterValue (bandIds (i).amount)->load() <= fillOffDb + 0.05f;
            if (! fillIsOff)
                drawHarmonics (g, plot, harmonicSpan (mode, extent.lo, extent.hi, sampleRate), tint, isSelected);
            if (isSelected && mode == BandMode::fill && ! fillIsOff && apvts.getRawParameterValue (bandIds (i).adapt)->load() > 0.5f)
                drawListening (g, plot, extent, tint);

            g.setColour (tint.withAlpha (isSelected ? 0.95f : 0.4f));
            g.drawVerticalLine ((int) x0, plot.getY(), plot.getBottom());
            g.drawVerticalLine ((int) x1, plot.getY(), plot.getBottom());
            if (isSelected)
            {
                g.drawVerticalLine ((int) x0 + 1, plot.getY(), plot.getBottom());
                g.drawVerticalLine ((int) x1 - 1, plot.getY(), plot.getBottom());
            }

            // Number tab, with what the adaptive fill is doing right now.
            const auto tab = tabs[(size_t) i];
            g.setColour (isSelected ? tint : tint.withAlpha (0.45f));
            g.fillRoundedRectangle (tab, 3.0f);
            g.setColour (colours::glass);
            g.setFont (juce::FontOptions (10.0f, juce::Font::bold));
            g.drawText (tabLabel (i), tab, juce::Justification::centred, false);
        }

        if (! any && ! selectedPass)
        {
            g.setColour (colours::dim);
            g.setFont (juce::FontOptions (13.0f));
            g.drawText ("Double-click to add a band", plot.reduced (0.0f, 40.0f), juce::Justification::centred);
        }
    }

    // Where the harmonics a band adds land (harmonicSpan): a faint zone for every band, bracketed and named for the selected one.
    void drawHarmonics (juce::Graphics& g, juce::Rectangle<float> plot, Range span, juce::Colour tint, bool isSelected) const
    {
        if (span.hi <= span.lo)
            return;

        const float x0 = xOf ((float) span.lo, plot), x1 = xOf ((float) span.hi, plot);
        if (x1 - x0 < 2.0f)
            return;

        g.setColour (tint.withAlpha (isSelected ? 0.07f : 0.035f));
        g.fillRect (x0, plot.getY(), x1 - x0, plot.getHeight());
        if (! isSelected)
            return;

        drawBracket (g, x0, x1, plot.getBottom() - 26.0f, tint.withAlpha (0.75f), "HARMONICS ADDED");
    }

    // Fill bands only: where the adaptive fill looks for energy that is already there (harmonicReach).
    void drawListening (juce::Graphics& g, juce::Rectangle<float> plot, Range band, juce::Colour tint) const
    {
        const auto reach = harmonicReach (band.hi);
        drawBracket (g, xOf ((float) reach.lo, plot), xOf ((float) reach.hi, plot), plot.getBottom() - 52.0f,
                     tint.withAlpha (0.6f), "ADAPT LISTENS HERE");
    }

    static void drawBracket (juce::Graphics& g, float x0, float x1, float y, juce::Colour line, const juce::String& text)
    {
        if (x1 - x0 < 2.0f)
            return;

        g.setColour (line);
        g.drawHorizontalLine ((int) y, x0, x1);
        g.drawVerticalLine ((int) x0, y - 4.0f, y + 4.0f);
        g.drawVerticalLine ((int) x1, y - 4.0f, y + 4.0f);
        // A dark pill behind the text keeps it readable over the traces and the band edges.
        const juce::FontOptions font (9.5f, juce::Font::bold);
        juce::GlyphArrangement glyphs;
        glyphs.addLineOfText (juce::Font (font), text, 0.0f, 0.0f);
        const float textWidth = glyphs.getBoundingBox (0, -1, true).getWidth() + 10.0f;
        const auto pill = juce::Rectangle<float> (textWidth, 12.0f).withCentre ({ 0.5f * (x0 + x1), y - 10.0f });
        g.setColour (colours::glass.withAlpha (0.8f));
        g.fillRoundedRectangle (pill, 3.0f);
        g.setColour (line);
        g.setFont (juce::Font (font));
        g.drawText (text, pill, juce::Justification::centred, false);
    }

    void drawAxisLabels (juce::Graphics& g, juce::Rectangle<float> plot) const
    {
        g.setFont (juce::FontOptions (10.0f));
        g.setColour (colours::dim);

        for (float f : { 20.f, 50.f, 100.f, 200.f, 500.f, 1000.f, 2000.f, 5000.f, 10000.f, 20000.f })
        {
            const auto text = f >= 1000.f ? juce::String (f / 1000.f, 0) + "k" : juce::String ((int) f);
            const int x = (int) xOf (f, plot) + 3;
            g.drawText (text, std::min (x, (int) plot.getRight() - 28), (int) plot.getBottom() - 14, 28, 12, juce::Justification::left);
        }

        const float step = rangeDb() > 100.0f ? 20.0f : 10.0f;
        for (float db = topDb - step; db > topDb - rangeDb(); db -= step)
            g.drawText (juce::String ((int) db), (int) plot.getRight() - 30, (int) yOf (db, plot) - 12, 26, 12, juce::Justification::right);

        g.setColour (colours::dim);
        g.drawText ("INPUT", (int) plot.getRight() - 170, (int) plot.getY() + 4, 60, 12, juce::Justification::right);
        g.setColour (colours::text);
        g.drawText ("OUTPUT", (int) plot.getRight() - 100, (int) plot.getY() + 4, 70, 12, juce::Justification::right);
    }

    void drawTrace (juce::Graphics& g, juce::Rectangle<float> plot, const pdsp::SpectrumAnalyzer& analyzer,
                    juce::Colour line, juce::Colour fill) const
    {
        juce::Path path;
        const int width = (int) plot.getWidth();
        const float binHz = (float) analyzer.binFrequency (1);

        for (int px = 0; px <= width; ++px)
        {
            const float f0 = FreqAxis::toHz ((float) px / (float) width);
            const float f1 = FreqAxis::toHz ((float) (px + 1) / (float) width);

            // Narrower than a bin: interpolate. Wider: show the strongest bin the pixel covers.
            const float db = (f1 - f0) < binHz ? analyzer.levelAtFrequency (0.5 * (f0 + f1))
                                                : analyzer.peakInRange (f0, f1);
            const float x = plot.getX() + (float) px, y = yOf (db, plot);
            if (px == 0) path.startNewSubPath (x, y); else path.lineTo (x, y);
        }

        juce::Path filled (path);
        filled.lineTo (plot.getRight(), plot.getBottom());
        filled.lineTo (plot.getX(), plot.getBottom());
        filled.closeSubPath();
        g.setColour (fill);
        g.fillPath (filled);
        g.setColour (line);
        g.strokePath (path, juce::PathStrokeType (1.4f));
    }

    void select (int band)
    {
        if (band == selected)
            return;
        selected = band;
        repaint();
        if (onSelectionChanged)
            onSelectionChanged (band);
    }

    void addBandAt (BandMode mode, float x)
    {
        const int slot = addBand (apvts, mode, FreqAxis::toHz (xNormalised (x)));
        if (slot >= 0)
            select (slot);
    }

    void showAddMenu (juce::Point<float> at)
    {
        juce::PopupMenu menu;
        menu.addItem (1, "Add Saturate band here");
        menu.addItem (2, "Add Harmonic Fill band here");
        menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (this).withTargetScreenArea (
                                localAreaToGlobal (juce::Rectangle<int> ((int) at.x, (int) at.y, 1, 1))),
                            [this, x = at.x] (int result)
                            {
                                if (result > 0)
                                    addBandAt (result == 2 ? BandMode::fill : BandMode::saturate, x);
                            });
    }

    void showBandMenu (int band)
    {
        juce::PopupMenu menu;
        const auto mode = bandMode (apvts, band);
        menu.addItem (1, "Saturate", true, mode == BandMode::saturate);
        menu.addItem (2, "Harmonic Fill", true, mode == BandMode::fill);
        menu.addSeparator();
        menu.addItem (3, "Remove band");
        menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (this),
                            [this, band] (int result)
                            {
                                if (result == 1 || result == 2)
                                    setBandMode (apvts, band, result == 2 ? BandMode::fill : BandMode::saturate);
                                else if (result == 3)
                                    removeBand (apvts, band);
                                repaint();
                            });
    }

    void showSettingsMenu (juce::Point<float> at)
    {
        juce::PopupMenu menu;
        int nextId = 1;
        std::vector<std::function<void()>> actions;

        menu.addItem (nextId++, "Add Saturate band here");
        actions.push_back ([this, at] { addBandAt (BandMode::saturate, at.x); });
        menu.addItem (nextId++, "Add Harmonic Fill band here");
        actions.push_back ([this, at] { addBandAt (BandMode::fill, at.x); });

        for (Setting* setting : { &range, &speed, &tilt })
        {
            if (setting == &range) menu.addSeparator();
            menu.addSectionHeader (setting->title);
            for (size_t i = 0; i < setting->options.size(); ++i)
            {
                menu.addItem (nextId++, setting->options[i].label, true, (int) i == setting->selected);
                actions.push_back ([this, setting, i] { setting->selected = (int) i; applyAnalyzerSettings(); });
            }
        }

        menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (this),
                            [this, actions] (int result)
                            {
                                if (result > 0)
                                {
                                    actions[(size_t) result - 1]();
                                    repaint();
                                }
                            });
    }

    void applyAnalyzerSettings()
    {
        for (auto* a : { &inTrace, &outTrace })
        {
            a->setTilt (tilt.value());
            a->setReleaseSeconds (speed.value());
        }
        for (Setting* setting : { &range, &speed, &tilt })
            apvts.state.setProperty (setting->property, setting->value(), nullptr);
    }

    void timerCallback() override
    {
        for (auto* a : { &inTrace, &outTrace })
            a->setSampleRate (sampleRate);

        inTrace.update (engine.tapIn);
        outTrace.update (engine.tapOut);
        repaint();
    }

    Engine& engine;
    juce::AudioProcessorValueTreeState& apvts;
    const double& sampleRate;

    // Defaults: 90 dB range and 4.5 dB/oct tilt; medium release.
    Setting range { "Range", "spectrumRange", { { "60 dB", 60.0f }, { "90 dB", 90.0f }, { "120 dB", 120.0f } }, 1 };
    Setting speed { "Speed", "spectrumSpeed", { { "Slow", 0.8f }, { "Medium", 0.4f }, { "Fast", 0.15f } }, 1 };
    Setting tilt  { "Tilt",  "spectrumTilt",  { { "0 dB/oct", 0.0f }, { "3 dB/oct", 3.0f }, { "4.5 dB/oct", pdsp::SpectrumAnalyzer::defaultTilt }, { "6 dB/oct", 6.0f } }, 2 };

    pdsp::SpectrumAnalyzer inTrace, outTrace;

    int selected = 0;

    struct Drag
    {
        bool active = false;
        Hit hit;
        Range range { 0.0, 0.0 };
        float startNormalised = 0.0f;
        juce::RangedAudioParameter* lo = nullptr;
        juce::RangedAudioParameter* hi = nullptr;
    } drag;
};
}
