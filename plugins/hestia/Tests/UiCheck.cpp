// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 The Pantheon authors

// Builds the editor on the real processor and checks what a host and a user would meet: the parameters, the name, the controls, that CUT ONLY is
// monitoring that ends with the window, the notes, and two processors finding each other through the shared bus.

#include "Advice.h"
#include "Check.h"
#include "PluginEditor.h"
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
    check (proc.getParameters().size() == 8, fmt ("%d parameters: role, mix, importance, depth, reach, attack, release, output", proc.getParameters().size()));
    check (roleNames().size() == kNumRoles && mixNames.size() == kMaxGroups + 1, "the choice lists match the engine's roles and mixes");
    const auto defaults = readParams (proc.apvts);
    check (defaults.role == Role::fullRange && defaults.depthDb == 6.0f && std::fabs (defaults.importance - 0.5f) < 1e-6f && defaults.outDb == 0.0f,
           "a new instance is Full Range, importance 50%, depth 6 dB, output 0");
    {
        HestiaProcessor fresh;
        check (readParams (fresh.apvts).group == kDefaultMix && mixNames[kDefaultMix] == "A", "a new instance is in mix A, so the stems of a session find each other without being set up");
    }

    {
        std::unique_ptr<juce::AudioProcessorEditor> holder (proc.createEditor());
        auto& editor = static_cast<HestiaEditor&> (*holder);
        editor.setVisible (true);
        check (editor.roleBox.getNumItems() == kNumRoles && editor.mixBox.getNumItems() == kMaxGroups + 1, "the role and mix selectors have the parameters' items");
        editor.roleBox.setSelectedItemIndex ((int) Role::bass, juce::sendNotificationSync);
        editor.mixBox.setSelectedItemIndex (3, juce::sendNotificationSync);
        check (readParams (proc.apvts).role == Role::bass && readParams (proc.apvts).group == 3, "the selectors set the role and the mix");
        check (editor.getWidth() > 800 && editor.importance.getRight() < editor.getWidth() && editor.release.getBottom() < editor.getHeight(), "the knobs fit the window");

        editor.cutOnly.triggerClick();
        juce::MessageManager::getInstance()->runDispatchLoopUntil (100);
        check (proc.engine.deltaOn.load() && editor.cutOnly.getToggleState(), "CUT ONLY listens to the cut and lights its button");
    }
    check (! proc.engine.deltaOn.load(), "closing the editor ends CUT ONLY");

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
        setValue (vocal.apvts, id::depth, 11.0f);
        setValue (vocal.apvts, id::importance, 80.0f);
        juce::MemoryBlock state;
        vocal.getStateInformation (state);
        HestiaProcessor other;
        other.setStateInformation (state.getData(), (int) state.getSize());
        const auto r = readParams (other.apvts);
        check (r.role == Role::leadVocal && r.group == 8 && std::fabs (r.depthDb - 11.0f) < 0.05f && std::fabs (r.importance - 0.8f) < 0.01f, "a saved state comes back in another instance");
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
        check (has (adviceFor (in), "CUT ONLY is on", Severity::warning), "CUT ONLY on: a warning");
        in = fine; in.params.group = 0;
        check (has (adviceFor (in), "MIX is Off", Severity::warning), "the mix Off: nothing is cut, and it says so");
        in = fine; in.others = 0;
        check (has (adviceFor (in), "No other stem", Severity::warning), "nobody else playing in the mix: a warning");
        in = fine; in.params.depthDb = 0.0f;
        check (has (adviceFor (in), "DEPTH is 0", Severity::warning), "a depth of 0: a warning");
        in = fine; in.sameClaim = 1;
        check (has (adviceFor (in), "this role and about this IMPORTANCE", Severity::info), "another stem of the same role and importance: neither gives way");
        in = fine; in.params.role = Role::fullRange;
        check (has (adviceFor (in), "Full Range", Severity::info), "role Full Range: pick the real one");
        in = fine; in.params.depthDb = 12.0f; in.strongestCutDb = -11.0f;
        check (has (adviceFor (in), "hollow", Severity::warning) && has (adviceFor (in), "11 dB", Severity::warning), "a cut of 11 dB: the stem can sound hollow");
        in.params.depthDb = 6.0f; in.strongestCutDb = -6.0f;
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
            all.params.depthDb = (mask & 4) ? 0.0f : ((mask & 8) ? 18.0f : 6.0f);
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
        check (has (editor.notes(), "CUT ONLY", Severity::warning), "CUT ONLY in the engine reaches the editor's notes");
        q.engine.deltaOn = false;
        check (editor.guide().isVisible() && std::strlen (guideText()) > 400 && std::string (guideText()).find ("NOT FOR") != std::string::npos,
               "the GUIDE button is there and its text says what the plugin is not for");
    }

    return test::finish ("UI CHECK");
}
