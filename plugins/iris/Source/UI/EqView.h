// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 The Pantheon authors

#pragma once

#include "../DSP/Engine.h"
#include "../Parameters.h"
#include "IrisUi.h"

#include <juce_gui_basics/juce_gui_basics.h>
#include <pantheon/dsp/SpectrumAnalyzer.h>
#include <pantheon/ui/FreqAxis.h>

#include "../DSP/Masking.h"
#include "../DSP/Peers.h"
#include "../DSP/ReferenceMatch.h"
#include "../DSP/ResonanceFinder.h"

namespace iris::gui
{
using FreqAxis = ui::FreqAxis;

// The equaliser's display. Behind: the spectrum before (dim) and after (cream) the equaliser. Over it: the curve each channel
// ends up with, one node per band, and along the bottom the six frequency regions, each a button that solos that part of the
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
        for (Setting* setting : { &spectrumRange, &speed, &tilt, &gainScale, &markers, &referenceShown })
            setting->load (apvts.state);
        applyAnalyzerSettings();
        resonances.reset();
        loadReference();

        setTooltip ("Double-click to add a band. Drag a node to set its frequency and gain (Shift = fine), turn the wheel over it for Q; double-click it to reset gain and Q. "
                    "Right-click a node for its menu. The strip along the bottom solos a part of the spectrum.");
        startTimerHz (30);
    }

    std::function<void (int)> onSelectionChanged;

    juce::String getTooltip() override
    {
        if (const int r = resonanceAt (hover); r >= 0)
            return resonanceTooltip (r);
        if (const int c = clashLabelAt (hover); c >= 0)
            return clashTooltip (c);
        return juce::SettableTooltipClient::getTooltip();
    }

    // The tap that carries the sidechain input (another track) for the masking view; nullptr = none.
    // `connected` is the processor's flag that the host handed the last block a sidechain (nullptr = unknown, treated as not connected).
    void setSidechain (pdsp::StereoTap* tap, const std::atomic<bool>* connected = nullptr)
    {
        sidechain = tap;
        sidechainConnected = connected;
    }

    // The other Iris instances in the host (DSP/Peers.h): the masking view can listen to one of them instead of the sidechain input.
    // The choice is the label of the instance, kept in the project like the display settings; "" is the sidechain input.
    void setPeers (iris::Peer* self)
    {
        selfPeer = self;
        listener = self != nullptr ? std::make_unique<iris::PeerListener> (*self) : nullptr;
    }

    juce::String maskSource() const { return apvts.state.getProperty (maskSourceKey).toString(); }

    void setMaskSource (const juce::String& label)
    {
        apvts.state.setProperty (maskSourceKey, label, nullptr);
        syncMaskSource();
        flash (label.isEmpty() ? "MASKING AGAINST THE SIDECHAIN INPUT" : "MASKING AGAINST " + label.toUpperCase());
    }

    // What the masking view is doing, in words (empty when it has nothing to say): shown as a pill at the bottom left of the display, and
    // a click on it opens the list of what to mask against. `help` also answers when nothing to mask against is there, which is otherwise
    // silent (or a hint that other instances exist) so that a plugin used on its own shows nothing.
    juce::String maskingStatus (bool help = false) const
    {
        const bool peerMode = listener != nullptr && maskSource().isNotEmpty();
        const bool connected = peerMode ? peerFound
                                        : ((sidechainConnected != nullptr && sidechainConnected->load (std::memory_order_relaxed)) || masking.hearsOther());
        if (! connected)
        {
            if (peerMode)
                return "MASKING: '" + maskSource().toUpperCase() + "' IS NOT LOADED, WAITING FOR IT";
            if (! others.empty())
                return juce::String ((int) others.size()) + " OTHER IRIS FOUND: CLICK HERE TO MASK AGAINST ONE";
            if (sidechain == nullptr || ! help)
                return {};
            return "MASKING NEEDS ANOTHER TRACK: LOAD IRIS ON IT TOO, OR SEND IT TO THE SIDECHAIN INPUT";
        }
        const juce::String other = peerMode ? maskSource().toUpperCase() : juce::String ("THE SIDECHAIN");
        switch (masking.status())
        {
            case MaskingMeter::Status::silent:
                return "MASKING: PLAY THIS TRACK (NOTHING IS COMING IN)";
            case MaskingMeter::Status::otherSilent:
                return "MASKING: " + other + " IS SILENT, PLAY THE OTHER TRACK";
            case MaskingMeter::Status::measuring:
                return "MASKING: MEASURING " + juce::String (masking.measuredSeconds(), 1) + " / " + juce::String (MaskingMeter::kMinSeconds, 0) + " s";
            case MaskingMeter::Status::clear:
            {
                const auto o = masking.overlap();
                if (o.bands == 0)
                    return "NO MASKING: THE TWO TRACKS ARE LOUD IN DIFFERENT RANGES";
                if (std::fabs (o.gapDb) > MaskingMeter::kCloseDb)
                    return "NO MASKING: " + other + " IS " + juce::String (std::fabs (o.gapDb), 0) + " dB " + (o.gapDb > 0.0f ? "QUIETER" : "LOUDER")
                           + " WHERE BOTH ARE LOUD, SO ONE STANDS CLEAR";
                return "NO MASKING: ONE OF THE TWO STANDS CLEAR IN EVERY SHARED RANGE";
            }
            case MaskingMeter::Status::found:
                return peerMode ? "MASKING AGAINST " + other + ": " + juce::String ((int) clashes.size()) + " ZONE(S)" : juce::String();
        }
        return {};
    }
    const std::vector<MaskingMeter::Clash>& currentClashes() const noexcept { return clashes; }

    // The resonances found so far (for the snapshot and the checks).
    const std::vector<ResonanceFinder::Resonance>& currentResonances() const noexcept { return found; }

    // ---- reference ------------------------------------------------------------------------------
    // A reference is the long-term shape of another track (learned while it plays through Iris, or pasted from Themis' Copy EQ). It
    // is drawn behind the spectrum, levelled to this track's average, and MATCH adds the bands that bring this track's shape to it.
    // The display averages what goes into Iris on its timer, so these need the editor open and some seconds of audio.
    bool isLearning() const noexcept { return learning; }
    double learnedSeconds() const noexcept { return learnProfile.seconds(); }
    bool hasReference() const noexcept { return haveReference; }
    bool hasAverage() const noexcept { return liveProfile.hasData(); }
    const juce::String& message() const noexcept { return note; }

    // First press starts learning from what plays now, the second keeps it as the reference.
    void toggleLearning()
    {
        if (! learning)
        {
            learnProfile.reset();
            learning = true;
            flash ("LEARNING: play the reference track, then press again");
            return;
        }
        learning = false;
        Shape s {};
        ShapeMask valid {};
        if (learnProfile.shape (usableMaxHz(), s, valid))
        {
            setReference (s, "learned");
            flash ("REFERENCE KEPT: " + juce::String (learnProfile.seconds(), 0) + " s heard");
        }
        else
            flash ("NOT ENOUGH AUDIO: play the reference for at least " + juce::String ((int) SpectrumProfile::kMinSeconds) + " s");
    }

    void clearReference()
    {
        learning = false;
        haveReference = false;
        referenceName.clear();
        apvts.state.removeProperty ("referenceCurve", nullptr);
        apvts.state.removeProperty ("referenceName", nullptr);
        flash ("REFERENCE CLEARED");
        repaint();
    }

    // Takes the curve another plugin copied along with its bands. False when the text holds none.
    bool pasteReference (const juce::String& text)
    {
        Shape s {};
        std::string name;
        if (! iris::pasteReference (text, s, name))
            return false;
        setReference (s, name.empty() ? "pasted" : name);
        flash ("REFERENCE PASTED" + (name.empty() ? juce::String() : ": " + juce::String (name)));
        return true;
    }

    void restartAverage()
    {
        liveProfile.reset();
        flash ("AVERAGE RESTARTED: play the track");
    }

    // The bands that bring this track's average shape to the reference, in free slots (what the stereo bands already on do is
    // counted). Returns the slots used; empty with a message when there is nothing to do.
    std::vector<int> matchToReference()
    {
        if (! haveReference)
        {
            flash ("NO REFERENCE: LEARN REF while the reference plays, or PASTE one from Themis");
            return {};
        }
        Shape live {};
        ShapeMask valid {};
        if (! liveProfile.shape (usableMaxHz(), live, valid))
        {
            flash ("PLAY THE TRACK: MATCH needs " + juce::String ((int) SpectrumProfile::kMinSeconds) + " s of it");
            return {};
        }
        if (freeBandSlots (apvts) == 0)
        {
            flash ("NO FREE BAND SLOT");
            return {};
        }
        addExistingCurve (apvts, sampleRate, live);
        const auto bands = match::matchBands (live, reference, valid, sampleRate, std::min (match::kMaxBands, freeBandSlots (apvts)));
        if (bands.empty())
        {
            flash ("ALREADY WITHIN " + juce::String (match::kMinDb, 0) + " dB OF THE REFERENCE");
            return {};
        }
        const float before = match::remainingDb (live, reference, valid, sampleRate, {}), after = match::remainingDb (live, reference, valid, sampleRate, bands);
        const auto slots = addMatchedBands (apvts, bands);
        if (! slots.empty())
            setSelectedBand (slots.front());
        flash ("MATCH: " + juce::String ((int) slots.size()) + " BAND(S), OFF BY " + juce::String (before, 1) + " DB -> " + juce::String (after, 1) + " DB");
        return slots;
    }

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
        if (masking.hearsOther()) // as soon as there is something on the sidechain: the trace is how you know it arrived
            drawTrace (g, plot, sideTrace, colours::accent.withAlpha (0.55f), juce::Colours::transparentBlack);
        drawClashes (g, plot);
        drawReference (g, plot);
        drawCurves (g, plot, bands);
        drawResonances (g);
        drawNodes (g, plot, bands);
        g.restoreState();

        drawAxisLabels (g, plot);
        drawStrip (g, crossover);
        drawSoloPill (g, plot);
        drawReferencePill (g, plot);
        drawMaskingPill (g, plot);
        ui::hw::drawGlassOverlay (g, window, 4.0f, true);
    }

    void mouseMove (const juce::MouseEvent& e) override
    {
        hover = e.position;
        if (stripArea().contains (e.position))
            setMouseCursor (boundaryAt (e.position.x) >= 0 ? juce::MouseCursor::LeftRightResizeCursor : juce::MouseCursor::PointingHandCursor);
        else if (pillArea().contains (e.position) && anySolo())
            setMouseCursor (juce::MouseCursor::PointingHandCursor);
        else if (maskingPillArea (maskingStatus()).contains (e.position))
            setMouseCursor (juce::MouseCursor::PointingHandCursor);
        else
            setMouseCursor (nodeAt (e.position) >= 0 ? juce::MouseCursor::DraggingHandCursor
                            : resonanceAt (e.position) >= 0 ? juce::MouseCursor::PointingHandCursor : juce::MouseCursor::NormalCursor);
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
        ui::beginUndoStep (apvts, "Drag"); // a drag of a node or a crossover is one step

        if (pillArea().contains (e.position) && anySolo())
        {
            clearSolos();
            return;
        }

        if (maskingPillArea (maskingStatus()).contains (e.position))
        {
            showMaskMenu();
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
        ui::beginUndoStep (apvts, "Reset");
        if (stripArea().contains (e.position))
        {
            const int boundary = boundaryAt (e.position.x);
            if (boundary >= 0)
                if (auto* p = apvts.getParameter (id::crossover[(size_t) boundary]))
                    setParameter (*p, p->getDefaultValue());
            return;
        }
        if (const int hit = nodeAt (e.position); hit >= 0)
        {
            // The node goes back to flat and to Q 1 and stays where it is in frequency (a knob does the same on its own double-click).
            const auto& ids = bandIds (hit);
            if (hasGain (bandParamsOf (apvts, hit).type))
                setParameterValue (apvts, ids.gain, 0.0f);
            setParameterValue (apvts, ids.q, 1.0f);
            return;
        }
        if (const int r = resonanceAt (e.position); r >= 0)
            addResonanceBand (found[(size_t) r]);
        else if (const int c = clashLabelAt (e.position); c >= 0)
            addClashBand (clashes[(size_t) c]);
        else
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
        ui::beginUndoStep (apvts, "Wheel");

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

    // The six regions as one row of push switches along the bottom of the glass.
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
        menu.addItem (2, "Reset the crossovers");
        menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (this), [this] (int result)
        {
            if (result == 1)
                engine.monitor.regions = 0u;
            else if (result == 2)
            {
                ui::beginUndoStep (apvts, "Reset crossovers");
                for (int k = 0; k < numCrossovers; ++k)
                    if (auto* p = apvts.getParameter (id::crossover[(size_t) k]))
                        setParameter (*p, p->getDefaultValue());
            }
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
            if (result >= 100 && result < 400)
                ui::beginUndoStep (apvts, "Band");
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
        menu.addSeparator();
        juce::PopupMenu places;
        for (int p = 0; p < placeNames.size(); ++p)
        {
            places.addItem (nextId++, placeNames[p]);
            actions.push_back ([this, p] { setAllPlacements (apvts, p); });
        }
        menu.addSubMenu ("All bands to", places);
        menu.addSectionHeader ("Reference");
        menu.addItem (nextId++, learning ? "Keep what was learned" : "Learn a reference from what plays now");
        actions.push_back ([this] { toggleLearning(); });
        menu.addItem (nextId++, "Match to the reference", haveReference);
        actions.push_back ([this] { matchToReference(); });
        menu.addItem (nextId++, "Clear the reference", haveReference || learning);
        actions.push_back ([this] { clearReference(); });
        menu.addItem (nextId++, "Restart this track's average");
        actions.push_back ([this] { restartAverage(); });
        menu.addSectionHeader ("Mask against");
        addMaskSourceItems (menu, nextId, actions);
        menu.addItem (nextId++, "Why is there no masking?");
        actions.push_back ([this]
        {
            const auto text = maskingStatus (true);
            flash (text.isEmpty() ? "MASKING: " + juce::String ((int) clashes.size()) + " ZONE(S) SHOWN, DOUBLE-CLICK A LABEL FOR A BAND" : text);
        });
        for (Setting* setting : { &gainScale, &spectrumRange, &speed, &tilt, &markers, &referenceShown })
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
        for (Setting* setting : { &spectrumRange, &speed, &tilt, &gainScale, &markers, &referenceShown })
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

        // Twice a second is plenty for what is averaged over seconds.
        const bool refresh = ++findTicks >= 15;
        if (refresh)
            findTicks = 0;

        // What goes in, without the display's tilt: the resonances, the masking and the reference are all read from it.
        for (int i = 0; i < pdsp::SpectrumAnalyzer::numBins; ++i)
            untilted[(size_t) i] = untilt (inTrace, i);
        liveProfile.update (untilted.data(), pdsp::SpectrumAnalyzer::numBins, inTrace.binFrequency (1), 1.0f / 30.0f);
        if (learning)
            learnProfile.update (untilted.data(), pdsp::SpectrumAnalyzer::numBins, inTrace.binFrequency (1), 1.0f / 30.0f);
        if (noteTicks > 0 && --noteTicks == 0)
            note.clear();

        // Resonances are looked for in what goes in (what the bands are there to fix), without the display's tilt.
        if (markers.value() > 0.5f)
        {
            resonances.update (untilted.data(), pdsp::SpectrumAnalyzer::numBins, inTrace.binFrequency (1), 1.0f / 30.0f);
            if (refresh)
                found = resonances.find();
        }
        else
            found.clear();

        // Masking: this track against whatever arrives at the sidechain input, or against another Iris instance, both without the display's tilt.
        syncMaskSource();
        const bool peerMode = listener != nullptr && listener->selected().length() > 0;
        if (refresh)
            others = selfPeer != nullptr ? iris::PeerRegistry::get().others (selfPeer) : std::vector<iris::PeerInfo>();
        if (sidechain != nullptr || peerMode)
        {
            sideTrace.setSampleRate (sampleRate);
            sideTrace.setTilt (tilt.value());
            sideTrace.setReleaseSeconds (speed.value());
            bool playing = true;
            if (peerMode)
            {
                peerFound = false;
                for (bool found = false;;)
                {
                    const int n = listener->pull (peerBlock[0].data(), peerBlock[1].data(), (int) peerBlock[0].size(), found, playing);
                    peerFound = found;
                    if (n <= 0)
                        break;
                    const float* both[2] = { peerBlock[0].data(), peerBlock[1].data() };
                    peerTap->push (both, 2, n);
                }
                sideTrace.update (*peerTap);
            }
            else if (sidechain != nullptr)
                sideTrace.update (*sidechain);
            for (int i = 0; i < pdsp::SpectrumAnalyzer::numBins; ++i)
                sideUntilted[(size_t) i] = peerMode && ! playing ? -200.0f : untilt (sideTrace, i); // a peer that stopped sends nothing, which would leave its last spectrum
            masking.update (untilted.data(), sideUntilted.data(), pdsp::SpectrumAnalyzer::numBins, inTrace.binFrequency (1), 1.0f / 30.0f);
            if (refresh)
                clashes = masking.clashes();
        }
        repaint();
    }

    static float untilt (const pdsp::SpectrumAnalyzer& a, int bin)
    {
        return a.levelDb (bin) - a.getTilt() * (float) std::log2 (std::max (a.binFrequency (bin), 1.0) / 1000.0);
    }

    // ---- reference ---------------------------------------------------------------------------------
    double usableMaxHz() const { return sampleRate * 0.45; }

    void flash (const juce::String& text)
    {
        note = text;
        noteTicks = 30 * 4;
        repaint();
    }

    void setReference (const Shape& s, const juce::String& name)
    {
        reference = s;
        haveReference = true;
        referenceName = name.toStdString();
        apvts.state.setProperty ("referenceCurve", juce::String (shapeToText (s)), nullptr);
        apvts.state.setProperty ("referenceName", name, nullptr);
        repaint();
    }

    void loadReference()
    {
        Shape s {};
        if (shapeFromText (apvts.state.getProperty ("referenceCurve").toString().toStdString(), s))
        {
            reference = s;
            haveReference = true;
            referenceName = apvts.state.getProperty ("referenceName").toString().toStdString();
        }
    }

    // The reference (dashed, levelled to this track's average) and the average itself (thin), so what MATCH compares can be seen.
    void drawReference (juce::Graphics& g, juce::Rectangle<float> plot) const
    {
        (void) plot;
        if (! haveReference || referenceShown.value() < 0.5f || ! liveProfile.hasData())
            return;
        const float anchor = liveProfile.anchorLevelDb();
        const auto path = [&] (const std::function<float (int)>& levelOf)
        {
            juce::Path p;
            bool pen = false;
            for (int i = 0; i < ProfileGrid::kBands; ++i)
            {
                const float level = levelOf (i);
                if (level < -150.0f)
                {
                    pen = false;
                    continue;
                }
                const double hz = ProfileGrid::centreHz (i);
                const float x = xOf ((float) hz), y = spectrumY (level + inTrace.getTilt() * (float) std::log2 (hz / 1000.0));
                pen ? p.lineTo (x, y) : p.startNewSubPath (x, y);
                pen = true;
            }
            return p;
        };
        g.setColour (colours::text.withAlpha (0.45f));
        g.strokePath (path ([&] (int i) { return liveProfile.levelDb (i); }), juce::PathStrokeType (1.0f, juce::PathStrokeType::curved));
        const auto line = path ([&] (int i) { return match::hasLevel (reference[(size_t) i]) ? reference[(size_t) i] + anchor : -200.0f; });
        juce::Path dashed;
        const float dashes[] = { 6.0f, 4.0f };
        juce::PathStrokeType (1.8f).createDashedStroke (dashed, line, dashes, 2);
        g.setColour (colours::accent.withAlpha (0.9f));
        g.fillPath (dashed);
    }

    // Top right: what the reference is doing (learning, kept, pasted), or the message of the last action.
    void drawReferencePill (juce::Graphics& g, juce::Rectangle<float> plot) const
    {
        juce::String text;
        bool lit = false;
        if (note.isNotEmpty())
            text = note, lit = true;
        else if (learning)
            text = "LEARNING  " + juce::String (learnProfile.seconds(), 0) + " s", lit = true;
        else if (haveReference && referenceShown.value() > 0.5f)
            text = "REF  " + juce::String (referenceName).toUpperCase() + (liveProfile.hasData() ? juce::String() : "  (WAITS FOR AUDIO)");
        if (text.isEmpty())
            return;
        drawStatusPill (g, plot, text, lit, true, plot.getY() + 5.0f);
    }

    static juce::Rectangle<float> statusPillRect (juce::Rectangle<float> plot, const juce::String& text, bool atRight, float y)
    {
        const float width = juce::jmin (plot.getWidth() - 12.0f, juce::GlyphArrangement::getStringWidth (ui::labelFont (10.0f), text) + 30.0f);
        return { atRight ? plot.getRight() - width - 6.0f : plot.getX() + 6.0f, y, width, 18.0f };
    }

    // A rounded pill with a lamp and a line of text, at the right or the left of the display.
    static void drawStatusPill (juce::Graphics& g, juce::Rectangle<float> plot, const juce::String& text, bool lit, bool atRight, float y)
    {
        const auto font = ui::labelFont (10.0f);
        const auto pill = statusPillRect (plot, text, atRight, y);
        const auto colour = lit ? colours::amber : colours::accent;
        g.setColour (colours::glass.withAlpha (0.9f));
        g.fillRoundedRectangle (pill, 9.0f);
        g.setColour (colour.withAlpha (0.7f));
        g.drawRoundedRectangle (pill, 9.0f, 1.0f);
        ui::hw::drawLamp (g, { pill.getX() + 11.0f, pill.getCentreY() }, 3.5f, colour, lit);
        g.setFont (font);
        g.setColour (colour);
        g.drawText (text, pill.withTrimmedLeft (22.0f).toNearestInt(), juce::Justification::centredLeft, false);
    }

    // ---- masking -----------------------------------------------------------------------------------
    static constexpr const char* maskSourceKey = "maskSource";

    // The project's choice is the truth (a host can restore it at any time): follow it, and start the averages again when it changes.
    void syncMaskSource()
    {
        if (listener == nullptr)
            return;
        const auto want = maskSource().toStdString();
        if (want == listener->selected())
            return;
        listener->select (want);
        masking.reset();
        clashes.clear();
        peerFound = false;
        float l[256], r[256];
        for (bool f, p; listener->pull (l, r, 256, f, p) > 0;) {} // what the last source left behind
        for (auto& ch : peerBlock)
            ch.fill (0.0f);
        while (peerTap->channel[0].pull (peerBlock[0].data(), (int) peerBlock[0].size()) > 0 || peerTap->channel[1].pull (peerBlock[1].data(), (int) peerBlock[1].size()) > 0) {}
    }

    juce::Rectangle<float> maskingPillArea (const juce::String& text) const
    {
        return text.isEmpty() ? juce::Rectangle<float>() : statusPillRect (plotArea(), text, false, plotArea().getBottom() - 38.0f);
    }

    void drawMaskingPill (juce::Graphics& g, juce::Rectangle<float> plot) const
    {
        const auto text = maskingStatus();
        if (text.isNotEmpty())
            drawStatusPill (g, plot, text, masking.status() == MaskingMeter::Status::measuring, false, plot.getBottom() - 38.0f);
    }

    // What to mask against: the sidechain input and every other Iris in the host, ticked when chosen. One action per item, in order.
    void addMaskSourceItems (juce::PopupMenu& menu, int& nextId, std::vector<std::function<void()>>& actions)
    {
        if (selfPeer != nullptr)
            others = iris::PeerRegistry::get().others (selfPeer);
        const auto current = maskSource();
        menu.addItem (nextId++, "Sidechain input", true, current.isEmpty());
        actions.push_back ([this] { setMaskSource ({}); });
        bool listed = current.isEmpty();
        for (const auto& peerInfo : others)
        {
            const juce::String label (peerInfo.label);
            listed = listed || label == current;
            menu.addItem (nextId++, label + (peerInfo.playing ? "" : "  (silent)"), true, label == current);
            actions.push_back ([this, label] { setMaskSource (label); });
        }
        if (! listed)
        {
            menu.addItem (nextId++, current + "  (not loaded)", true, true);
            actions.push_back ([] {});
        }
        if (others.empty())
        {
            menu.addItem (nextId++, "No other Iris found: load one on another track", false);
            actions.push_back ([] {});
        }
    }

    void showMaskMenu()
    {
        juce::PopupMenu menu;
        int nextId = 1;
        std::vector<std::function<void()>> actions;
        menu.addSectionHeader ("Mask against");
        addMaskSourceItems (menu, nextId, actions);
        menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (this), [this, actions] (int result)
        {
            if (result > 0)
            {
                actions[(size_t) result - 1]();
                repaint();
            }
        });
    }

    // Each clash zone is shaded across the display with a label at the top saying how far to cut this track there.
    juce::Rectangle<float> clashLabel (const MaskingMeter::Clash& c) const
    {
        const auto plot = plotArea();
        return { xOf ((float) c.centreHz) - 34.0f, plot.getY() + 26.0f, 68.0f, 15.0f };
    }

    void drawClashes (juce::Graphics& g, juce::Rectangle<float> plot) const
    {
        for (const auto& c : clashes)
        {
            const float x0 = xOf ((float) c.loHz), x1 = xOf ((float) c.hiHz);
            g.setColour (colours::accent.withAlpha (0.10f));
            g.fillRect (x0, plot.getY(), x1 - x0, plot.getHeight());
            const auto label = clashLabel (c);
            g.setColour (colours::accent.withAlpha (0.85f));
            g.drawRoundedRectangle (label, 3.0f, 1.0f);
            g.setFont (ui::lcdFont (10.0f));
            g.drawText ("MASK " + juce::String (c.cutDb, 1), label, juce::Justification::centred, false);
        }
    }

    int clashLabelAt (juce::Point<float> at) const
    {
        for (size_t i = 0; i < clashes.size(); ++i)
            if (clashLabel (clashes[i]).expanded (2.0f).contains (at))
                return (int) i;
        return -1;
    }

    // A bell across the zone that cuts this track far enough for the other one to sit clear of it there.
    void addClashBand (const MaskingMeter::Clash& c)
    {
        const int slot = addBand (apvts, BandType::bell, c.centreHz, c.cutDb);
        if (slot < 0)
            return;
        setParameterValue (apvts, bandIds (slot).q, (float) c.q);
        setSelectedBand (slot);
    }

    juce::String clashTooltip (int i) const
    {
        const auto& c = clashes[(size_t) i];
        return "This track and the sidechain are both loud from " + juce::String (juce::roundToInt (c.loHz)) + " to " + juce::String (juce::roundToInt (c.hiHz))
               + " Hz and within " + juce::String (MaskingMeter::kCloseDb, 0) + " dB of each other (here " + juce::String (c.ownDb, 1) + " against "
               + juce::String (c.otherDb, 1) + " dB), so they mask each other. Double-click to cut this track by " + juce::String (-c.cutDb, 1)
               + " dB there, which leaves the other one " + juce::String (MaskingMeter::kClearDb, 0) + " dB clear. Or cut the other track instead.";
    }

    // ---- resonances ------------------------------------------------------------------------------
    // A small marker on the input trace at each resonance, labelled with how far it stands out. Double-click one for a band.
    void drawResonances (juce::Graphics& g) const
    {
        g.setFont (ui::lcdFont (10.0f));
        for (const auto& r : found)
        {
            const auto p = resonancePoint (r);
            juce::Path tri;
            tri.addTriangle (p.x - 5.0f, p.y - 14.0f, p.x + 5.0f, p.y - 14.0f, p.x, p.y - 5.0f);
            g.setColour (colours::warn.withAlpha (0.9f));
            g.fillPath (tri);
            g.drawText ("+" + juce::String (juce::roundToInt (r.prominenceDb)), juce::Rectangle<float> (p.x - 20.0f, p.y - 28.0f, 40.0f, 13.0f), juce::Justification::centred, false);
        }
    }

    juce::Point<float> resonancePoint (const ResonanceFinder::Resonance& r) const
    {
        const float tiltDb = inTrace.getTilt() * (float) std::log2 (r.freqHz / 1000.0);
        return { xOf ((float) r.freqHz), spectrumY (r.levelDb + tiltDb) };
    }

    int resonanceAt (juce::Point<float> at) const
    {
        for (size_t i = 0; i < found.size(); ++i)
            if (at.getDistanceFrom (resonancePoint (found[i]).translated (0.0f, -12.0f)) < 11.0f)
                return (int) i;
        return -1;
    }

    // A narrow dynamic bell on the resonance: at rest it does nothing, and it cuts as far as the resonance stands out while the
    // level at that frequency is at or above what was measured (threshold 3 dB under it), so it only works while the ring is there.
    void addResonanceBand (const ResonanceFinder::Resonance& r)
    {
        const int slot = addBand (apvts, BandType::bell, r.freqHz, 0.0);
        if (slot < 0)
            return;
        const auto& ids = bandIds (slot);
        setParameterValue (apvts, ids.q, (float) r.q);
        setParameterValue (apvts, ids.dyn, 1.0f);
        setParameterValue (apvts, ids.dynMode, (float) (int) DynMode::above);
        setParameterValue (apvts, ids.threshold, juce::jlimit (-60.0f, 0.0f, r.levelDb - 3.0f));
        setParameterValue (apvts, ids.ratio, 4.0f);
        setParameterValue (apvts, ids.range, -juce::jlimit (1.0f, 24.0f, r.prominenceDb));
        setSelectedBand (slot);
    }

    std::string resonanceTooltip (int i) const
    {
        const auto& r = found[(size_t) i];
        return juce::String ("Resonance at " + juce::String (juce::roundToInt (r.freqHz)) + " Hz: " + juce::String (r.prominenceDb, 1)
                             + " dB above its neighbourhood for the last few seconds (Q about " + juce::String (r.q, 1)
                             + "). Double-click for a narrow dynamic bell that cuts it only while it rings. Turn the markers off in the right-click menu.").toStdString();
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

    Setting markers { "Resonance markers", "resonanceMarkers", { { "Off", 0.0f }, { "On", 1.0f } }, 1 };
    Setting referenceShown { "Reference curve", "referenceShown", { { "Hidden", 0.0f }, { "Shown", 1.0f } }, 1 };

    pdsp::SpectrumAnalyzer inTrace, outTrace;
    ResonanceFinder resonances;
    std::array<float, (size_t) pdsp::SpectrumAnalyzer::numBins> untilted {};
    std::vector<ResonanceFinder::Resonance> found;
    int findTicks = 0;
    pdsp::StereoTap* sidechain = nullptr;
    const std::atomic<bool>* sidechainConnected = nullptr;
    iris::Peer* selfPeer = nullptr;
    std::unique_ptr<iris::PeerListener> listener;
    std::vector<iris::PeerInfo> others;       // the other instances, refreshed twice a second
    std::unique_ptr<pdsp::StereoTap> peerTap = std::make_unique<pdsp::StereoTap>(); // (256 KB, so on the heap) what the chosen instance sent, on its way into sideTrace
    std::array<std::array<float, 4096>, 2> peerBlock {};
    bool peerFound = false;
    pdsp::SpectrumAnalyzer sideTrace;
    std::array<float, (size_t) pdsp::SpectrumAnalyzer::numBins> sideUntilted {};
    MaskingMeter masking;
    std::vector<MaskingMeter::Clash> clashes;
    SpectrumProfile liveProfile, learnProfile;
    bool learning = false, haveReference = false;
    Shape reference {};
    std::string referenceName;
    juce::String note;
    int noteTicks = 0;
    int selected = 0;
    juce::Point<float> hover { -1.0f, -1.0f };
};
} // namespace iris::gui
