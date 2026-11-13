// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 The Pantheon authors

// Builds the editor on the real processor and checks what a host and a user would meet: the parameters, the name, the controls, that DELTA is
// monitoring that ends with the window, the notes, and two processors finding each other through the shared bus.

#include "Advice.h"
#include "Check.h"
#include "PluginEditor.h"
#include <pantheon/ui/Scaled.h>
#include <cstring>

using namespace hestia;
using test::check;
using test::fmt;

namespace
{
float tailDb (const std::vector<float>& x)
{
    double sum = 0.0;
    for (float v : x)
        sum += (double) v * v;
    return 20.0f * std::log10 (std::fmax (std::sqrt (2.0 * sum / (double) x.size()), 1e-9));
}
}

int main()
{
    juce::ScopedJuceInitialiser_GUI gui;
    std::printf ("Hestia UI check\n");
    HestiaProcessor proc;
    proc.setPlayConfigDetails (2, 2, 48000.0, 512);
    proc.prepareToPlay (48000.0, 512);
    setValue (proc.apvts, id::mix, 0.0f); // this one is on its own; the pair below meet in mix H

    check (proc.getName() == juce::String (gui::kProjectWord) + " " + gui::kPluginWord, "the reported name is the nameplate's two words");
    check (proc.getParameters().size() == 11, fmt ("%d parameters: role, mix, importance, carve, reach, attack, release, output, depth, and the sidechain's role and importance", proc.getParameters().size()));
    check (roleNames().size() == kNumRoles && mixNames.size() == kMaxGroups + 1, "the choice lists match the engine's roles and mixes");
    const auto defaults = readParams (proc.apvts);
    check (defaults.role == Role::fullRange && std::fabs (defaults.carveDb - 6.0f) < 1e-3f && std::fabs (defaults.distance) < 1e-6f && defaults.scRole == Role::fullRange && std::fabs (defaults.importance - 0.5f) < 1e-6f && std::fabs (defaults.outDb) < 1e-3f, // (a stepped range lands within a rounding error of its default, and which way depends on the build)
           "a new instance is Full Range, importance 50%, carve 6 dB, at the front (depth 0%), output 0");
    {
        HestiaProcessor fresh;
        check (readParams (fresh.apvts).group == kDefaultMix && mixNames[kDefaultMix] == "A", "a new instance is in mix A, so the stems of a session find each other without being set up");
    }

    {
        std::unique_ptr<juce::AudioProcessorEditor> holder (proc.createEditor());
        auto& editor = ui::innerOf<HestiaEditor> (*holder);
        editor.setVisible (true);
        check (editor.roleBox.getNumItems() == kNumRoles && editor.mixBox.getNumItems() == kMaxGroups + 1, "the role and mix selectors have the parameters' items");
        editor.roleBox.setSelectedItemIndex ((int) Role::bass, juce::sendNotificationSync);
        editor.mixBox.setSelectedItemIndex (3, juce::sendNotificationSync);
        check (readParams (proc.apvts).role == Role::bass && readParams (proc.apvts).group == 3, "the selectors set the role and the mix");
        check (editor.getWidth() > 800 && editor.importance.getRight() < editor.getWidth() && editor.release.getBottom() < editor.getHeight(), "the knobs fit the window");

        editor.deltaButton.triggerClick();
        juce::MessageManager::getInstance()->runDispatchLoopUntil (100);
        check (proc.engine.deltaOn.load() && editor.deltaButton.getToggleState(), "DELTA listens to what Hestia changes and lights its button");
    }
    check (! proc.engine.deltaOn.load(), "closing the editor ends DELTA");

    // Two stems in one mix, through the processors a host would make.
    {
        HestiaProcessor bass, vocal;
        for (auto* p : { &bass, &vocal })
        {
            p->setPlayConfigDetails (2, 2, 48000.0, 512);
            p->prepareToPlay (48000.0, 512);
            setValue (p->apvts, id::mix, 8.0f); // H
        }
        setValue (bass.apvts, id::role, (float) (int) Role::bass);
        setValue (vocal.apvts, id::role, (float) (int) Role::leadVocal);
        check (bass.engine.slotIndex() >= 0 && vocal.engine.slotIndex() >= 0 && bass.engine.slotIndex() != vocal.engine.slotIndex(), "each processor has its own place on the shared bus");
        juce::AudioBuffer<float> b (2, 512), v (2, 512);
        juce::MidiBuffer midi;
        std::vector<float> in, out;
        const double tone = bandCentreHz (5);
        for (int block = 0; block < 190; ++block)
        {
            for (int i = 0; i < 512; ++i)
            {
                const float s = 0.25f * std::sin ((float) (2.0 * 3.14159265358979 * tone * (double) (block * 512 + i) / 48000.0));
                for (int c = 0; c < 2; ++c) { b.setSample (c, i, s); v.setSample (c, i, s); }
            }
            bass.processBlock (b, midi);
            vocal.processBlock (v, midi);
            if (block >= 170)
                for (int i = 0; i < 512; ++i) { in.push_back (0.25f * std::sin ((float) (2.0 * 3.14159265358979 * tone * (double) (block * 512 + i) / 48000.0))); out.push_back (v.getSample (0, i)); }
        }
        check (tailDb (out) < tailDb (in) - 4.5f && bass.engine.linkedStems() == 1,
               fmt ("a vocal and a bass in mix H on one tone: the vocal comes out %.1f dB under what went in", tailDb (in) - tailDb (out)));

        // A state round trip: role, mix and everything else come back in another instance.
        setValue (vocal.apvts, id::carve, 11.0f);
        setValue (vocal.apvts, id::distance, 40.0f);
        setValue (vocal.apvts, id::scRole, (float) (int) Role::kick);
        setValue (vocal.apvts, id::importance, 80.0f);
        juce::MemoryBlock state;
        vocal.getStateInformation (state);
        HestiaProcessor other;
        other.setStateInformation (state.getData(), (int) state.getSize());
        const auto r = readParams (other.apvts);
        check (r.role == Role::leadVocal && r.group == 8 && std::fabs (r.carveDb - 11.0f) < 0.05f && std::fabs (r.distance - 0.4f) < 0.01f && r.scRole == Role::kick && std::fabs (r.importance - 0.8f) < 0.01f,
               "a saved state comes back in another instance");
    }
    {
        // An instance that goes gives its place back.
        int slot = -1;
        {
            HestiaProcessor temp;
            slot = temp.engine.slotIndex();
        }
        HestiaProcessor next;
        check (slot >= 0 && next.engine.slotIndex() == slot, "the place of a closed instance is the next instance's");
    }

    // The sidechain bus and the depth, through the processor a host would make.
    {
        HestiaProcessor q;
        q.setPlayConfigDetails (2, 2, 48000.0, 512);
        auto layout = q.getBusesLayout();
        layout.inputBuses.getReference (1) = juce::AudioChannelSet::stereo();
        check (q.isBusesLayoutSupported (layout) && q.setBusesLayout (layout), "a stereo sidechain input is accepted");
        auto bad = q.getBusesLayout();
        bad.inputBuses.getReference (1) = juce::AudioChannelSet::create5point1();
        check (! q.isBusesLayoutSupported (bad), "... and a 5.1 one is not");
        q.prepareToPlay (48000.0, 512);
        setValue (q.apvts, id::mix, 5.0f); // E
        setValue (q.apvts, id::role, (float) (int) Role::leadVocal);
        setValue (q.apvts, id::scRole, (float) (int) Role::bass);
        juce::AudioBuffer<float> four (4, 512);
        juce::MidiBuffer midi;
        const double tone = bandCentreHz (5);
        const auto measure = [&] (float key)
        {
            q.reset();
            std::vector<float> in, out;
            for (int block = 0; block < 190; ++block)
            {
                for (int i = 0; i < 512; ++i)
                {
                    const float t = (float) (block * 512 + i) / 48000.0f;
                    const float voice = 0.25f * std::sin ((float) (2.0 * 3.14159265358979 * tone * t)), kick = key * std::sin ((float) (2.0 * 3.14159265358979 * tone * t));
                    four.setSample (0, i, voice); four.setSample (1, i, voice); four.setSample (2, i, kick); four.setSample (3, i, kick);
                }
                q.processBlock (four, midi);
                if (block >= 170)
                    for (int i = 0; i < 512; ++i) { in.push_back (0.25f * std::sin ((float) (2.0 * 3.14159265358979 * tone * (double) (block * 512 + i) / 48000.0))); out.push_back (four.getSample (0, i)); }
            }
            return tailDb (in) - tailDb (out);
        };
        const float without = measure (0.0f), with = measure (0.25f);
        check (q.sidechainConnected.load() && without < 0.2f && with > 4.5f,
               fmt ("a bass on the sidechain: the vocal is %.1f dB down with a silent key and %.1f dB down with the bass playing the same tone", without, with));

        // Depth: a stem at 100% is not the stem at 0%, and comes back to it.
        setValue (q.apvts, id::mix, 0.0f);
        const auto burst = [&]
        {
            std::vector<float> out;
            q.reset();
            for (int block = 0; block < 12; ++block)
            {
                for (int i = 0; i < 512; ++i)
                {
                    const float x = block == 0 && i == 10 ? 1.0f : 0.0f;
                    four.setSample (0, i, x); four.setSample (1, i, 0.0f); four.setSample (2, i, 0.0f); four.setSample (3, i, 0.0f);
                }
                q.processBlock (four, midi);
                for (int i = 0; i < 512; ++i) out.push_back (four.getSample (0, i));
            }
            return out;
        };
        for (int block = 0; block < 500; ++block) { four.clear(); q.processBlock (four, midi); } // the cut of the test before lets go (250 ms release, to under 0.0005 dB)
        const auto front = burst();
        setValue (q.apvts, id::distance, 100.0f);
        for (int block = 0; block < 100; ++block) { four.clear(); q.processBlock (four, midi); } // let it settle
        const auto back = burst();
        float reflections = 0.0f;
        for (size_t i = 12 + 200; i < 12 + 1600; ++i)
            reflections = std::fmax (reflections, std::fabs (back[i]));
        check (front[10] == 1.0f && back[10] < 0.8f && reflections > 0.02f,
               fmt ("depth 100%% through the processor: an impulse is %.2f at the front and %.2f at the back, with reflections of %.3f after it", front[10], back[10], reflections));
    }

    // The window: opens smaller than it is drawn, scales with the corner, remembers the scale with the session.
    {
        HestiaProcessor q;
        std::unique_ptr<juce::AudioProcessorEditor> first (q.createEditor());
        auto& drawn = ui::innerOf<HestiaEditor> (*first);
        check (first->isResizable() && std::abs (first->getWidth() - juce::roundToInt ((float) drawn.getWidth() * ui::kDefaultEditorScale)) <= 1
                   && first->getConstrainer() != nullptr,
               fmt ("the editor opens at %d x %d, %.0f%% of the %d x %d it is drawn at, and can be resized", first->getWidth(), first->getHeight(), 100.0f * ui::kDefaultEditorScale, drawn.getWidth(), drawn.getHeight()));
        check (std::abs (drawn.getTransform().mat00 - ui::kDefaultEditorScale) < 0.01f, "the editor inside is scaled by a transform: it is laid out at its own size");
        first->setSize (drawn.getWidth(), drawn.getHeight());
        check (std::abs (drawn.getTransform().mat00 - 1.0f) < 0.01f && drawn.getWidth() > 800, "dragged to full size the scale is 1, and the editor inside has not changed size");
        auto* limits = first->getConstrainer();
        check (limits->getMinimumWidth() == juce::roundToInt ((float) drawn.getWidth() * ui::kMinEditorScale) && limits->getMaximumWidth() == juce::roundToInt ((float) drawn.getWidth() * ui::kMaxEditorScale)
                   && std::abs (limits->getFixedAspectRatio() - (double) drawn.getWidth() / (double) drawn.getHeight()) < 1e-6,
               "the corner scales it between half and one and a half times, and keeps its proportions");
        first->setSize (juce::roundToInt ((float) drawn.getWidth() * 0.9f), juce::roundToInt ((float) drawn.getHeight() * 0.9f));
        juce::MemoryBlock state;
        q.getStateInformation (state);
        first.reset();
        HestiaProcessor other;
        other.setStateInformation (state.getData(), (int) state.getSize());
        std::unique_ptr<juce::AudioProcessorEditor> again (other.createEditor());
        check (std::abs ((float) again->getWidth() / (float) ui::innerOf<HestiaEditor> (*again).getWidth() - 0.9f) < 0.01f, "the scale is saved with the session: another instance opens at 90%");
    }

    // Mono host.
    {
        HestiaProcessor mono;
        mono.setPlayConfigDetails (1, 1, 48000.0, 512);
        mono.prepareToPlay (48000.0, 512);
        setValue (mono.apvts, id::mix, 0.0f);
        juce::AudioBuffer<float> buffer (1, 512);
        juce::MidiBuffer midi;
        for (int i = 0; i < 512; ++i) buffer.setSample (0, i, 0.1f * std::sin (0.05f * (float) i));
        const auto before = buffer.getSample (0, 300);
        mono.processBlock (buffer, midi);
        check (buffer.getSample (0, 300) == before, "a mono host works, and a stem with no one to see passes unchanged");
    }

    std::printf ("\nAdvice: the notes that keep it from being used for the wrong job\n");
    {
        using ui::Severity;
        const auto has = [] (const std::vector<ui::Note>& n, const char* word, Severity sev) { return ui::hasNote (n, word, sev); };

        AdviceInput fine;
        fine.params.role = Role::bass;
        fine.params.group = 1;
        fine.others = 2;
        check (adviceFor (fine).empty(), "a bass in a mix with two others, 6 dB depth: nothing to say");

        auto in = fine; in.delta = true;
        check (has (adviceFor (in), "DELTA is on", Severity::warning), "DELTA on: a warning");
        in = fine; in.params.group = 0;
        check (has (adviceFor (in), "MIX is Off", Severity::warning), "the mix Off: nothing is cut, and it says so");
        in = fine; in.others = 0;
        check (has (adviceFor (in), "No other stem", Severity::warning), "nobody else playing in the mix: a warning");
        in = fine; in.params.carveDb = 0.0f;
        check (has (adviceFor (in), "CARVE is 0 dB and DEPTH is 0", Severity::warning), "no carve and no depth: Hestia changes nothing, and says so");
        in.params.distance = 0.4f;
        check (adviceFor (in).empty(), "... but with a depth there is something to do, and nothing to say");
        in = fine; in.params.distance = 0.9f;
        check (has (adviceFor (in), "far back", Severity::info), "a depth of 90%: it is far back, and not for a lead");
        in = fine; in.sidechain = true; in.params.scRole = Role::fullRange;
        check (has (adviceFor (in), "set SC ROLE", Severity::info), "a sidechain with its role left on Full Range: set it");
        in.params.scRole = Role::kick;
        check (adviceFor (in).empty(), "... and not once it is a kick");
        in = fine; in.sameClaim = 1;
        check (has (adviceFor (in), "this role and about this IMPORTANCE", Severity::info), "another stem of the same role and importance: neither gives way");
        in = fine; in.params.role = Role::fullRange;
        check (has (adviceFor (in), "Full Range", Severity::info), "role Full Range: pick the real one");
        in = fine; in.params.carveDb = 12.0f; in.strongestCutDb = -11.0f;
        check (has (adviceFor (in), "hollow", Severity::warning) && has (adviceFor (in), "11 dB", Severity::warning), "a cut of 11 dB: the stem can sound hollow");
        in.params.carveDb = 6.0f; in.strongestCutDb = -6.0f;
        check (! has (adviceFor (in), "hollow", Severity::warning), "... but not at 6 dB");
        in = fine; in.params.releaseMs = 30.0f;
        check (has (adviceFor (in), "RELEASE under 60 ms", Severity::info), "a 30 ms release: the cuts can flutter");
        in = fine; in.params.importance = 0.0f;
        check (has (adviceFor (in), "IMPORTANCE is 0", Severity::info), "an importance of 0: said");

        size_t longest = 0;
        for (int mask = 0; mask < 4096; ++mask)
        {
            AdviceInput all;
            all.params.role = (mask & 1) ? Role::fullRange : Role::bass;
            all.params.group = (mask & 2) ? 0 : 1;
            all.params.carveDb = (mask & 4) ? 0.0f : ((mask & 8) ? 18.0f : 6.0f);
            all.params.distance = (mask & 1024) ? 0.9f : 0.0f;
            all.params.scRole = (mask & 2048) ? Role::fullRange : Role::kick;
            all.sidechain = mask & 2048;
            all.params.releaseMs = (mask & 16) ? 20.0f : 250.0f;
            all.params.importance = (mask & 32) ? 0.0f : 0.5f;
            all.others = (mask & 64) ? 0 : 3;
            all.sameClaim = (mask & 128) ? 1 : 0;
            all.strongestCutDb = (mask & 256) ? -18.0f : -1.0f;
            all.delta = mask & 512;
            for (auto& n : adviceFor (all))
                longest = std::max (longest, n.text.size());
        }
        check ((int) longest <= ui::kMaxNoteChars, fmt ("the longest note is %d characters (a line holds %d)", (int) longest, ui::kMaxNoteChars));

        HestiaProcessor q;
        q.setPlayConfigDetails (2, 2, 48000.0, 512);
        q.prepareToPlay (48000.0, 512);
        setValue (q.apvts, id::mix, 7.0f); // G, alone
        HestiaEditor editor (q);
        check (has (editor.notes(), "No other stem", Severity::warning), "the editor reads the real mix: a stem alone in G is told so");
        q.engine.deltaOn = true;
        check (has (editor.notes(), "DELTA", Severity::warning), "DELTA in the engine reaches the editor's notes");
        q.engine.deltaOn = false;
        check (editor.guide().isVisible() && std::strlen (guideText()) > 400 && std::string (guideText()).find ("NOT FOR") != std::string::npos,
               "the GUIDE button is there and its text says what the plugin is not for");
    }

    // Presets: each applies, has a name and a one-line purpose, the chosen one is kept with the session, and the box is in the header.
    {
        HestiaProcessor q;
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
        const auto indexOf = [&] (const char* name)
        {
            for (int i = 0; i < q.getNumPrograms(); ++i)
                if (q.getProgramName (i) == name)
                    return i;
            check (false, std::string ("a preset called ") + name);
            return 0;
        };
        const auto raw = [&] (const juce::String& pid) { return q.apvts.getRawParameterValue (pid)->load(); };
        setValue (q.apvts, id::mix, 3.0f);
        setValue (q.apvts, id::out, -2.0f);
        q.setCurrentProgram (indexOf ("Pads and Strings"));
        check ((int) raw (id::role) == 8 && std::fabs (raw (id::distance) - 55.0f) < 0.5f && std::fabs (raw (id::importance) - 25.0f) < 0.5f, "Pads and Strings: the pad role, importance 25%, DEPTH 55%");
        check ((int) raw (id::mix) == 3 && std::fabs (raw (id::out) + 2.0f) < 0.01f, "the mix letter and OUTPUT are kept by a preset");
        q.setCurrentProgram (indexOf ("Kick"));
        check ((int) raw (id::role) == 0 && std::fabs (raw (id::distance)) < 0.5f, "Kick: the kick role, and DEPTH back at the front (a preset does not inherit the last one)");
        q.setCurrentProgram (0);
        check ((int) raw (id::role) == 12 && std::fabs (raw (id::importance) - 50.0f) < 0.5f && std::fabs (raw (id::carve) - 6.0f) < 0.05f, "Init is the defaults (Full Range, 50%, 6 dB)");
        const int chosen = indexOf ("Lead Vocal");
        q.setCurrentProgram (chosen);
        check ((int) raw (id::role) == 4 && (int) raw (id::mix) == 3, "Lead Vocal: the lead vocal role, in the same mix");
        juce::MemoryBlock saved;
        q.getStateInformation (saved);
        HestiaProcessor other;
        other.setStateInformation (saved.getData(), (int) saved.getSize());
        check (other.getCurrentProgram() == chosen, "the chosen preset is saved with the session");
        std::unique_ptr<juce::AudioProcessorEditor> holder (q.createEditor());
        auto& editor = ui::innerOf<HestiaEditor> (*holder);
        editor.setVisible (true);
        check (editor.presetBox.getNumItems() == q.getNumPrograms() && editor.presetBox.getSelectedId() == chosen + 1, "the editor's preset box lists the presets and shows the current one");
        check (editor.presetBox.getRight() < editor.getWidth() && ! editor.presetBox.getBounds().intersects (editor.guideButton.getBounds()), "the preset box fits the header beside the GUIDE button");
    }

    return test::finish ("UI CHECK");
}
