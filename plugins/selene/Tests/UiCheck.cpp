// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 The Pantheon authors

// Builds the editor on the real processor and checks what a host and a user would meet: the parameters, the name, MIDI -> audio,
// the controls layout, presets, A/B compare, undo/redo, audition keyboard, state round-trip, and advice notes.

#include "Advice.h"
#include "Check.h"
#include "PluginEditor.h"
#include <cmath>
#include <cstring>
#include <pantheon/ui/Scaled.h>
#include <vector>

using namespace selene;
using test::check;
using test::fmt;

int main()
{
    juce::ScopedJuceInitialiser_GUI gui;
    std::printf ("Selene Poly UI check\n");

    SeleneProcessor proc;
    proc.setPlayConfigDetails (0, 2, 48000.0, 512);
    proc.prepareToPlay (48000.0, 512);

    check (proc.getName() == juce::String (gui::kProjectWord) + " " + gui::kPluginWord,
           "the reported name is Selene Poly");
    check (proc.acceptsMidi() && ! proc.producesMidi() && ! proc.isMidiEffect(),
           "processor accepts MIDI and is a stereo synth (not MIDI effect)");
    check (proc.getParameters().size() == 38,
           fmt ("%d parameters (expected 38)", proc.getParameters().size()));
    check (voiceModeNames.size() == kNumVoiceModes && oscWaveNames.size() == kNumOscWaves
               && filterModeNames.size() == kNumFilterModes && lfoWaveNames.size() == kNumLfoWaves
               && chorusModeNames.size() == kNumChorusModes,
           "choice lists match engine enums");

    // Audio output on MIDI NoteOn and silence after NoteOff
    {
        SeleneProcessor q;
        q.setPlayConfigDetails (0, 2, 48000.0, 512);
        q.prepareToPlay (48000.0, 512);
        setValue (q.apvts, id::ampAttack, 1.0f);
        setValue (q.apvts, id::ampRelease, 20.0f);
        setValue (q.apvts, id::echoMix, 0.0f);

        juce::AudioBuffer<float> buf (2, 512);
        juce::MidiBuffer midi;
        midi.addEvent (juce::MidiMessage::noteOn (1, 60, 0.9f), 0);

        float onPeak = 0.0f;
        for (int b = 0; b < 20; ++b)
        {
            q.processBlock (buf, midi);
            midi.clear();
            onPeak = std::max (onPeak, buf.getMagnitude (0, 512));
        }

        midi.addEvent (juce::MidiMessage::noteOff (1, 60), 0);
        for (int b = 0; b < 120; ++b)
        {
            q.processBlock (buf, midi);
            midi.clear();
        }
        const float tailPeak = buf.getMagnitude (0, 512);
        check (onPeak > 0.05f && tailPeak < 1e-4f && q.engine.activeVoices.load() == 0,
               fmt ("MIDI NoteOn produces audio (peak %.3f) and NoteOff decays to silence (tail %.2e)", onPeak, tailPeak));
    }

    // Editor aspect ratio, visibility, bounds containment, and non-overlap
    {
        std::unique_ptr<juce::AudioProcessorEditor> holder (proc.createEditor());
        auto& editor = ui::innerOf<SeleneEditor> (*holder);
        editor.setVisible (true);

        check (ui::hasEditorAspect (editor.getWidth(), editor.getHeight()) && editor.getWidth() == 1280 && editor.getHeight() == 800,
               fmt ("editor is 16:10 at %dx%d", editor.getWidth(), editor.getHeight()));

        const auto checkNonOverlapping = [&] (const char* groupName, std::initializer_list<juce::Component*> items)
        {
            std::vector<juce::Rectangle<int>> rects;
            bool ok = true;
            for (auto* c : items)
            {
                const auto r = c->getBounds();
                ok = ok && c->isVisible() && ! r.isEmpty() && editor.getLocalBounds().contains (r);
                for (const auto& prev : rects)
                    if (prev.intersects (r))
                        ok = false;
                rects.push_back (r);
            }
            check (ok, fmt ("%s: all controls visible, inside bounds, and non-overlapping", groupName));
        };

        checkNonOverlapping ("Header",
                             { &editor.undoButtons, &editor.abButtons, &editor.presetBox, &editor.prevPresetButton, &editor.nextPresetButton,
                               &editor.voiceMode, &editor.guideButton });
        checkNonOverlapping ("Row 1 (OSC 1, OSC 2, FILTER)",
                             { &editor.osc1Wave, &editor.osc1Pwm, &editor.subLevel,
                               &editor.osc2Wave, &editor.osc2Semi, &editor.osc2Detune, &editor.osc2Fm, &editor.oscMix, &editor.noiseLevel,
                               &editor.filterMode, &editor.cutoff, &editor.resonance, &editor.filterEnv, &editor.keytrack, &editor.drive });
        checkNonOverlapping ("Row 2 (AMP ENV, FILTER ENV, LFO)",
                             { &editor.ampA, &editor.ampD, &editor.ampS, &editor.ampR, &editor.velAmp,
                               &editor.fltA, &editor.fltD, &editor.fltS, &editor.fltR, &editor.velFlt,
                               &editor.lfoWave, &editor.lfoRate, &editor.lfoPitch, &editor.lfoFilter, &editor.lfoPwm });
        checkNonOverlapping ("Row 3 (ANALOG, CHORUS & ECHO, AUDITION)",
                             { &editor.glide, &editor.drift, &editor.masterGain,
                               &editor.chorusMode, &editor.echoTime, &editor.echoFeedback, &editor.echoMix,
                               &editor.auditionKeyboard });
    }

    // Presets: City Pop presets load and reset unspecified parameters
    {
        SeleneProcessor q;
        q.setPlayConfigDetails (0, 2, 48000.0, 512);
        q.prepareToPlay (48000.0, 512);
        std::unique_ptr<juce::AudioProcessorEditor> holder (q.createEditor());
        auto& editor = ui::innerOf<SeleneEditor> (*holder);
        editor.setVisible (true);

        check (editor.presetBox.getNumItems() == (int) presets().size() && presets().size() == 24 && q.getNumPrograms() == 24,
               fmt ("presetBox lists all %d presets", q.getNumPrograms()));

        const auto findPreset = [&] (const juce::String& name)
        {
            for (int i = 0; i < q.getNumPrograms(); ++i)
                if (q.getProgramName (i) == name)
                    return i;
            return -1;
        };

        const int padIdx = findPreset ("Tokyo Night Pad");
        const int brassIdx = findPreset ("Sunset Brass");
        const int bassIdx = findPreset ("Slap Synth Bass");
        check (padIdx >= 0 && brassIdx >= 0 && bassIdx >= 0, "City Pop presets present");

        q.setCurrentProgram (padIdx);
        editor.tickDisplay();
        const auto pad = readParams (q.apvts);
        check (pad.osc1Wave == OscWave::pulse && pad.chorusMode == ChorusMode::both && editor.presetBox.getSelectedId() == padIdx + 1,
               "Tokyo Night Pad loads Pulse + Chorus I+II and syncs presetBox");

        q.setCurrentProgram (brassIdx);
        editor.tickDisplay();
        const auto brass = readParams (q.apvts);
        check (brass.osc1Wave == OscWave::saw && brass.chorusMode == ChorusMode::mode1 && std::fabs (brass.filterEnvAmt - 0.62f) < 0.01f,
               "Sunset Brass loads Saw + Mode I + 62% filter env");

        q.setCurrentProgram (bassIdx);
        editor.tickDisplay();
        const auto bass = readParams (q.apvts);
        check (bass.voiceMode == VoiceMode::mono && bass.chorusMode == ChorusMode::off && bass.subLevel > 0.65f,
               "Slap Synth Bass loads Mono + Chorus Off + high Sub");

        // Preset step buttons (< / >) advance and wrap cleanly
        q.setCurrentProgram (0);
        editor.tickDisplay();
        editor.prevPresetButton.triggerClick();
        juce::MessageManager::getInstance()->runDispatchLoopUntil (20);
        const int wrappedBack = q.getCurrentProgram();
        const int syncedBackId = editor.presetBox.getSelectedId();
        editor.nextPresetButton.triggerClick();
        juce::MessageManager::getInstance()->runDispatchLoopUntil (20);
        const int wrappedForward = q.getCurrentProgram();
        const int syncedFwdId = editor.presetBox.getSelectedId();
        editor.nextPresetButton.triggerClick();
        juce::MessageManager::getInstance()->runDispatchLoopUntil (20);
        const int steppedOne = q.getCurrentProgram();
        const int syncedOneId = editor.presetBox.getSelectedId();
        check (wrappedBack == 23 && syncedBackId == 24 && wrappedForward == 0 && syncedFwdId == 1 && steppedOne == 1 && syncedOneId == 2,
               fmt ("prev/next preset buttons wrap and step cleanly (0 -> %d -> %d -> %d)", wrappedBack, wrappedForward, steppedOne));
    }

    // A/B compare & Undo/Redo
    {
        SeleneProcessor q;
        q.setPlayConfigDetails (0, 2, 48000.0, 512);
        q.prepareToPlay (48000.0, 512);
        std::unique_ptr<juce::AudioProcessorEditor> holder (q.createEditor());
        auto& editor = ui::innerOf<SeleneEditor> (*holder);
        editor.setVisible (true);

        const auto pumpUntil = [] (auto&& done)
        {
            for (int waited = 0; ! done() && waited < 2000; waited += 10)
                juce::MessageManager::getInstance()->runDispatchLoopUntil (10);
            return done();
        };
        const auto inTree = [&q] (const juce::String& pid, float plain)
        {
            for (int i = 0; i < q.apvts.state.getNumChildren(); ++i)
            {
                const auto child = q.apvts.state.getChild (i);
                if (child.getProperty ("id").toString() == pid)
                    return std::fabs ((float) child.getProperty ("value") - plain) < 1e-2f;
            }
            return false;
        };

        // A/B switching
        setValue (q.apvts, id::cutoff, 1200.0f);
        editor.abControls().button (1).triggerClick();
        pumpUntil ([&] { return q.ab.active() == 1; });
        setValue (q.apvts, id::cutoff, 8400.0f);
        editor.abControls().button (0).triggerClick();
        pumpUntil ([&] { return q.ab.active() == 0; });
        const float cutoffA = readParams (q.apvts).cutoffHz;
        editor.abControls().button (1).triggerClick();
        pumpUntil ([&] { return q.ab.active() == 1; });
        const float cutoffB = readParams (q.apvts).cutoffHz;
        check (std::fabs (cutoffA - 1200.0f) < 5.0f && std::fabs (cutoffB - 8400.0f) < 10.0f,
               fmt ("A/B compare keeps independent settings (A=%.0f Hz, B=%.0f Hz)", cutoffA, cutoffB));

        // Undo / Redo
        const float beforeCutoff = readParams (q.apvts).cutoffHz;
        pumpUntil ([&] { return inTree (id::cutoff, beforeCutoff); });
        q.undo.clearUndoHistory();
        ui::beginUndoStep (q.apvts, "Cutoff");
        setValue (q.apvts, id::cutoff, 420.0f);
        pumpUntil ([&] { return inTree (id::cutoff, 420.0f); });
        check (editor.undoControls().undoSwitch().isEnabled(), "UNDO enabled after parameter edit");
        editor.undoControls().undo();
        pumpUntil ([&] { return inTree (id::cutoff, beforeCutoff); });
        check (std::fabs (readParams (q.apvts).cutoffHz - beforeCutoff) < 10.0f, "UNDO restores cutoff");
        pumpUntil ([&] { return editor.undoControls().redoSwitch().isEnabled(); });
        editor.undoControls().redo();
        pumpUntil ([&] { return inTree (id::cutoff, 420.0f); });
        check (std::fabs (readParams (q.apvts).cutoffHz - 420.0f) < 5.0f, "REDO restores edited cutoff");
    }

    // Audition keyboard triggers audio via keyboardState
    {
        SeleneProcessor q;
        q.setPlayConfigDetails (0, 2, 48000.0, 512);
        q.prepareToPlay (48000.0, 512);
        q.keyboardState.noteOn (1, 60, 0.85f);
        juce::AudioBuffer<float> buf (2, 512);
        juce::MidiBuffer midi;
        float peak = 0.0f;
        for (int b = 0; b < 8; ++b)
        {
            q.processBlock (buf, midi);
            midi.clear();
            peak = std::max (peak, buf.getMagnitude (0, 512));
        }
        check (peak > 0.02f && q.engine.activeVoices.load() >= 1,
               fmt ("audition keyboardState triggers sound (peak %.3f, voices=%d)", peak, q.engine.activeVoices.load()));
    }

    // Advice notes length
    {
        AdviceInput in;
        in.params.masterGainDb = 8.0f;
        in.params.resonance = 0.95f;
        in.params.cutoffHz = 80.0f;
        in.params.filterEnvAmt = 0.0f;
        in.params.keytrack = 0.0f;
        in.params.voiceMode = VoiceMode::unison;
        in.params.chorusMode = ChorusMode::both;
        const auto list = adviceFor (in);
        bool fits = ! list.empty();
        for (const auto& n : list)
            fits = fits && (int) n.text.size() <= ui::kMaxNoteChars;
        check (fits, fmt ("adviceFor produces %d notes, all <= %d chars", (int) list.size(), ui::kMaxNoteChars));
    }

    // State save/restore round-trip
    {
        SeleneProcessor a;
        a.setCurrentProgram (2);
        setValue (a.apvts, id::cutoff, 2750.0f);
        setValue (a.apvts, id::echoMix, 0.44f);
        a.ab.select (1);

        juce::MemoryBlock saved;
        a.getStateInformation (saved);

        SeleneProcessor b;
        b.setStateInformation (saved.getData(), (int) saved.getSize());
        const auto bp = readParams (b.apvts);
        check (b.getCurrentProgram() == 2 && b.ab.active() == 1
                   && std::fabs (bp.cutoffHz - 2750.0f) < 5.0f && std::fabs (bp.echoMix - 0.44f) < 0.01f,
               "state save/restore preserves preset index, A/B slot, and parameters");
    }

    return test::finish ("SeleneUiCheck");
}
