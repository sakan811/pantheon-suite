// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 The Pantheon authors

#pragma once

#include "../DSP/Engine.h"
#include "../Parameters.h"
#include "IrisUi.h"

#include <juce_gui_basics/juce_gui_basics.h>
#include <pantheon/dsp/SpectrumAnalyzer.h>
#include <pantheon/ui/FreqAxis.h>

namespace iris::gui
{
using FreqAxis = ui::FreqAxis;

// The equaliser's display. Behind: the spectrum before (dim) and after (cream) the equaliser. Over it: the curve each channel
// ends up with, one node per band, and along the bottom the five frequency regions, each a button that solos that part of the
// spectrum (and a handle between two of them that moves the crossover).
//
// Bands are made and edited here: double-click empty space to add a bell, drag a node (frequency across, gain up and down;
// Shift for fine), turn the wheel over a node for Q, right-click a node for its menu. A dynamic band shows its range as a bar
// from its resting gain, and a lit tick where the dynamics have moved it to.
//
// Curves: bands on Stereo shape both channels; Left, Right, Mid and Side shape only theirs. The display draws one curve for
// each channel that has a band of its own (Left, Right, Mid, Side), and a single curve when every band is on Stereo.
class EqView : public juce::Component, public juce::SettableTooltipClient, private juce::Timer
{
public:
    EqView (Engine& e, juce::AudioProcessorValueTreeState& s, const double& sampleRateRef)
        : engine (e), apvts (s), sampleRate (sampleRateRef)
    {
        for (Setting* setting : { &spectrumRange, &speed, &tilt, &gainScale })
            setting->load (apvts.state);
        applyAnalyzerSettings();

        setTooltip ("Double-click to add a band. Drag a node to set its frequency and gain (Shift = fine), turn the wheel over it for Q. "
                    "Right-click a node for its menu. The strip along the bottom solos a part of the spectrum.");
        startTimerHz (30);
    }

    std::function<void (int)> onSelectionChanged;

    int getSelectedBand() const noexcept { return selected; }

    void setSelectedBand (int band)
    {
        band = juce::jlimit (0, maxBands - 1, band);
        if (band == selected)
            return;
        selected = band;
        repaint();
        if (onSelectionChanged)
            onSelectionChanged (band);
    }

    // ---- monitoring -----------------------------------------------------------------------------
    // Solos a band in one of its two ways, or releases it if that is what is already soloed.
    void toggleBandSolo (int band, SoloKind kind)
    {
        auto& m = engine.monitor;
        if (m.soloBand.load() == band && m.soloKind.load() == (int) kind)
            m.soloBand = -1;
        else
        {
            m.soloKind = (int) kind;
            m.soloBand = band;
        }
        repaint();
    }

    void toggleRegion (int region, bool exclusive = false)
    {
        auto& m = engine.monitor;
        const unsigned bit = 1u << region;
        m.regions = exclusive ? (m.regions.load() == bit ? 0u : bit) : (m.regions.load() ^ bit);
        repaint();
    }

    void clearSolos()
    {
        engine.monitor.soloBand = -1;
        engine.monitor.regions = 0u;
        repaint();
    }

    bool anySolo() const { return engine.monitor.soloBand.load() >= 0 || engine.monitor.regions.load() != 0; }

    // ---- painting -------------------------------------------------------------------------------
    void paint (juce::Graphics& g) override
    {
        const auto window = getLocalBounds().toFloat();
        ui::hw::drawGlass (g, window, 4.0f, colours::amber);

        const auto plot = plotArea();
        const auto bands = readParams (apvts).bands;
        const auto crossover = crossovers();

        g.saveState();
        g.reduceClipRegion (plot.toNearestInt());
        drawRegionShading (g, plot, crossover);
        drawGrid (g, plot);
        drawTrace (g, plot, inTrace, colours::dim.withAlpha (0.7f), colours::dim.withAlpha (0.12f));
        drawTrace (g, plot, outTrace, colours::text.withAlpha (0.85f), colours::text.withAlpha (0.10f));
        drawCurves (g, plot, bands);
        drawNodes (g, plot, bands);
        g.restoreState();

        drawAxisLabels (g, plot);
        drawStrip (g, crossover);
        drawSoloPill (g, plot);
        ui::hw::drawGlassOverlay (g, window, 4.0f, true);
    }

    void mouseMove (const juce::MouseEvent& e) override
    {
        hover = e.position;
        if (stripArea().contains (e.position))
            setMouseCursor (boundaryAt (e.position.x) >= 0 ? juce::MouseCursor::LeftRightResizeCursor : juce::MouseCursor::PointingHandCursor);
        else if (pillArea().contains (e.position) && anySolo())
            setMouseCursor (juce::MouseCursor::PointingHandCursor);
        else
            setMouseCursor (nodeAt (e.position) >= 0 ? juce::MouseCursor::DraggingHandCursor : juce::MouseCursor::NormalCursor);
        repaint();
    }

    void mouseExit (const juce::MouseEvent&) override
    {
        hover = { -1.0f, -1.0f };
        repaint();
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        drag = {};

        if (pillArea().contains (e.position) && anySolo())
        {
            clearSolos();
            return;
        }

        if (stripArea().contains (e.position))
        {
            if (e.mods.isPopupMenu())
            {
                showStripMenu();
                return;
            }
            const int boundary = boundaryAt (e.position.x);
            if (boundary >= 0)
            {
                drag.kind = Drag::boundary;
                drag.index = boundary;
                drag.param = apvts.getParameter (id::crossover[(size_t) boundary]);
                drag.param->beginChangeGesture();
                return;
            }
            toggleRegion (regionAt (e.position.x), e.mods.isAltDown());
            return;
        }

        const int hit = nodeAt (e.position);
        if (hit >= 0)
            setSelectedBand (hit);

        if (e.mods.isPopupMenu())
        {
            if (hit >= 0) showBandMenu (hit);
            else          showSettingsMenu (e.position);
            return;
        }

        if (hit < 0)
            return;

        const auto& ids = bandIds (hit);
        const auto b = bandParamsOf (apvts, hit);
        drag.kind = Drag::node;
        drag.index = hit;
        drag.freq = apvts.getParameter (ids.freq);
        drag.gain = hasGain (b.type) ? apvts.getParameter (ids.gain) : nullptr;
        drag.startFreq = b.freq;
        drag.startGain = b.gainDb;
        drag.start = e.position;
        drag.freq->beginChangeGesture();
        if (drag.gain != nullptr)
            drag.gain->beginChangeGesture();
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (drag.kind == Drag::boundary)
        {
            auto xo = crossovers();
            const int k = drag.index;
            const double lo = k == 0 ? minHz : xo[(size_t) k - 1] * 1.25, hi = k == numCrossovers - 1 ? maxHz : xo[(size_t) k + 1] / 1.25;
            const double hz = std::clamp ((double) FreqAxis::toHz (xNormalised (e.position.x)), lo, std::max (lo, hi));
            drag.param->setValueNotifyingHost (drag.param->convertTo0to1 ((float) hz));
            return;
        }

        if (drag.kind != Drag::node)
            return;

        const float fine = e.mods.isShiftDown() ? 0.2f : 1.0f;
        const auto plot = plotArea();
        const float dx = (e.position.x - drag.start.x) * fine / plot.getWidth();
        const double octaves = dx * std::log2 ((double) FreqAxis::maxHz / FreqAxis::minHz);
        const double hz = std::clamp (drag.startFreq * std::pow (2.0, octaves), minHz, maxHz);
        drag.freq->setValueNotifyingHost (drag.freq->convertTo0to1 ((float) hz));

        if (drag.gain != nullptr)
        {
            const double dGain = -(e.position.y - drag.start.y) * fine * 2.0 * gainScale.value() / plot.getHeight();
            const double gain = std::clamp (drag.startGain + dGain, (double) minGainDb, (double) maxGainDb);
            drag.gain->setValueNotifyingHost (drag.gain->convertTo0to1 ((float) gain));
        }
    }

    void mouseUp (const juce::MouseEvent&) override
    {
        if (drag.kind == Drag::boundary)
            drag.param->endChangeGesture();
        else if (drag.kind == Drag::node)
        {
            drag.freq->endChangeGesture();
            if (drag.gain != nullptr)
                drag.gain->endChangeGesture();
        }
        drag = {};
    }

    void mouseDoubleClick (const juce::MouseEvent& e) override
    {
        if (stripArea().contains (e.position))
        {
            const int boundary = boundaryAt (e.position.x);
            if (boundary >= 0)
                if (auto* p = apvts.getParameter (id::crossover[(size_t) boundary]))
                    setParameter (*p, p->getDefaultValue());
            return;
        }
        if (nodeAt (e.position) < 0)
            addBandAt (BandType::bell, e.position);
    }

    void mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel) override
    {
        const int band = nodeAt (e.position);
        if (band < 0)
            return;
        setSelectedBand (band);
        if (wheel.deltaY == 0.0f) // a sideways scroll is not a step up or down
            return;

        const auto b = bandParamsOf (apvts, band);
        const auto& ids = bandIds (band);
        if (isCut (b.type))
        {
            if (auto* slope = apvts.getParameter (ids.slope))
            {
                const int next = juce::jlimit (0, 3, b.slope + (wheel.deltaY > 0.0f ? 1 : -1));
                setParameter (*slope, slope->convertTo0to1 ((float) next));
            }
            return;
        }
        if (auto* q = apvts.getParameter (ids.q))
            setParameter (*q, q->convertTo0to1 ((float) std::clamp (b.q * std::exp (wheel.deltaY * 2.5), 0.1, 18.0)));
    }

private:
    static constexpr float stripHeight = 24.0f, nodeRadius = 8.0f;

    // One analyzer or display setting, persisted as a property on the plugin state.
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

    struct Drag
    {
        enum Kind { none, node, boundary } kind = none;
        int index = -1;
        juce::RangedAudioParameter *freq = nullptr, *gain = nullptr, *param = nullptr;
        double startFreq = 0.0, startGain = 0.0;
        juce::Point<float> start;
    } drag;

    // ---- geometry -------------------------------------------------------------------------------
    juce::Rectangle<float> contentArea() const { return getLocalBounds().toFloat().reduced (6.0f, 6.0f); }
    juce::Rectangle<float> plotArea() const { return contentArea().withTrimmedBottom (stripHeight + 4.0f); }
    juce::Rectangle<float> stripArea() const { return contentArea().removeFromBottom (stripHeight); }
    juce::Rectangle<float> pillArea() const { return { plotArea().getX() + 6.0f, plotArea().getY() + 5.0f, 400.0f, 18.0f }; }

    float xOf (float hz) const
    {
        const auto r = plotArea();
        return r.getX() + FreqAxis::toNormalised (hz) * r.getWidth();
    }

    float xNormalised (float x) const
    {
        const auto r = plotArea();
        return juce::jlimit (0.0f, 1.0f, (x - r.getX()) / r.getWidth());
    }

    float gainY (double db) const
    {
        const auto r = plotArea();
        const float scale = gainScale.value();
        return r.getCentreY() - (float) juce::jlimit ((double) -scale, (double) scale, db) / scale * r.getHeight() * 0.5f;
    }

    float spectrumY (float db) const
    {
        const auto r = plotArea();
        const float range = spectrumRange.value();
        const float t = (juce::jlimit (topDb - range, topDb, db) - (topDb - range)) / range;
        return r.getY() + (1.0f - t) * r.getHeight();
    }

    std::array<double, numCrossovers> crossovers() const
    {
        std::array<double, numCrossovers> xo {};
        for (int k = 0; k < numCrossovers; ++k)
            xo[(size_t) k] = apvts.getRawParameterValue (id::crossover[(size_t) k])->load();
        for (int k = 1; k < numCrossovers; ++k)
            xo[(size_t) k] = std::max (xo[(size_t) k], xo[(size_t) k - 1] * 1.2); // the same order the engine puts them in
        return xo;
    }

    juce::Point<float> nodePosition (const BandParams& b) const
    {
        return { xOf ((float) b.freq), hasGain (b.type) ? gainY (b.gainDb) : plotArea().getCentreY() };
    }

    // The band under a point; of nodes on top of each other, the selected one wins, else the one drawn last.
    int nodeAt (juce::Point<float> at) const
    {
        const auto bands = readParams (apvts).bands;
        int best = -1;
        float bestDistance = nodeRadius + 3.0f;
        for (int i = 0; i < maxBands; ++i)
        {
            if (! bands[(size_t) i].on)
                continue;
            const float d = at.getDistanceFrom (nodePosition (bands[(size_t) i]));
            if (d < bestDistance || (d <= bestDistance && i == selected))
            {
                best = i;
                bestDistance = d;
            }
        }
        return best;
    }

    int regionAt (float x) const
    {
        const auto xo = crossovers();
        int region = 0;
        for (int k = 0; k < numCrossovers; ++k)
            if (x >= xOf ((float) xo[(size_t) k]))
                region = k + 1;
        return region;
    }

    // The handle between two regions that is within reach of x, or -1.
    int boundaryAt (float x) const
    {
        const auto xo = crossovers();
        int best = -1;
        float distance = 6.0f;
        for (int k = 0; k < numCrossovers; ++k)
        {
            const float d = std::fabs (x - xOf ((float) xo[(size_t) k]));
            if (d <= distance)
            {
                best = k;
                distance = d;
            }
        }
        return best;
    }

    // ---- drawing --------------------------------------------------------------------------------
    void drawGrid (juce::Graphics& g, juce::Rectangle<float> plot) const
    {
        g.setColour (colours::glassGrid);
        for (float f : { 50.f, 100.f, 200.f, 500.f, 1000.f, 2000.f, 5000.f, 10000.f })
            g.drawVerticalLine ((int) xOf (f), plot.getY(), plot.getBottom());

        const float scale = gainScale.value(), step = scale <= 6.0f ? 3.0f : (scale <= 12.0f ? 6.0f : 12.0f);
        for (float db = step; db < scale + 0.01f; db += step)
            for (float sign : { 1.0f, -1.0f })
                g.drawHorizontalLine ((int) gainY (sign * db), plot.getX(), plot.getRight());

        g.setColour (colours::dim.withAlpha (0.45f)); // 0 dB
        g.drawHorizontalLine ((int) gainY (0.0), plot.getX(), plot.getRight());
    }

    void drawAxisLabels (juce::Graphics& g, juce::Rectangle<float> plot) const
    {
        g.setFont (juce::FontOptions (10.0f));
        g.setColour (colours::dim);
        for (float f : { 20.f, 50.f, 100.f, 200.f, 500.f, 1000.f, 2000.f, 5000.f, 10000.f, 20000.f })
        {
            const auto text = f >= 1000.f ? juce::String (f / 1000.f, 0) + "k" : juce::String ((int) f);
            const int x = (int) xOf (f) + 3;
            g.drawText (text, std::min (x, (int) plot.getRight() - 28), (int) plot.getBottom() - 14, 28, 12, juce::Justification::left);
        }

        const float scale = gainScale.value(), step = scale <= 6.0f ? 3.0f : (scale <= 12.0f ? 6.0f : 12.0f);
        for (float db = 0.0f; db < scale - 0.01f; db += step) // the end lines have no label: it would sit on the edge of the glass
            for (float sign : { 1.0f, -1.0f })
            {
                if (db == 0.0f && sign < 0.0f)
                    continue;
                const auto text = db == 0.0f ? juce::String ("0") : (sign > 0 ? "+" : "-") + juce::String ((int) db);
                g.drawText (text, (int) plot.getRight() - 30, (int) gainY (sign * db) - 12, 26, 12, juce::Justification::right);
            }
    }

    void drawTrace (juce::Graphics& g, juce::Rectangle<float> plot, const pdsp::SpectrumAnalyzer& analyzer, juce::Colour line, juce::Colour fill) const
    {
        juce::Path path;
        const int width = (int) plot.getWidth();
        const float binHz = (float) analyzer.binFrequency (1);

        for (int px = 0; px <= width; ++px)
        {
            const float f0 = FreqAxis::toHz ((float) px / (float) width), f1 = FreqAxis::toHz ((float) (px + 1) / (float) width);
            // Narrower than a bin: interpolate. Wider: show the strongest bin the pixel covers.
            const float db = (f1 - f0) < binHz ? analyzer.levelAtFrequency (0.5 * (f0 + f1)) : analyzer.peakInRange (f0, f1);
            const float x = plot.getX() + (float) px, y = spectrumY (db);
            if (px == 0) path.startNewSubPath (x, y); else path.lineTo (x, y);
        }

        juce::Path filled (path);
        filled.lineTo (plot.getRight(), plot.getBottom());
        filled.lineTo (plot.getX(), plot.getBottom());
        filled.closeSubPath();
        g.setColour (fill);
        g.fillPath (filled);
        g.setColour (line);
        g.strokePath (path, juce::PathStrokeType (1.2f));
    }

    // Soloed regions are lit and the rest are dimmed, so the part of the spectrum being listened to is plain to see.
    void drawRegionShading (juce::Graphics& g, juce::Rectangle<float> plot, const std::array<double, numCrossovers>& xo) const
    {
        const unsigned mask = engine.monitor.regions.load();
        if (mask == 0)
            return;
        for (int r = 0; r < numRegions; ++r)
        {
            const float x0 = r == 0 ? plot.getX() : xOf ((float) xo[(size_t) r - 1]);
            const float x1 = r == numRegions - 1 ? plot.getRight() : xOf ((float) xo[(size_t) r]);
            const bool lit = (mask & (1u << r)) != 0;
            g.setColour (lit ? colours::amber.withAlpha (0.09f) : juce::Colours::black.withAlpha (0.38f));
            g.fillRect (x0, plot.getY(), x1 - x0, plot.getHeight());
        }
    }

    struct ChannelCurves
    {
        std::array<std::vector<float>, 4> db; // L, R, M, S
        std::array<bool, 4> own {};           // a band of its own on that channel
        bool any = false;
    };

    // The summed response of every band for each channel, one value per two pixels.
    ChannelCurves computeCurves (juce::Rectangle<float> plot, const std::array<BandParams, maxBands>& bands) const
    {
        constexpr int step = 2;
        const int points = (int) plot.getWidth() / step + 2;
        ChannelCurves c;
        for (auto& v : c.db)
            v.assign ((size_t) points, 0.0f);

        for (int i = 0; i < maxBands; ++i)
        {
            const auto& b = bands[(size_t) i];
            if (! b.on)
                continue;
            c.any = true;
            c.own[0] = c.own[0] || b.place == Placement::left;
            c.own[1] = c.own[1] || b.place == Placement::right;
            c.own[2] = c.own[2] || b.place == Placement::mid;
            c.own[3] = c.own[3] || b.place == Placement::side;

            // Stereo bands shape left, right, mid and side alike.
            const bool toL = touchesLeft (b.place) || b.place == Placement::stereo, toR = touchesRight (b.place);
            const bool toM = touchesMid (b.place) || b.place == Placement::stereo, toS = touchesSide (b.place) || b.place == Placement::stereo;

            BandFilter filter;
            const double dyn = engine.dynamicDb[i].load (std::memory_order_relaxed);
            filter.configure (b.type, sampleRate, b.freq, b.q, std::clamp (b.gainDb + dyn, -36.0, 36.0), orderForSlope (b.slope));
            for (int p = 0; p < points; ++p)
            {
                const double hz = FreqAxis::toHz ((float) (p * step) / plot.getWidth());
                const float db = (float) (20.0 * std::log10 (std::max (filter.magnitudeAt (sampleRate, hz), 1.0e-9)));
                if (toL) c.db[0][(size_t) p] += db;
                if (toR) c.db[1][(size_t) p] += db;
                if (toM) c.db[2][(size_t) p] += db;
                if (toS) c.db[3][(size_t) p] += db;
            }
        }
        return c;
    }

    juce::Path pathOf (juce::Rectangle<float> plot, const std::vector<float>& db) const
    {
        juce::Path path;
        for (size_t p = 0; p < db.size(); ++p)
        {
            const float x = juce::jmin (plot.getRight(), plot.getX() + (float) (p * 2)), y = gainY (db[p]);
            if (p == 0) path.startNewSubPath (x, y); else path.lineTo (x, y);
        }
        return path;
    }

    void drawCurves (juce::Graphics& g, juce::Rectangle<float> plot, const std::array<BandParams, maxBands>& bands) const
    {
        const auto curves = computeCurves (plot, bands);
        if (! curves.any)
            return;

        // The selected band's own curve, filled to the 0 dB line.
        if (bands[(size_t) selected].on)
        {
            const auto& b = bands[(size_t) selected];
            ChannelCurves single;
            auto only = bands;
            for (int i = 0; i < maxBands; ++i)
                only[(size_t) i].on = i == selected;
            single = computeCurves (plot, only);
            const auto& v = single.db[b.place == Placement::right ? 1 : (b.place == Placement::mid ? 2 : (b.place == Placement::side ? 3 : 0))];
            auto path = pathOf (plot, v);
            path.lineTo (plot.getRight(), gainY (0.0));
            path.lineTo (plot.getX(), gainY (0.0));
            path.closeSubPath();
            g.setColour (forPlacement (b.place).withAlpha (0.16f));
            g.fillPath (path);
        }

        const bool anyOwn = std::any_of (curves.own.begin(), curves.own.end(), [] (bool v) { return v; });
        const auto draw = [&] (const std::vector<float>& v, juce::Colour colour, bool thick)
        {
            g.setColour (juce::Colours::black.withAlpha (0.5f));
            g.strokePath (pathOf (plot, v), juce::PathStrokeType (thick ? 3.6f : 3.0f));
            g.setColour (colour);
            g.strokePath (pathOf (plot, v), juce::PathStrokeType (thick ? 2.0f : 1.6f));
        };

        if (! anyOwn)
        {
            draw (curves.db[0], colours::amber, true);
            return;
        }
        const bool showLR = curves.own[0] || curves.own[1], showMS = curves.own[2] || curves.own[3];
        if (showLR) { draw (curves.db[0], forPlacement (Placement::left), false); draw (curves.db[1], forPlacement (Placement::right), false); }
        if (showMS) { draw (curves.db[2], forPlacement (Placement::mid), false);  draw (curves.db[3], forPlacement (Placement::side), false); }
        if (! showLR && ! showMS)
            draw (curves.db[0], colours::amber, true);
    }

    void drawNodes (juce::Graphics& g, juce::Rectangle<float>, const std::array<BandParams, maxBands>& bands) const
    {
        const int soloed = engine.monitor.soloBand.load();
        const int hovered = nodeAt (hover);

        for (int pass = 0; pass < 2; ++pass)
            for (int i = 0; i < maxBands; ++i)
            {
                const auto& b = bands[(size_t) i];
                if (! b.on || (i == selected) != (pass == 1))
                    continue;

                const auto colour = forPlacement (b.place);
                const auto c = nodePosition (b);

                // A dynamic band: its resting gain, the range it can move through, and where it is now.
                if (b.isDynamic())
                {
                    const float y0 = gainY (b.gainDb), y1 = gainY (b.gainDb + b.dyn.rangeDb);
                    const float live = gainY (b.gainDb + engine.dynamicDb[i].load (std::memory_order_relaxed));
                    g.setColour (colours::text.withAlpha (0.25f));
                    g.drawLine (c.x, y0, c.x, y1, 5.0f);
                    g.setColour (colour.withAlpha (0.8f));
                    g.drawLine (c.x, y0, c.x, y1, 1.5f);
                    g.setColour (colours::text);
                    g.fillRect (c.x - 5.0f, y1 - 1.0f, 10.0f, 2.0f);
                    g.setColour (juce::Colours::white);
                    g.fillEllipse (juce::Rectangle<float> (7.0f, 7.0f).withCentre ({ c.x, live }));
                }

                const bool isSelected = i == selected, lit = i == hovered || isSelected;
                g.setColour (juce::Colours::black.withAlpha (0.55f));
                g.fillEllipse (juce::Rectangle<float> (nodeRadius * 2.0f + 3.0f, nodeRadius * 2.0f + 3.0f).withCentre (c + juce::Point<float> (0.0f, 1.0f)));
                g.setGradientFill (juce::ColourGradient (colour.brighter (lit ? 0.5f : 0.2f), c.x - 3.0f, c.y - 4.0f, colour.darker (0.35f), c.x + 5.0f, c.y + 6.0f, true));
                g.fillEllipse (juce::Rectangle<float> (nodeRadius * 2.0f, nodeRadius * 2.0f).withCentre (c));
                if (isSelected || i == soloed)
                {
                    g.setColour (i == soloed ? juce::Colours::white : colours::text);
                    g.drawEllipse (juce::Rectangle<float> (nodeRadius * 2.0f + 4.0f, nodeRadius * 2.0f + 4.0f).withCentre (c), i == soloed ? 2.0f : 1.2f);
                }
                g.setColour (colours::lampInk);
                g.setFont (ui::labelFont (10.5f));
                g.drawText (juce::String (i + 1), juce::Rectangle<float> (nodeRadius * 2.0f, nodeRadius * 2.0f).withCentre (c).toNearestInt(), juce::Justification::centred, false);

                if (b.place != Placement::stereo)
                {
                    g.setColour (colour);
                    g.setFont (ui::labelFont (9.0f));
                    g.drawText (placementLetter (b.place), juce::Rectangle<float> (14.0f, 12.0f).withCentre (c + juce::Point<float> (0.0f, -nodeRadius - 8.0f)).toNearestInt(), juce::Justification::centred, false);
                }
            }
    }

    // The five regions as one row of push switches along the bottom of the glass.
    void drawStrip (juce::Graphics& g, const std::array<double, numCrossovers>& xo) const
    {
        namespace hw = ui::hw;
        const auto strip = stripArea();
        const unsigned mask = engine.monitor.regions.load();
        const int overBoundary = stripArea().contains (hover) ? boundaryAt (hover.x) : -1;
        const int overRegion = stripArea().contains (hover) && overBoundary < 0 ? regionAt (hover.x) : -1;

        for (int r = 0; r < numRegions; ++r)
        {
            const float x0 = r == 0 ? strip.getX() : xOf ((float) xo[(size_t) r - 1]);
            const float x1 = r == numRegions - 1 ? strip.getRight() : xOf ((float) xo[(size_t) r]);
            const auto cell = juce::Rectangle<float> (x0 + 1.5f, strip.getY(), x1 - x0 - 3.0f, strip.getHeight());
            const bool lit = (mask & (1u << r)) != 0;

            hw::drawPushButton (g, cell, lit, colours::amber, false, r == overRegion);
            g.setColour (lit ? colours::lampInk : colours::text);
            g.setFont (ui::labelFont (10.0f));
            const juce::String name = juce::String (regionName (r)).toUpperCase();
            g.drawText (cell.getWidth() > 70.0f ? name : (cell.getWidth() > 30.0f ? name.substring (0, 3) : juce::String()), cell.toNearestInt(), juce::Justification::centred, false);
        }

        // The handles: where the crossovers are.
        for (int k = 0; k < numCrossovers; ++k)
        {
            const float x = xOf ((float) xo[(size_t) k]);
            const bool active = k == overBoundary || (drag.kind == Drag::boundary && drag.index == k);
            juce::Path grip;
            grip.addTriangle (x - 4.0f, strip.getY() - 1.0f, x + 4.0f, strip.getY() - 1.0f, x, strip.getY() + 5.0f);
            g.setColour (active ? colours::text : colours::dim);
            g.fillPath (grip);
            if (active)
            {
                const auto hz = xo[(size_t) k];
                g.setColour (colours::glass);
                const auto tip = juce::Rectangle<float> (62.0f, 16.0f).withCentre ({ x, strip.getY() - 14.0f });
                g.fillRoundedRectangle (tip, 3.0f);
                g.setColour (colours::amber);
                g.setFont (ui::lcdFont (11.0f));
                g.drawText (hz < 1000.0 ? juce::String (juce::roundToInt (hz)) + " Hz" : juce::String (hz / 1000.0, 2) + " kHz", tip.toNearestInt(), juce::Justification::centred, false);
            }
        }
    }

    juce::String soloSummary() const
    {
        juce::StringArray parts;
        const int band = engine.monitor.soloBand.load();
        if (band >= 0)
            parts.add ("BAND " + juce::String (band + 1) + (engine.monitor.soloKind.load() == (int) SoloKind::curve ? " CURVE" : " REGION"));
        const unsigned mask = engine.monitor.regions.load();
        for (int r = 0; r < numRegions; ++r)
            if (mask & (1u << r))
                parts.add (juce::String (regionName (r)).toUpperCase());
        return "SOLO  " + parts.joinIntoString (" + ");
    }

    void drawSoloPill (juce::Graphics& g, juce::Rectangle<float> plot) const
    {
        if (! anySolo())
            return;
        const auto text = soloSummary();
        const auto font = ui::labelFont (10.0f);
        constexpr float lampSpace = 24.0f, clearSpace = 52.0f;
        const float textWidth = juce::GlyphArrangement::getStringWidth (font, text);
        const float width = juce::jmin (plot.getWidth() - 12.0f, textWidth + lampSpace + clearSpace);
        const auto pill = juce::Rectangle<float> (plot.getX() + 6.0f, plot.getY() + 5.0f, width, 18.0f);
        g.setColour (colours::glass.withAlpha (0.9f));
        g.fillRoundedRectangle (pill, 9.0f);
        g.setColour (colours::amber.withAlpha (0.7f));
        g.drawRoundedRectangle (pill, 9.0f, 1.0f);
        ui::hw::drawLamp (g, { pill.getX() + 11.0f, pill.getCentreY() }, 3.5f, colours::amber, true);
        g.setFont (font);
        g.setColour (colours::amber);
        g.drawText (text, pill.withTrimmedLeft (lampSpace).withTrimmedRight (clearSpace).toNearestInt(), juce::Justification::centredLeft, false);
        g.setColour (colours::dim);
        g.drawText ("CLEAR", pill.withTrimmedLeft (pill.getWidth() - clearSpace).toNearestInt(), juce::Justification::centred, false);
    }

    // ---- menus ----------------------------------------------------------------------------------
    void addBandAt (BandType type, juce::Point<float> at)
    {
        const auto plot = plotArea();
        const double hz = FreqAxis::toHz (xNormalised (at.x));
        const double gain = hasGain (type) ? -(at.y - plot.getCentreY()) / (plot.getHeight() * 0.5) * gainScale.value() : 0.0;
        const int slot = addBand (apvts, type, hz, type == BandType::bell && std::fabs (gain) < 0.5 ? 0.0 : gain);
        if (slot >= 0)
            setSelectedBand (slot);
    }

    void showStripMenu()
    {
        juce::PopupMenu menu;
        menu.addItem (1, "Clear region solo", engine.monitor.regions.load() != 0);
        menu.addItem (2, "Reset the four crossovers");
        menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (this), [this] (int result)
        {
            if (result == 1)
                engine.monitor.regions = 0u;
            else if (result == 2)
                for (int k = 0; k < numCrossovers; ++k)
                    if (auto* p = apvts.getParameter (id::crossover[(size_t) k]))
                        setParameter (*p, p->getDefaultValue());
            repaint();
        });
    }

    void showBandMenu (int band)
    {
        const auto b = bandParamsOf (apvts, band);
        const auto& ids = bandIds (band);
        juce::PopupMenu types, places;
        for (int t = 0; t < typeNames.size(); ++t)
            types.addItem (100 + t, typeNames[t], true, (int) b.type == t);
        for (int p = 0; p < placeNames.size(); ++p)
            places.addItem (200 + p, placeNames[p], true, (int) b.place == p);

        juce::PopupMenu menu;
        menu.addSubMenu ("Type", types);
        menu.addSubMenu ("Placement", places);
        menu.addItem (300, "Dynamic", hasGain (b.type), b.dynamic);
        menu.addSeparator();
        menu.addItem (400, "Solo band region", true, engine.monitor.soloBand.load() == band && engine.monitor.soloKind.load() == (int) SoloKind::band);
        menu.addItem (401, "Solo band curve", true, engine.monitor.soloBand.load() == band && engine.monitor.soloKind.load() == (int) SoloKind::curve);
        menu.addSeparator();
        menu.addItem (500, "Remove band");

        menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (this), [this, band, ids] (int result)
        {
            if (result >= 100 && result < 200)
                setParameterValue (apvts, ids.type, (float) (result - 100));
            else if (result >= 200 && result < 300)
                setParameterValue (apvts, ids.place, (float) (result - 200));
            else if (result == 300)
                setParameterValue (apvts, ids.dyn, apvts.getRawParameterValue (ids.dyn)->load() > 0.5f ? 0.0f : 1.0f);
            else if (result == 400)
                toggleBandSolo (band, SoloKind::band);
            else if (result == 401)
                toggleBandSolo (band, SoloKind::curve);
            else if (result == 500)
                removeBand (apvts, band);
            repaint();
        });
    }

    void showSettingsMenu (juce::Point<float> at)
    {
        juce::PopupMenu menu;
        int nextId = 1;
        std::vector<std::function<void()>> actions;

        for (int t = 0; t < typeNames.size(); ++t)
        {
            menu.addItem (nextId++, "Add " + typeNames[t] + " here");
            actions.push_back ([this, at, t] { addBandAt ((BandType) t, at); });
        }
        for (Setting* setting : { &gainScale, &spectrumRange, &speed, &tilt })
        {
            menu.addSeparator();
            menu.addSectionHeader (setting->title);
            for (size_t i = 0; i < setting->options.size(); ++i)
            {
                menu.addItem (nextId++, setting->options[i].label, true, (int) i == setting->selected);
                actions.push_back ([this, setting, i] { setting->selected = (int) i; applyAnalyzerSettings(); });
            }
        }

        menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (this), [this, actions] (int result)
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
        for (Setting* setting : { &spectrumRange, &speed, &tilt, &gainScale })
            apvts.state.setProperty (setting->property, setting->value(), nullptr);
    }

    void timerCallback() override
    {
        // A band that is removed (or a preset that frees its slot) takes its solo with it; otherwise the pill would say SOLO while the
        // engine plays everything, and the solo would come back the moment a band was put in that slot again.
        if (const int soloed = engine.monitor.soloBand.load(); soloed >= 0 && ! isBandOn (apvts, soloed))
            engine.monitor.soloBand = -1;

        for (auto* a : { &inTrace, &outTrace })
            a->setSampleRate (sampleRate);

        inTrace.update (engine.tapIn);
        outTrace.update (engine.tapOut);
        repaint();
    }

    static constexpr float topDb = -10.0f;

    Engine& engine;
    juce::AudioProcessorValueTreeState& apvts;
    const double& sampleRate;

    // Defaults: a 90 dB analyzer range and 4.5 dB/oct tilt, medium release; the gain axis shows +-12 dB.
    Setting spectrumRange { "Analyzer range", "spectrumRange", { { "60 dB", 60.0f }, { "90 dB", 90.0f }, { "120 dB", 120.0f } }, 1 };
    Setting speed { "Analyzer speed", "spectrumSpeed", { { "Slow", 0.8f }, { "Medium", 0.4f }, { "Fast", 0.15f } }, 1 };
    Setting tilt  { "Analyzer tilt", "spectrumTilt", { { "0 dB/oct", 0.0f }, { "3 dB/oct", 3.0f }, { "4.5 dB/oct", pdsp::SpectrumAnalyzer::defaultTilt }, { "6 dB/oct", 6.0f } }, 2 };
    Setting gainScale { "Gain scale", "gainScale", { { "+/- 6 dB", 6.0f }, { "+/- 12 dB", 12.0f }, { "+/- 18 dB", 18.0f }, { "+/- 24 dB", 24.0f } }, 1 };

    pdsp::SpectrumAnalyzer inTrace, outTrace;
    int selected = 0;
    juce::Point<float> hover { -1.0f, -1.0f };
};
} // namespace iris::gui
