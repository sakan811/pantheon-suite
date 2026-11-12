// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 The Pantheon authors

#include "PluginProcessor.h"
#include "PluginEditor.h"
#include <pantheon/ui/Scaled.h>

HermesProcessor::HermesProcessor()
    : AudioProcessor (BusesProperties()
                          .withInput  ("Input",  juce::AudioChannelSet::stereo(), true)
                          .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "STATE", hermes::createLayout())
{
    formats.registerBasicFormats();
}

bool HermesProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto& in = layouts.getMainInputChannelSet();
    return in == layouts.getMainOutputChannelSet() && (in == juce::AudioChannelSet::stereo() || in == juce::AudioChannelSet::mono());
}

void HermesProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    engine.prepare (sampleRate);
    monoRight.assign ((size_t) std::max (samplesPerBlock, 1), 0.0f);
    setLatencySamples (0);
    // A reference made for another sample rate is made again for this one.
    if (referencePath.isNotEmpty() && std::fabs (referenceRate - sampleRate) > 0.5)
        juce::MessageManager::callAsync ([weak = juce::WeakReference<HermesProcessor> (this), path = referencePath] { if (weak != nullptr) weak->loadReference (juce::File (path)); });
}

void HermesProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;
    const auto params = hermes::readParams (apvts);
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
}

void HermesProcessor::getStateInformation (juce::MemoryBlock& dest)
{
    auto state = apvts.copyState();
    state.setProperty ("refPath", referencePath, nullptr);
    if (auto xml = state.createXml())
        copyXmlToBinary (*xml, dest);
}

void HermesProcessor::setStateInformation (const void* data, int size)
{
    if (auto xml = getXmlFromBinary (data, size))
        if (xml->hasTagName (apvts.state.getType()))
        {
            auto tree = juce::ValueTree::fromXml (*xml);
            const auto path = tree.getProperty ("refPath").toString();
            apvts.replaceState (tree);
            if (path.isNotEmpty() && path != referencePath && juce::File (path).existsAsFile())
                juce::MessageManager::callAsync ([weak = juce::WeakReference<HermesProcessor> (this), path] { if (weak != nullptr) weak->loadReference (juce::File (path)); });
        }
}

juce::String HermesProcessor::loadReference (const juce::File& file)
{
    std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (file));
    if (reader == nullptr)
        return "Not an audio file this plug-in can read (WAV, AIFF, FLAC, OGG, MP3 where the system has it).";
    const double rate = getSampleRate() > 0.0 ? getSampleRate() : reader->sampleRate;
    if ((double) reader->lengthInSamples > kMaxReferenceMinutes * 60.0 * reader->sampleRate)
        return "The file is longer than " + juce::String ((int) kMaxReferenceMinutes) + " minutes: it is kept whole in memory.";
    const int length = (int) reader->lengthInSamples;
    juce::AudioBuffer<float> source (2, std::max (length, 1));
    reader->read (&source, 0, length, 0, true, true);
    if (reader->numChannels < 2)
        source.copyFrom (1, 0, source, 0, 0, length); // a mono file is both channels

    auto made = std::make_unique<hermes::Reference>();
    const int outLength = rate == reader->sampleRate ? length : (int) std::ceil ((double) length * rate / reader->sampleRate);
    made->left.resize ((size_t) outLength);
    made->right.resize ((size_t) outLength);
    if (rate == reader->sampleRate)
    {
        std::copy (source.getReadPointer (0), source.getReadPointer (0) + length, made->left.begin());
        std::copy (source.getReadPointer (1), source.getReadPointer (1) + length, made->right.begin());
    }
    else
    {
        juce::LagrangeInterpolator a, b;
        a.process (reader->sampleRate / rate, source.getReadPointer (0), made->left.data(), outLength);
        b.process (reader->sampleRate / rate, source.getReadPointer (1), made->right.data(), outLength);
    }
    if (outLength < 4800)
        return "The file is shorter than 100 ms.";
    made->integratedLufs = hermes::measureIntegrated (made->left, made->right, rate);

    loaded.push_back (std::move (made));
    engine.setReference (loaded.back().get());
    while (loaded.size() > 2)
        loaded.erase (loaded.begin()); // the oldest: the audio thread has long moved on from it
    reference = file.getFileName();
    referencePath = file.getFullPathName();
    referenceRate = rate;
    return {};
}

void HermesProcessor::clearReference()
{
    engine.setReference (nullptr);
    reference = {};
    referencePath = {};
}

juce::AudioProcessorEditor* HermesProcessor::createEditor() { return new ui::ScaledEditor<HermesEditor> (*this, apvts.state, *this); }

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new HermesProcessor(); }
