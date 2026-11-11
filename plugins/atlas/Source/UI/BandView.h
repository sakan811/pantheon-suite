// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 The Pantheon authors

#pragma once

#include "../PluginProcessor.h"
#include "AtlasUi.h"
#include <pantheon/dsp/SpectrumAnalyzer.h>
#include <pantheon/dsp/SpectrumIssues.h>
#include <pantheon/ui/FreqAxis.h>
#include <pantheon/ui/Spectrogram.h>

namespace atlas::gui
{
// The display: a spectrogram of the input (frequency across, time running down) with the bands laid over it. The bands are
// made here: double-click to split one, drag an edge, double-click an edge to join two. It also points at trouble: PEAK marks a
// narrow region that stands out and stays, JUMPS a region whose level keeps leaping over its own average, which is what a
// compressor band is for; clicking a JUMPS marker makes that region a band and sets its threshold from what the band then reads.
// Everything here runs on the editor's timer; nothing on the audio thread.
class BandView : public juce::Component, public juce::SettableTooltipClient
{
public:
    static constexpr int kRows = 180;               // six seconds at the editor's 30 Hz
    static constexpr float kFloorDb = -78.0f, kCeilingDb = 0.0f; // what the spectrogram draws as dark and as full light (the analyzer's tilted dB)
    static constexpr int kGrabPixels = 6;
    static constexpr float kLearnSeconds = 2.0f;    // how long a new band is listened to before its threshold is set

    explicit BandView (AtlasProcessor& p) : proc (p)
    {
        setTooltip ("The input over the last six seconds, low to high across, newest at the bottom.\nDouble-click: split the band there (up to six bands). "
                    "Drag an edge: move it. Double-click an edge: join the two bands.\nPEAK: a narrow region that stands out and stays. "
                    "JUMPS: a region whose level keeps leaping over its average; click the marker to make it a band with a threshold set to catch the leaps.");
        analyzer.setReleaseSeconds (0.15f);
    }

    std::function<void()> onLayoutChanged;

    float xForHz (double hz) const noexcept { return plot().getX() + ui::FreqAxis::toNormalised ((float) hz) * plot().getWidth(); }
    double hzForX (float x) const noexcept { return ui::FreqAxis::toHz ((x - plot().getX()) / plot().getWidth()); }

    // From the editor's timer.
    void tick (float dt)
    {
        analyzer.setSampleRate (std::max (proc.getSampleRate(), 8000.0));
        analyzer.update (proc.tapIn);
        pdsp::SpectrumGrid::Column column;
        for (int i = 0; i < pdsp::SpectrumGrid::kSteps; ++i)
        {
            // A step narrower than an analyzer bin (the bass) reads between bins; a wider one takes its loudest bin.
            const double lo = pdsp::SpectrumGrid::hz (i - 0.5), hi = pdsp::SpectrumGrid::hz (i + 0.5);
            column[(size_t) i] = std::max (analyzer.levelAtFrequency (pdsp::SpectrumGrid::hz (i)), analyzer.peakInRange (lo, hi));
        }
        spectrogram.push (column.data(), kFloorDb, kCeilingDb);
        finder.push (column, dt);
        numPeaks = finder.peaks (peaks);
        numSwings = finder.swings (swings);
        learn (dt);
        repaint();
    }

    // Makes the region of a JUMPS marker a band: splits at its edges where there is room, switches the band on, and sets its
    // threshold once the band has been heard (the spectrogram's levels are per frequency, not the band's).
    void makeBand (const pdsp::IssueFinder::Swing& s)
    {
        addSplit (proc.apvts, s.loHz);
        addSplit (proc.apvts, s.hiHz);
        const auto l = layoutOf (proc.apvts);
        const double centre = std::sqrt (s.loHz * s.hiHz);
        int band = 0;
        while (band < l.numBands - 1 && l.edgeHz[(size_t) (band + 1)] <= centre)
            ++band;
        learning = { l.slot[(size_t) band], s.swingDb, 0.0f, -120.0f };
        setValue (proc.apvts, bandIds (learning.slot).on, 1.0f);
        layoutChanged();
    }

    void paint (juce::Graphics& g) override
    {
        namespace hw = ui::hw;
        const auto frame = getLocalBounds().toFloat();
        const auto area = plot();
        hw::drawGlass (g, frame, 4.0f, colours::accent);
        spectrogram.draw (g, area);

        for (double hz : { 50.0, 100.0, 200.0, 500.0, 1000.0, 2000.0, 5000.0, 10000.0 })
        {
            g.setColour (colours::glassGrid.withAlpha (0.8f));
            g.drawVerticalLine (juce::roundToInt (xForHz (hz)), area.getY(), area.getBottom());
            hw::drawLabel (g, hz < 1000.0 ? juce::String ((int) hz) : juce::String ((int) (hz / 1000.0)) + "k",
                           { juce::roundToInt (xForHz (hz)) + 3, (int) area.getBottom() - 13, 30, 11 }, juce::Justification::left, colours::text.withAlpha (0.75f), 8.0f);
        }

        const auto l = layoutOf (proc.apvts);
        const unsigned solo = proc.engine.monitor.solo.load();
        for (int b = 0; b < l.numBands; ++b)
        {
            const int slot = l.slot[(size_t) b];
            const float x0 = xForHz (l.edgeHz[(size_t) b]), x1 = xForHz (l.edgeHz[(size_t) (b + 1)]);
            // What the band's compressor is taking off hangs from the top.
            const float taken = juce::jlimit (0.0f, 1.0f, proc.engine.reductionMeter[(size_t) slot].load() / kMeterRangeDb);
            g.setColour (colours::amber.withAlpha (0.25f));
            g.fillRect (x0 + 1.0f, area.getY(), x1 - x0 - 2.0f, taken * area.getHeight() * 0.4f);
            const bool heard = solo == 0u || (solo & (1u << slot)) != 0u;
            if (! heard)
            {
                g.setColour (juce::Colours::black.withAlpha (0.45f));
                g.fillRect (x0, area.getY(), x1 - x0, area.getHeight());
            }
            g.setColour (colours::glass.withAlpha (0.75f)); // a plate behind the number: the spectrogram under it can be any colour
            g.fillRoundedRectangle ((x0 + x1) * 0.5f - 9.0f, area.getY() + 3.0f, 18.0f, 14.0f, 3.0f);
            hw::drawLabel (g, juce::String (b + 1), { (int) x0, (int) area.getY() + 4, (int) (x1 - x0), 12 }, juce::Justification::centred,
                           heard ? colours::amber : colours::dim, 10.0f);
            if (b > 0)
            {
                g.setColour (colours::text.withAlpha (dragging == b - 1 ? 1.0f : 0.75f));
                g.fillRect (x0 - 0.75f, area.getY(), 1.5f, area.getHeight());
                g.fillRoundedRectangle (x0 - 4.0f, area.getY(), 8.0f, 10.0f, 2.0f);
            }
        }

        for (int i = 0; i < numPeaks; ++i)
        {
            const float x = xForHz (peaks[(size_t) i].freqHz);
            g.setColour (colours::bad.withAlpha (0.9f));
            juce::Path p;
            p.addTriangle (x - 5.0f, area.getY() + 20.0f, x + 5.0f, area.getY() + 20.0f, x, area.getY() + 28.0f);
            g.fillPath (p);
            hw::drawLabel (g, "Peak +" + juce::String (juce::roundToInt (peaks[(size_t) i].prominenceDb)), { (int) x - 30, (int) area.getY() + 29, 60, 11 },
                           juce::Justification::centred, colours::bad, 8.0f);
        }
        for (int i = 0; i < numSwings; ++i)
        {
            const auto r = swingBounds (i);
            g.setColour (colours::warn.withAlpha (0.85f));
            g.fillRoundedRectangle (r.withTrimmedTop (r.getHeight() - 4.0f), 2.0f);
            hw::drawLabel (g, "Jumps " + juce::String (juce::roundToInt (swings[(size_t) i].swingDb)) + " dB", r.toNearestInt().withTrimmedBottom (5),
                           juce::Justification::centred, colours::warn, 8.5f);
        }
        if (learning.slot >= 0)
            hw::drawLabel (g, "Listening to the new band to set its threshold ...", area.toNearestInt().reduced (8, 18).removeFromTop (12),
                           juce::Justification::right, colours::warn, 9.0f);
        hw::drawGlassOverlay (g, frame, 4.0f, false);
    }

    void mouseMove (const juce::MouseEvent& e) override
    {
        setMouseCursor (edgeAt (e.position.x) >= 0 ? juce::MouseCursor::LeftRightResizeCursor
                        : swingAt (e.position) >= 0 ? juce::MouseCursor::PointingHandCursor : juce::MouseCursor::NormalCursor);
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        dragging = edgeAt (e.position.x);
        if (dragging >= 0)
        {
            const auto l = layoutOf (proc.apvts);
            dragParam = proc.apvts.getParameter (id::crossover[(size_t) l.splitAbove[(size_t) dragging]]);
            dragParam->beginChangeGesture();
        }
        else if (const int s = swingAt (e.position); s >= 0)
        {
            makeBand (swings[(size_t) s]);
        }
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (dragging >= 0)
            moveSplit (proc.apvts, dragging, hzForX (e.position.x));
    }

    void mouseUp (const juce::MouseEvent&) override
    {
        if (dragging >= 0 && dragParam != nullptr)
            dragParam->endChangeGesture();
        dragging = -1;
        dragParam = nullptr;
    }

    void mouseDoubleClick (const juce::MouseEvent& e) override
    {
        if (const int edge = edgeAt (e.position.x); edge >= 0)
            removeSplit (proc.apvts, edge);
        else if (plot().contains (e.position))
            addSplit (proc.apvts, hzForX (e.position.x));
        layoutChanged();
    }

    // For the check tool and the snapshot.
    int swingCount() const noexcept { return numSwings; }
    int peakCount() const noexcept { return numPeaks; }

private:
    juce::Rectangle<float> plot() const noexcept { return getLocalBounds().toFloat().reduced (3.0f); }

    // The band whose upper edge is under x, or -1.
    int edgeAt (float x) const
    {
        const auto l = layoutOf (proc.apvts);
        for (int b = 0; b < l.numBands - 1; ++b)
            if (std::fabs (x - xForHz (l.edgeHz[(size_t) (b + 1)])) <= (float) kGrabPixels)
                return b;
        return -1;
    }

    juce::Rectangle<float> swingBounds (int i) const
    {
        const float x0 = xForHz (swings[(size_t) i].loHz), x1 = xForHz (swings[(size_t) i].hiHz);
        const float w = std::max (x1 - x0, 76.0f);
        return { (x0 + x1 - w) * 0.5f, plot().getBottom() - 34.0f - 20.0f * (float) (i % 2), w, 18.0f }; // staggered, so two neighbours do not print over each other
    }

    int swingAt (juce::Point<float> p) const
    {
        for (int i = 0; i < numSwings; ++i)
            if (swingBounds (i).contains (p))
                return i;
        return -1;
    }

    void layoutChanged()
    {
        // The strips mean other bands now.
        proc.engine.monitor.solo = 0u;
        proc.engine.monitor.delta = 0u;
        if (onLayoutChanged)
            onLayoutChanged();
        repaint();
    }

    // A band made from a JUMPS marker: its threshold is set half the jump under the loudest level the band read.
    void learn (float dt)
    {
        if (learning.slot < 0)
            return;
        learning.seconds += dt;
        if (learning.seconds > 0.3f) // the band has to start first
            learning.loudestDb = std::max (learning.loudestDb, proc.engine.levelMeter[(size_t) learning.slot].load());
        if (learning.seconds >= kLearnSeconds)
        {
            if (learning.loudestDb > -100.0f)
                setValue (proc.apvts, bandIds (learning.slot).threshold, juce::jlimit (-60.0f, 0.0f, learning.loudestDb - 0.5f * learning.swingDb));
            learning.slot = -1;
        }
    }

    AtlasProcessor& proc;
    pdsp::SpectrumAnalyzer analyzer;
    ui::Spectrogram spectrogram { pdsp::SpectrumGrid::kSteps, kRows };
    pdsp::IssueFinder finder;
    std::array<pdsp::IssueFinder::Peak, pdsp::IssueFinder::kMaxPeaks> peaks {};
    std::array<pdsp::IssueFinder::Swing, pdsp::IssueFinder::kMaxSwings> swings {};
    int numPeaks = 0, numSwings = 0;
    int dragging = -1;
    juce::RangedAudioParameter* dragParam = nullptr;
    struct Learning { int slot = -1; float swingDb = 0.0f, seconds = 0.0f, loudestDb = -120.0f; } learning;
};
} // namespace atlas::gui
