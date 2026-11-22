// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 The Pantheon authors

#pragma once

#include "Catalog.h"

#include <limits>

// Plugins made in code for the tests, so the session, the render and the server are checked without a built plugin:
// a gain with two presets, and the same with a latency (a delay that reports it, like a look-ahead limiter does).
namespace test
{
class GainPlugin : public juce::AudioPluginInstance
{
public:
    double preparedRate = 0.0; // the rate of the last prepareToPlay (the rate checks read it)
    bool emitNan = false;      // when set, one NaN at the start of every block it outputs (the render's sanitising checks); off by default
    bool rateLatency = false;  // when set, the latency follows the prepared rate (preparedRate / 100 samples); off by default
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

    void prepareToPlay (double rate, int) override
    {
        preparedRate = rate;
        if (rateLatency)
            setLatencySamples ((int) (rate / 100.0)); // getLatencySamples is not virtual here: the rate-dependent latency is reported, not overridden
        ring.setSize (2, juce::jmax (1, latency)); ring.clear(); pos = 0;
    }
    // Opt in while it sits in a chain: reports the latency of the rate it is prepared at now, and of every later prepare.
    void useRateLatency() { rateLatency = true; setLatencySamples ((int) (preparedRate / 100.0)); }
    void releaseResources() override {}
    void reset() override { ring.clear(); pos = 0; }
    // A plugin that changes its latency while it runs (a linear-phase switch, say): it reports the change to its listeners, as
    // setLatencySamples does, and its delay line follows.
    void changeLatency (int samples)
    {
        latency = samples;
        setLatencySamples (samples);
        ring.setSize (2, juce::jmax (1, samples));
        ring.clear();
        pos = 0;
    }
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
        if (emitNan && b.getNumSamples() > 0)
            b.setSample (0, 0, std::numeric_limits<float>::quiet_NaN());
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

// Plugins made in code with a channel layout of their own, for the bus checks. PassPlugin passes its block through and counts what it is
// handed: `shortBlocks` counts the blocks with fewer channels than the plugin's total input or output needs, which must stay 0.
class PassPlugin : public juce::AudioPluginInstance
{
public:
    PassPlugin (const juce::String& pluginName, juce::AudioChannelSet mainSet) : displayName (pluginName), main (mainSet) {}

    // the main pair is the one the plugin was made for: a stereo plugin refuses a mono layout, and the other way round
    bool isBusesLayoutSupported (const BusesLayout& layout) const override
    {
        return layout.getMainInputChannelSet() == main && layout.getMainOutputChannelSet() == main;
    }

    const juce::String getName() const override { return displayName; }
    void fillInPluginDescription (juce::PluginDescription& d) const override { d = GainPlugin::description (displayName); }
    void prepareToPlay (double rate, int) override { preparedRate = rate; }
    void releaseResources() override {}
    void reset() override {}
    void processBlock (juce::AudioBuffer<float>& b, juce::MidiBuffer&) override
    {
        ++blocks;
        lastChannels = b.getNumChannels();
        if (b.getNumChannels() < juce::jmax (getTotalNumInputChannels(), getTotalNumOutputChannels()))
            ++shortBlocks;
    }
    using juce::AudioProcessor::processBlock;

    juce::AudioProcessorEditor* createEditor() override { return nullptr; }
    bool hasEditor() const override { return false; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }
    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return "Default"; }
    void changeProgramName (int, const juce::String&) override {}
    void getStateInformation (juce::MemoryBlock& m) override { m.setSize (0); }
    void setStateInformation (const void*, int) override {}

    double preparedRate = 0.0;
    int blocks = 0, shortBlocks = 0, lastChannels = 0;

protected:
    // A plugin that picks a layout of its own (a programme that goes mono, say) applies both main buses at once; `isBusesLayoutSupported`
    // then refuses the layout the host asks for, which is what a real plugin that changes its buses from its state does.
    void selectMainLayout (const juce::AudioChannelSet& set)
    {
        main = set;
        BusesLayout layouts;
        layouts.inputBuses.add (set);
        layouts.outputBuses.add (set);
        applyBusLayouts (layouts);
    }

private:
    juce::String displayName;
    juce::AudioChannelSet main;
};

// Stereo, with a stereo sidechain input that is on by default (a bus added to the processor). The sidechain is switched off by the host's
// layout (Session::configure), so the plugin runs on its two channels.
class AuxPlugin : public PassPlugin
{
public:
    AuxPlugin() : PassPlugin ("Test Aux", juce::AudioChannelSet::stereo())
    {
        addBus (true);   // main input
        addBus (false);  // main output
        addBus (true);   // sidechain
    }
    bool canAddBus (bool) const override { return true; }
    bool canApplyBusCountChange (bool isInput, bool isAdding, BusProperties& out) override
    {
        if (! isAdding) return false;
        const bool sidechain = isInput && getBusCount (true) > 0;
        out.busName = sidechain ? "Sidechain" : (isInput ? "Input" : "Output");
        out.defaultLayout = juce::AudioChannelSet::stereo();
        out.isActivatedByDefault = true;
        return true;
    }
};

// Mono in and out: it does not run on a stereo track, and says so.
class MonoPlugin : public PassPlugin
{
public:
    MonoPlugin() : PassPlugin ("Test Mono", juce::AudioChannelSet::mono()) { setPlayConfigDetails (1, 1, 48000.0, 512); }
};

// Stereo until its saved state says otherwise: the state is one byte, 0 stereo and 1 mono, so a project can hold a plugin whose programme
// changed its bus layout (a stereo plugin that loads as mono). `goMono` is the same change while it sits in a chain.
class StateLayoutPlugin : public PassPlugin
{
public:
    StateLayoutPlugin() : PassPlugin ("Test Layout", juce::AudioChannelSet::stereo()) { setPlayConfigDetails (2, 2, 48000.0, 512); }
    void goMono (bool on) { selectMainLayout (on ? juce::AudioChannelSet::mono() : juce::AudioChannelSet::stereo()); }
    void getStateInformation (juce::MemoryBlock& m) override
    {
        m.setSize (0);
        juce::MemoryOutputStream o (m, false);
        o.writeByte (static_cast<juce::uint8> (getTotalNumInputChannels() == 1 ? 1 : 0));
    }
    void setStateInformation (const void* data, int size) override { goMono (size > 0 && ((const juce::uint8*) data)[0] != 0); }
};

// Stereo main input + stereo sidechain input (4 inputs, 2 outputs). Ducks main stereo output based on channels 2-3 amplitude.
class SidechainPlugin : public juce::AudioPluginInstance
{
public:
    SidechainPlugin()
        : juce::AudioPluginInstance (BusesProperties()
                                         .withInput ("Input", juce::AudioChannelSet::stereo(), true)
                                         .withInput ("Sidechain", juce::AudioChannelSet::stereo(), true)
                                         .withOutput ("Output", juce::AudioChannelSet::stereo(), true))
    {
        auto param = std::make_unique<juce::AudioParameterFloat> (juce::ParameterID { "amount", 1 }, "Amount",
                                                                  juce::NormalisableRange<float> (0.0f, 1.0f), 1.0f);
        amount = param.get();
        addHostedParameter (std::move (param));
    }

    bool isBusesLayoutSupported (const BusesLayout& layout) const override
    {
        if (layout.getMainInputChannelSet() != juce::AudioChannelSet::stereo()
            || layout.getMainOutputChannelSet() != juce::AudioChannelSet::stereo())
            return false;
        if (layout.inputBuses.size() > 1)
        {
            const auto& sc = layout.inputBuses.getReference (1);
            if (! sc.isDisabled() && sc != juce::AudioChannelSet::stereo())
                return false;
        }
        return layout.outputBuses.size() <= 1;
    }

    const juce::String getName() const override { return "Test Sidechain"; }
    void fillInPluginDescription (juce::PluginDescription& d) const override
    {
        d = GainPlugin::description ("Test Sidechain");
        d.numInputChannels = 4;
        d.numOutputChannels = 2;
    }
    void prepareToPlay (double rate, int) override { preparedRate = rate; }
    void releaseResources() override {}
    void reset() override {}
    void processBlock (juce::AudioBuffer<float>& b, juce::MidiBuffer&) override
    {
        const int n = b.getNumSamples();
        if (b.getNumChannels() < 4 || getChannelCountOfBus (true, 1) < 2)
            return;
        const float amt = amount != nullptr ? amount->get() : 1.0f;
        auto* l = b.getWritePointer (0);
        auto* r = b.getWritePointer (1);
        const auto* scL = b.getReadPointer (2);
        const auto* scR = b.getReadPointer (3);
        for (int i = 0; i < n; ++i)
        {
            const float scAmp = 0.5f * (std::abs (scL[i]) + std::abs (scR[i]));
            const float duck = 1.0f / (1.0f + amt * scAmp * 10.0f);
            l[i] *= duck;
            r[i] *= duck;
        }
    }
    using juce::AudioProcessor::processBlock;

    juce::AudioProcessorEditor* createEditor() override { return nullptr; }
    bool hasEditor() const override { return false; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }
    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return "Default"; }
    void changeProgramName (int, const juce::String&) override {}
    void getStateInformation (juce::MemoryBlock& m) override
    {
        m.setSize (0);
        juce::MemoryOutputStream o (m, false);
        o.writeFloat (amount != nullptr ? amount->get() : 1.0f);
    }
    void setStateInformation (const void* data, int size) override
    {
        if (amount != nullptr && size >= (int) sizeof (float))
        {
            juce::MemoryInputStream in (data, (size_t) size, false);
            *amount = juce::jlimit (0.0f, 1.0f, in.readFloat());
        }
    }

    double preparedRate = 0.0;

private:
    juce::AudioParameterFloat* amount = nullptr;
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

// The two bus plugins, for the checks that need them (not in addTestPlugins: the catalog's counts of the other checks stay as they are).
inline void addBusPlugins (olympus::Catalog& catalog)
{
    catalog.addInternal (GainPlugin::description ("Test Aux"), [] (double, int) { return std::make_unique<AuxPlugin>(); });
    catalog.addInternal (GainPlugin::description ("Test Mono"), [] (double, int) { return std::make_unique<MonoPlugin>(); });
}

// The plugin whose state chooses its layout, for the checks that need it (its own function, so the catalog's counts stay as they are).
inline void addLayoutPlugin (olympus::Catalog& catalog)
{
    catalog.addInternal (GainPlugin::description ("Test Layout"), [] (double, int) { return std::make_unique<StateLayoutPlugin>(); });
}

// Sidechain ducking plugin for sidechain routing tests (kept out of addTestPlugins so existing catalog counts stay unchanged).
inline void addSidechainPlugin (olympus::Catalog& catalog)
{
    juce::PluginDescription d = GainPlugin::description ("Test Sidechain");
    d.numInputChannels = 4;
    d.numOutputChannels = 2;
    catalog.addInternal (d, [] (double, int) { return std::make_unique<SidechainPlugin>(); });
}
}
