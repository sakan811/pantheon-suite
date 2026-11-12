// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 The Pantheon authors

#include "PluginProcessor.h"
#include "PluginEditor.h"
#include <pantheon/ui/Scaled.h>

CastorProcessor::CastorProcessor()
    : AudioProcessor (BusesProperties()
                          .withInput  ("Input",  juce::AudioChannelSet::stereo(), true)
                          .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "STATE", castor::createLayout())
{
}

bool CastorProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto& in = layouts.getMainInputChannelSet();
    return in == layouts.getMainOutputChannelSet() && (in == juce::AudioChannelSet::stereo() || in == juce::AudioChannelSet::mono());
}

void CastorProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    engine.prepare (sampleRate);
    monoRight.assign ((size_t) std::max (samplesPerBlock, 1), 0.0f);
    setLatencySamples (0);
}

void CastorProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;
    const auto params = castor::readParams (apvts);
    auto main = getBusBuffer (buffer, false, 0);
    const int n = main.getNumSamples();
    if (main.getNumChannels() >= 2)
        engine.process (main.getWritePointer (0), main.getWritePointer (1), n, params);
    else if (main.getNumChannels() == 1)
    {
        // Mono: both channels of the engine get the signal, in pieces the scratch channel holds (a host may send more than it promised).
        auto* x = main.getWritePointer (0);
        for (int start = 0; start < n;)
        {
            const int m = std::min (n - start, (int) monoRight.size());
            std::copy (x + start, x + start + m, monoRight.begin());
            engine.process (x + start, monoRight.data(), m, params);
            start += m;
        }
    }
    if (main.getNumChannels() > 0)
        tapOut.push (main.getArrayOfReadPointers(), main.getNumChannels(), n);
}

void CastorProcessor::getStateInformation (juce::MemoryBlock& dest)
{
    if (auto xml = apvts.copyState().createXml())
        copyXmlToBinary (*xml, dest);
}

void CastorProcessor::setStateInformation (const void* data, int size)
{
    if (auto xml = getXmlFromBinary (data, size))
        if (xml->hasTagName (apvts.state.getType()))
            apvts.replaceState (juce::ValueTree::fromXml (*xml));
}

juce::AudioProcessorEditor* CastorProcessor::createEditor() { return new ui::ScaledEditor<CastorEditor> (*this, apvts.state, *this); }

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new CastorProcessor(); }
