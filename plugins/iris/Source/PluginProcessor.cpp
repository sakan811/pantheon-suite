// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 The Pantheon authors

#include "PluginProcessor.h"
#include "PluginEditor.h"
#include <pantheon/ui/Scaled.h>

IrisProcessor::IrisProcessor()
    : AudioProcessor (BusesProperties()
                          .withInput  ("Input",  juce::AudioChannelSet::stereo(), true)
                          .withOutput ("Output", juce::AudioChannelSet::stereo(), true)
                          .withInput  ("Sidechain", juce::AudioChannelSet::stereo(), false)),
      apvts (*this, nullptr, "STATE", iris::createLayout())
{
}

bool IrisProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto& in = layouts.getMainInputChannelSet();
    const auto& out = layouts.getMainOutputChannelSet();
    const auto side = layouts.inputBuses.size() > 1 ? layouts.getChannelSet (true, 1) : juce::AudioChannelSet::disabled();
    const bool sideOk = side.isDisabled() || side == juce::AudioChannelSet::stereo() || side == juce::AudioChannelSet::mono();
    return in == out && (in == juce::AudioChannelSet::stereo() || in == juce::AudioChannelSet::mono()) && sideOk;
}

void IrisProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    currentSampleRate = sampleRate;
    engine.prepare (sampleRate, samplesPerBlock, iris::readParams (apvts).phase);
    reportedLatency = engine.latencySamples.load();
    setLatencySamples (reportedLatency.load());
}

void IrisProcessor::handleAsyncUpdate()
{
    reportedLatency = engine.latencySamples.load();
    setLatencySamples (reportedLatency.load());
}

void IrisProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;

    // The sidechain is only listened to (the masking view, and the dynamic bands set to listen to it); it is never heard.
    juce::AudioBuffer<float> side;
    if (getBusCount (true) > 1 && getBus (true, 1)->isEnabled())
    {
        side = getBusBuffer (buffer, true, 1);
        if (side.getNumChannels() > 0)
            sidechainTap.push (side.getArrayOfReadPointers(), side.getNumChannels(), side.getNumSamples());
    }
    sidechainConnected.store (side.getNumChannels() > 0, std::memory_order_relaxed);

    auto main = getBusBuffer (buffer, false, 0);
    const int mainIn = getMainBusNumInputChannels();
    for (int c = mainIn; c < main.getNumChannels(); ++c)
        main.clear (c, 0, main.getNumSamples());

    engine.process (main, iris::readParams (apvts), side.getNumChannels() > 0 ? &side : nullptr);
    if (main.getNumChannels() > 0)
    {
        const float* out[2] = { main.getReadPointer (0), main.getReadPointer (main.getNumChannels() > 1 ? 1 : 0) };
        peer.publish (out, main.getNumChannels(), main.getNumSamples(), engine.outPeak.load (std::memory_order_relaxed));
    }
    if (engine.latencySamples.load (std::memory_order_relaxed) != reportedLatency)
        triggerAsyncUpdate();
}

void IrisProcessor::setCurrentProgram (int index)
{
    currentProgram = index;
    iris::applyPreset (apvts, index);
}

const juce::String IrisProcessor::getProgramName (int index)
{
    const auto& list = iris::presets();
    return juce::isPositiveAndBelow (index, (int) list.size()) ? juce::String (list[(size_t) index].name) : juce::String();
}

void IrisProcessor::getStateInformation (juce::MemoryBlock& dest)
{
    auto state = apvts.copyState();
    state.setProperty ("program", currentProgram, nullptr);
    if (auto xml = state.createXml())
        copyXmlToBinary (*xml, dest);
}

void IrisProcessor::setStateInformation (const void* data, int size)
{
    if (auto xml = getXmlFromBinary (data, size))
        if (xml->hasTagName (apvts.state.getType()))
        {
            auto tree = juce::ValueTree::fromXml (*xml);
            currentProgram = (int) tree.getProperty ("program", 0);
            apvts.replaceState (tree);
        }
}

juce::AudioProcessorEditor* IrisProcessor::createEditor()
{
    return new ui::ScaledEditor<IrisEditor> (*this, apvts.state, *this);
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new IrisProcessor();
}
