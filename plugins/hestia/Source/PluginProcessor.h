// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 The Pantheon authors

#pragma once

#include "Parameters.h"
#include <juce_audio_processors/juce_audio_processors.h>

class HestiaProcessor : public juce::AudioProcessor
{
public:
    HestiaProcessor();

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
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock&) override;
    void setStateInformation (const void*, int) override;

    hestia::Engine engine; // makes its place on the shared bus when the instance is made and gives it back when it goes
    juce::AudioProcessorValueTreeState apvts;
    // Whether the host handed the last block a sidechain: the editor says so.
    std::atomic<bool> sidechainConnected { false };

private:
    std::vector<float> monoRight; // a mono host: the engine's second channel

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (HestiaProcessor)
};
