// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 The Pantheon authors

// Builds the editor on the real processor and checks what a host and a user would meet: the parameters, the name, the controls, the sidechain bus,
// the state, and the notes.

#include "Advice.h"
#include "Check.h"
#include "PluginEditor.h"
#include <pantheon/ui/Scaled.h>
#include <algorithm>
#include <array>

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
    check (proc.getParameters().size() == 17, fmt ("%d parameters: ten knobs, the detector, the topology, three switches, the look-ahead and LEVEL MATCH", proc.getParameters().size()));
    check (lookaheadNames.size() == kNumLookaheads, "the look-ahead list matches the engine's");
    check (detectorNames.size() == kNumDetectors, "the choice list matches the engine's detectors");
    check (topologyNames.size() == kNumTopologies && readParams (proc.apvts).topology == Topology::feedForward, "the topology list matches the engine's, and a new instance is Feed-forward");
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
        check (ui::hasEditorAspect (editor.getWidth(), editor.getHeight()), "the editor is drawn at 16:10, the shape of every plugin");
        editor.setVisible (true);
        check (editor.detector.getNumItems() == kNumDetectors, "the detector selector has the parameter's items");
        check (editor.getWidth() > 800 && editor.hpf.getRight() <= editor.getWidth() && editor.key.getBottom() < editor.getHeight(), "the knobs and switches fit the window");
        check (editor.topology.getNumItems() == kNumTopologies && editor.detector.getRight() < editor.topology.getX() && editor.topology.getRight() < editor.autoRelease.getX()
                   && editor.topology.getRight() < editor.getWidth(),
               "TOPOLOGY sits in the switch row after DETECTOR, overlapping nothing, with the parameter's two items");
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

    // TOPOLOGY: feedback reads its own output, so the same settings turn a tone down less; it has no look-ahead, so the host is told no latency there,
    // and the look-ahead setting comes back with feed-forward.
    {
        // The processor's gain on a -6 dBFS tone at 4:1 over -24 dB (RMS detector, 1 ms attack), in dB: negative.
        const auto gainThrough = [] (float topologyValue)
        {
            HeraProcessor q;
            q.setPlayConfigDetails (2, 2, 48000.0, 512);
            q.prepareToPlay (48000.0, 512);
            setValue (q.apvts, id::threshold, -24.0f);
            setValue (q.apvts, id::ratio, 4.0f);
            setValue (q.apvts, id::knee, 0.0f);
            setValue (q.apvts, id::attack, 1.0f);
            setValue (q.apvts, id::topology, topologyValue);
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
            return 20.0f * std::log10 (peak / 0.5f);
        };
        const float ffGain = gainThrough (0.0f), fbGain = gainThrough (1.0f);
        check (std::fabs (ffGain + 13.5f) < 0.3f && std::fabs (fbGain + 7.7f) < 0.3f,
               fmt ("through the processor, a -6 dBFS tone at 4:1 over -24 dB: feed-forward %.2f dB (the law -13.5), feedback %.2f dB (the steady state -7.7)", ffGain, fbGain));

        // Look-ahead and latency: feedback has none and reports 0, and feed-forward gets its look-ahead, and its latency, back.
        HeraProcessor t;
        t.setPlayConfigDetails (2, 2, 48000.0, 512);
        t.prepareToPlay (48000.0, 512);
        setValue (t.apvts, id::lookahead, 3.0f); // 5 ms
        juce::AudioBuffer<float> buffer (2, 512);
        juce::MidiBuffer midi;
        const auto runBlock = [&] (juce::AudioProcessor& p)
        {
            buffer.clear();
            p.processBlock (buffer, midi);
            juce::MessageManager::getInstance()->runDispatchLoopUntil (100);
        };
        runBlock (t);
        check (t.getLatencySamples() == 240, fmt ("feed-forward with LOOK-AHEAD 5 ms: the host is told %d samples (240)", t.getLatencySamples()));
        setValue (t.apvts, id::topology, 1.0f);
        runBlock (t);
        check (t.getLatencySamples() == 0 && readParams (t.apvts).lookahead == 3, fmt ("changing TOPOLOGY to Feedback: the host is told %d samples (0), and the LOOK-AHEAD setting is kept", t.getLatencySamples()));
        setValue (t.apvts, id::topology, 0.0f);
        runBlock (t);
        check (t.getLatencySamples() == 240, fmt ("back to Feed-forward: the host is told %d samples again (240)", t.getLatencySamples()));
        setValue (t.apvts, id::topology, 1.0f);
        runBlock (t);
        juce::MemoryBlock feedbackState;
        t.getStateInformation (feedbackState);
        HeraProcessor reopened;
        reopened.setStateInformation (feedbackState.getData(), (int) feedbackState.getSize());
        check (readParams (reopened.apvts).topology == Topology::feedback, "a session saved in Feedback opens in Feedback");
        HeraProcessor fresh;
        fresh.setPlayConfigDetails (2, 2, 48000.0, 512);
        setValue (fresh.apvts, id::lookahead, 3.0f);
        setValue (fresh.apvts, id::topology, 1.0f);
        fresh.prepareToPlay (48000.0, 512);
        check (fresh.getLatencySamples() == 0, "a session that opens in Feedback with LOOK-AHEAD on tells the host 0 samples at once");

        // The notes say what Feedback changes, with its law in numbers, and fit a line of the strip.
        AdviceInput fbIn;
        fbIn.params.topology = Topology::feedback;
        fbIn.params.lookahead = 2;
        fbIn.params.ratio = 4.0f;
        fbIn.params.thresholdDb = -20.0f;
        check (ui::hasNote (adviceFor (fbIn), "Feedback does not use LOOK-AHEAD", ui::Severity::info), "Feedback with LOOK-AHEAD on: a note says the look-ahead is not used");
        check (ui::hasNote (adviceFor (fbIn), "Feedback: each dB over THRESH takes 0.43 dB off at 4.0:1", ui::Severity::info), "... and the law in numbers: 0.43 dB per dB over THRESH at 4:1");
        fbIn.params.external = true;
        fbIn.sidechainConnected = true;
        check (ui::hasNote (adviceFor (fbIn), "with KEY and a sidechain connected", ui::Severity::info), "KEY with a sidechain connected: a note says the detector reads the key");
        bool fits = true;
        for (const auto& note : adviceFor (fbIn))
            fits = fits && (int) note.text.size() <= ui::kMaxNoteChars;
        check (fits, "every Feedback note fits a line of the strip");
        AdviceInput ffIn;
        check (! ui::hasNote (adviceFor (ffIn), "Feedback", ui::Severity::info), "Feed-forward says nothing about Feedback");
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
        check (std::fabs (d0.thresholdDb + 18.0f) < 1e-3f && ! d0.autoRelease && d0.lookahead == 0 && d0.mix > 0.999f && d0.topology == Topology::feedForward, "Init is the defaults");
        setValue (q.apvts, id::topology, 1.0f);
        q.setCurrentProgram (duck);
        check (readParams (q.apvts).topology == Topology::feedForward, "a preset puts TOPOLOGY back to Feed-forward when it was Feedback: no preset sets it, so none changes the sound");
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

    // UNDO / REDO, A / B and the header strip they sit in.
    {
        HeraProcessor q;
        q.setPlayConfigDetails (2, 2, 48000.0, 512);
        q.prepareToPlay (48000.0, 512);
        std::unique_ptr<juce::AudioProcessorEditor> holder (q.createEditor());
        auto& editor = ui::innerOf<HeraEditor> (*holder);
        editor.setVisible (true);
        // The state records a change when its timer next writes it to the tree (in the step open then), so a change has settled when the tree holds it.
        // Pumps the message loop in 10 ms steps for at most 2 s; a check after it says what is wrong if the condition never holds.
        const auto pumpUntil = [] (auto&& done) {
            for (int waited = 0; ! done() && waited < 2000; waited += 10)
                juce::MessageManager::getInstance()->runDispatchLoopUntil (10);
            return done();
        };
        const auto inTree = [&q] (const juce::String& pid, float plain) {
            for (int i = 0; i < q.apvts.state.getNumChildren(); ++i)
            {
                const auto child = q.apvts.state.getChild (i);
                if (child.getProperty ("id").toString() == pid)
                    return std::fabs ((float) child.getProperty ("value") - plain) < 1e-3f;
            }
            return false;
        };

        const auto undoR = editor.undoControls().getBounds(), abR = editor.abControls().getBounds();
        check (editor.undoControls().isVisible() && editor.abControls().isVisible() && editor.undoControls().undoSwitch().isVisible() && editor.abControls().button (1).isVisible(),
               "UNDO / REDO and A / B are on screen");
        check (editor.getLocalBounds().contains (undoR) && editor.getLocalBounds().contains (abR), "UNDO / REDO and A / B are inside the editor");
        check (! undoR.intersects (abR) && abR.getX() > undoR.getRight(), "UNDO / REDO and A / B sit side by side without overlapping");
        check (undoR.getX() >= 12 + 250 && undoR.getBottom() <= editor.presetBox.getY() - 15, "UNDO / REDO are right of the nameplate and above the preset label");
        check (! undoR.intersects (editor.presetBox.getBounds()) && ! undoR.intersects (editor.lookahead.getBounds()) && ! undoR.intersects (editor.guideButton.getBounds())
                   && ! abR.intersects (editor.presetBox.getBounds()) && ! abR.intersects (editor.lookahead.getBounds()) && ! abR.intersects (editor.guideButton.getBounds()),
               "UNDO / REDO and A / B overlap neither the preset box, LOOK-AHEAD nor GUIDE");

        // Each change made with its own step is taken back by one UNDO, and REDO brings it again.
        q.undo.clearUndoHistory();
        ui::beginUndoStep (q.apvts, "Threshold");
        setValue (q.apvts, id::threshold, -30.0f);
        pumpUntil ([&] { return inTree (id::threshold, -30.0f); });
        ui::beginUndoStep (q.apvts, "Ratio");
        setValue (q.apvts, id::ratio, 6.0f);
        pumpUntil ([&] { return inTree (id::ratio, 6.0f); });
        check (editor.undoControls().undoSwitch().isEnabled() && ! editor.undoControls().redoSwitch().isEnabled(), "after two changes UNDO is lit and REDO is not");
        editor.undoControls().undo();
        check (std::fabs (readParams (q.apvts).ratio - 2.0f) < 1e-3f && std::fabs (readParams (q.apvts).thresholdDb + 30.0f) < 1e-3f, "UNDO takes back the last change only (RATIO back to 2:1)");
        editor.undoControls().undo();
        check (std::fabs (readParams (q.apvts).thresholdDb + 18.0f) < 1e-3f, "a second UNDO takes back the threshold too (-18 dB)");
        pumpUntil ([&] { return editor.undoControls().redoSwitch().isEnabled(); });
        check (editor.undoControls().redoSwitch().isEnabled(), "REDO is lit after an UNDO");
        editor.undoControls().redo();
        check (std::fabs (readParams (q.apvts).thresholdDb + 30.0f) < 1e-3f, "REDO brings the threshold back (-30 dB)");

        // Cmd+Z and Shift+Cmd+Z reach the same buttons from the keyboard: the editor hands them to UndoButtons::handleKey.
        check (editor.undoControls().handleKey (juce::KeyPress ('z', juce::ModifierKeys::commandModifier, 0)) && std::fabs (readParams (q.apvts).thresholdDb + 18.0f) < 1e-3f,
               "Cmd+Z takes back the threshold (-18 dB)");
        check (editor.undoControls().handleKey (juce::KeyPress ('z', juce::ModifierKeys (juce::ModifierKeys::commandModifier | juce::ModifierKeys::shiftModifier), 0))
                   && std::fabs (readParams (q.apvts).thresholdDb + 30.0f) < 1e-3f,
               "Shift+Cmd+Z brings it back (-30 dB)");

        // A preset is one step: one UNDO puts back everything it moved.
        ui::beginUndoStep (q.apvts, "Threshold");
        setValue (q.apvts, id::threshold, -26.0f);
        pumpUntil ([&] { return inTree (id::threshold, -26.0f); });
        int catcher = 0;
        for (int i = 0; i < q.getNumPrograms(); ++i)
            if (q.getProgramName (i) == "Peak Catcher") catcher = i;
        q.setCurrentProgram (catcher); // sets the look-ahead, the ratio and more
        pumpUntil ([&] { return inTree (id::lookahead, 2.0f); });
        check (readParams (q.apvts).lookahead == 2, "the preset moved the look-ahead");
        editor.undoControls().undo();
        check (readParams (q.apvts).lookahead == 0 && std::fabs (readParams (q.apvts).thresholdDb + 26.0f) < 1e-3f, "one UNDO takes back the whole preset, and the change made before it stays");

        // A / B: each slot keeps its settings, and switching restores both.
        q.undo.clearUndoHistory();
        setValue (q.apvts, id::threshold, -30.0f);
        setValue (q.apvts, id::mix, 40.0f);
        pumpUntil ([&] { return inTree (id::threshold, -30.0f) && inTree (id::mix, 40.0f); });
        editor.abControls().button (1).onClick(); // B starts as a copy of A
        check (q.ab.active() == 1 && editor.abControls().button (1).getToggleState() && ! editor.abControls().button (0).getToggleState(), "B is chosen and lit, A is not");
        setValue (q.apvts, id::threshold, -10.0f);
        setValue (q.apvts, id::mix, 80.0f);
        pumpUntil ([&] { return inTree (id::threshold, -10.0f) && inTree (id::mix, 80.0f); });
        editor.abControls().button (0).onClick();
        pumpUntil ([&] { return inTree (id::threshold, -30.0f) && inTree (id::mix, 40.0f); });
        check (std::fabs (readParams (q.apvts).thresholdDb + 30.0f) < 0.05f && std::fabs (readParams (q.apvts).mix - 0.4f) < 0.01f, "switching to A brings back A's settings (-30 dB, MIX 40%)");
        editor.abControls().button (1).onClick();
        pumpUntil ([&] { return inTree (id::threshold, -10.0f) && inTree (id::mix, 80.0f); }); // also lets the switch be recorded as its own step
        check (std::fabs (readParams (q.apvts).thresholdDb + 10.0f) < 0.05f && std::fabs (readParams (q.apvts).mix - 0.8f) < 0.01f, "switching back to B brings back B's settings (-10 dB, MIX 80%)");
        editor.undoControls().undo();
        check (std::fabs (readParams (q.apvts).thresholdDb + 30.0f) < 0.05f && std::fabs (readParams (q.apvts).mix - 0.4f) < 0.01f, "UNDO takes back the A / B switch as one step (A again)");

        // A session saved with A / B keeps the active slot; one from before A / B opens on slot A.
        editor.abControls().button (0).onClick();
        juce::MemoryBlock saved;
        q.getStateInformation (saved);
        HeraProcessor reopened;
        reopened.setStateInformation (saved.getData(), (int) saved.getSize());
        check (reopened.ab.active() == 0 && ! reopened.ab.hasSettings (1), "the session keeps which slot is active (A here), not the slots themselves");
        editor.abControls().button (1).onClick();
        q.getStateInformation (saved);
        HeraProcessor onB;
        onB.setStateInformation (saved.getData(), (int) saved.getSize());
        check (onB.ab.active() == 1, "a session saved on B opens on B");

        // A session from before A / B has no abSlot: take the B session saved above, remove the property from its XML, and open it through the processor.
        if (auto xml = juce::AudioProcessor::getXmlFromBinary (saved.getData(), (int) saved.getSize()))
        {
            check (xml->getIntAttribute ("abSlot", -1) == 1, "the B session carries abSlot 1 before it is taken out");
            xml->removeAttribute ("abSlot");
            juce::MemoryBlock legacy;
            juce::AudioProcessor::copyXmlToBinary (*xml, legacy);
            HeraProcessor before;
            before.setStateInformation (legacy.getData(), (int) legacy.getSize());
            check (before.ab.active() == 0, "a session from before A / B (no abSlot) opens on slot A");
        }
        else
            check (false, "the saved session is readable as XML");
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
            check (q.getParameters().size() == 17 && ! (bool) q.apvts.state.getProperty ("delta", false), "neither is a parameter: they are never saved or automated");
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

    // AUTO SETUP: the button sits in the header inside the editor; a press sets RATIO, ATTACK, RELEASE and THRESH from the sound, as one undo step, and nothing else moves.
    {
        HeraProcessor q;
        q.setPlayConfigDetails (2, 2, 48000.0, 512);
        q.prepareToPlay (48000.0, 512);
        std::unique_ptr<juce::AudioProcessorEditor> holder (q.createEditor());
        auto& editor = ui::innerOf<HeraEditor> (*holder);

        const auto autoR = editor.autoThresh().getBounds();
        check (editor.autoThresh().isVisible() && editor.getLocalBounds().contains (autoR), "AUTO SETUP is on screen, inside the editor");
        check (! autoR.intersects (editor.presetBox.getBounds()) && ! autoR.intersects (editor.lookahead.getBounds()) && ! autoR.intersects (editor.guideButton.getBounds())
                   && ! autoR.intersects (editor.undoControls().getBounds()) && ! autoR.intersects (editor.abControls().getBounds()),
               "AUTO SETUP overlaps neither the preset box, LOOK-AHEAD, GUIDE, UNDO / REDO nor A / B");

        // Feeds `blocks` blocks of the sample `at (index)` to the processor, as a host would (512 samples a block).
        const auto feed = [&q] (int blocks, auto&& at)
        {
            juce::AudioBuffer<float> b (2, 512);
            juce::MidiBuffer midi;
            for (int blk = 0; blk < blocks; ++blk)
            {
                for (int i = 0; i < 512; ++i)
                    for (int c = 0; c < 2; ++c)
                        b.setSample (c, i, at (blk * 512 + i));
                q.processBlock (b, midi);
            }
        };
        // Runs the message loop until the condition holds (the editor's timer takes the answer), or two seconds pass.
        const auto pumpUntil = [] (auto&& done)
        {
            for (int waited = 0; ! done() && waited < 2000; waited += 10)
                juce::MessageManager::getInstance()->runDispatchLoopUntil (10);
            return done();
        };
        const auto thresholdInTree = [&q] (float plain)
        {
            for (int i = 0; i < q.apvts.state.getNumChildren(); ++i)
            {
                const auto child = q.apvts.state.getChild (i);
                if (child.getProperty ("id").toString() == id::threshold)
                    return std::fabs ((float) child.getProperty ("value") - plain) < 1e-3f;
            }
            return false;
        };
        const auto waiting = [&editor] { return editor.autoThresh().getButtonText() == "LISTENING"; };

        // Silence for four seconds: nothing audible, THRESH stays where it is, and the strip says so.
        setValue (q.apvts, id::threshold, -18.0f);
        editor.autoThresh().onClick();
        check (waiting(), "a press starts the listening: the button says LISTENING");
        feed (430, [] (int) { return 0.0f; }); // about 4.6 s
        pumpUntil ([&] { return ! waiting(); });
        check (editor.autoThresh().getButtonText() == "NO AUDIO" && std::fabs (readParams (q.apvts).thresholdDb + 18.0f) < 1e-3f,
               "four seconds of silence: THRESH stays at -18 dB and the button says NO AUDIO");
        check (ui::hasNote (editor.notes(), "AUTO SETUP heard no sound", ui::Severity::info), "... and the strip says that nothing was heard");
        check (std::fabs (readParams (q.apvts).ratio - 2.0f) < 1e-3f && std::fabs (readParams (q.apvts).attackMs - 10.0f) < 0.01f && std::fabs (readParams (q.apvts).releaseMs - 150.0f) < 0.1f,
               "... and RATIO, ATTACK and RELEASE stay at 2:1, 10 ms and 150 ms");

        // A programme of loud bursts over a quiet tone (0 dB for 100 ms of each second, -30 dB for the rest), with MIX, MAKEUP and the rest set.
        setValue (q.apvts, id::ratio, 4.0f);
        setValue (q.apvts, id::knee, 6.0f);
        setValue (q.apvts, id::mix, 50.0f);
        setValue (q.apvts, id::makeup, 3.0f);
        setValue (q.apvts, id::autoMakeup, 1.0f);
        std::vector<float> before;
        for (auto* p : q.getParameters())
            before.push_back (p->getValue());
        // The state follows the parameters through the message loop: let it take these changes first, so they are not in the press's step.
        for (int waited = 0; waited < 200; waited += 10)
            juce::MessageManager::getInstance()->runDispatchLoopUntil (10);
        q.undo.clearUndoHistory();

        editor.autoThresh().onClick();
        feed (430, [] (int i) {
            const double level = (i % 48000) < 4800 ? 0.0 : -30.0;
            return (float) (std::pow (10.0, level / 20.0) * std::sin (2.0 * 3.14159265358979 * 1000.0 * i / 48000.0));
        });
        pumpUntil ([&] { return ! waiting(); });
        pumpUntil ([&] { return thresholdInTree (readParams (q.apvts).thresholdDb); });
        const float set = readParams (q.apvts).thresholdDb;
        const auto now = readParams (q.apvts);
        check (editor.autoThresh().getButtonText() == "DONE" && set < -10.0f && set > -40.0f, fmt ("loud bursts: one press sets THRESH to %.1f dB", set));
        check (now.ratio > 2.6f && now.ratio < 3.0f && now.attackMs > 15.0f && now.attackMs < 17.5f && now.releaseMs > 205.0f && now.releaseMs < 230.0f,
               fmt ("... and the same press sets RATIO %.1f:1, ATTACK %.1f ms and RELEASE %.0f ms from how peaky the bursts are (about 13 dB)", (double) now.ratio, (double) now.attackMs, (double) now.releaseMs));
        check (ui::hasNote (editor.notes(), "AUTO SETUP set RATIO", ui::Severity::info), "the strip says what it set");

        const std::array<juce::AudioProcessorParameter*, 4> pressed { q.apvts.getParameter (id::threshold), q.apvts.getParameter (id::ratio),
                                                                      q.apvts.getParameter (id::attack), q.apvts.getParameter (id::release) };
        bool othersSame = true;
        std::size_t i = 0;
        for (auto* p : q.getParameters())
        {
            const bool setByPress = std::find (pressed.begin(), pressed.end(), p) != pressed.end();
            if (! setByPress && p->getValue() != before[i])
                othersSame = false;
            ++i;
        }
        check (othersSame && std::fabs (now.mix - 0.5f) < 1e-3f && std::fabs (now.makeupDb - 3.0f) < 1e-3f && std::fabs (now.kneeDb - 6.0f) < 1e-3f,
               "every other control is as it was: MIX stays at 50%, MAKEUP, AUTO MAKEUP and KNEE too");

        // One UNDO takes all four back, and nothing else was an undo step.
        q.undo.undo();
        pumpUntil ([&] { return thresholdInTree (-18.0f); });
        const auto back = readParams (q.apvts);
        check (std::fabs (back.thresholdDb + 18.0f) < 1e-3f && std::fabs (back.ratio - 4.0f) < 1e-3f && std::fabs (back.attackMs - 10.0f) < 0.01f
                   && std::fabs (back.releaseMs - 150.0f) < 0.1f && std::fabs (back.mix - 0.5f) < 1e-3f && ! q.undo.canUndo(),
               "one UNDO takes THRESH, RATIO, ATTACK and RELEASE back (-18 dB, 4:1, 10 ms, 150 ms) and leaves MIX at 50%: the press was one step");
    }

    return test::finish ("HeraUiCheck");
}
