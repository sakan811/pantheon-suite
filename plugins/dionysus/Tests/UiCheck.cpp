// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 The Pantheon authors

// Builds the editor on the real processor and checks what a host and a user would meet: the parameters, the name, the controls, the latency the host is told,
// the audio path, the state, and the notes.

#include "Advice.h"
#include "Check.h"
#include "PluginEditor.h"
#include <pantheon/ui/Scaled.h>

using namespace dionysus;
using test::check;
using test::fmt;

int main()
{
    juce::ScopedJuceInitialiser_GUI gui;
    std::printf ("Dionysus UI check\n");
    DionysusProcessor proc;
    proc.setPlayConfigDetails (2, 2, 48000.0, 512);
    proc.prepareToPlay (48000.0, 512);

    check (proc.getName() == juce::String (gui::kProjectWord) + " " + gui::kPluginWord, "the reported name is the nameplate's two words");
    check (proc.getParameters().size() == 8, fmt ("%d parameters: drive, curve, bias, tone, mix, output, auto gain and oversampling", proc.getParameters().size()));
    check (shapeNames.size() == kNumShapes && oversamplingNames.size() == kNumOversampling, "the choice lists match the engine's");
    const auto d = readParams (proc.apvts);
    check (std::fabs (d.driveDb - 12.0f) < 0.01f && d.shape == Shape::tape && std::fabs (d.bias) < 1e-4f && std::fabs (d.tone) < 1e-4f && d.mix > 0.999f && d.autoGain && d.oversampling == 1,
           "a new instance: 12 dB of drive, tape, no bias or tone, MIX 100%, AUTO GAIN on, 2x");
    check (proc.getLatencySamples() == oversamplingLatency (1), fmt ("the host is told %d samples of latency", proc.getLatencySamples()));
    {
        setValue (proc.apvts, id::oversampling, 3.0f);
        juce::AudioBuffer<float> buffer (2, 512);
        buffer.clear();
        juce::MidiBuffer midi;
        proc.processBlock (buffer, midi);
        juce::MessageManager::getInstance()->runDispatchLoopUntil (100);
        check (proc.getLatencySamples() == oversamplingLatency (3), fmt ("after OVERSAMPLING is set to 8x the host is told %d samples", proc.getLatencySamples()));
        setValue (proc.apvts, id::oversampling, 1.0f);
    }

    // The audio path through the real processor: MIX 0% is the input, delayed.
    {
        DionysusProcessor q;
        q.setPlayConfigDetails (2, 2, 48000.0, 512);
        q.prepareToPlay (48000.0, 512);
        setValue (q.apvts, id::mix, 0.0f);
        juce::AudioBuffer<float> buffer (2, 512);
        juce::MidiBuffer midi;
        std::vector<float> in, out;
        for (int block = 0; block < 8; ++block)
        {
            for (int i = 0; i < 512; ++i)
                for (int c = 0; c < 2; ++c)
                    buffer.setSample (c, i, 0.3f * std::sin (0.05f * (float) (block * 512 + i) * (float) (c + 1)));
            for (int i = 0; i < 512; ++i)
                in.push_back (buffer.getSample (0, i));
            q.processBlock (buffer, midi);
            for (int i = 0; i < 512; ++i)
                out.push_back (buffer.getSample (0, i));
        }
        const size_t latency = (size_t) q.getLatencySamples();
        bool same = true;
        for (size_t i = latency; i < in.size(); ++i)
            same = same && out[i] == in[i - latency];
        check (same, fmt ("MIX 0%% through the processor is the input delayed by %d samples, bit for bit", (int) latency));
    }

    {
        std::unique_ptr<juce::AudioProcessorEditor> holder (proc.createEditor());
        auto& editor = ui::innerOf<DionysusEditor> (*holder);
        editor.setVisible (true);
        check (editor.shape.getNumItems() == kNumShapes && editor.oversampling.getNumItems() == kNumOversampling, "the selectors have the parameters' items");
        check (editor.getWidth() > 700 && editor.out.getRight() < editor.shape.getX() && editor.autoGain.getRight() <= editor.getWidth() && editor.autoGain.getBottom() < editor.getHeight(), "the controls fit the window");
        editor.autoGain.triggerClick();
        juce::MessageManager::getInstance()->runDispatchLoopUntil (100);
        check (! readParams (proc.apvts).autoGain && ui::hasNote (editor.notes(), "AUTO GAIN is off", ui::Severity::info), "AUTO GAIN sets the parameter, and with it off a note says louder sounds better");
        editor.autoGain.triggerClick();
    }

    // The state survives a round trip.
    {
        setValue (proc.apvts, id::drive, 22.5f);
        setValue (proc.apvts, id::shape, (float) (int) Shape::diode);
        setValue (proc.apvts, id::bias, -40.0f);
        juce::MemoryBlock saved;
        proc.getStateInformation (saved);
        DionysusProcessor other;
        other.setStateInformation (saved.getData(), (int) saved.getSize());
        const auto o = readParams (other.apvts);
        check (std::fabs (o.driveDb - 22.5f) < 0.01f && o.shape == Shape::diode && std::fabs (o.bias + 0.4f) < 0.01f, "the state of a session comes back as it was saved");
    }

    // The notes.
    {
        AdviceInput in;
        check (ui::hasNote (adviceFor (in), "BIAS", ui::Severity::info) == false && adviceFor (in).size() <= 2, "a default instance has little to say");
        in.params.mix = 0.0f;
        in.params.oversampling = 0;
        in.params.shape = Shape::clip;
        in.params.driveDb = 26.0f;
        check (ui::hasNote (adviceFor (in), "MIX is 0%", ui::Severity::warning) && ui::hasNote (adviceFor (in), "aliases harshly", ui::Severity::warning) && ui::hasNote (adviceFor (in), "A DRIVE of 26 dB", ui::Severity::info),
               "MIX 0%, a hard curve without oversampling and a heavy drive are flagged");
        in.params.mix = 0.5f;
        in.params.oversampling = 3;
        in.params.bias = 0.4f;
        in.latencyMs = 0.9;
        check (ui::hasNote (adviceFor (in), "BIAS makes even harmonics", ui::Severity::info) && ui::hasNote (adviceFor (in), "dry sound is delayed to match", ui::Severity::info) && ui::hasNote (adviceFor (in), "delays the sound by 1 ms", ui::Severity::info),
               "bias, a parallel mix and the oversampler's delay are explained");
        bool tooLong = false;
        for (const auto& n : adviceFor (in))
            if ((int) n.text.size() > ui::kMaxNoteChars)
            {
                tooLong = true;
                std::printf ("  too long (%d): %s\n", (int) n.text.size(), n.text.c_str());
            }
        in.params.mix = 0.0f;
        in.params.oversampling = 0;
        in.params.autoGain = false;
        for (const auto& n : adviceFor (in))
            if ((int) n.text.size() > ui::kMaxNoteChars)
            {
                tooLong = true;
                std::printf ("  too long (%d): %s\n", (int) n.text.size(), n.text.c_str());
            }
        check (! tooLong, "every note fits a line of the strip");
    }

    // Presets: each applies, has a name and a one-line purpose, the chosen one is kept with the session, and the box is in the header.
    {
        DionysusProcessor q;
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
        q.setCurrentProgram (indexOf ("Fold Fuzz"));
        check ((int) raw (id::shape) == 4 && (int) raw (id::oversampling) == 3 && std::fabs (raw (id::mix) - 60.0f) < 0.5f, "Fold Fuzz: the fold curve, 8x oversampling, 60% mix");
        q.setCurrentProgram (0);
        check ((int) raw (id::shape) == 0 && (int) raw (id::oversampling) == 1 && std::fabs (raw (id::mix) - 100.0f) < 0.5f && std::fabs (raw (id::drive) - 12.0f) < 0.1f, "Init is the defaults");
        const int chosen = indexOf ("Parallel Grit");
        q.setCurrentProgram (chosen);
        check ((int) raw (id::shape) == 3 && std::fabs (raw (id::mix) - 30.0f) < 0.5f, "Parallel Grit: the diode curve under the dry sound");
        juce::MemoryBlock saved;
        q.getStateInformation (saved);
        DionysusProcessor other;
        other.setStateInformation (saved.getData(), (int) saved.getSize());
        check (other.getCurrentProgram() == chosen, "the chosen preset is saved with the session");
        std::unique_ptr<juce::AudioProcessorEditor> holder (q.createEditor());
        auto& editor = ui::innerOf<DionysusEditor> (*holder);
        editor.setVisible (true);
        check (editor.presetBox.getNumItems() == q.getNumPrograms() && editor.presetBox.getSelectedId() == chosen + 1, "the editor's preset box lists the presets and shows the current one");
        check (editor.presetBox.getRight() < editor.getWidth() && ! editor.presetBox.getBounds().intersects (editor.guideButton.getBounds()), "the preset box fits the header beside the GUIDE button");
    }

    return test::finish ("DionysusUiCheck");
}
