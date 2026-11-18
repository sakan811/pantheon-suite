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
    check (proc.getParameters().size() == 12, fmt ("%d parameters: role, mix, importance, carve, reach, attack, release, output, depth, the sidechain's role and importance, and hold", proc.getParameters().size()));
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
        check (ui::hasEditorAspect (editor.getWidth(), editor.getHeight()), "the editor is drawn at 16:10, the shape of every plugin");
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

        // The host's names for the tracks: the lists and the depth map call a stem by it. (updateTrackProperties is what a host calls.)
        {
            juce::AudioProcessor::TrackProperties props;
            props.name = "Bass DI";
            bass.updateTrackProperties (props);
            props.name = "Lead";
            vocal.updateTrackProperties (props);
            std::unique_ptr<juce::AudioProcessorEditor> holder (vocal.createEditor());
            auto& editor = ui::innerOf<HestiaEditor> (*holder);
            editor.setVisible (true);
            editor.view.tick();
            check (editor.view.otherStems() == 1 && editor.view.stemLabel (0) == "Bass DI" && editor.view.thisLabel() == "Lead",
                   "the other stem is listed by its track's name ('" + editor.view.stemLabel (0).toStdString() + "'), and this one by its own ('" + editor.view.thisLabel().toStdString() + "')");
            props.name = std::nullopt;
            bass.updateTrackProperties (props);
            editor.view.tick();
            check (editor.view.stemLabel (0) == juce::String (roleName (Role::bass)), "a host that gives no name leaves the role as the label");

            // COPY TO MIX: the other instance of the mix takes CARVE, REACH, ATTACK and RELEASE (on the message thread, after its next block).
            setValue (vocal.apvts, id::carve, 11.0f);
            setValue (vocal.apvts, id::reach, 20.0f);
            setValue (vocal.apvts, id::attack, 5.0f);
            setValue (vocal.apvts, id::release, 400.0f);
            editor.copyToMix();
            for (int block = 0; block < 4; ++block)
                bass.processBlock (b, midi);
            juce::MessageManager::getInstance()->runDispatchLoopUntil (150);
            const auto got = readParams (bass.apvts);
            check (std::fabs (got.carveDb - 11.0f) < 0.05f && std::fabs (got.reachDb - 20.0f) < 0.3f && std::fabs (got.attackMs - 5.0f) < 0.6f && std::fabs (got.releaseMs - 400.0f) < 3.0f,
                   fmt ("COPY TO MIX: the bass has CARVE %.1f, REACH %.1f, ATTACK %.1f and RELEASE %.0f from the vocal", got.carveDb, got.reachDb, got.attackMs, got.releaseMs));
            check (readParams (bass.apvts).role == Role::bass && readParams (bass.apvts).group == 8, "... and keeps its own role and mix");
            check (ui::hasNote (editor.notes(), "Copied CARVE, REACH, ATTACK and RELEASE to 1 other instance", ui::Severity::info), "the strip says what was copied and to how many");
            setValue (vocal.apvts, id::hold, 1.0f);
            check (ui::hasNote (editor.notes(), "HOLD is on", ui::Severity::info), "HOLD on is said in the strip");
            setValue (vocal.apvts, id::hold, 0.0f);
        }
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
        // The four corner handles (ui::ResizeGrips, shared by every plugin): they answer the mouse in the corners only, and the window keeps 16:10 between its limits.
        {
            ui::ResizeGrips* grips = nullptr;
            for (auto* child : first->getChildren())
                if (auto* g = dynamic_cast<ui::ResizeGrips*> (child))
                    grips = g;
            check (grips != nullptr && grips->getBounds() == first->getLocalBounds(), "the window has corner handles over its whole area");
            const int w = first->getWidth(), h = first->getHeight();
            check (ui::ResizeGrips::cornerAt ({ 2, 2 }, w, h) == 0 && ui::ResizeGrips::cornerAt ({ w - 2, 2 }, w, h) == 1
                       && ui::ResizeGrips::cornerAt ({ 2, h - 2 }, w, h) == 2 && ui::ResizeGrips::cornerAt ({ w - 2, h - 2 }, w, h) == 3
                       && ui::ResizeGrips::cornerAt ({ w / 2, h / 2 }, w, h) == -1 && ui::ResizeGrips::cornerAt ({ w / 2, 2 }, w, h) == -1,
                   "all four corners are handles and nothing else is: the middle and the edges are left to the editor");
            check (grips != nullptr && grips->hitTest (3, 3) && grips->hitTest (w - 3, h - 3) && ! grips->hitTest (w / 2, h / 2), "a click in a corner goes to the handle, one in the middle to the editor");
            check (first->getConstrainer()->getFixedAspectRatio() == ui::kEditorAspect, "the host's edge drag keeps 16:10 too");
            check (ui::hasEditorAspect (limits->getMinimumWidth(), limits->getMinimumHeight()) && ui::hasEditorAspect (limits->getMaximumWidth(), limits->getMaximumHeight()),
                   "the smallest and the largest size are both 16:10");
        }
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
        check (editor.guideButton.getWidth() >= 48 && editor.getLocalBounds().contains (editor.guideButton.getBounds())
                   && ! editor.guideButton.getBounds().intersects (editor.copyButton.getBounds()) && ! editor.guideButton.getBounds().intersects (editor.outKnob.getBounds()),
               "GUIDE has room in the header: wide enough, inside the editor, clear of COPY TO MIX and OUTPUT");
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
        setValue (q.apvts, id::hold, 1.0f);
        q.setCurrentProgram (0);
        check (raw (id::hold) > 0.5f, "HOLD is kept by a preset");
        setValue (q.apvts, id::hold, 0.0f);
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

    // UNDO / REDO, A / B and the header strip they sit in.
    std::printf ("\nUndo and A / B: one step per change, the settings of each slot, and sessions from before A / B\n");
    {
        HestiaProcessor q;
        q.setPlayConfigDetails (2, 2, 48000.0, 512);
        q.prepareToPlay (48000.0, 512);
        std::unique_ptr<juce::AudioProcessorEditor> holder (q.createEditor());
        auto& editor = ui::innerOf<HestiaEditor> (*holder);
        editor.setVisible (true);
        // The state records a change when the editor's timer writes it to the tree (in the step open then), so a change has settled when the tree holds it.
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
        check (undoR.getX() >= 12 + 250 && undoR.getBottom() <= editor.roleBox.getY() - 15, "UNDO / REDO are right of the nameplate and above the row's labels");
        const juce::Rectangle<int> controls[] { editor.roleBox.getBounds(), editor.mixBox.getBounds(), editor.presetBox.getBounds(), editor.holdButton.getBounds(),
                                                editor.deltaButton.getBounds(), editor.copyButton.getBounds(), editor.guideButton.getBounds(), editor.outKnob.getBounds() };
        bool clear = true;
        for (const auto& r : controls)
            clear = clear && ! undoR.intersects (r) && ! abR.intersects (r);
        check (clear, "UNDO / REDO and A / B overlap no control of the header");

        // Each change made with its own step is taken back by one UNDO, and REDO brings it again.
        q.undo.clearUndoHistory();
        ui::beginUndoStep (q.apvts, "Carve");
        setValue (q.apvts, id::carve, 9.0f);
        pumpUntil ([&] { return inTree (id::carve, 9.0f); });
        ui::beginUndoStep (q.apvts, "Reach");
        setValue (q.apvts, id::reach, 20.0f);
        pumpUntil ([&] { return inTree (id::reach, 20.0f); });
        pumpUntil ([&] { return editor.undoControls().undoSwitch().isEnabled(); });
        check (editor.undoControls().undoSwitch().isEnabled() && ! editor.undoControls().redoSwitch().isEnabled(), "after two changes UNDO is lit and REDO is not");
        editor.undoControls().undo();
        check (std::fabs (readParams (q.apvts).reachDb - 12.0f) < 1e-3f && std::fabs (readParams (q.apvts).carveDb - 9.0f) < 1e-3f, "UNDO takes back the last change only (REACH back to 12 dB, CARVE stays 9 dB)");
        editor.undoControls().undo();
        check (std::fabs (readParams (q.apvts).carveDb - 6.0f) < 1e-3f, "a second UNDO takes back the carve too (6 dB)");
        pumpUntil ([&] { return editor.undoControls().redoSwitch().isEnabled(); });
        check (editor.undoControls().redoSwitch().isEnabled(), "REDO is lit after an UNDO");
        editor.undoControls().redo();
        check (std::fabs (readParams (q.apvts).carveDb - 9.0f) < 1e-3f, "REDO brings the carve back (9 dB)");

        // Cmd+Z and Shift+Cmd+Z reach the same buttons from the keyboard: the editor hands them to UndoButtons::handleKey.
        check (editor.undoControls().handleKey (juce::KeyPress ('z', juce::ModifierKeys::commandModifier, 0)) && std::fabs (readParams (q.apvts).carveDb - 6.0f) < 1e-3f,
               "Cmd+Z takes back the carve (6 dB)");
        check (editor.undoControls().handleKey (juce::KeyPress ('z', juce::ModifierKeys (juce::ModifierKeys::commandModifier | juce::ModifierKeys::shiftModifier), 0))
                   && std::fabs (readParams (q.apvts).carveDb - 9.0f) < 1e-3f,
               "Shift+Cmd+Z brings it back (9 dB)");

        // A preset is one step: one UNDO puts back everything it moved (Kick: role, importance and carve).
        q.undo.clearUndoHistory();
        int kick = 0;
        for (int i = 0; i < q.getNumPrograms(); ++i)
            if (q.getProgramName (i) == "Kick") kick = i;
        q.setCurrentProgram (kick);
        pumpUntil ([&] { return inTree (id::role, 0.0f) && inTree (id::carve, 4.0f); });
        check (readParams (q.apvts).role == Role::kick && std::fabs (readParams (q.apvts).carveDb - 4.0f) < 1e-3f, "the Kick preset moved the role and the carve");
        editor.undoControls().undo();
        check (readParams (q.apvts).role == Role::fullRange && std::fabs (readParams (q.apvts).importance - 0.5f) < 1e-3f && std::fabs (readParams (q.apvts).carveDb - 9.0f) < 1e-3f,
               "one UNDO takes back the whole preset (role, importance and carve), the change before it stays");

        // A / B: each slot keeps its settings, MIX is kept across a switch, and each switch is one step.
        q.undo.clearUndoHistory();
        setValue (q.apvts, id::carve, 9.0f);
        setValue (q.apvts, id::importance, 80.0f);
        pumpUntil ([&] { return inTree (id::carve, 9.0f) && inTree (id::importance, 80.0f); });
        editor.abControls().button (1).onClick(); // B starts as a copy of A
        check (q.ab.active() == 1 && editor.abControls().button (1).getToggleState() && ! editor.abControls().button (0).getToggleState(), "B is chosen and lit, A is not");
        setValue (q.apvts, id::carve, 3.0f);
        setValue (q.apvts, id::importance, 30.0f);
        setValue (q.apvts, id::mix, 5.0f); // MIX is not part of a slot: it must survive the switches below
        pumpUntil ([&] { return inTree (id::carve, 3.0f) && inTree (id::importance, 30.0f) && inTree (id::mix, 5.0f); });
        editor.abControls().button (0).onClick();
        pumpUntil ([&] { return inTree (id::carve, 9.0f) && inTree (id::importance, 80.0f); });
        check (std::fabs (readParams (q.apvts).carveDb - 9.0f) < 0.05f && std::fabs (readParams (q.apvts).importance - 0.8f) < 0.01f, "switching to A brings back A's settings (9 dB, IMPORTANCE 80%)");
        check (readParams (q.apvts).group == 5, "MIX stays where it was across a switch (5, E)");
        editor.abControls().button (1).onClick();
        pumpUntil ([&] { return inTree (id::carve, 3.0f) && inTree (id::importance, 30.0f); });
        check (std::fabs (readParams (q.apvts).carveDb - 3.0f) < 0.05f && std::fabs (readParams (q.apvts).importance - 0.3f) < 0.01f, "switching back to B brings back B's settings (3 dB, IMPORTANCE 30%)");
        editor.undoControls().undo();
        check (std::fabs (readParams (q.apvts).carveDb - 9.0f) < 0.05f, "UNDO takes back the A / B switch as one step (A's 9 dB again)");

        // A session keeps which slot is active, not the slots themselves; a session from before A / B has no abSlot and opens on A.
        editor.abControls().button (0).onClick();
        juce::MemoryBlock saved;
        q.getStateInformation (saved);
        HestiaProcessor reopened;
        reopened.setStateInformation (saved.getData(), (int) saved.getSize());
        check (reopened.ab.active() == 0 && ! reopened.ab.hasSettings (1), "the session keeps which slot is active (A here), not the slots themselves");
        editor.abControls().button (1).onClick();
        q.getStateInformation (saved);
        HestiaProcessor onB;
        onB.setStateInformation (saved.getData(), (int) saved.getSize());
        check (onB.ab.active() == 1, "a session saved on B opens on B");
        if (auto xml = juce::AudioProcessor::getXmlFromBinary (saved.getData(), (int) saved.getSize()))
        {
            check (xml->getIntAttribute ("abSlot", -1) == 1, "the B session carries abSlot 1 before it is taken out");
            xml->removeAttribute ("abSlot");
            juce::MemoryBlock legacy;
            juce::AudioProcessor::copyXmlToBinary (*xml, legacy);
            HestiaProcessor before;
            before.setStateInformation (legacy.getData(), (int) legacy.getSize());
            check (before.ab.active() == 0, "a session from before A / B (no abSlot) opens on slot A");
        }
        else
            check (false, "the saved session is readable as XML");
    }

    std::printf ("\nAUTO ROLE: ROLE from the track's name, one UNDO step, and a name with no role word is said so\n");
    {
        HestiaProcessor q;
        q.setPlayConfigDetails (2, 2, 48000.0, 512);
        q.prepareToPlay (48000.0, 512);
        std::unique_ptr<juce::AudioProcessorEditor> holder (q.createEditor());
        auto& editor = ui::innerOf<HestiaEditor> (*holder);
        editor.setVisible (true);
        const auto pumpUntil = [] (auto&& done) {
            for (int waited = 0; ! done() && waited < 2000; waited += 10)
                juce::MessageManager::getInstance()->runDispatchLoopUntil (10);
            return done();
        };
        setValue (q.apvts, id::role, (float) (int) Role::bass);
        for (int i = 0; i < 40; ++i) // the editor's timer writes the change to the tree: let it, before the step below starts
            juce::MessageManager::getInstance()->runDispatchLoopUntil (10);
        q.undo.clearUndoHistory();

        const auto autoR = editor.autoRoleButton().getBounds();
        check (editor.getLocalBounds().contains (autoR) && ! autoR.intersects (editor.roleBox.getBounds()) && autoR.getBottom() <= editor.roleBox.getY(),
               "AUTO ROLE is inside the editor, on the Role label's line, clear of the box");

        const auto before = readParams (q.apvts);
        q.engine.setTrackName ("Kick_01");
        editor.autoRoleButton().onClick();
        pumpUntil ([&] { return readParams (q.apvts).role == Role::kick; });
        check (readParams (q.apvts).role == Role::kick, "AUTO ROLE on the track 'Kick_01' sets ROLE to Kick");
        check (readParams (q.apvts).carveDb == before.carveDb && readParams (q.apvts).group == before.group && readParams (q.apvts).importance == before.importance,
               "... and nothing else is changed");
        // The editor's timer writes the change into the state tree, and that is the step UNDO takes back: let it settle first.
        for (int i = 0; i < 40; ++i)
            juce::MessageManager::getInstance()->runDispatchLoopUntil (10);
        editor.undoControls().undo();
        pumpUntil ([&] { return readParams (q.apvts).role == Role::bass; });
        check (readParams (q.apvts).role == Role::bass, "one UNDO takes the AUTO ROLE back (ROLE is Bass again)");

        q.engine.setTrackName ("Track 1");
        editor.autoRole();
        check (readParams (q.apvts).role == Role::bass, "a name with no role word leaves ROLE as it is");
        check (ui::hasNote (editor.notes(), "AUTO ROLE found no role word in the track name \"Track 1\"", ui::Severity::info), "... and the strip says so");

        q.engine.setTrackName ("Kick_01");
        editor.autoRole();
        check (! ui::hasNote (editor.notes(), "AUTO ROLE found no role word", ui::Severity::info), "a match after a miss clears the note");

        q.engine.setTrackName ("");
        editor.autoRole();
        check (ui::hasNote (editor.notes(), "the host gave this track no name", ui::Severity::info), "a track with no name is said to have none");
    }

    std::printf ("\nAUTO ROLE by itself: once, on a fresh instance's first clear name; a saved session is never changed; the strip says what it did\n");
    {
        const auto pumpFor = [] (int ms) {
            for (int t = 0; t < ms; t += 10)
                juce::MessageManager::getInstance()->runDispatchLoopUntil (10);
        };
        const auto giveName = [] (HestiaProcessor& p, const char* s) {
            juce::AudioProcessor::TrackProperties props;
            props.name = s;
            p.updateTrackProperties (props);
        };

        // A fresh instance given a clear name sets ROLE by itself, as one UNDO step, and the strip says so until ROLE is changed.
        HestiaProcessor q;
        q.setPlayConfigDetails (2, 2, 48000.0, 512);
        q.prepareToPlay (48000.0, 512);
        std::unique_ptr<juce::AudioProcessorEditor> holder (q.createEditor());
        auto& editor = ui::innerOf<HestiaEditor> (*holder);
        editor.setVisible (true);
        giveName (q, "Bass DI");
        pumpFor (300);
        check (readParams (q.apvts).role == Role::bass, "a fresh instance given 'Bass DI' sets ROLE to Bass by itself");
        check (ui::hasNote (editor.notes(), "Role set to Bass from the track name 'Bass DI'; change it any time.", ui::Severity::info),
               "the strip says what it did, with the name it read");
        check (readParams (q.apvts).carveDb == EngineParams().carveDb && readParams (q.apvts).group == EngineParams().group,
               "... and nothing else is changed");
        pumpFor (300);
        q.undo.undo();
        pumpFor (300);
        check (readParams (q.apvts).role == Role::fullRange, "one UNDO takes the automatic ROLE back (Full Range again)");
        check (! ui::hasNote (editor.notes(), "Role set to", ui::Severity::info), "... and the strip stops saying it");
        q.undo.redo();
        pumpFor (300);
        check (readParams (q.apvts).role == Role::bass, "REDO brings it back");
        setValue (q.apvts, id::role, (float) (int) Role::kick);
        pumpFor (100);
        check (! ui::hasNote (editor.notes(), "Role set to", ui::Severity::info), "once the user changes ROLE, the strip no longer says what was set");

        // It runs once: a second name does not set anything, and a name with no confident word sets nothing at all.
        HestiaProcessor r;
        r.setPlayConfigDetails (2, 2, 48000.0, 512);
        r.prepareToPlay (48000.0, 512);
        giveName (r, "Track 1");
        giveName (r, "Bass DI");
        pumpFor (300);
        check (readParams (r.apvts).role == Role::fullRange, "a name with no word is not a name to act on, and the first one ends the try");
        HestiaProcessor w;
        w.setPlayConfigDetails (2, 2, 48000.0, 512);
        w.prepareToPlay (48000.0, 512);
        giveName (w, "Synth");
        pumpFor (300);
        check (readParams (w.apvts).role == Role::fullRange, "the weakest word alone ('Synth') sets nothing");

        // A saved session is never changed: a saved ROLE stays, whatever the track is called, and the default saved ROLE stays too.
        HestiaProcessor saved;
        saved.setPlayConfigDetails (2, 2, 48000.0, 512);
        saved.prepareToPlay (48000.0, 512);
        setValue (saved.apvts, id::role, (float) (int) Role::kick);
        pumpFor (100);
        juce::MemoryBlock kickState;
        saved.getStateInformation (kickState);
        setValue (saved.apvts, id::role, (float) (int) Role::fullRange);
        pumpFor (100);
        juce::MemoryBlock fullState;
        saved.getStateInformation (fullState);

        HestiaProcessor reopened;
        reopened.setPlayConfigDetails (2, 2, 48000.0, 512);
        reopened.prepareToPlay (48000.0, 512);
        reopened.setStateInformation (kickState.getData(), (int) kickState.getSize());
        giveName (reopened, "Bass DI");
        pumpFor (300);
        check (readParams (reopened.apvts).role == Role::kick, "a saved session with ROLE Kick is not overwritten by 'Bass DI'");
        check (! reopened.autoRoleSet, "... and nothing is said about it");

        HestiaProcessor reopenedDefault;
        reopenedDefault.setPlayConfigDetails (2, 2, 48000.0, 512);
        reopenedDefault.prepareToPlay (48000.0, 512);
        reopenedDefault.setStateInformation (fullState.getData(), (int) fullState.getSize());
        giveName (reopenedDefault, "Bass DI");
        pumpFor (300);
        check (readParams (reopenedDefault.apvts).role == Role::fullRange, "a saved session whose ROLE is still the default is not treated as fresh");

        // A name given before the saved session is loaded is overwritten by the saved state, which wins.
        HestiaProcessor late;
        late.setPlayConfigDetails (2, 2, 48000.0, 512);
        late.prepareToPlay (48000.0, 512);
        giveName (late, "Bass DI");
        pumpFor (300);
        check (readParams (late.apvts).role == Role::bass, "(a name first, then a saved session) the name sets Bass");
        late.setStateInformation (fullState.getData(), (int) fullState.getSize());
        pumpFor (300);
        check (readParams (late.apvts).role == Role::fullRange && ! late.autoRoleSet, "... and the saved session's ROLE then wins, with no note");
    }

    return test::finish ("UI CHECK");
}
