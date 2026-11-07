// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 The Pantheon authors

#pragma once

#include "Parameters.h"
#include <juce_audio_processors/juce_audio_processors.h>

class IrisProcessor : public juce::AudioProcessor, private juce::AsyncUpdater
{
public:
    IrisProcessor();
    ~IrisProcessor() override { cancelPendingUpdate(); }

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

    int getNumPrograms() override { return (int) iris::presets().size(); }
    int getCurrentProgram() override { return currentProgram; }
    void setCurrentProgram (int index) override;
    const juce::String getProgramName (int index) override;
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock&) override;
    void setStateInformation (const void*, int) override;

    iris::Engine engine;
    juce::AudioProcessorValueTreeState apvts;
    // What arrives at the sidechain input (another track), for the masking view only: it never reaches the audio.
    pdsp::StereoTap sidechainTap;
    double currentSampleRate = 48000.0;

private:
    // The latency changes with the phase mode, which a host or the editor can change at any time; setLatencySamples belongs to the
    // message thread, so the audio thread only asks for it.
    void handleAsyncUpdate() override;
    std::atomic<int> reportedLatency { 0 };

    int currentProgram = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (IrisProcessor)
};
