// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 The Pantheon authors

#include "PluginProcessor.h"
#include "PluginEditor.h"

HestiaProcessor::HestiaProcessor()
    : AudioProcessor (BusesProperties()
                          .withInput  ("Input",  juce::AudioChannelSet::stereo(), true)
                          .withOutput ("Output", juce::AudioChannelSet::stereo(), true)
                          .withInput  ("Sidechain", juce::AudioChannelSet::stereo(), false)),
      apvts (*this, nullptr, "STATE", hestia::createLayout())
{
}

bool HestiaProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto& in = layouts.getMainInputChannelSet();
    const auto side = layouts.inputBuses.size() > 1 ? layouts.getChannelSet (true, 1) : juce::AudioChannelSet::disabled();
    const bool sideOk = side.isDisabled() || side == juce::AudioChannelSet::stereo() || side == juce::AudioChannelSet::mono();
    return in == layouts.getMainOutputChannelSet() && (in == juce::AudioChannelSet::stereo() || in == juce::AudioChannelSet::mono()) && sideOk;
}

void HestiaProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    engine.prepare (sampleRate);
    monoRight.assign ((size_t) std::max (samplesPerBlock, 1), 0.0f);
}

void HestiaProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;
    const auto params = hestia::readParams (apvts);
    const auto now = (std::uint32_t) juce::Time::getMillisecondCounter();

    // The sidechain is only listened to: it is one more stem of the mix, and it is never heard.
    const bool hasSide = getBusCount (true) > 1 && getBus (true, 1)->isEnabled();
    auto side = hasSide ? getBusBuffer (buffer, true, 1) : juce::AudioBuffer<float>();
    sidechainConnected.store (side.getNumChannels() > 0, std::memory_order_relaxed);
    const float* scL = side.getNumChannels() > 0 ? side.getReadPointer (0) : nullptr;
    const float* scR = side.getNumChannels() > 1 ? side.getReadPointer (1) : nullptr;

    auto main = getBusBuffer (buffer, false, 0);
    const int n = main.getNumSamples();
    if (main.getNumChannels() >= 2)
    {
        engine.process (main.getWritePointer (0), main.getWritePointer (1), n, params, now, scL, scR);
    }
    else if (main.getNumChannels() == 1)
    {
        // Mono: both channels of the engine get the signal, in pieces the scratch channel holds (a host may send more than it promised).
        auto* x = main.getWritePointer (0);
        for (int start = 0; start < n;)
        {
            const int m = std::min (n - start, (int) monoRight.size());
            std::copy (x + start, x + start + m, monoRight.begin());
            engine.process (x + start, monoRight.data(), m, params, now, scL != nullptr ? scL + start : nullptr, scR != nullptr ? scR + start : nullptr);
            start += m;
        }
    }
}

void HestiaProcessor::getStateInformation (juce::MemoryBlock& dest)
{
    if (auto xml = apvts.copyState().createXml())
        copyXmlToBinary (*xml, dest);
}

void HestiaProcessor::setStateInformation (const void* data, int size)
{
    if (auto xml = getXmlFromBinary (data, size))
        if (xml->hasTagName (apvts.state.getType()))
            apvts.replaceState (juce::ValueTree::fromXml (*xml));
}

juce::AudioProcessorEditor* HestiaProcessor::createEditor() { return new HestiaEditor (*this); }

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new HestiaProcessor(); }
