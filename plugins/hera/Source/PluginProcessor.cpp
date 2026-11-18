// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 The Pantheon authors

#include "PluginProcessor.h"
#include "PluginEditor.h"
#include <pantheon/ui/Scaled.h>

HeraProcessor::HeraProcessor()
    : AudioProcessor (BusesProperties()
                          .withInput  ("Input",  juce::AudioChannelSet::stereo(), true)
                          .withOutput ("Output", juce::AudioChannelSet::stereo(), true)
                          .withInput  ("Sidechain", juce::AudioChannelSet::stereo(), false)),
      apvts (*this, &undo, "STATE", hera::createLayout())
{
}

bool HeraProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto& in = layouts.getMainInputChannelSet();
    const auto side = layouts.inputBuses.size() > 1 ? layouts.getChannelSet (true, 1) : juce::AudioChannelSet::disabled();
    const bool sideOk = side.isDisabled() || side == juce::AudioChannelSet::stereo() || side == juce::AudioChannelSet::mono();
    return in == layouts.getMainOutputChannelSet() && (in == juce::AudioChannelSet::stereo() || in == juce::AudioChannelSet::mono()) && sideOk;
}

void HeraProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    engine.prepare (sampleRate, hera::readParams (apvts).lookahead);
    bypassDelay.prepare (2, engine.maxLatencySamples()); // the longest look-ahead
    monoRight.assign ((size_t) std::max (samplesPerBlock, 1), 0.0f);
    reportedLatency = engine.latencySamples.load();
    setLatencySamples (reportedLatency.load());
}

void HeraProcessor::handleAsyncUpdate()
{
    reportedLatency = engine.latencySamples.load();
    setLatencySamples (reportedLatency.load());
}

void HeraProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;
    const auto params = hera::readParams (apvts);
    // The sidechain is only listened to, by the detector, when KEY is on: it is never heard.
    // Only pointers are kept (no AudioBuffer is copied or assigned here: nothing on this path may allocate).
    const float* scL = nullptr;
    const float* scR = nullptr;
    if (getBusCount (true) > 1 && getBus (true, 1)->isEnabled())
    {
        const auto side = getBusBuffer (buffer, true, 1);
        scL = side.getNumChannels() > 0 ? side.getReadPointer (0) : nullptr;
        scR = side.getNumChannels() > 1 ? side.getReadPointer (1) : nullptr;
    }
    sidechainConnected.store (scL != nullptr, std::memory_order_relaxed);

    auto main = getBusBuffer (buffer, false, 0);
    const int n = main.getNumSamples();
    if (main.getNumChannels() >= 2)
        engine.process (main.getWritePointer (0), main.getWritePointer (1), n, params, scL, scR);
    else if (main.getNumChannels() == 1)
    {
        // Mono: both channels of the engine get the signal, in pieces the scratch channel holds (a host may send more than it promised).
        auto* x = main.getWritePointer (0);
        for (int start = 0; start < n;)
        {
            const int m = std::min (n - start, (int) monoRight.size());
            if (m <= 0)
                break; // not prepared: nothing to chunk with, and the loop would never advance
            std::copy (x + start, x + start + m, monoRight.begin());
            engine.process (x + start, monoRight.data(), m, params, scL != nullptr ? scL + start : nullptr, scR != nullptr ? scR + start : nullptr);
            start += m;
        }
    }
    if (engine.latencySamples.load (std::memory_order_relaxed) != reportedLatency)
        triggerAsyncUpdate();
}

void HeraProcessor::processBlockBypassed (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;
    auto main = getBusBuffer (buffer, false, 0);
    bypassDelay.process (main.getArrayOfWritePointers(), main.getNumChannels(), main.getNumSamples(), getLatencySamples());
    // The only output is the main bus (the sidechain is an input, never heard), so there is nothing else to clear.
}

void HeraProcessor::getStateInformation (juce::MemoryBlock& dest)
{
    programs.saveTo (apvts.state);
    ab.saveTo (apvts.state);
    if (auto xml = apvts.copyState().createXml())
        copyXmlToBinary (*xml, dest);
}

void HeraProcessor::setStateInformation (const void* data, int size)
{
    if (auto xml = getXmlFromBinary (data, size))
        if (xml->hasTagName (apvts.state.getType()))
        {
            auto tree = juce::ValueTree::fromXml (*xml);
            ab.loadFrom (tree); // a session from before A / B has no abSlot: it opens on slot 0
            apvts.replaceState (tree);
            programs.loadFrom (apvts.state);
        }
}

juce::AudioProcessorEditor* HeraProcessor::createEditor() { return new ui::ScaledEditor<HeraEditor> (*this, apvts.state, *this); }

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new HeraProcessor(); }
