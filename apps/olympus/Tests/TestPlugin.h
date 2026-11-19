// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 The Pantheon authors

#pragma once

#include "Catalog.h"

// Plugins made in code for the tests, so the session, the render and the server are checked without a built plugin:
// a gain with two presets, and the same with a latency (a delay that reports it, like a look-ahead limiter does).
namespace test
{
class GainPlugin : public juce::AudioPluginInstance
{
public:
    explicit GainPlugin (int latencyInSamples = 0) : GainPlugin (latencyInSamples > 0 ? "Test Delay" : "Test Gain", latencyInSamples) {}
    GainPlugin (const juce::String& pluginName, int latencyInSamples) : displayName (pluginName), latency (latencyInSamples)
    {
        auto param = std::make_unique<juce::AudioParameterFloat> (juce::ParameterID { "gain", 1 }, "Gain", juce::NormalisableRange<float> (-24.0f, 24.0f), 0.0f,
                                                                  juce::AudioParameterFloatAttributes().withLabel ("dB"));
        gain = param.get();
        addHostedParameter (std::move (param));
        setLatencySamples (latency);
        setPlayConfigDetails (2, 2, 48000.0, 512);
    }

    const juce::String getName() const override { return displayName; }
    void fillInPluginDescription (juce::PluginDescription& d) const override { d = description (displayName); }
    static juce::PluginDescription description (const juce::String& pluginName)
    {
        juce::PluginDescription d;
        d.name = pluginName;
        d.pluginFormatName = "Internal";
        d.fileOrIdentifier = d.name;
        d.uniqueId = pluginName.hashCode();
        d.numInputChannels = d.numOutputChannels = 2;
        return d;
    }

    void prepareToPlay (double, int) override { ring.setSize (2, juce::jmax (1, latency)); ring.clear(); pos = 0; }
    void releaseResources() override {}
    void reset() override { ring.clear(); pos = 0; }
    void processBlock (juce::AudioBuffer<float>& b, juce::MidiBuffer&) override
    {
        const auto g = juce::Decibels::decibelsToGain (gain->get());
        for (int ch = 0; ch < juce::jmin (2, b.getNumChannels()); ++ch)
        {
            auto* d = b.getWritePointer (ch);
            for (int i = 0; i < b.getNumSamples(); ++i)
            {
                if (latency > 0)
                {
                    auto* r = ring.getWritePointer (ch);
                    const float in = d[i];
                    d[i] = r[(pos + i) % latency];
                    r[(pos + i) % latency] = in;
                }
                d[i] *= g;
            }
        }
        if (latency > 0) pos = (pos + b.getNumSamples()) % latency;
    }
    using juce::AudioProcessor::processBlock;

    juce::AudioProcessorEditor* createEditor() override { return nullptr; }
    bool hasEditor() const override { return false; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }
    int getNumPrograms() override { return 2; }
    int getCurrentProgram() override { return program; }
    void setCurrentProgram (int i) override { program = i; *gain = i == 1 ? 6.0f : 0.0f; }
    const juce::String getProgramName (int i) override { return i == 1 ? "Boost" : "Flat"; }
    void changeProgramName (int, const juce::String&) override {}
    void getStateInformation (juce::MemoryBlock& m) override { m.setSize (0); juce::MemoryOutputStream o (m, false); o.writeFloat (gain->get()); o.writeInt (program); }
    void setStateInformation (const void* data, int size) override
    {
        juce::MemoryInputStream in (data, (size_t) size, false);
        *gain = in.readFloat();
        program = in.readInt();
    }

private:
    juce::AudioParameterFloat* gain = nullptr;
    juce::String displayName;
    int latency, program = 0, pos = 0;
    juce::AudioBuffer<float> ring;
};

// A plugin of this name that only changes the level (for the snapshot, where the names are the suite's), reporting `latency` samples.
inline void addStandIn (olympus::Catalog& catalog, const juce::String& name, int latency)
{
    catalog.addInternal (GainPlugin::description (name), [name, latency] (double, int) { return std::make_unique<GainPlugin> (name, latency); });
}

inline void addTestPlugins (olympus::Catalog& catalog)
{
    addStandIn (catalog, "Test Gain", 0);
    addStandIn (catalog, "Test Delay", 64);
}
}
