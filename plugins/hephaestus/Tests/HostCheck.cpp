// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 The Pantheon authors

// Loads the built VST3 the way a host does and exercises it: instantiate, prepare at several
// sample rates / block sizes, fuzz every parameter while processing, state + preset round trips,
// latency reporting.
//
//   HephaestusHostCheck <path/to/Hephaestus Forge.vst3>

#include "Check.h"
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>
#include <random>

namespace
{
void check (bool ok, const juce::String& what) { test::check (ok, what.toStdString()); }

void pump (int ms) { juce::MessageManager::getInstance()->runDispatchLoopUntil (ms); }

bool allFinite (const juce::AudioBuffer<float>& b, float limit = 1000.0f)
{
    for (int c = 0; c < b.getNumChannels(); ++c)
        for (int i = 0; i < b.getNumSamples(); ++i)
            if (! std::isfinite (b.getSample (c, i)) || std::fabs (b.getSample (c, i)) > limit)
                return false;
    return true;
}

// A random value a host could really send: a stepped parameter (a choice, a switch, the wrapper's Program) gets one of its own steps, so that
// the value it reads back is the one it was given. A raw float for the Program parameter reads as one preset through its text and as the
// next through the program it selects, which is a difference in the host wrapper and not in the plugin.
float randomValue (juce::AudioProcessorParameter& p, std::mt19937& rng)
{
    std::uniform_real_distribution<float> u (0.0f, 1.0f);
    if (p.isDiscrete() && p.getNumSteps() > 1)
        return (float) std::min ((int) (u (rng) * (float) p.getNumSteps()), p.getNumSteps() - 1) / (float) (p.getNumSteps() - 1);
    return u (rng);
}

void setNamed (juce::AudioProcessor& plugin, const juce::String& name, float value)
{
    for (auto* p : plugin.getParameters())
        if (p->getName (100) == name)
            p->setValue (value);
}
}

int main (int argc, char** argv)
{
    if (argc < 2)
    {
        std::printf ("usage: HephaestusHostCheck <plugin.vst3>\n");
        return 2;
    }

    juce::ScopedJuceInitialiser_GUI gui;
    juce::AudioPluginFormatManager formats;
    formats.addFormat (std::make_unique<juce::VST3PluginFormat>());

    juce::OwnedArray<juce::PluginDescription> found;
    formats.getFormat (0)->findAllTypesForFile (found, argv[1]);
    if (found.isEmpty())
    {
        std::printf ("no plugin found at %s\n", argv[1]);
        return 2;
    }

    std::printf ("Plugin: %s %s (%s)\n", found[0]->name.toRawUTF8(), found[0]->version.toRawUTF8(),
                 found[0]->pluginFormatName.toRawUTF8());

    juce::String error;
    auto plugin = formats.createPluginInstance (*found[0], 48000.0, 512, error);
    if (plugin == nullptr)
    {
        std::printf ("instantiate failed: %s\n", error.toRawUTF8());
        return 2;
    }
    check (true, "instantiated; " + juce::String (plugin->getParameters().size()) + " parameters, "
                 + juce::String (plugin->getNumPrograms()) + " presets");

    std::mt19937 rng (7);
    std::uniform_real_distribution<float> u (0.0f, 1.0f);

    // --- sample rates / block sizes, with every parameter randomised between blocks --------------
    std::printf ("\nProcessing\n");
    for (double sr : { 22050.0, 44100.0, 48000.0, 96000.0, 192000.0 })
        for (int block : { 1, 32, 480, 1024, 4096 })
        {
            plugin->setPlayConfigDetails (2, 2, sr, block);
            plugin->prepareToPlay (sr, block);
            juce::AudioBuffer<float> buffer (2, block);
            juce::MidiBuffer midi;
            bool ok = true;

            const int iterations = std::max (4, 6000 / block);
            for (int it = 0; it < iterations && ok; ++it)
            {
                if (it % 2 == 0)
                    for (auto* p : plugin->getParameters())
                        p->setValue (u (rng));

                for (int c = 0; c < 2; ++c)
                    for (int i = 0; i < block; ++i)
                        buffer.setSample (c, i, 0.7f * (2.0f * u (rng) - 1.0f));

                plugin->processBlock (buffer, midi);
                ok = allFinite (buffer);
            }
            plugin->releaseResources();
            if (! ok)
                check (false, juce::String (sr) + " Hz, block " + juce::String (block) + ": non-finite or runaway output");
        }
    check (test::failures == 0, "5 sample rates x 5 block sizes with randomised parameters: output always finite");

    // --- latency reporting follows the oversampling choice --------------------------------------------
    std::printf ("\nLatency reporting\n");
    // The fuzzing above randomised every parameter, the host wrapper's own Bypass included. Which
    // value it ends on depends on how many parameters precede it, so clear it before measuring.
    if (auto* bypass = plugin->getBypassParameter())
        bypass->setValue (0.0f);
    // The same goes for the plugin's own Delta (it plays wet minus dry, so there is no impulse to find), Gain Match and Mix: the impulse
    // must come out of the dry path, which is what the reported latency is for.
    setNamed (*plugin, "Delta", 0.0f);
    setNamed (*plugin, "Gain Match", 0.0f);
    setNamed (*plugin, "Mix", 0.0f);
    // A band the fuzzing left in Octave adds that generator's latency, which is not what this section measures.
    for (int band = 1; band <= 6; ++band)
        setNamed (*plugin, "Band " + juce::String (band) + " Character", 0.0f);
    plugin->setPlayConfigDetails (2, 2, 48000.0, 512);
    plugin->prepareToPlay (48000.0, 512);
    juce::AudioBuffer<float> buffer (2, 512);
    juce::MidiBuffer midi;
    // A hosted VST3 exposes wrapper parameters, so find the oversampling choice by name.
    juce::AudioProcessorParameter* os = nullptr;
    for (auto* p : plugin->getParameters())
        if (p->getName (100) == "Oversampling")
            os = p;
    check (os != nullptr, "oversampling parameter found");
    if (os != nullptr)
        for (int mode = 0; mode < 4; ++mode)
        {
            os->setValueNotifyingHost ((float) mode / 3.0f);
            for (int i = 0; i < 3; ++i)
            {
                buffer.clear();
                plugin->processBlock (buffer, midi);
                pump (100); // latency is pushed to the host from the message thread
            }
            std::printf ("    oversampling='%s' (value %.2f) -> reported latency %d samples\n",
                         os->getCurrentValueAsText().toRawUTF8(), os->getValue(), plugin->getLatencySamples());
            check ((mode == 0) == (plugin->getLatencySamples() == 0), "latency is 0 only when oversampling is off");

            // What the audio really does: an impulse must come out exactly `latency` samples later.
            buffer.clear();
            buffer.setSample (0, 100, 0.5f);
            buffer.setSample (1, 100, 0.5f);
            plugin->processBlock (buffer, midi);
            int peak = 0;
            for (int i = 0; i < buffer.getNumSamples(); ++i)
                if (std::fabs (buffer.getSample (0, i)) > std::fabs (buffer.getSample (0, peak))) peak = i;
            check (peak - 100 == plugin->getLatencySamples(),
                   "an impulse comes out exactly " + juce::String (plugin->getLatencySamples()) + " samples later (measured " + juce::String (peak - 100) + ")");
        }

    // --- state + presets ---------------------------------------------------------------------------------
    std::printf ("\nState and presets\n");
    for (auto* p : plugin->getParameters()) p->setValue (randomValue (*p, rng));
    // Compare displayed values: normalised floats are quantised differently by stepped parameters.
    std::vector<juce::String> before;
    for (auto* p : plugin->getParameters()) before.push_back (p->getCurrentValueAsText());

    juce::MemoryBlock state;
    plugin->getStateInformation (state);
    for (auto* p : plugin->getParameters()) p->setValue (randomValue (*p, rng)); // scramble
    plugin->setStateInformation (state.getData(), (int) state.getSize());

    bool same = true;
    int idx = 0;
    for (auto* p : plugin->getParameters())
    {
        const auto now = p->getCurrentValueAsText();
        if (now != before[(size_t) idx])
        {
            same = false;
            std::printf ("    mismatch: %s was '%s' now '%s'\n", p->getName (50).toRawUTF8(),
                         before[(size_t) idx].toRawUTF8(), now.toRawUTF8());
        }
        ++idx;
    }
    check (same, "state save/restore round-trips all parameters (" + juce::String ((int) state.getSize()) + " bytes)");

    bool presetsOk = true;
    for (int i = 0; i < plugin->getNumPrograms(); ++i)
    {
        plugin->setCurrentProgram (i);
        presetsOk = presetsOk && plugin->getProgramName (i).isNotEmpty();
        buffer.clear();
        plugin->processBlock (buffer, midi);
        presetsOk = presetsOk && allFinite (buffer);
    }
    check (presetsOk, "all " + juce::String (plugin->getNumPrograms()) + " presets load and process");

    // --- editor ------------------------------------------------------------------------------------------
    std::printf ("\nEditor\n");
    {
        std::unique_ptr<juce::AudioProcessorEditor> editor (plugin->createEditorAndMakeActive());
        check (editor != nullptr && editor->getWidth() > 0, "editor created");
        pump (100);
    }
    plugin->releaseResources();
    plugin.reset();

    return test::finish ("HOST CHECK");
}
