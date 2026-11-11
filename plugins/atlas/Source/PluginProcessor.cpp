// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 The Pantheon authors

#include "PluginProcessor.h"
#include "PluginEditor.h"

AtlasProcessor::AtlasProcessor()
    : AudioProcessor (BusesProperties()
                          .withInput  ("Input",  juce::AudioChannelSet::stereo(), true)
                          .withOutput ("Output", juce::AudioChannelSet::stereo(), true)
                          .withInput  ("Sidechain", juce::AudioChannelSet::stereo(), false)),
      apvts (*this, nullptr, "STATE", atlas::createLayout())
{
}

bool AtlasProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto& in = layouts.getMainInputChannelSet();
    const auto side = layouts.inputBuses.size() > 1 ? layouts.getChannelSet (true, 1) : juce::AudioChannelSet::disabled();
    const bool sideOk = side.isDisabled() || side == juce::AudioChannelSet::stereo() || side == juce::AudioChannelSet::mono();
    return in == layouts.getMainOutputChannelSet() && (in == juce::AudioChannelSet::stereo() || in == juce::AudioChannelSet::mono()) && sideOk;
}

void AtlasProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    const auto p = atlas::readParams (apvts);
    engine.prepare (sampleRate, p.phase, p.oversampling, p.lookahead);
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

    // The sidechain is only listened to, by the bands set to it: it is never heard.
    juce::AudioBuffer<float> side;
    if (getBusCount (true) > 1 && getBus (true, 1)->isEnabled())
        side = getBusBuffer (buffer, true, 1);
    sidechainConnected.store (side.getNumChannels() > 0, std::memory_order_relaxed);
    const float* scL = side.getNumChannels() > 0 ? side.getReadPointer (0) : nullptr;
    const float* scR = side.getNumChannels() > 1 ? side.getReadPointer (1) : nullptr;

    auto main = getBusBuffer (buffer, false, 0);
    const int n = main.getNumSamples();
    if (main.getNumChannels() > 0)
        tapIn.push (main.getArrayOfReadPointers(), main.getNumChannels(), n);
    if (main.getNumChannels() >= 2)
    {
        engine.process (main.getWritePointer (0), main.getWritePointer (1), n, params, scL, scR);
    }
    else if (main.getNumChannels() == 1)
    {
        // Mono: both channels of the engine get the signal, in pieces the scratch channel holds (a host may send more than it promised).
        auto* x = main.getWritePointer (0);
        for (int start = 0; start < n;)
        {
            const int m = std::min (n - start, (int) monoRight.size());
            std::copy (x + start, x + start + m, monoRight.begin());
            engine.process (x + start, monoRight.data(), m, params, scL != nullptr ? scL + start : nullptr, scR != nullptr ? scR + start : nullptr);
            start += m;
        }
    }
    if (engine.latencySamples.load (std::memory_order_relaxed) != reportedLatency)
        triggerAsyncUpdate();
}

void AtlasProcessor::setCurrentProgram (int index)
{
    currentProgram = index;
    atlas::applyPreset (apvts, index);
}

const juce::String AtlasProcessor::getProgramName (int index)
{
    const auto& list = atlas::presets();
    return juce::isPositiveAndBelow (index, (int) list.size()) ? juce::String (list[(size_t) index].name) : juce::String();
}

void AtlasProcessor::getStateInformation (juce::MemoryBlock& dest)
{
    auto state = apvts.copyState();
    state.setProperty ("program", currentProgram, nullptr);
    if (auto xml = state.createXml())
        copyXmlToBinary (*xml, dest);
}

void AtlasProcessor::setStateInformation (const void* data, int size)
{
    if (auto xml = getXmlFromBinary (data, size))
        if (xml->hasTagName (apvts.state.getType()))
        {
            auto tree = juce::ValueTree::fromXml (*xml);
            currentProgram = (int) tree.getProperty ("program", 0);
            apvts.replaceState (tree);
        }
}

juce::AudioProcessorEditor* AtlasProcessor::createEditor() { return new AtlasEditor (*this); }

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new AtlasProcessor(); }
