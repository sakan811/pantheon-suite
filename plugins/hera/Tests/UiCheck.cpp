// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 The Pantheon authors

// Builds the editor on the real processor and checks what a host and a user would meet: the parameters, the name, the controls, the sidechain bus,
// the state, and the notes.

#include "Advice.h"
#include "Check.h"
#include "PluginEditor.h"
#include <pantheon/ui/Scaled.h>

using namespace hera;
using test::check;
using test::fmt;

int main()
{
    juce::ScopedJuceInitialiser_GUI gui;
    std::printf ("Hera UI check\n");
    HeraProcessor proc;
    proc.setPlayConfigDetails (2, 2, 48000.0, 512);
    proc.prepareToPlay (48000.0, 512);

    check (proc.getName() == juce::String (gui::kProjectWord) + " " + gui::kPluginWord, "the reported name is the nameplate's two words");
    check (proc.getParameters().size() == 15, fmt ("%d parameters: nine knobs, the detector, three switches, the look-ahead and LEVEL MATCH", proc.getParameters().size()));
    check (lookaheadNames.size() == kNumLookaheads, "the look-ahead list matches the engine's");
    check (detectorNames.size() == kNumDetectors, "the choice list matches the engine's detectors");
    const auto d = readParams (proc.apvts);
    check (std::fabs (d.thresholdDb + 18.0f) < 1e-3f && std::fabs (d.ratio - 2.0f) < 1e-3f && std::fabs (d.attackMs - 10.0f) < 0.01f && std::fabs (d.releaseMs - 150.0f) < 0.1f &&
               std::fabs (d.kneeDb - 6.0f) < 1e-3f && d.mix > 0.999f && d.link > 0.999f && d.detector == Detector::rms && ! d.autoRelease && ! d.autoMakeup && ! d.external && d.lookahead == 0 && ! d.match && proc.getLatencySamples() == 0,
           "a new instance: -18 dB, 2:1, 10 ms, 150 ms, 6 dB knee, MIX and LINK 100%, RMS, no look-ahead, no latency");

    // The sidechain bus is there, off, and a mono layout is supported.
    check (proc.getBusCount (true) == 2 && ! proc.getBus (true, 1)->isEnabledByDefault(), "a second input, the sidechain, is off until the host routes a track to it");
    check (proc.isBusesLayoutSupported ([&] { auto l = proc.getBusesLayout(); l.getChannelSet (true, 0) = juce::AudioChannelSet::mono(); l.getChannelSet (false, 0) = juce::AudioChannelSet::mono(); return l; }()), "a mono layout is supported");

    // The audio path through the real processor.
    {
        HeraProcessor q;
        q.setPlayConfigDetails (2, 2, 48000.0, 512);
        q.prepareToPlay (48000.0, 512);
        setValue (q.apvts, id::threshold, -24.0f);
        setValue (q.apvts, id::ratio, 4.0f);
        setValue (q.apvts, id::knee, 0.0f);
        setValue (q.apvts, id::attack, 1.0f);
        juce::AudioBuffer<float> buffer (2, 512);
        juce::MidiBuffer midi;
        float peak = 0.0f;
        for (int block = 0; block < 200; ++block)
        {
            for (int i = 0; i < 512; ++i)
                for (int c = 0; c < 2; ++c)
                    buffer.setSample (c, i, 0.5f * std::sin (2.0f * 3.14159265f * 1000.0f * (float) (block * 512 + i) / 48000.0f));
            q.processBlock (buffer, midi);
            if (block > 100)
                peak = std::max (peak, buffer.getMagnitude (0, 512));
        }
        check (std::fabs (20.0f * std::log10 (peak / 0.5f) + 13.5f) < 0.3f, fmt ("a -6 dBFS tone over a -24 dB threshold at 4:1 is turned down by %.2f dB through the processor (law 13.5)", -20.0f * std::log10 (peak / 0.5f)));
    }

    {
        std::unique_ptr<juce::AudioProcessorEditor> holder (proc.createEditor());
        auto& editor = ui::innerOf<HeraEditor> (*holder);
        editor.setVisible (true);
        check (editor.detector.getNumItems() == kNumDetectors, "the detector selector has the parameter's items");
        check (editor.getWidth() > 800 && editor.hpf.getRight() <= editor.getWidth() && editor.key.getBottom() < editor.getHeight(), "the knobs and switches fit the window");
        editor.autoRelease.triggerClick();
        editor.key.triggerClick();
        juce::MessageManager::getInstance()->runDispatchLoopUntil (100);
        check (readParams (proc.apvts).autoRelease && readParams (proc.apvts).external, "the switches set their parameters");
        check (ui::hasNote (editor.notes(), "KEY is on, but no sidechain", ui::Severity::warning), "KEY on with nothing connected: a note says it listens to itself");
        check (ui::hasNote (editor.notes(), "AUTO release", ui::Severity::info), "AUTO release: a note says RELEASE is not used");
    }

    // The look-ahead is a delay the host is told about.
    {
        HeraProcessor q;
        q.setPlayConfigDetails (2, 2, 48000.0, 512);
        q.prepareToPlay (48000.0, 512);
        setValue (q.apvts, id::lookahead, 3.0f);
        juce::AudioBuffer<float> buffer (2, 512);
        buffer.clear();
        juce::MidiBuffer midi;
        q.processBlock (buffer, midi);
        juce::MessageManager::getInstance()->runDispatchLoopUntil (100);
        check (q.getLatencySamples() == 240, fmt ("LOOK-AHEAD 5 ms: the host is told %d samples (240)", q.getLatencySamples()));
        HeraProcessor r;
        setValue (r.apvts, id::lookahead, 4.0f);
        r.setPlayConfigDetails (2, 2, 48000.0, 512);
        r.prepareToPlay (48000.0, 512);
        check (r.getLatencySamples() == 480, "a session opened with LOOK-AHEAD 10 ms tells the host 480 samples at once");
    }

    // Presets: each applies, has a name and a one-line purpose, and the chosen one is kept with the session.
    {
        HeraProcessor q;
        q.setPlayConfigDetails (2, 2, 48000.0, 512);
        q.prepareToPlay (48000.0, 512);
        bool all = q.getNumPrograms() == (int) presets().size() && q.getNumPrograms() >= 6;
        for (int i = 0; i < q.getNumPrograms(); ++i)
        {
            q.setCurrentProgram (i);
            const auto len = std::strlen (presets()[(size_t) i].purpose);
            all = all && q.getProgramName (i).isNotEmpty() && q.getCurrentProgram() == i && len > 20 && len <= 140;
        }
        check (all, fmt ("all %d presets apply, have a name and a one-line purpose", q.getNumPrograms()));
        int duck = 0, glue = 0, catcher = 0;
        for (int i = 0; i < q.getNumPrograms(); ++i)
        {
            if (q.getProgramName (i) == "Sidechain Duck") duck = i;
            if (q.getProgramName (i) == "Bus Glue") glue = i;
            if (q.getProgramName (i) == "Peak Catcher") catcher = i;
        }
        q.setCurrentProgram (duck);
        check (readParams (q.apvts).external && readParams (q.apvts).ratio > 5.0f, "Sidechain Duck turns KEY on and sets a high ratio");
        q.setCurrentProgram (catcher);
        check (readParams (q.apvts).lookahead == 2 && ! readParams (q.apvts).external, "Peak Catcher sets 2 ms of look-ahead, and KEY goes back to off (a preset does not inherit the last one)");
        q.setCurrentProgram (glue);
        check (readParams (q.apvts).autoRelease && readParams (q.apvts).lookahead == 0, "Bus Glue: AUTO release on, and the look-ahead back to Off");
        q.setCurrentProgram (0);
        const auto d0 = readParams (q.apvts);
        check (std::fabs (d0.thresholdDb + 18.0f) < 1e-3f && ! d0.autoRelease && d0.lookahead == 0 && d0.mix > 0.999f, "Init is the defaults");
        q.setCurrentProgram (duck);
        juce::MemoryBlock saved;
        q.getStateInformation (saved);
        HeraProcessor other;
        other.setStateInformation (saved.getData(), (int) saved.getSize());
        check (other.getCurrentProgram() == duck && readParams (other.apvts).external, "the chosen preset and its settings are saved with the session");
        std::unique_ptr<juce::AudioProcessorEditor> holder (q.createEditor());
        auto& editor = ui::innerOf<HeraEditor> (*holder);
        editor.setVisible (true);
        check (editor.presetBox.getNumItems() == q.getNumPrograms() && editor.presetBox.getSelectedId() == duck + 1, "the editor's preset box lists the presets and shows the current one");
        check (editor.presetBox.getRight() < editor.getWidth() && editor.lookahead.getRight() < editor.getWidth() && editor.lookahead.getNumItems() == kNumLookaheads, "the preset box and LOOK-AHEAD fit the header");
    }

    // The state survives a round trip.
    {
        setValue (proc.apvts, id::threshold, -31.5f);
        setValue (proc.apvts, id::mix, 40.0f);
        juce::MemoryBlock saved;
        proc.getStateInformation (saved);
        HeraProcessor other;
        other.setStateInformation (saved.getData(), (int) saved.getSize());
        check (std::fabs (readParams (other.apvts).thresholdDb + 31.5f) < 0.01f && std::fabs (readParams (other.apvts).mix - 0.4f) < 0.01f, "the state of a session comes back as it was saved");
    }

    // Bypassed by the host: the sound is still delayed by the look-ahead the host was told, or the track would jump early.
    {
        HeraProcessor q;
        q.setPlayConfigDetails (2, 2, 48000.0, 512);
        q.prepareToPlay (48000.0, 512);
        setValue (q.apvts, id::lookahead, 4.0f); // 10 ms
        juce::AudioBuffer<float> buffer (2, 512);
        juce::MidiBuffer midi;
        buffer.clear();
        q.processBlock (buffer, midi);
        juce::MessageManager::getInstance()->runDispatchLoopUntil (100);
        const int latency = q.getLatencySamples();
        int at = -1, seen = 0;
        for (int block = 0; block < 4; ++block)
        {
            buffer.clear();
            if (block == 0)
                buffer.setSample (0, 0, 1.0f), buffer.setSample (1, 0, 0.5f);
            q.processBlockBypassed (buffer, midi);
            for (int i = 0; i < 512; ++i)
                if (buffer.getSample (0, i) != 0.0f)
                    at = block * 512 + i, ++seen;
        }
        check (latency == 480 && at == latency && seen == 1, fmt ("bypassed with 10 ms of look-ahead, an impulse comes out %d samples late (the reported latency, %d)", at, latency));
    }

    // LEVEL MATCH, DELTA and SC LISTEN: the controls, and what the notes say about them.
    {
        HeraProcessor q;
        q.setPlayConfigDetails (2, 2, 48000.0, 512);
        q.prepareToPlay (48000.0, 512);
        {
            std::unique_ptr<juce::AudioProcessorEditor> holder (q.createEditor());
            auto& editor = ui::innerOf<HeraEditor> (*holder);
            editor.setVisible (true);
            check (editor.key.getRight() < editor.levelMatch.getX() && editor.levelMatch.getRight() < editor.deltaButton.getX() && editor.deltaButton.getRight() < editor.listenButton.getX()
                       && editor.listenButton.getRight() < editor.getWidth(),
                   "LEVEL MATCH, DELTA and SC LISTEN sit in the switch row after KEY, overlapping nothing");
            editor.deltaButton.triggerClick();
            juce::MessageManager::getInstance()->runDispatchLoopUntil (100);
            check (q.engine.delta.load() && ui::hasNote (editor.notes(), "DELTA is on", ui::Severity::warning), "DELTA turns the engine's monitor on, and a warning says you do not hear your track");
            editor.listenButton.triggerClick();
            juce::MessageManager::getInstance()->runDispatchLoopUntil (100);
            check (q.engine.scListen.load() && ui::hasNote (editor.notes(), "SC LISTEN is on", ui::Severity::warning), "SC LISTEN turns the engine's monitor on, and a warning says you hear the key");
            check (q.getParameters().size() == 15 && ! (bool) q.apvts.state.getProperty ("delta", false), "neither is a parameter: they are never saved or automated");
        }
        check (! q.engine.delta.load() && ! q.engine.scListen.load(), "closing the editor ends both: monitoring never outlives its window");
        setValue (q.apvts, id::match, 1.0f);
        q.setCurrentProgram (2);
        check (readParams (q.apvts).match, "a preset keeps LEVEL MATCH");
        AdviceInput in;
        in.params.match = true;
        check (ui::hasNote (adviceFor (in), "LEVEL MATCH is on", ui::Severity::info), "LEVEL MATCH on says what it does");
    }

    // The notes.
    {
        AdviceInput in;
        check (adviceFor (in).empty(), "a default instance has nothing to warn about");
        in.params.ratio = 1.0f;
        check (ui::hasNote (adviceFor (in), "RATIO is 1:1", ui::Severity::warning), "RATIO 1:1 does nothing");
        in.params.ratio = 12.0f;
        in.params.attackMs = 1.0f;
        in.params.detector = Detector::peak;
        in.params.thresholdDb = 0.0f;
        in.heldDb = 14.0f;
        check (ui::hasNote (adviceFor (in), "THRESH is at the top", ui::Severity::warning) && ui::hasNote (adviceFor (in), "Taking off 14 dB", ui::Severity::warning), "THRESH at the top, and 14 dB of reduction, are warned about");
        check (ui::hasNote (adviceFor (in), "high RATIO with a fast ATTACK", ui::Severity::info) && ui::hasNote (adviceFor (in), "very fast ATTACK", ui::Severity::info), "a high ratio and a fast attack are called limiting, and a fast peak attack a loss of punch");
        in.params.mix = 0.5f;
        in.params.autoMakeup = true;
        in.params.makeupDb = 3.0f;
        in.params.external = true;
        in.params.autoRelease = true;
        in.params.match = true;
        in.delta = true;
        check (ui::hasNote (adviceFor (in), "MIX under 100%", ui::Severity::info) && ui::hasNote (adviceFor (in), "AUTO makeup is on", ui::Severity::info), "parallel mix and auto makeup are explained");
        bool tooLong = false;
        for (const auto& n : adviceFor (in))
            if ((int) n.text.size() > ui::kMaxNoteChars)
            {
                tooLong = true;
                std::printf ("  too long (%d): %s\n", (int) n.text.size(), n.text.c_str());
            }
        check (! tooLong, "every note fits a line of the strip");
    }

    return test::finish ("HeraUiCheck");
}
