// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 The Pantheon authors

// Loads the built VST3 the way a host does and exercises it: instantiate, prepare at several sample rates / block sizes, fuzz
// every parameter while processing, state + preset round trips, zero latency, and the editor.
//
//   IrisHostCheck <path/to/Iris Prism.vst3>

#include "Check.h"
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>
#include <random>

namespace
{
void check (bool ok, const juce::String& what) { test::check (ok, what.toStdString()); }

void pump (int ms) { juce::MessageManager::getInstance()->runDispatchLoopUntil (ms); }

bool allFinite (const juce::AudioBuffer<float>& b, float limit = 10000.0f)
{
    for (int c = 0; c < b.getNumChannels(); ++c)
        for (int i = 0; i < b.getNumSamples(); ++i)
            if (! std::isfinite (b.getSample (c, i)) || std::fabs (b.getSample (c, i)) > limit)
                return false;
    return true;
}
}

int main (int argc, char** argv)
{
    std::setvbuf (stdout, nullptr, _IONBF, 0); // a crash in the plugin must not take the report with it

    if (argc < 2)
    {
        std::printf ("usage: IrisHostCheck <plugin.vst3>\n");
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

    std::printf ("Plugin: %s %s (%s)\n", found[0]->name.toRawUTF8(), found[0]->version.toRawUTF8(), found[0]->pluginFormatName.toRawUTF8());

    juce::String error;
    auto plugin = formats.createPluginInstance (*found[0], 48000.0, 512, error);
    if (plugin == nullptr)
    {
        std::printf ("instantiate failed: %s\n", error.toRawUTF8());
        return 2;
    }
    check (true, "instantiated; " + juce::String (plugin->getParameters().size()) + " parameters, " + juce::String (plugin->getNumPrograms()) + " presets");

    std::mt19937 rng (7);
    std::uniform_real_distribution<float> u (0.0f, 1.0f);

    // --- fresh from the box it is transparent and has no latency --------------------------------------------
    std::printf ("\nDefaults\n");
    plugin->setPlayConfigDetails (2, 2, 48000.0, 512);
    plugin->prepareToPlay (48000.0, 512);
    {
        juce::AudioBuffer<float> buffer (2, 512), original (2, 512);
        juce::MidiBuffer midi;
        for (int c = 0; c < 2; ++c)
            for (int i = 0; i < 512; ++i)
                original.setSample (c, i, 0.8f * (2.0f * u (rng) - 1.0f));
        buffer.makeCopyOf (original);
        plugin->processBlock (buffer, midi);
        bool same = true;
        for (int c = 0; c < 2; ++c)
            for (int i = 0; i < 512; ++i)
                same = same && buffer.getSample (c, i) == original.getSample (c, i);
        check (same, "with its default settings the audio passes bit for bit");
        check (plugin->getLatencySamples() == 0, "it reports no latency");
    }

    // --- sample rates / block sizes, with every parameter randomised between blocks --------------------------
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

    // A mono track. The bus layout is changed the way a host does it (setBusesLayout), and put back afterwards: changing only
    // the play config would leave the plugin's buses mono while the buffers are stereo.
    {
        // The second input is the sidechain (for the masking view only), off unless the host connects it.
        const auto layoutOf = [] (const juce::AudioChannelSet& set, const juce::AudioChannelSet& side = juce::AudioChannelSet::disabled())
        {
            juce::AudioProcessor::BusesLayout layout;
            layout.inputBuses.add (set);
            layout.inputBuses.add (side);
            layout.outputBuses.add (set);
            return layout;
        };
        check (plugin->getBusCount (true) == 2, "the plugin offers a sidechain input next to its main one");
        check (plugin->checkBusesLayoutSupported (layoutOf (juce::AudioChannelSet::stereo(), juce::AudioChannelSet::stereo())),
               "a stereo sidechain is accepted");
        const bool monoOk = plugin->checkBusesLayoutSupported (layoutOf (juce::AudioChannelSet::mono())) && plugin->setBusesLayout (layoutOf (juce::AudioChannelSet::mono()));
        check (monoOk, "a mono layout is accepted");
        plugin->setPlayConfigDetails (1, 1, 48000.0, 256);
        plugin->prepareToPlay (48000.0, 256);
        juce::AudioBuffer<float> mono (1, 256);
        juce::MidiBuffer midi;
        bool ok = true;
        for (int it = 0; it < 20 && ok; ++it)
        {
            if (it % 4 == 0)
                for (auto* p : plugin->getParameters())
                    p->setValue (u (rng));
            for (int i = 0; i < 256; ++i)
                mono.setSample (0, i, 0.5f * (2.0f * u (rng) - 1.0f));
            plugin->processBlock (mono, midi);
            ok = allFinite (mono);
        }
        check (ok, "a one-channel layout works with randomised parameters");
        plugin->releaseResources();
        check (plugin->setBusesLayout (layoutOf (juce::AudioChannelSet::stereo())), "and the stereo layout comes back");
    }

    // --- latency follows the phase mode ------------------------------------------------------------------------
    std::printf ("\nLatency reporting\n");
    if (auto* bypass = plugin->getBypassParameter()) // the fuzzing randomised the host's own Bypass too
        bypass->setValue (0.0f);
    plugin->setPlayConfigDetails (2, 2, 48000.0, 512);
    plugin->prepareToPlay (48000.0, 512);
    juce::AudioBuffer<float> buffer (2, 512);
    juce::MidiBuffer midi;
    {
        // A hosted VST3 exposes wrapper parameters: find the Phase choice by name, and put every band and the output stage back to rest.
        juce::AudioProcessorParameter* phase = nullptr;
        for (auto* p : plugin->getParameters())
        {
            const auto name = p->getName (100);
            if (name == "Phase")
                phase = p;
            else if (name.contains ("On") && name.startsWith ("Band"))
                p->setValue (0.0f);
            else if (name == "Gain Match")
                p->setValue (0.0f);
            else if (name == "Output Gain")
                p->setValue (0.5f);
        }
        check (phase != nullptr, "phase parameter found");
        if (phase != nullptr)
            for (int mode : { 0, 2, 1, 0 })
            {
                phase->setValueNotifyingHost ((float) mode / 2.0f);
                for (int i = 0; i < 3; ++i)
                {
                    buffer.clear();
                    plugin->processBlock (buffer, midi);
                    pump (100); // the latency is pushed to the host from the message thread
                }
                const int expected = mode == 0 ? 0 : mode == 1 ? 2304 : 9216;
                std::printf ("    phase='%s' -> reported latency %d samples\n", phase->getCurrentValueAsText().toRawUTF8(), plugin->getLatencySamples());
                check (plugin->getLatencySamples() == expected, "the reported latency is " + juce::String (expected));

                // What the audio really does: an impulse comes out exactly `latency` samples later, in blocks of any size.
                juce::AudioBuffer<float> long_ (2, 512);
                int peakAt = -1, seen = 0;
                float peakValue = 0.0f;
                for (int block = 0; block < (expected + 700) / 512 + 2; ++block)
                {
                    long_.clear();
                    if (block == 0)
                        long_.setSample (0, 100, 0.5f), long_.setSample (1, 100, 0.5f);
                    plugin->processBlock (long_, midi);
                    for (int i = 0; i < 512; ++i)
                        if (std::fabs (long_.getSample (0, i)) > peakValue)
                        {
                            peakValue = std::fabs (long_.getSample (0, i));
                            peakAt = seen + i;
                        }
                    seen += 512;
                }
                check (peakAt - 100 == expected, "an impulse comes out exactly " + juce::String (expected) + " samples later (measured " + juce::String (peakAt - 100) + ")");
            }
    }

    // --- state + presets ---------------------------------------------------------------------------------------
    std::printf ("\nState and presets\n");
    plugin->setPlayConfigDetails (2, 2, 48000.0, 512);
    plugin->prepareToPlay (48000.0, 512);

    for (auto* p : plugin->getParameters()) p->setValue (u (rng));
    // Compare displayed values: normalised floats are quantised differently by stepped parameters.
    std::vector<juce::String> before;
    for (auto* p : plugin->getParameters()) before.push_back (p->getCurrentValueAsText());

    juce::MemoryBlock state;
    plugin->getStateInformation (state);
    for (auto* p : plugin->getParameters()) p->setValue (u (rng)); // scramble
    plugin->setStateInformation (state.getData(), (int) state.getSize());

    bool same = true;
    int idx = 0;
    for (auto* p : plugin->getParameters())
    {
        const auto now = p->getCurrentValueAsText();
        if (now != before[(size_t) idx])
        {
            same = false;
            std::printf ("    mismatch: %s was '%s' now '%s'\n", p->getName (50).toRawUTF8(), before[(size_t) idx].toRawUTF8(), now.toRawUTF8());
        }
        ++idx;
    }
    check (same, "state save/restore round-trips all parameters (" + juce::String ((int) state.getSize()) + " bytes)");

    bool presetsOk = true;
    for (int i = 0; i < plugin->getNumPrograms(); ++i)
    {
        plugin->setCurrentProgram (i);
        presetsOk = presetsOk && plugin->getProgramName (i).isNotEmpty();
        for (int c = 0; c < 2; ++c)
            for (int s = 0; s < 512; ++s)
                buffer.setSample (c, s, 0.3f * (2.0f * u (rng) - 1.0f));
        plugin->processBlock (buffer, midi);
        presetsOk = presetsOk && allFinite (buffer);
    }
    check (presetsOk, "all " + juce::String (plugin->getNumPrograms()) + " presets load and process");

    // --- editor ------------------------------------------------------------------------------------------------
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
