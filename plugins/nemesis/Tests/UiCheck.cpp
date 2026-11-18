// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 The Pantheon authors

// Builds the editor on the real processor and checks what a host and a user would meet: the parameters, the name, the controls, the audio path,
// the state, and the notes.

#include "Advice.h"
#include "Check.h"
#include <cmath>
#include <functional>
#include "PluginEditor.h"
#include <pantheon/ui/Scaled.h>

using namespace nemesis;
using test::check;
using test::fmt;

int main()
{
    juce::ScopedJuceInitialiser_GUI gui;
    std::printf ("Nemesis UI check\n");
    NemesisProcessor proc;
    proc.setPlayConfigDetails (2, 2, 48000.0, 512);
    proc.prepareToPlay (48000.0, 512);

    check (proc.getName() == juce::String (gui::kProjectWord) + " " + gui::kPluginWord, "the reported name is the nameplate's two words");
    check (proc.getParameters().size() == 10, fmt ("%d parameters: frequency, width, threshold, range, release, mode, detection, listen, look-ahead and delta", proc.getParameters().size()));
    check (lookaheadNames.size() == kNumLookaheads, "the look-ahead list matches the engine's");
    check (modeNames.size() == kNumModes && detectionNames.size() == kNumDetections, "the choice lists match the engine's");
    const auto d = readParams (proc.apvts);
    check (std::fabs (d.freqHz - 6500.0f) < 5.0f && std::fabs (d.q - 1.5f) < 0.01f && std::fabs (d.thresholdDb + 30.0f) < 0.01f && std::fabs (d.rangeDb - 8.0f) < 0.01f && std::fabs (d.releaseMs - 40.0f) < 0.01f &&
               d.mode == Mode::splitBand && d.detection == Detection::absolute && ! d.listen && ! d.delta && d.lookahead == 0 && proc.getLatencySamples() == 0,
           "a new instance: 6.5 kHz, Q 1.5, -30 dB, 8 dB range, 40 ms, split-band, absolute, no look-ahead, no latency");

    // The audio path through the real processor: a loud tone in the band is turned down, a quiet one is not.
    {
        const auto gainOf = [] (float levelDb)
        {
            NemesisProcessor q;
            q.setPlayConfigDetails (2, 2, 48000.0, 512);
            q.prepareToPlay (48000.0, 512);
            setValue (q.apvts, id::range, 24.0f);
            juce::AudioBuffer<float> buffer (2, 512);
            juce::MidiBuffer midi;
            const float a = std::pow (10.0f, levelDb / 20.0f);
            float peak = 0.0f;
            for (int block = 0; block < 200; ++block)
            {
                for (int i = 0; i < 512; ++i)
                    for (int c = 0; c < 2; ++c)
                        buffer.setSample (c, i, a * std::sin (2.0f * 3.14159265f * 6500.0f * (float) (block * 512 + i) / 48000.0f));
                q.processBlock (buffer, midi);
                if (block > 100)
                    peak = std::max (peak, buffer.getMagnitude (0, 512));
            }
            return 20.0f * std::log10 (peak / a);
        };
        const float loud = gainOf (-20.0f), quiet = gainOf (-50.0f);
        check (std::fabs (loud + 10.0f) < 0.5f && std::fabs (quiet) < 0.01f, fmt ("through the processor: a sibilant at -20 dBFS is turned down by %.1f dB (10 by the law), one at -50 dBFS by %.3f dB", -loud, -quiet));
    }

    {
        std::unique_ptr<juce::AudioProcessorEditor> holder (proc.createEditor());
        auto& editor = ui::innerOf<NemesisEditor> (*holder);
        check (ui::hasEditorAspect (editor.getWidth(), editor.getHeight()), "the editor is drawn at 16:10, the shape of every plugin");
        editor.setVisible (true);
        check (editor.mode.getNumItems() == kNumModes && editor.detection.getNumItems() == kNumDetections, "the selectors have the parameters' items");
        check (editor.getWidth() > 700 && editor.release.getRight() < editor.mode.getX() && editor.listen.getBottom() < editor.getHeight(), "the controls fit the window");
        editor.listen.triggerClick();
        juce::MessageManager::getInstance()->runDispatchLoopUntil (100);
        check (readParams (proc.apvts).listen && ui::hasNote (editor.notes(), "LISTEN is on", ui::Severity::warning), "LISTEN sets the parameter, and a note says you hear only the band");
        editor.listen.triggerClick();
    }

    // The look-ahead is a delay the host is told about.
    {
        NemesisProcessor q;
        q.setPlayConfigDetails (2, 2, 48000.0, 512);
        q.prepareToPlay (48000.0, 512);
        setValue (q.apvts, id::lookahead, 4.0f);
        juce::AudioBuffer<float> buffer (2, 512);
        buffer.clear();
        juce::MidiBuffer midi;
        q.processBlock (buffer, midi);
        juce::MessageManager::getInstance()->runDispatchLoopUntil (100);
        check (q.getLatencySamples() == 240, fmt ("LOOK-AHEAD 5 ms: the host is told %d samples (240)", q.getLatencySamples()));
        NemesisProcessor r;
        setValue (r.apvts, id::lookahead, 2.0f);
        r.setPlayConfigDetails (2, 2, 48000.0, 512);
        r.prepareToPlay (48000.0, 512);
        check (r.getLatencySamples() == 48, "a session opened with LOOK-AHEAD 1 ms tells the host 48 samples at once");
    }

    // Bypassed by the host: the sound comes out as late as the look-ahead the plugin reports.
    {
        NemesisProcessor q;
        q.setPlayConfigDetails (2, 2, 48000.0, 512);
        setValue (q.apvts, id::lookahead, 3.0f);
        q.prepareToPlay (48000.0, 512);
        const int latency = q.getLatencySamples();
        juce::AudioBuffer<float> buffer (2, 512);
        juce::MidiBuffer midi;
        std::vector<float> heard;
        for (int block = 0; block < 3; ++block)
        {
            buffer.clear();
            if (block == 0)
                for (int c = 0; c < 2; ++c)
                    buffer.setSample (c, 7, 0.5f);
            q.processBlockBypassed (buffer, midi);
            for (int i = 0; i < 512; ++i)
                heard.push_back (buffer.getSample (0, i));
        }
        int at = -1;
        for (size_t i = 0; i < heard.size(); ++i)
            if (heard[i] != 0.0f)
                at = (int) i;
        check (latency == 96 && at == 7 + latency && heard[(size_t) at] == 0.5f, fmt ("a bypassed block comes out delayed by the reported latency: an impulse at 7 is heard at %d (latency %d)", at, latency));
    }

    // DELTA through the processor, and FIND in the editor: it sets FREQ from the sound that plays.
    {
        NemesisProcessor q;
        q.setPlayConfigDetails (2, 2, 48000.0, 512);
        q.prepareToPlay (48000.0, 512);
        std::unique_ptr<juce::AudioProcessorEditor> holder (q.createEditor());
        auto& editor = ui::innerOf<NemesisEditor> (*holder);
        editor.setVisible (true);
        check (editor.findButton.getRight() <= editor.getWidth() && ! editor.listen.getBounds().intersects (editor.delta.getBounds()) && ! editor.delta.getBounds().intersects (editor.findButton.getBounds()) && editor.findButton.getBottom() <= editor.getHeight(),
               "LISTEN, DELTA and FIND fit side by side in the window");
        editor.tickDisplay();
        check (! editor.findButton.isEnabled(), "FIND is off while LISTEN is off");
        setValue (q.apvts, id::listen, 1.0f);
        editor.tickDisplay();
        check (editor.findButton.isEnabled(), "FIND is on while LISTEN is on");
        editor.findButton.triggerClick();
        juce::MessageManager::getInstance()->runDispatchLoopUntil (100);
        check (editor.findPending() && editor.findButton.getButtonText() == "FINDING", "pressing FIND starts listening");
        juce::AudioBuffer<float> buffer (2, 512);
        juce::MidiBuffer midi;
        for (int block = 0; block < 200; ++block)
        {
            for (int i = 0; i < 512; ++i)
                for (int c = 0; c < 2; ++c)
                    buffer.setSample (c, i, 0.1f * std::sin (2.0f * 3.14159265f * 8200.0f * (float) (block * 512 + i) / 48000.0f));
            q.processBlock (buffer, midi);
        }
        editor.tickDisplay();
        check (! editor.findPending() && std::fabs (readParams (q.apvts).freqHz - 8200.0f) < 330.0f, fmt ("FIND set FREQ to %.0f Hz for an 8.2 kHz sound", readParams (q.apvts).freqHz));
        setValue (q.apvts, id::delta, 1.0f);
        check (ui::hasNote (editor.notes(), "DELTA is on", ui::Severity::warning) && ! ui::hasNote (editor.notes(), "LISTEN is on", ui::Severity::warning), "DELTA on is flagged (and replaces LISTEN's note)");
    }

    // SET THRESH: sets THRESH from the sound that plays, and nothing else; one undo step; silence and a sound with no s leave THRESH as it was, and the strip says so.
    {
        NemesisProcessor q;
        q.setPlayConfigDetails (2, 2, 48000.0, 512);
        q.prepareToPlay (48000.0, 512);
        std::unique_ptr<juce::AudioProcessorEditor> holder (q.createEditor());
        auto& editor = ui::innerOf<NemesisEditor> (*holder);
        editor.setVisible (true);
        const auto& b = editor.setThreshButton.getBounds();
        check (editor.getLocalBounds().contains (b) && ! b.intersects (editor.guideButton.getBounds()) && ! b.intersects (editor.presetBox.getBounds()) && ! b.intersects (editor.lookahead.getBounds()) && b.getX() > editor.lookahead.getRight(),
               "SET THRESH is in the header, inside the window, beside the look-ahead and clear of GUIDE");

        // Plays `seconds` of a signal into the processor, presses SET THRESH first and lets the editor read the answer.
        const auto pressOn = [] (NemesisProcessor& p, NemesisEditor& ed, const std::function<float (int)>& sample, double seconds)
        {
            ed.setThreshButton.triggerClick(); // asynchronous: the press reaches the engine on the message loop, before the audio
            juce::MessageManager::getInstance()->runDispatchLoopUntil (10);
            juce::AudioBuffer<float> buffer (2, 512);
            juce::MidiBuffer midi;
            const int blocks = (int) std::ceil (seconds * 48000.0 / 512.0);
            for (int block = 0; block < blocks; ++block)
            {
                for (int i = 0; i < 512; ++i)
                    for (int c = 0; c < 2; ++c)
                        buffer.setSample (c, i, sample (block * 512 + i));
                p.processBlock (buffer, midi);
            }
            juce::MessageManager::getInstance()->runDispatchLoopUntil (100);
            ed.tickDisplay();
        };
        // A speech-like sound: a vowel (300 Hz, -12 dBFS) all the time, every 0.3 s a 120 ms s at 6.5 kHz: every fourth one -12 dBFS, the others -24 and -20.
        const auto speech = [] (int i)
        {
            float v = 0.251f * std::sin (2.0f * 3.14159265f * 300.0f * (float) i / 48000.0f);
            if (i >= 4800 && (i - 4800) % 14400 < 5760 && (i - 4800) / 14400 <= 9)
            {
                const float dbs[3] { -12.0f, -24.0f, -20.0f };
                const int k = (i - 4800) / 14400;
                v += std::pow (10.0f, dbs[k % 3] / 20.0f) * std::sin (2.0f * 3.14159265f * 6500.0f * (float) i / 48000.0f);
            }
            return v;
        };
        const auto before = readParams (q.apvts);
        pressOn (q, editor, speech, 3.0);
        const auto after = readParams (q.apvts);
        const bool onlyThresh = std::fabs (after.freqHz - before.freqHz) < 1e-3f && std::fabs (after.q - before.q) < 1e-6f && std::fabs (after.rangeDb - before.rangeDb) < 1e-6f &&
                                std::fabs (after.releaseMs - before.releaseMs) < 1e-6f && after.mode == before.mode && after.detection == before.detection && after.listen == before.listen &&
                                after.delta == before.delta && after.lookahead == before.lookahead;
        check (std::fabs (after.thresholdDb - before.thresholdDb) > 5.0f && after.thresholdDb >= -60.0f && after.thresholdDb <= 0.0f && onlyThresh,
               fmt ("SET THRESH on a speech-like sound: THRESH %.1f -> %.1f dB, and no other knob or switch moved", before.thresholdDb, after.thresholdDb));
        check (ui::hasNote (editor.notes(), "SET THRESH: ", ui::Severity::info) && editor.setNote().find ("dBFS") != std::string::npos, "the strip says what THRESH was set to");
        const float set = after.thresholdDb;
        editor.undoControls().undo();
        juce::MessageManager::getInstance()->runDispatchLoopUntil (100);
        check (std::fabs (readParams (q.apvts).thresholdDb - before.thresholdDb) < 0.01f && ! q.undo.canUndo(), "UNDO (the editor's button) takes SET THRESH back in one step");
        editor.undoControls().redo();
        juce::MessageManager::getInstance()->runDispatchLoopUntil (100);
        check (std::fabs (readParams (q.apvts).thresholdDb - set) < 0.01f, "REDO brings it back");

        // A vowel alone: no s stands out, so THRESH stays and the strip says why.
        const auto vowel = [] (int i) { return 0.251f * std::sin (2.0f * 3.14159265f * 300.0f * (float) i / 48000.0f); };
        const float keep = readParams (q.apvts).thresholdDb;
        pressOn (q, editor, vowel, 3.0);
        check (std::fabs (readParams (q.apvts).thresholdDb - keep) < 1e-4f && ui::hasNote (editor.notes(), "no s stands out", ui::Severity::info),
               "a vowel alone (no s): THRESH is unchanged, and the strip says no s stands out");
        pressOn (q, editor, [] (int) { return 0.0f; }, 2.0);
        check (std::fabs (readParams (q.apvts).thresholdDb - keep) < 1e-4f && ui::hasNote (editor.notes(), "was silent", ui::Severity::info),
               "silence: THRESH is unchanged, and the strip says the sound was silent");
        bool fits = true;
        for (const auto& n : editor.notes())
            fits = fits && (int) n.text.size() <= ui::kMaxNoteChars;
        check (fits, "every SET THRESH note fits a line of the strip");
    }

    // UNDO / REDO: on screen in the header, clear of the controls; a knob move is taken back by UNDO and brought again by REDO.
    {
        NemesisProcessor q;
        q.setPlayConfigDetails (2, 2, 48000.0, 512);
        q.prepareToPlay (48000.0, 512);
        std::unique_ptr<juce::AudioProcessorEditor> holder (q.createEditor());
        auto& editor = ui::innerOf<NemesisEditor> (*holder);
        editor.setVisible (true);
        const auto undoR = editor.undoControls().getBounds();
        check (editor.undoControls().isVisible() && editor.undoControls().undoSwitch().isVisible() && editor.undoControls().redoSwitch().isVisible() && editor.getLocalBounds().contains (undoR),
               "UNDO / REDO are on screen, inside the window");
        check (! undoR.intersects (editor.setThreshButton.getBounds()) && ! undoR.intersects (editor.guideButton.getBounds()) && ! undoR.intersects (editor.presetBox.getBounds())
                   && ! undoR.intersects (editor.lookahead.getBounds()) && ! undoR.intersects (editor.listen.getBounds()),
               "UNDO / REDO overlap neither SET THRESH, GUIDE, the preset box, LOOK-AHEAD nor LISTEN");

        // The APVTS writes a change into its state on the message loop, so each step waits until the state holds it before the next one begins.
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

        q.undo.clearUndoHistory();
        ui::beginUndoStep (q.apvts, "Range");
        setValue (q.apvts, id::range, 20.0f);
        pumpUntil ([&] { return inTree (id::range, 20.0f); });
        check (editor.undoControls().undoSwitch().isEnabled() && ! editor.undoControls().redoSwitch().isEnabled(), "after a knob move UNDO is lit and REDO is not");
        editor.undoControls().undo();
        pumpUntil ([&] { return inTree (id::range, 8.0f); });
        check (std::fabs (readParams (q.apvts).rangeDb - 8.0f) < 1e-3f, "UNDO takes the RANGE move back to 8 dB");
        pumpUntil ([&] { return editor.undoControls().redoSwitch().isEnabled(); }); // the buttons follow the manager on the message loop, as in Hera
        check (editor.undoControls().redoSwitch().isEnabled() && ! editor.undoControls().undoSwitch().isEnabled(), "after that UNDO is off and REDO is lit");
        editor.undoControls().redo();
        pumpUntil ([&] { return inTree (id::range, 20.0f); });
        check (std::fabs (readParams (q.apvts).rangeDb - 20.0f) < 1e-3f, "REDO brings the RANGE move back to 20 dB");

        // SET THRESH is one undo step of its own: it is checked where it presses (above); here it is one step among the knob moves.
        q.undo.clearUndoHistory();
        ui::beginUndoStep (q.apvts, "Range");
        setValue (q.apvts, id::range, 12.0f);
        pumpUntil ([&] { return inTree (id::range, 12.0f); });
        ui::beginUndoStep (q.apvts, "SET THRESH");
        setValue (q.apvts, id::threshold, -20.0f);
        pumpUntil ([&] { return inTree (id::threshold, -20.0f); });
        editor.undoControls().undo();
        pumpUntil ([&] { return inTree (id::threshold, -30.0f); });
        check (std::fabs (readParams (q.apvts).thresholdDb + 30.0f) < 1e-3f && std::fabs (readParams (q.apvts).rangeDb - 12.0f) < 1e-3f,
               "one UNDO takes back a SET THRESH step and leaves the RANGE move before it");
    }

    // Presets: each applies, has a name and a one-line purpose, and the chosen one is kept with the session.
    {
        NemesisProcessor q;
        q.setPlayConfigDetails (2, 2, 48000.0, 512);
        q.prepareToPlay (48000.0, 512);
        bool all = q.getNumPrograms() == (int) presets().size() && q.getNumPrograms() >= 5;
        for (int i = 0; i < q.getNumPrograms(); ++i)
        {
            q.setCurrentProgram (i);
            const auto len = std::strlen (presets()[(size_t) i].purpose);
            all = all && q.getProgramName (i).isNotEmpty() && q.getCurrentProgram() == i && len > 20 && len <= 140;
        }
        check (all, fmt ("all %d presets apply, have a name and a one-line purpose", q.getNumPrograms()));
        int male = 0, find = 0, broadcast = 0;
        for (int i = 0; i < q.getNumPrograms(); ++i)
        {
            if (q.getProgramName (i) == "Male Vocal") male = i;
            if (q.getProgramName (i) == "Find the S") find = i;
            if (q.getProgramName (i) == "Broadcast Voice") broadcast = i;
        }
        q.setCurrentProgram (broadcast);
        check (readParams (q.apvts).mode == Mode::wideband && readParams (q.apvts).detection == Detection::relative, "Broadcast Voice is wideband with relative detection");
        q.setCurrentProgram (find);
        check (readParams (q.apvts).listen && readParams (q.apvts).q > 2.5f, "Find the S switches LISTEN on with a narrow band");
        q.setCurrentProgram (male);
        const auto m = readParams (q.apvts);
        check (! m.listen && m.mode == Mode::splitBand && m.lookahead == 2 && std::fabs (m.freqHz - 5500.0f) < 5.0f, "Male Vocal: 5.5 kHz, split-band, 1 ms of look-ahead, and LISTEN back off (a preset does not inherit the last one)");
        q.setCurrentProgram (0);
        const auto d0 = readParams (q.apvts);
        check (std::fabs (d0.freqHz - 6500.0f) < 5.0f && std::fabs (d0.thresholdDb + 30.0f) < 0.01f && d0.detection == Detection::absolute && d0.lookahead == 0, "Init is the defaults");
        q.setCurrentProgram (male);
        juce::MemoryBlock saved;
        q.getStateInformation (saved);
        NemesisProcessor other;
        other.setStateInformation (saved.getData(), (int) saved.getSize());
        check (other.getCurrentProgram() == male && readParams (other.apvts).detection == Detection::relative, "the chosen preset and its settings are saved with the session");
        std::unique_ptr<juce::AudioProcessorEditor> holder (q.createEditor());
        auto& editor = ui::innerOf<NemesisEditor> (*holder);
        editor.setVisible (true);
        check (editor.presetBox.getNumItems() == q.getNumPrograms() && editor.presetBox.getSelectedId() == male + 1, "the editor's preset box lists the presets and shows the current one");
        check (editor.lookahead.getRight() < editor.getWidth() - 70 && editor.lookahead.getNumItems() == kNumLookaheads, "the preset box and LOOK-AHEAD fit the header beside the GUIDE button");
    }

    // The state survives a round trip.
    {
        setValue (proc.apvts, id::freq, 8200.0f);
        setValue (proc.apvts, id::detection, 1.0f);
        juce::MemoryBlock saved;
        proc.getStateInformation (saved);
        NemesisProcessor other;
        other.setStateInformation (saved.getData(), (int) saved.getSize());
        check (std::fabs (readParams (other.apvts).freqHz - 8200.0f) < 10.0f && readParams (other.apvts).detection == Detection::relative, "the state of a session comes back as it was saved");
    }

    // The notes.
    {
        AdviceInput in;
        check (adviceFor (in).empty(), "a default instance has nothing to warn about");
        in.params.rangeDb = 0.0f;
        in.params.thresholdDb = 0.0f;
        in.heldDb = 12.0f;
        check (ui::hasNote (adviceFor (in), "RANGE is 0 dB", ui::Severity::warning) && ui::hasNote (adviceFor (in), "THRESH is at the top", ui::Severity::warning) && ui::hasNote (adviceFor (in), "makes a lisp", ui::Severity::warning),
               "RANGE 0, THRESH at the top and 12 dB of reduction are warned about");
        in.params.mode = Mode::wideband;
        in.params.detection = Detection::relative;
        in.params.freqHz = 3000.0f;
        check (ui::hasNote (adviceFor (in), "WIDEBAND turns the whole voice", ui::Severity::info) && ui::hasNote (adviceFor (in), "RELATIVE reads", ui::Severity::info) && ui::hasNote (adviceFor (in), "Under 4.5 kHz", ui::Severity::info),
               "wideband, relative detection and a low frequency are explained");
        bool tooLong = false;
        in.params.listen = true;
        for (const auto& n : adviceFor (in))
            if ((int) n.text.size() > ui::kMaxNoteChars)
            {
                tooLong = true;
                std::printf ("  too long (%d): %s\n", (int) n.text.size(), n.text.c_str());
            }
        check (! tooLong, "every note fits a line of the strip");
    }

    return test::finish ("NemesisUiCheck");
}
