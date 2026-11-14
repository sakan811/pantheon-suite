// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 The Pantheon authors

#include "PluginProcessor.h"
#include "PluginEditor.h"
#include <pantheon/ui/Scaled.h>

HephaestusProcessor::HephaestusProcessor()
    : AudioProcessor (BusesProperties()
                          .withInput  ("Input",  juce::AudioChannelSet::stereo(), true)
                          .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, &undo, "STATE", hep::createLayout())
{
}

bool HephaestusProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto& in = layouts.getMainInputChannelSet();
    const auto& out = layouts.getMainOutputChannelSet();
    return in == out && (in == juce::AudioChannelSet::stereo() || in == juce::AudioChannelSet::mono());
}

void HephaestusProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    currentSampleRate = sampleRate;
    engine.prepare (sampleRate, std::max (samplesPerBlock, 64), (int) apvts.getRawParameterValue (hep::id::oversampling)->load(),
                    hep::usesOctave (hep::readParams (apvts)));
    bypassDelay.prepare (std::max (getTotalNumInputChannels(), hep::Engine::maxChannels), engine.getMaxLatency());
    reportedLatency = -1;
    requestedLatency = engine.getLatency();
    triggerAsyncUpdate();
}

void HephaestusProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;

    for (int c = getTotalNumInputChannels(); c < getTotalNumOutputChannels(); ++c)
        buffer.clear (c, 0, buffer.getNumSamples());

    engine.process (buffer, hep::readParams (apvts));

    // The oversampling mode (hence the latency) can change from the UI or automation.
    // setLatencySamples must not be called from the audio thread, so hand it to the message thread.
    // Once per change: while the message thread has not caught up the value is the same, so the block does not ask again.
    const int latency = engine.getLatency();
    if (latency != requestedLatency.load (std::memory_order_relaxed))
    {
        requestedLatency.store (latency, std::memory_order_relaxed);
        triggerAsyncUpdate();
    }
}

void HephaestusProcessor::processBlockBypassed (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    for (int c = getTotalNumInputChannels(); c < getTotalNumOutputChannels(); ++c)
        buffer.clear (c, 0, buffer.getNumSamples());
    bypassDelay.process (buffer.getArrayOfWritePointers(), std::min (buffer.getNumChannels(), getTotalNumInputChannels()), buffer.getNumSamples(),
                         getLatencySamples());
}

void HephaestusProcessor::handleAsyncUpdate()
{
    const int latency = engine.getLatency();
    requestedLatency = latency;
    if (latency != reportedLatency.load())
    {
        reportedLatency = latency;
        setLatencySamples (latency);
    }
}

void HephaestusProcessor::setCurrentProgram (int index)
{
    currentProgram = index;
    currentUserPreset = {};
    hep::applyPreset (apvts, index);
}

const juce::String HephaestusProcessor::getProgramName (int index)
{
    const auto& list = hep::presets();
    return juce::isPositiveAndBelow (index, (int) list.size()) ? juce::String (list[(size_t) index].name)
                                                                : juce::String();
}

void HephaestusProcessor::getStateInformation (juce::MemoryBlock& dest)
{
    auto state = apvts.copyState();
    state.setProperty ("program", currentProgram, nullptr);
    state.setProperty ("userPreset", currentUserPreset, nullptr);
    ab.saveTo (state);
    if (auto xml = state.createXml())
        copyXmlToBinary (*xml, dest);
}

void HephaestusProcessor::setStateInformation (const void* data, int size)
{
    if (auto xml = getXmlFromBinary (data, size))
        if (xml->hasTagName (apvts.state.getType()))
        {
            auto tree = juce::ValueTree::fromXml (*xml);
            hep::migrateLegacyState (tree);
            currentProgram = (int) tree.getProperty ("program", 0);
            currentUserPreset = tree.getProperty ("userPreset", "").toString();
            ab.loadFrom (tree);
            apvts.replaceState (tree);
        }
}

juce::AudioProcessorEditor* HephaestusProcessor::createEditor()
{
    return new ui::ScaledEditor<HephaestusEditor> (*this, apvts.state, *this);
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new HephaestusProcessor();
}
