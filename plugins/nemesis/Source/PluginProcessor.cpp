// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 The Pantheon authors

#include "PluginProcessor.h"
#include "PluginEditor.h"
#include <pantheon/ui/Scaled.h>

NemesisProcessor::NemesisProcessor()
    : AudioProcessor (BusesProperties()
                          .withInput  ("Input",  juce::AudioChannelSet::stereo(), true)
                          .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "STATE", nemesis::createLayout())
{
}

bool NemesisProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto& in = layouts.getMainInputChannelSet();
    return in == layouts.getMainOutputChannelSet() && (in == juce::AudioChannelSet::stereo() || in == juce::AudioChannelSet::mono());
}

void NemesisProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    engine.prepare (sampleRate, nemesis::readParams (apvts).lookahead);
    monoRight.assign ((size_t) std::max (samplesPerBlock, 1), 0.0f);
    reportedLatency = engine.latencySamples.load();
    setLatencySamples (reportedLatency.load());
}

void NemesisProcessor::handleAsyncUpdate()
{
    reportedLatency = engine.latencySamples.load();
    setLatencySamples (reportedLatency.load());
}

void NemesisProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;
    const auto params = nemesis::readParams (apvts);
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
    if (engine.latencySamples.load (std::memory_order_relaxed) != reportedLatency)
        triggerAsyncUpdate();
}

void NemesisProcessor::getStateInformation (juce::MemoryBlock& dest)
{
    programs.saveTo (apvts.state);
    if (auto xml = apvts.copyState().createXml())
        copyXmlToBinary (*xml, dest);
}

void NemesisProcessor::setStateInformation (const void* data, int size)
{
    if (auto xml = getXmlFromBinary (data, size))
        if (xml->hasTagName (apvts.state.getType()))
        {
            apvts.replaceState (juce::ValueTree::fromXml (*xml));
            programs.loadFrom (apvts.state);
        }
}

juce::AudioProcessorEditor* NemesisProcessor::createEditor() { return new ui::ScaledEditor<NemesisEditor> (*this, apvts.state, *this); }

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new NemesisProcessor(); }
