// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 The Pantheon authors

#pragma once

#include "Parameters.h"
#include <pantheon/dsp/AnalyzerTap.h>
#include <juce_audio_processors/juce_audio_processors.h>

class AtlasProcessor : public juce::AudioProcessor, private juce::AsyncUpdater
{
public:
    AtlasProcessor();
    ~AtlasProcessor() override { cancelPendingUpdate(); }

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

    int getNumPrograms() override { return (int) atlas::presets().size(); }
    int getCurrentProgram() override { return currentProgram; }
    void setCurrentProgram (int) override;
    const juce::String getProgramName (int) override;
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock&) override;
    void setStateInformation (const void*, int) override;

    atlas::Engine engine;
    juce::AudioProcessorValueTreeState apvts;
    pdsp::StereoTap tapIn; // the input, for the display's spectrogram
    // Whether the host handed the last block a sidechain: the editor says so next to a band set to listen to it.
    std::atomic<bool> sidechainConnected { false };

private:
    // The latency changes with the phase and oversampling modes; setLatencySamples belongs to the message thread, so the
    // audio thread only asks for it.
    void handleAsyncUpdate() override;
    std::atomic<int> reportedLatency { 0 };
    int currentProgram = 0;
    std::vector<float> monoRight; // a mono host: the engine's second channel

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AtlasProcessor)
};
