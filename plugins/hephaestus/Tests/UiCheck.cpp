// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 The Pantheon authors

// Drives the real spectrum component with synthetic mouse events and checks what lands in the
// parameters: edge and body drags, "narrowest band wins", the minimum width, fill-band limits,
// slot allocation. Also loads a session saved before bands existed, and follows latency reporting
// through the processor. Headless; no window is shown.
//
//   HephaestusUiCheck

#include "Check.h"
#include "PluginProcessor.h"
#include "UI/SpectrumView.h"
#include <pantheon/ui/Controls.h>

using namespace hep;
using namespace test;

namespace
{
constexpr int viewWidth = 1008, viewHeight = 224;

// The view's plot is its bounds inset by 6 px on every side (see SpectrumView::plotArea).
float xOf (double hz)
{
    return 6.0f + gui::FreqAxis::toNormalised ((float) hz) * (float) (viewWidth - 12);
}

float value (HephaestusProcessor& p, const juce::String& id) { return p.apvts.getRawParameterValue (id)->load(); }

juce::MouseEvent mouse (juce::Component& c, float x, float downX, bool dragged)
{
    return { juce::Desktop::getInstance().getMainMouseSource(), { x, 100.0f }, juce::ModifierKeys(),
             juce::MouseInputSource::defaultPressure, juce::MouseInputSource::defaultOrientation,
             juce::MouseInputSource::defaultRotation, juce::MouseInputSource::defaultTiltX,
             juce::MouseInputSource::defaultTiltY, &c, &c, juce::Time::getCurrentTime(),
             { downX, 100.0f }, juce::Time::getCurrentTime(), 1, dragged };
}

// A full press-drag-release gesture from one x position to another.
void dragFrom (juce::Component& view, float fromX, float toX)
{
    view.mouseDown (mouse (view, fromX, fromX, false));
    for (int step = 1; step <= 8; ++step)
        view.mouseDrag (mouse (view, fromX + (toX - fromX) * (float) step / 8.0f, fromX, true));
    view.mouseUp (mouse (view, toX, fromX, true));
}
}

int main()
{
    juce::ScopedJuceInitialiser_GUI gui;
    HephaestusProcessor proc;
    proc.setPlayConfigDetails (2, 2, 48000.0, 512);
    proc.prepareToPlay (48000.0, 512);

    gui::SpectrumView view (proc.engine, proc.apvts, proc.currentSampleRate);
    view.setBounds (0, 0, viewWidth, viewHeight);

    check (proc.getName() == juce::String (gui::kProjectWord) + " " + gui::kPluginWord, "the reported name is the nameplate's two words");

    std::printf ("\nBand slots\n");
    check (isBandOn (proc.apvts, 0) && ! isBandOn (proc.apvts, 1), "a fresh plugin has one band (a gentle full-range saturator)");

    const int slot = addBand (proc.apvts, BandMode::saturate, 1000.0);
    check (slot == 1 && std::fabs (value (proc, bandIds (1).lo) - 1000.0f / 1.8f) < 1.0f
               && std::fabs (value (proc, bandIds (1).hi) - 1000.0f * 1.8f) < 1.0f,
           fmt ("a band added at 1 kHz takes the first free slot (%d), spanning %.0f - %.0f Hz", slot,
                value (proc, bandIds (1).lo), value (proc, bandIds (1).hi)));

    std::printf ("\nMouse\n");
    view.setSelectedBand (0);
    view.mouseDown (mouse (view, xOf (1000.0), xOf (1000.0), false));
    view.mouseUp (mouse (view, xOf (1000.0), xOf (1000.0), false));
    check (view.getSelectedBand() == 1, "clicking where bands overlap selects the narrowest one");

    dragFrom (view, xOf (value (proc, bandIds (1).lo)), xOf (400.0));
    check (std::fabs (value (proc, bandIds (1).lo) / 400.0f - 1.0f) < 0.03f && std::fabs (value (proc, bandIds (1).hi) - 1800.0f) < 2.0f,
           fmt ("dragging the low edge to 400 Hz: low %.0f Hz, high untouched at %.0f Hz", value (proc, bandIds (1).lo), value (proc, bandIds (1).hi)));

    const float ratioBefore = value (proc, bandIds (1).hi) / value (proc, bandIds (1).lo);
    const float centreX = (xOf (value (proc, bandIds (1).lo)) + xOf (value (proc, bandIds (1).hi))) * 0.5f;
    dragFrom (view, centreX, centreX + 120.0f);
    const float ratioAfter = value (proc, bandIds (1).hi) / value (proc, bandIds (1).lo);
    check (value (proc, bandIds (1).lo) > 800.0f && std::fabs (ratioAfter / ratioBefore - 1.0f) < 0.01f,
           fmt ("dragging the body right: %.0f - %.0f Hz, width ratio %.3f (was %.3f)", value (proc, bandIds (1).lo),
                value (proc, bandIds (1).hi), ratioAfter, ratioBefore));

    dragFrom (view, xOf (value (proc, bandIds (1).hi)), xOf (100.0));
    check (std::fabs (value (proc, bandIds (1).hi) / value (proc, bandIds (1).lo) - (float) minRangeRatio) < 0.01f,
           fmt ("dragging the high edge past the low one stops at the minimum width (ratio %.2f)",
                value (proc, bandIds (1).hi) / value (proc, bandIds (1).lo)));

    std::printf ("\nModes and limits\n");
    setParameterValue (proc.apvts, bandIds (1).lo, 3000.0f);
    setParameterValue (proc.apvts, bandIds (1).hi, 10000.0f);
    setBandMode (proc.apvts, 1, BandMode::fill);
    check (bandMode (proc.apvts, 1) == BandMode::fill && value (proc, bandIds (1).hi) <= (float) fillMaxHz + 0.5f
               && value (proc, bandIds (1).hi) >= value (proc, bandIds (1).lo) * (float) minRangeRatio - 0.5f,
           fmt ("switching a 3 - 10 kHz band to fill pulls it into range: %.0f - %.0f Hz (limit %.0f)",
                value (proc, bandIds (1).lo), value (proc, bandIds (1).hi), fillMaxHz));

    dragFrom (view, xOf (value (proc, bandIds (1).hi)), xOf (9000.0));
    check (value (proc, bandIds (1).hi) <= (float) fillMaxHz + 0.5f,
           fmt ("a fill band's edge can't be dragged above %.0f Hz (now %.0f)", fillMaxHz, value (proc, bandIds (1).hi)));

    std::printf ("\nSlots\n");
    for (int i = 0; i < maxBands; ++i)
        addBand (proc.apvts, BandMode::fill, 80.0);
    check (addBand (proc.apvts, BandMode::saturate, 1000.0) == -1, "no free slot left: adding returns -1");

    removeBand (proc.apvts, 3);
    check (addBand (proc.apvts, BandMode::saturate, 1000.0, 3) == 3, "a freed slot is reused, and is the preferred one when asked for");

    std::printf ("\nLatency follows oversampling (processor level)\n");
    {
        HephaestusProcessor p2;
        p2.setPlayConfigDetails (2, 2, 48000.0, 512);
        p2.prepareToPlay (48000.0, 512);
        juce::AudioBuffer<float> buffer (2, 512);
        juce::MidiBuffer midi;
        std::string line;
        bool ok = true;
        int previous = -1;
        for (int mode = 0; mode < 4; ++mode)
        {
            setParameterValue (p2.apvts, id::oversampling, (float) mode);
            for (int i = 0; i < 3; ++i)
            {
                buffer.clear();
                p2.processBlock (buffer, midi);
                juce::MessageManager::getInstance()->runDispatchLoopUntil (50);
            }
            line += fmt (" %d:%d", mode, p2.getLatencySamples());
            ok = ok && (mode == 0) == (p2.getLatencySamples() == 0) && p2.getLatencySamples() >= previous;
            previous = p2.getLatencySamples();
        }
        check (ok, "reported latency by oversampling mode (0 = off):" + line);
    }

    std::printf ("\nLegacy sessions (3 fixed saturator bands + one bass engine)\n");
    {
        HephaestusProcessor p3;
        p3.setPlayConfigDetails (2, 2, 48000.0, 512);
        p3.prepareToPlay (48000.0, 512);

        juce::XmlElement legacy ("STATE");
        for (auto [pid, v] : { std::pair<const char*, double> { "driveLow", 6.0 }, { "driveMid", 2.0 }, { "driveHigh", 9.0 },
                               { "xoverLow", 200.0 }, { "xoverHigh", 3000.0 }, { "fundLo", 45.0 }, { "fundHi", 100.0 },
                               { "extAmount", -6.0 }, { "satOn", 1.0 } })
        {
            auto* node = legacy.createNewChildElement ("PARAM");
            node->setAttribute ("id", pid);
            node->setAttribute ("value", v);
        }
        juce::MemoryBlock block;
        juce::AudioProcessor::copyXmlToBinary (legacy, block);
        p3.setStateInformation (block.getData(), (int) block.getSize());

        const auto at = [&] (int band, const juce::String BandIds::* field) { return value (p3, bandIds (band).*field); };
        check (std::fabs (at (0, &BandIds::hi) - 200.0f) < 0.5f && std::fabs (at (1, &BandIds::hi) - 3000.0f) < 0.5f
                   && std::fabs (at (2, &BandIds::lo) - 3000.0f) < 0.5f && std::fabs (at (2, &BandIds::drive) - 9.0f) < 0.05f,
               fmt ("three saturate bands: 20-%.0f, %.0f-%.0f, %.0f-%.0f Hz at %.0f / %.0f / %.0f dB",
                    at (0, &BandIds::hi), at (1, &BandIds::lo), at (1, &BandIds::hi), at (2, &BandIds::lo), at (2, &BandIds::hi),
                    at (0, &BandIds::drive), at (1, &BandIds::drive), at (2, &BandIds::drive)));
        check (isBandOn (p3.apvts, 3) && bandMode (p3.apvts, 3) == BandMode::fill && std::fabs (at (3, &BandIds::lo) - 45.0f) < 0.5f
                   && std::fabs (at (3, &BandIds::hi) - 100.0f) < 0.5f && at (3, &BandIds::adapt) == 0.0f
                   && ! isBandOn (p3.apvts, 4),
               fmt ("the bass engine becomes fill band 4: %.0f-%.0f Hz, amount %.1f dB, adapt %.0f %% (the old engine was open-loop)",
                    at (3, &BandIds::lo), at (3, &BandIds::hi), at (3, &BandIds::amount), at (3, &BandIds::adapt)));
    }

    std::printf ("\nTyping a value into a knob\n");
    {
        ui::Knob knob (proc.apvts, id::outGain, "Output", "tip", ui::Knob::Layout::below);
        knob.setBounds (0, 0, 120, 140);
        const auto entry = [&] () -> juce::TextEditor*
        {
            for (auto* child : knob.getChildren())
                if (auto* te = dynamic_cast<juce::TextEditor*> (child))
                    return te;
            return nullptr;
        };
        const auto typeIn = [&] (const juce::String& text)
        {
            knob.mouseDown (mouse (knob, 60.0f, 60.0f, false).withNewPosition (juce::Point<float> (60.0f, 130.0f))); // on the read-out
            auto* te = entry();
            if (te == nullptr || ! te->isVisible())
                return false;
            te->setText (text, false);
            te->onReturnKey();
            return true;
        };

        const auto expectTyped = [&] (const juce::String& text, float expectedDb, const char* what)
        {
            const bool opened = typeIn (text);
            const float now = value (proc, id::outGain);
            check (opened && std::fabs (now - expectedDb) < 0.01f, fmt ("%s (%.2f dB)", what, (double) now));
        };
        expectTyped ("-7.5", -7.5f, "typing -7.5 sets the output gain");
        expectTyped ("3 dB", 3.0f, "a value with its unit is accepted");
        expectTyped ("99", 12.0f, "a value out of range is clamped");
        typeIn ("abc");
        check (std::fabs (value (proc, id::outGain) - 12.0f) < 0.01f, "text that is not a number leaves the value alone");
        check (entry() != nullptr && ! entry()->isVisible(), "the entry closes after Enter");

        knob.mouseDown (mouse (knob, 60.0f, 60.0f, false).withNewPosition (juce::Point<float> (60.0f, 130.0f)));
        entry()->setText ("-1", false);
        entry()->onEscapeKey();
        check (std::fabs (value (proc, id::outGain) - 12.0f) < 0.01f && ! entry()->isVisible(), "Esc cancels the entry");

        // A word the parameter itself shows ("Off") is a value too; the knob is re-bound, as the band panel does.
        const auto rebuild = bandIds (0).rebuild;
        knob.bind (proc.apvts, rebuild);
        typeIn ("0");
        const float before = value (proc, rebuild);
        typeIn ("off");
        check (std::fabs (before) < 0.01f && value (proc, rebuild) <= offDb + 0.05f,
               fmt ("typing \"off\" turns Rebuild off (%.1f dB, then %.1f dB)", (double) before, (double) value (proc, rebuild)));
        typeIn ("loud");
        check (value (proc, rebuild) <= offDb + 0.05f, "a word the parameter does not show is ignored");

        // A detent switch takes a position's name, or as much of it as tells it apart.
        knob.bind (proc.apvts, id::curve); // Tanh, Atan, Soft
        typeIn ("soft");
        check (std::lround (value (proc, id::curve)) == 2, "typing \"soft\" picks Soft");
        typeIn ("a");
        check (std::lround (value (proc, id::curve)) == 1, "a unique start (\"a\") picks Atan");
        typeIn ("sine");
        check (std::lround (value (proc, id::curve)) == 1, "a word that names no position (\"sine\") is ignored");
        knob.bind (proc.apvts, id::stereo); // Stereo (L/R), Mid/Side
        typeIn ("mid / side");
        check (std::lround (value (proc, id::stereo)) == 1, "spaces and case do not matter (\"mid / side\")");
        typeIn ("s");
        check (std::lround (value (proc, id::stereo)) == 0, "\"s\" picks Stereo (L/R)");
    }

    std::printf ("\nPaste bands from another plugin\n");
    {
        HephaestusProcessor fresh;
        for (int i = 0; i < maxBands; ++i)
            removeBand (fresh.apvts, i);
        int skipped = 0;
        const auto slots = pasteBands (fresh.apvts, "pantheon-bands 1\nfill lo=40 hi=90 place=mid\nbell freq=270 gain=-2.0 q=0.70 place=mid", skipped);
        check (slots.size() == 1 && skipped == 1, fmt ("the fill band is added, the equaliser band left for Iris (%d added, %d skipped)", (int) slots.size(), skipped));
        if (slots.size() == 1)
            check (bandMode (fresh.apvts, slots[0]) == BandMode::fill
                       && std::fabs (fresh.apvts.getRawParameterValue (bandIds (slots[0]).lo)->load() - 40.0f) < 0.5f
                       && std::fabs (fresh.apvts.getRawParameterValue (bandIds (slots[0]).hi)->load() - 90.0f) < 0.5f,
                   "... over the range it was given");
    }

    return finish ("UI CHECK");
}
