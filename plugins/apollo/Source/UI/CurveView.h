// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 The Pantheon authors

#pragma once

#include "../Parameters.h"
#include "ApolloUi.h"
#include <pantheon/dsp/SpectrumAnalyzer.h>
#include <pantheon/ui/FreqAxis.h>

namespace apollo::gui
{
// The curve of the bands on glass, and a node for each: drag it sideways for the frequency and up and down for the gain, turn the wheel over it for the width,
// double-click it to switch the band on or off. The curve is `responseDb` of the settings, so it is the filter that runs (minimum phase and linear phase have
// the same magnitude). Behind the curve, an analyser draws the sound before the bands (dim) and after them (bright), on its own scale (kAnalyzerTopDb down
// kAnalyzerRangeDb) with the tilt every analyser of the repository has; the ANALYZER switch in the corner turns it off, and it is kept with the session.
class CurveView : public juce::Component
{
public:
    CurveView (juce::AudioProcessorValueTreeState& state, std::function<double()> sampleRateOf, pdsp::StereoTap& before, pdsp::StereoTap& after)
        : apvts (state), sampleRate (std::move (sampleRateOf)), tapBefore (before), tapAfter (after) {}

    static constexpr float kAnalyzerTopDb = -10.0f, kAnalyzerRangeDb = 90.0f;
    pdsp::SpectrumAnalyzer inTrace, outTrace;

    bool analyzerOn() const { return (bool) apvts.state.getProperty (analyzerProperty, true); }
    void setAnalyzer (bool on) { apvts.state.setProperty (analyzerProperty, on, nullptr); repaint(); }
    juce::Rectangle<float> analyzerButton() const { return getLocalBounds().toFloat().removeFromTop (22.0f).removeFromRight (84.0f).reduced (4.0f, 3.0f); }

    // The editor's timer calls this: it takes what the audio thread has sent and updates the two traces. Off, nothing is read (and nothing is computed).
    void tick()
    {
        if (! analyzerOn())
            return;
        const double fs = std::max (sampleRate(), 8000.0);
        if (fs != analyzerRate)
        {
            analyzerRate = fs;
            inTrace.setSampleRate (fs);
            outTrace.setSampleRate (fs);
        }
        inTrace.update (tapBefore);
        outTrace.update (tapAfter);
    }

    std::function<void (int)> onSelect;
    int selected = 0;

    juce::Rectangle<float> plot() const { return getLocalBounds().toFloat().reduced (34.0f, 12.0f).withTrimmedBottom (10.0f); }
    float xOf (double hz) const { const auto r = plot(); return r.getX() + r.getWidth() * ui::FreqAxis::toNormalised ((float) hz); }
    float yOf (double db) const { const auto r = plot(); return r.getCentreY() - r.getHeight() * 0.5f * (float) (db / kCurveRangeDb); }
    double hzAt (float x) const { const auto r = plot(); return ui::FreqAxis::toHz (juce::jlimit (0.0f, 1.0f, (x - r.getX()) / r.getWidth())); }
    double dbAt (float y) const { const auto r = plot(); return juce::jlimit (-kCurveRangeDb, kCurveRangeDb, (r.getCentreY() - y) / (r.getHeight() * 0.5f) * kCurveRangeDb); }

    // Where a band's node is: on its gain, or on the 0 dB line for a band that has none.
    juce::Point<float> nodeOf (int band) const
    {
        const auto b = readParams (apvts).bands[(size_t) band];
        return { xOf (b.freqHz), yOf (hasGain (b.type) ? b.gainDb : 0.0) };
    }

    // The band whose node is nearest to a point, within the reach of a mouse; -1 if none.
    int nodeNear (juce::Point<float> p) const
    {
        int best = -1;
        float bestDistance = 16.0f;
        for (int i = 0; i < kBands; ++i)
        {
            const float d = nodeOf (i).getDistanceFrom (p);
            if (d < bestDistance)
            {
                best = i;
                bestDistance = d;
            }
        }
        return best;
    }

    // What dragging a node to a point does (the mouse calls it; the checks call it too).
    void dragTo (int band, juce::Point<float> p)
    {
        const auto& ids = bandIds (band);
        const auto type = readParams (apvts).bands[(size_t) band].type;
        set (ids.freq, (float) juce::jlimit (kMinHz, kMaxHz, hzAt (p.x)));
        if (hasGain (type))
            set (ids.gain, (float) dbAt (p.y));
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        if (analyzerButton().contains (e.position))
        {
            setAnalyzer (! analyzerOn());
            return;
        }
        dragged = nodeNear (e.position);
        if (dragged < 0)
            return;
        select (dragged);
        for (auto* name : { &bandIds (dragged).freq, &bandIds (dragged).gain })
            apvts.getParameter (*name)->beginChangeGesture();
    }
    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (dragged >= 0)
            dragTo (dragged, e.position);
    }
    void mouseUp (const juce::MouseEvent&) override
    {
        if (dragged < 0)
            return;
        for (auto* name : { &bandIds (dragged).freq, &bandIds (dragged).gain })
            apvts.getParameter (*name)->endChangeGesture();
        dragged = -1;
    }
    void mouseDoubleClick (const juce::MouseEvent& e) override
    {
        if (const int band = nodeNear (e.position); band >= 0)
        {
            auto* on = apvts.getParameter (bandIds (band).on);
            on->setValueNotifyingHost (on->getValue() > 0.5f ? 0.0f : 1.0f);
            select (band);
        }
    }
    void mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& w) override
    {
        if (const int band = nodeNear (e.position); band >= 0)
        {
            const float q = readParams (apvts).bands[(size_t) band].q;
            set (bandIds (band).q, juce::jlimit (0.1f, 10.0f, q * std::pow (1.25f, w.deltaY > 0.0f ? 1.0f : -1.0f)));
            select (band);
        }
    }

    void paint (juce::Graphics& g) override
    {
        const auto glass = getLocalBounds().toFloat();
        ui::hw::drawGlass (g, glass, 4.0f, colours::accent);
        const auto r = plot();
        const auto params = readParams (apvts);
        const double fs = std::max (sampleRate(), 8000.0);

        g.setFont (ui::lcdFont (9.5f));
        for (int d = (int) -kCurveRangeDb; d <= (int) kCurveRangeDb; d += 6)
        {
            g.setColour (d == 0 ? colours::glassGrid.brighter (0.5f) : colours::glassGrid);
            g.drawHorizontalLine ((int) yOf (d), r.getX(), r.getRight());
            g.setColour (colours::dim);
            g.drawText ((d > 0 ? "+" : "") + juce::String (d), juce::Rectangle<float> (glass.getX() + 4.0f, yOf (d) - 7.0f, 26.0f, 14.0f), juce::Justification::centredRight);
        }
        for (const double hz : { 20.0, 50.0, 100.0, 200.0, 500.0, 1000.0, 2000.0, 5000.0, 10000.0, 20000.0 })
        {
            g.setColour (colours::glassGrid);
            g.drawVerticalLine ((int) xOf (hz), r.getY(), r.getBottom());
            g.setColour (colours::dim);
            g.drawText (hz < 1000.0 ? juce::String ((int) hz) : juce::String ((int) (hz / 1000.0)) + "k", juce::Rectangle<float> (xOf (hz) - 16.0f, r.getBottom() + 1.0f, 32.0f, 11.0f), juce::Justification::centred);
        }

        if (analyzerOn())
        {
            drawTrace (g, inTrace, colours::dim.withAlpha (0.7f), colours::dim.withAlpha (0.12f));
            drawTrace (g, outTrace, colours::text.withAlpha (0.85f), colours::text.withAlpha (0.10f));
        }

        const auto curve = [&] (const std::array<BandParams, kBands>& bands, Target path)
        {
            juce::Path p;
            const int steps = (int) r.getWidth();
            for (int i = 0; i <= steps; ++i)
            {
                const double hz = ui::FreqAxis::toHz ((float) i / (float) steps);
                const float y = juce::jlimit (r.getY(), r.getBottom(), yOf (responseDb (bands, fs, hz, path)));
                i == 0 ? p.startNewSubPath (r.getX(), y) : p.lineTo (r.getX() + (float) i, y);
            }
            return p;
        };
        bool split = false;
        for (int i = 0; i < kBands; ++i)
        {
            const auto& b = params.bands[(size_t) i];
            if (isIdentity (b))
                continue;
            split = split || b.target != Target::stereo;
            std::array<BandParams, kBands> alone {};
            for (auto& a : alone)
                a.on = false;
            alone[0] = b;
            alone[0].target = Target::mid;
            g.setColour (bandColour (i).withAlpha (i == selected ? 0.55f : 0.25f));
            g.strokePath (curve (alone, Target::mid), juce::PathStrokeType (1.0f));
        }
        if (split)
        {
            g.setColour (colours::accent.withAlpha (0.8f));
            g.strokePath (curve (params.bands, Target::side), juce::PathStrokeType (1.2f));
        }
        g.setColour (colours::amber);
        g.strokePath (curve (params.bands, Target::mid), juce::PathStrokeType (1.8f));

        for (int i = 0; i < kBands; ++i)
        {
            const auto b = params.bands[(size_t) i];
            const auto c = nodeOf (i);
            const bool live = ! isIdentity (b) || b.on;
            g.setColour (bandColour (i).withAlpha (live ? 1.0f : 0.35f));
            g.fillEllipse (c.x - 8.0f, c.y - 8.0f, 16.0f, 16.0f);
            if (i == selected)
            {
                g.setColour (colours::text);
                g.drawEllipse (c.x - 11.0f, c.y - 11.0f, 22.0f, 22.0f, 1.5f);
            }
            g.setColour (colours::lampInk);
            g.setFont (ui::lcdFont (10.0f, juce::Font::bold));
            g.drawText (juce::String (i + 1), juce::Rectangle<float> (c.x - 8.0f, c.y - 8.0f, 16.0f, 16.0f), juce::Justification::centred);
        }
        const auto button = analyzerButton();
        ui::hw::drawPushButton (g, button, analyzerOn(), colours::accent);
        g.setColour (analyzerOn() ? colours::lampInk : colours::dim);
        g.setFont (ui::lcdFont (9.0f, juce::Font::bold));
        g.drawText ("ANALYZER", button, juce::Justification::centred);
        ui::hw::drawGlassOverlay (g, glass, 4.0f, false);
    }

private:
    float analyzerY (float db) const
    {
        const auto r = plot();
        return r.getY() + (1.0f - (juce::jlimit (kAnalyzerTopDb - kAnalyzerRangeDb, kAnalyzerTopDb, db) - (kAnalyzerTopDb - kAnalyzerRangeDb)) / kAnalyzerRangeDb) * r.getHeight();
    }

    // A pixel narrower than a bin: interpolate; wider: the strongest bin it covers.
    void drawTrace (juce::Graphics& g, const pdsp::SpectrumAnalyzer& analyzer, juce::Colour line, juce::Colour fill) const
    {
        const auto r = plot();
        const int width = (int) r.getWidth();
        const float binHz = (float) analyzer.binFrequency (1);
        juce::Path path;
        for (int px = 0; px <= width; ++px)
        {
            const float f0 = ui::FreqAxis::toHz ((float) px / (float) width), f1 = ui::FreqAxis::toHz ((float) (px + 1) / (float) width);
            const float db = (f1 - f0) < binHz ? analyzer.levelAtFrequency (0.5 * (f0 + f1)) : analyzer.peakInRange (f0, f1);
            const float x = r.getX() + (float) px, y = analyzerY (db);
            px == 0 ? path.startNewSubPath (x, y) : path.lineTo (x, y);
        }
        juce::Path filled (path);
        filled.lineTo (r.getRight(), r.getBottom());
        filled.lineTo (r.getX(), r.getBottom());
        filled.closeSubPath();
        g.setColour (fill);
        g.fillPath (filled);
        g.setColour (line);
        g.strokePath (path, juce::PathStrokeType (1.2f));
    }

    void select (int band)
    {
        selected = band;
        if (onSelect)
            onSelect (band);
        repaint();
    }
    void set (const juce::String& pid, float value)
    {
        auto* p = apvts.getParameter (pid);
        p->setValueNotifyingHost (p->convertTo0to1 (value));
    }

    juce::AudioProcessorValueTreeState& apvts;
    std::function<double()> sampleRate;
    pdsp::StereoTap& tapBefore;
    pdsp::StereoTap& tapAfter;
    double analyzerRate = 0.0;
    inline static const juce::Identifier analyzerProperty { "analyzer" };
    int dragged = -1;
};
} // namespace apollo::gui
