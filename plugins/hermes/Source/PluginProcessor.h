// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 The Pantheon authors

#pragma once

#include "Parameters.h"
#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_audio_processors/juce_audio_processors.h>

class HermesProcessor : public juce::AudioProcessor
{
public:
    HermesProcessor();
    ~HermesProcessor() override = default;

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported (const BusesLayout&) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return JucePlugin_Name; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return "Default"; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock&) override;
    void setStateInformation (const void*, int) override;

    hermes::Engine engine;
    juce::AudioProcessorValueTreeState apvts;

    // On the message thread. Decodes the whole file (at most kMaxReferenceMinutes), brings it to the session's sample rate, measures its loudness and hands it to the engine.
    // Returns an empty string on success, else why not.
    juce::String loadReference (const juce::File& file);
    void clearReference();
    juce::String referenceName() const { return reference; }
    double referenceLufs() const { return loaded.empty() ? -200.0 : loaded.back()->integratedLufs; }
    static constexpr double kMaxReferenceMinutes = 10.0;

private:
    juce::AudioFormatManager formats;
    juce::String referencePath;
    std::vector<float> monoRight; // a mono host: the engine's second channel
    std::vector<std::unique_ptr<hermes::Reference>> loaded; // the references, newest last: the audio thread may still read the one before the newest for a block, never the one before that
    juce::String reference;                                 // the name of the file on the engine, for the editor
    double referenceRate = 0.0;                             // the sample rate it was brought to

    JUCE_DECLARE_WEAK_REFERENCEABLE (HermesProcessor)
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (HermesProcessor)
};
