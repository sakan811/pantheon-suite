// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 The Pantheon authors

#include "PluginProcessor.h"
#include "PluginEditor.h"

HephaestusProcessor::HephaestusProcessor()
    : AudioProcessor (BusesProperties()
                          .withInput  ("Input",  juce::AudioChannelSet::stereo(), true)
                          .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "STATE", hep::createLayout())
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
    reportedLatency = -1;
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
    if (engine.getLatency() != reportedLatency)
        triggerAsyncUpdate();
}

void HephaestusProcessor::handleAsyncUpdate()
{
    const int latency = engine.getLatency();
    if (latency != reportedLatency)
    {
        reportedLatency = latency;
        setLatencySamples (latency);
    }
}

void HephaestusProcessor::setCurrentProgram (int index)
{
    currentProgram = index;
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
            apvts.replaceState (tree);
        }
}

juce::AudioProcessorEditor* HephaestusProcessor::createEditor()
{
    return new HephaestusEditor (*this);
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new HephaestusProcessor();
}
