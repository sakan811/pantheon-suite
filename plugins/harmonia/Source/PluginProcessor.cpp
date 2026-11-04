// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 The Pantheon authors

#include "PluginProcessor.h"
#include "PluginEditor.h"

using namespace harmonia;

namespace
{
juce::String speedText (float v)
{
    return v < 1.0f ? juce::String ("Instant") : juce::String (juce::roundToInt (v)) + " ms";
}

template <typename Item, size_t N>
juce::StringArray namesOf (const Item (&items)[N])
{
    juce::StringArray names;
    for (auto& item : items)
        names.add (item.name);
    return names;
}
} // namespace

juce::AudioProcessorValueTreeState::ParameterLayout HarmoniaProcessor::createLayout()
{
    using juce::AudioParameterBool;
    using juce::AudioParameterChoice;
    using juce::AudioParameterFloat;
    using juce::NormalisableRange;
    using juce::ParameterID;
    using juce::StringArray;

    juce::AudioProcessorValueTreeState::ParameterLayout layout;

    layout.add (std::make_unique<AudioParameterChoice> (ParameterID { ids::key, 1 }, "Key", StringArray (kNoteNames, 12), 0));
    layout.add (std::make_unique<AudioParameterChoice> (ParameterID { ids::scale, 1 }, "Scale", namesOf (kScales), kChromaticScale));

    for (int i = 0; i < 12; ++i)
        layout.add (std::make_unique<AudioParameterBool> (ParameterID { ids::note (i), 1 },
                                                          juce::String ("Custom ") + kNoteNames[i], true));

    layout.add (std::make_unique<AudioParameterFloat> (
        ParameterID { ids::speed, 1 }, "Retune Speed", NormalisableRange<float> (0.0f, 400.0f, 1.0f, 0.45f), 35.0f,
        juce::AudioParameterFloatAttributes().withStringFromValueFunction ([] (float v, int) { return speedText (v); })));

    layout.add (std::make_unique<AudioParameterFloat> (
        ParameterID { ids::strength, 1 }, "Strength", NormalisableRange<float> (0.0f, 100.0f, 1.0f), 100.0f,
        juce::AudioParameterFloatAttributes().withLabel ("%")));

    layout.add (std::make_unique<AudioParameterFloat> (
        ParameterID { ids::natural, 1 }, "Natural", NormalisableRange<float> (0.0f, 100.0f, 1.0f), 70.0f,
        juce::AudioParameterFloatAttributes().withLabel ("%")));

    layout.add (std::make_unique<AudioParameterFloat> (
        ParameterID { ids::formant, 1 }, "Formant", NormalisableRange<float> (-6.0f, 6.0f, 0.1f), 0.0f,
        juce::AudioParameterFloatAttributes().withLabel ("st")));

    layout.add (std::make_unique<AudioParameterFloat> (
        ParameterID { ids::hold, 1 }, "Note Hold", NormalisableRange<float> (0.0f, 200.0f, 1.0f), 70.0f,
        juce::AudioParameterFloatAttributes().withLabel ("ms")));

    layout.add (std::make_unique<AudioParameterFloat> (
        ParameterID { ids::tuning, 1 }, "Tuning (A4)", NormalisableRange<float> (415.0f, 466.0f, 0.1f), 440.0f,
        juce::AudioParameterFloatAttributes().withLabel ("Hz")));

    layout.add (std::make_unique<AudioParameterChoice> (ParameterID { ids::range, 1 }, "Voice", namesOf (kVoiceRanges), 0));

    layout.add (std::make_unique<AudioParameterBool> (ParameterID { ids::bypass, 1 }, "Bypass", false));
    layout.add (std::make_unique<AudioParameterBool> (ParameterID { ids::autoKey, 1 }, "Auto Key", false));

    return layout;
}

HarmoniaProcessor::HarmoniaProcessor()
    : AudioProcessor (BusesProperties()
                          .withInput ("Input", juce::AudioChannelSet::stereo(), true)
                          .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "STATE", createLayout()),
      engine (std::make_unique<AutoTuneEngine>())
{
    pKey = apvts.getRawParameterValue (ids::key);
    pScale = apvts.getRawParameterValue (ids::scale);
    pSpeed = apvts.getRawParameterValue (ids::speed);
    pStrength = apvts.getRawParameterValue (ids::strength);
    pNatural = apvts.getRawParameterValue (ids::natural);
    pFormant = apvts.getRawParameterValue (ids::formant);
    pHold = apvts.getRawParameterValue (ids::hold);
    pTuning = apvts.getRawParameterValue (ids::tuning);
    pRange = apvts.getRawParameterValue (ids::range);
    pBypass = apvts.getRawParameterValue (ids::bypass);
    pAutoKey = apvts.getRawParameterValue (ids::autoKey);
    for (int i = 0; i < 12; ++i)
        pNote[i] = apvts.getRawParameterValue (ids::note (i));

    bypassParam = dynamic_cast<juce::AudioParameterBool*> (apvts.getParameter (ids::bypass));
}

bool HarmoniaProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto& in = layouts.getMainInputChannelSet();
    const auto& out = layouts.getMainOutputChannelSet();
    if (in != out)
        return false;
    return in == juce::AudioChannelSet::mono() || in == juce::AudioChannelSet::stereo();
}

bool HarmoniaProcessor::followedKey (int& root, bool& minor) const
{
    if (pAutoKey->load() < 0.5f || ! prepared.load())
        return false;
    const auto k = engine->trackedKey();
    root = k.root;
    minor = k.minor;
    return k.valid;
}

uint16_t HarmoniaProcessor::currentNoteMask() const
{
    int root = 0;
    bool minor = false;
    if (followedKey (root, minor))
        return scaleMask (root, minor ? kMinorScale : kMajorScale);

    const int scale = scaleIndex();
    if (scale == kCustomScale)
    {
        uint16_t m = 0;
        for (int i = 0; i < 12; ++i)
            if (pNote[i]->load() >= 0.5f)
                m = (uint16_t) (m | (1u << i));
        return m;
    }
    return scaleMask ((int) pKey->load(), juce::jlimit (0, kNumScales - 1, scale));
}

EngineParams HarmoniaProcessor::readParams() const
{
    EngineParams p;
    p.corrector.noteMask = currentNoteMask();
    p.corrector.retuneMs = pSpeed->load();
    p.corrector.strength = pStrength->load() * 0.01f;
    p.corrector.natural = pNatural->load() * 0.01f;
    p.corrector.dwellMs = pHold->load();
    p.corrector.refA4 = pTuning->load();
    p.formantSemitones = pFormant->load();
    p.mix = pBypass->load() >= 0.5f ? 0.0f : 1.0f;
    p.autoKey = pAutoKey->load() >= 0.5f;
    p.setVoiceRange ((int) pRange->load(), p.corrector.refA4);
    return p;
}

void HarmoniaProcessor::prepareToPlay (double sampleRate, int)
{
    const int channels = juce::jlimit (1, 2, getTotalNumInputChannels());
    prepared.store (false);
    engine->prepare (sampleRate, channels);
    setLatencySamples (engine->latencySamples());
    prepared.store (true);
}

void HarmoniaProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;

    for (int ch = getTotalNumInputChannels(); ch < getTotalNumOutputChannels(); ++ch)
        buffer.clear (ch, 0, buffer.getNumSamples());

    if (! prepared.load())
        return;

    if (resetRequested.exchange (false))
        engine->reset();

    engine->setParams (readParams());

    // Key detection requests from the UI.
    switch (learnCommand.exchange (0))
    {
        case 1:
            engine->startLearning();
            learning.store (true);
            break;
        case 2:
        {
            const auto guess = engine->stopLearning();
            learning.store (false);
            if (engine->learnedAmount() >= 150.0f) // roughly >1 s of confidently voiced singing
            {
                resultRoot.store (guess.root);
                resultMinor.store (guess.minor);
                resultScore.store (guess.score);
                resultReady.store (true);
            }
            break;
        }
        default: break;
    }
    if (learning.load())
        learnAmount.store (engine->learnedAmount());

    engine->process (buffer.getArrayOfWritePointers(), buffer.getNumChannels(), buffer.getNumSamples());
}

bool HarmoniaProcessor::takeKeyResult (KeyResult& out)
{
    if (! resultReady.exchange (false))
        return false;
    out.valid = true;
    out.root = resultRoot.load();
    out.minor = resultMinor.load();
    out.score = resultScore.load();
    return true;
}

juce::AudioProcessorEditor* HarmoniaProcessor::createEditor()
{
    return new HarmoniaEditor (*this);
}

void HarmoniaProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    if (auto xml = apvts.copyState().createXml())
        copyXmlToBinary (*xml, destData);
}

void HarmoniaProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    if (auto xml = getXmlFromBinary (data, sizeInBytes))
        if (xml->hasTagName (apvts.state.getType()))
            apvts.replaceState (juce::ValueTree::fromXml (*xml));
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new HarmoniaProcessor();
}
