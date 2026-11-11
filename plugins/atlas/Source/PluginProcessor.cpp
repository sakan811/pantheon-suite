// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 The Pantheon authors

#include "PluginProcessor.h"
#include "PluginEditor.h"

AtlasProcessor::AtlasProcessor()
    : AudioProcessor (BusesProperties()
                          .withInput  ("Input",  juce::AudioChannelSet::stereo(), true)
                          .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "STATE", atlas::createLayout())
{
}

bool AtlasProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto& in = layouts.getMainInputChannelSet();
    return in == layouts.getMainOutputChannelSet() && (in == juce::AudioChannelSet::stereo() || in == juce::AudioChannelSet::mono());
}

void AtlasProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    const auto p = atlas::readParams (apvts);
    engine.prepare (sampleRate, p.phase, p.oversampling);
    monoRight.assign ((size_t) std::max (samplesPerBlock, 1), 0.0f);
    reportedLatency = engine.latencySamples.load();
    setLatencySamples (reportedLatency.load());
}

void AtlasProcessor::handleAsyncUpdate()
{
    reportedLatency = engine.latencySamples.load();
    setLatencySamples (reportedLatency.load());
}

void AtlasProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;
    const auto params = atlas::readParams (apvts);
    const int n = buffer.getNumSamples();
    if (buffer.getNumChannels() >= 2)
    {
        engine.process (buffer.getWritePointer (0), buffer.getWritePointer (1), n, params);
    }
    else if (buffer.getNumChannels() == 1)
    {
        // Mono: both channels of the engine get the signal, in pieces the scratch channel holds (a host may send more than it promised).
        auto* x = buffer.getWritePointer (0);
        for (int start = 0; start < n;)
        {
            const int m = std::min (n - start, (int) monoRight.size());
            std::copy (x + start, x + start + m, monoRight.begin());
            engine.process (x + start, monoRight.data(), m, params);
            start += m;
        }
    }
    if (engine.latencySamples.load (std::memory_order_relaxed) != reportedLatency)
        triggerAsyncUpdate();
}

void AtlasProcessor::getStateInformation (juce::MemoryBlock& dest)
{
    if (auto xml = apvts.copyState().createXml())
        copyXmlToBinary (*xml, dest);
}

void AtlasProcessor::setStateInformation (const void* data, int size)
{
    if (auto xml = getXmlFromBinary (data, size))
        if (xml->hasTagName (apvts.state.getType()))
            apvts.replaceState (juce::ValueTree::fromXml (*xml));
}

juce::AudioProcessorEditor* AtlasProcessor::createEditor() { return new AtlasEditor (*this); }

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new AtlasProcessor(); }
