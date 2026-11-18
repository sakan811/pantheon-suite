// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 The Pantheon authors

#pragma once

#include "../PluginProcessor.h"
#include "AtlasUi.h"
#include "../Target.h"
#include <pantheon/dsp/SpectrumAnalyzer.h>
#include <pantheon/dsp/SpectrumIssues.h>
#include <pantheon/ui/FreqAxis.h>
#include <pantheon/ui/Spectrogram.h>

namespace atlas::gui
{
// The display: a spectrogram of the input (frequency across, time running down) with the bands laid over it. The bands are
// made here: double-click to split one, drag an edge, double-click an edge to join two. It also points at trouble: PEAK marks a
// narrow region that stands out and stays, JUMPS a region whose level keeps leaping over its own average, which is what a
// compressor band is for; clicking a JUMPS marker makes that region a band and sets its compressor from what the band then hears.
// Everything here runs on the editor's timer; nothing on the audio thread.
class BandView : public juce::Component, public juce::SettableTooltipClient
{
public:
    static constexpr int kRows = 180;               // six seconds at the editor's 30 Hz
    static constexpr float kFloorDb = -78.0f, kCeilingDb = 0.0f; // what the spectrogram draws as dark and as full light (the analyzer's tilted dB)
    static constexpr int kGrabPixels = 6;
    static constexpr float kTimeoutSeconds = 3.0f * (float) BandProfile::kListenSeconds; // the engine listens in audio time: with nothing playing it never finishes
    static constexpr float kNoticeSeconds = 4.0f;   // how long "heard nothing" stays on the display

    explicit BandView (AtlasProcessor& p) : proc (p)
    {
        setTooltip ("The input over the last six seconds, low to high across, newest at the bottom.\nDouble-click: split the band there (up to six bands). "
                    "Drag an edge: move it. Double-click an edge: join the two bands.\nPEAK: a narrow region that stands out and stays. "
                    "JUMPS: a region whose level keeps leaping over its average; click the marker to make it a band with its compressor set to catch the leaps.");
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
        matchOutputStep (dt);
        repaint();
    }

    // Makes the region of a JUMPS marker a band: splits at its edges where there is room, switches the band on, and sets its
    // compressor once the band has been heard (the spectrogram's levels are per frequency, not the band's).
    void makeBand (const pdsp::IssueFinder::Swing& s)
    {
        addSplit (proc.apvts, s.loHz);
        addSplit (proc.apvts, s.hiHz);
        const auto l = layoutOf (proc.apvts);
        const double centre = std::sqrt (s.loHz * s.hiHz);
        int band = 0;
        while (band < l.numBands - 1 && l.edgeHz[(size_t) (band + 1)] <= centre)
            ++band;
        learnBand (l.slot[(size_t) band]);
        layoutChanged();
    }

    // The LEARN button of a strip, and a band made from a JUMPS marker: the band listens to itself for BandProfile::kListenSeconds of
    // audio and THRESH, RATIO, ATTACK and RELEASE are set from what it heard (DSP/Learn.h says how). Switches the band on: an off band
    // reads nothing.
    void learnBand (int slot) { listen ({ slot }, nullptr); }

    // LEARN ALL: every band at once, from the same stretch of audio. With a `target` (a curve copied from another plugin: TARGET) the bands that
    // stand out of its shape also get a lower THRESH (Target.h). Then OUTPUT is set so the output is as loud as the input (matchOutputStep).
    void learnAll (const pdsp::LinkCurve* target = nullptr)
    {
        const auto l = layoutOf (proc.apvts);
        std::vector<int> slots;
        for (int b = 0; b < l.numBands; ++b)
            slots.push_back (l.slot[(size_t) b]);
        listen (slots, target);
        learning.matchOutput = true;
    }
    void say (const juce::String& text) { notice = text; noticeSeconds = kNoticeSeconds; }
    // The first band that is listening, or -1; isLearning says it for one.
    int learningSlot() const noexcept
    {
        for (int i = 0; i < kBands; ++i)
            if (learning.active[(size_t) i])
                return i;
        return -1;
    }
    bool isLearning (int slot) const noexcept { return learning.active[(size_t) slot]; }
    bool matchingOutput() const noexcept { return loudness.active; } // LEARN ALL is setting OUTPUT (the engine is measuring)

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
            // What the band's compressor is taking off (or, teal, adding) hangs from the top.
            const float change = proc.engine.reductionMeter[(size_t) slot].load(); // negative: the band is being lifted
            const float taken = juce::jlimit (0.0f, 1.0f, std::fabs (change) / kMeterRangeDb);
            g.setColour ((change < 0.0f ? colours::accent : colours::amber).withAlpha (0.25f));
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
        if (const int first = learningSlot(); first >= 0)
        {
            int n = 0;
            for (bool a : learning.active)
                n += a ? 1 : 0;
            hw::drawLabel (g, n == 1 ? "Listening to band " + juce::String (first + 1) + " to set its compressor ..." : "Listening to " + juce::String (n) + " bands to set their compressors ...",
                           area.toNearestInt().reduced (8, 18).removeFromTop (12), juce::Justification::right, colours::warn, 9.0f);
        }
        else if (noticeSeconds > 0.0f)
            hw::drawLabel (g, notice, area.toNearestInt().reduced (8, 18).removeFromTop (12), juce::Justification::right, colours::warn, 9.0f);
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

    void listen (const std::vector<int>& slots, const pdsp::LinkCurve* target)
    {
        for (int i = 0; i < kBands; ++i)
            if (learning.active[(size_t) i])
                proc.engine.learnCancel (i);
        cancelOutputMatch(); // a new listening supersedes a measurement of the old settings
        learning = {};
        learning.hasTarget = target != nullptr;
        if (target != nullptr)
            learning.curve = *target;
        noticeSeconds = 0.0f;
        const auto layout = layoutOf (proc.apvts);
        for (int slot : slots)
        {
            learning.active[(size_t) slot] = true;
            for (int b = 0; b < layout.numBands; ++b)
                if (layout.slot[(size_t) b] == slot)
                    learning.edges[(size_t) slot] = { layout.edgeHz[(size_t) b], layout.edgeHz[(size_t) (b + 1)] };
            setValue (proc.apvts, bandIds (slot).on, 1.0f);
            proc.engine.learnStart (slot);
        }
    }

    // The engine listens in audio time; this takes its answers, or gives up when no audio comes. The settings are written when every band
    // that was listening has answered (with a target the bands are compared with each other first).
    void learn (float dt)
    {
        noticeSeconds = std::max (0.0f, noticeSeconds - dt);
        if (learningSlot() < 0)
            return;
        learning.seconds += dt;
        for (int i = 0; i < kBands; ++i)
        {
            if (! learning.active[(size_t) i])
                continue;
            if (proc.engine.learnTake (i, learning.result[(size_t) i]))
            {
                learning.active[(size_t) i] = false;
                learning.got[(size_t) i] = true;
            }
            else if (proc.engine.learnState (i) == Engine::LearnState::silent || learning.seconds >= kTimeoutSeconds)
            {
                proc.engine.learnCancel (i);
                learning.active[(size_t) i] = false;
                learning.silent = true;
            }
        }
        if (learningSlot() < 0)
            finishLearning();
    }

    void finishLearning()
    {
        const auto l = layoutOf (proc.apvts);
        std::array<double, kBands> shift {};
        juce::String targetNote;
        if (learning.hasTarget)
        {
            std::array<TargetBand, kBands> bands {};
            int n = 0;
            for (int slot = 0; slot < kBands; ++slot)
                if (learning.edges[(size_t) slot][1] > 0.0) // the bands that listened, in the order of the spectrum (a slot is a band's strip)
                    bands[(size_t) n++] = { learning.edges[(size_t) slot][0], learning.edges[(size_t) slot][1], (double) learning.result[(size_t) slot].typicalDb, learning.got[(size_t) slot] };
            const auto s = targetShifts (learning.curve, bands, n);
            int moved = 0, at = 0;
            for (int slot = 0; slot < kBands; ++slot)
                if (learning.edges[(size_t) slot][1] > 0.0)
                {
                    shift[(size_t) slot] = s[(size_t) at];
                    moved += s[(size_t) at++] > 0.05 ? 1 : 0;
                }
            targetNote = moved > 0 ? "Target: " + juce::String (moved) + (moved == 1 ? " band stands" : " bands stand") + " out of the curve and got a lower THRESH."
                                   : "Target: no band stands out of the curve.";
        }
        int set = 0;
        for (int slot = 0; slot < kBands; ++slot)
            if (learning.got[(size_t) slot])
            {
                const auto& ids = bandIds (slot);
                const auto& r = learning.result[(size_t) slot];
                setValue (proc.apvts, ids.threshold, (float) std::max (-60.0, (double) r.thresholdDb - shift[(size_t) slot]));
                setValue (proc.apvts, ids.ratio, r.ratio);
                setValue (proc.apvts, ids.attack, r.attackMs);
                setValue (proc.apvts, ids.release, r.releaseMs);
                ++set;
            }
        const bool someSilent = learning.silent, matchOutput = learning.matchOutput && set > 0;
        learning = {};
        if (set == 0)
            say ("Heard nothing: play the part the band is for, then press LEARN again.");
        else if (someSilent)
            say ("Some bands heard nothing and were left as they were. " + targetNote);
        else if (targetNote.isNotEmpty())
            say (targetNote);
        if (matchOutput)
        {
            // OUTPUT needs the learned settings in effect: the engine measures a second window of audio with them (the notice says so).
            proc.engine.loudnessStart();
            loudness = { true, 0.0f };
            say ("Matching OUTPUT to the input: play the same part for four more seconds.");
        }
    }

    // OUTPUT for LEARN ALL: the engine measures the input and the output (before OUTPUT) over four seconds with the learned settings; the
    // gain that makes them equal is written to OUTPUT. Silence, or no audio at all, leaves OUTPUT as it was (LoudnessMatch, Learn.h).
    void matchOutputStep (float dt)
    {
        if (! loudness.active)
            return;
        loudness.seconds += dt;
        double inPower = 0.0, outPower = 0.0;
        if (proc.engine.loudnessTake (inPower, outPower))
        {
            loudness.active = false;
            double db = 0.0;
            if (! LoudnessMatch::gainDb (inPower, outPower, db))
            {
                say ("Heard nothing to match: OUTPUT is as it was.");
                return;
            }
            const auto range = proc.apvts.getParameterRange (id::out);
            db = std::clamp (db, (double) range.start, (double) range.end);
            setValue (proc.apvts, id::out, (float) db);
            say (juce::String::formatted ("OUTPUT is %+.1f dB: the output is as loud as the input.", db));
        }
        else if (proc.engine.loudnessState() == Engine::LoudState::silent || loudness.seconds >= kTimeoutSeconds)
        {
            cancelOutputMatch();
            say ("No audio to match: OUTPUT is as it was.");
        }
    }

    void cancelOutputMatch()
    {
        if (loudness.active)
            proc.engine.loudnessCancel();
        loudness = {};
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
    struct Learning
    {
        std::array<bool, kBands> active {}, got {};
        std::array<std::array<double, 2>, kBands> edges {}; // each listening band's lower and upper edge (Hz) when it started: a split moved meanwhile does not change what was heard
        std::array<Learned, kBands> result {};
        float seconds = 0.0f;
        bool silent = false, hasTarget = false, matchOutput = false; // matchOutput: LEARN ALL (also TARGET), which then sets OUTPUT
        pdsp::LinkCurve curve;
    } learning;
    struct OutputMatch
    {
        bool active = false;
        float seconds = 0.0f; // while the engine measures: how long it has been waiting
    } loudness;
    juce::String notice;
    float noticeSeconds = 0.0f; // seconds left of the message
};
} // namespace atlas::gui
