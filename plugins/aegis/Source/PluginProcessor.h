// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 The Pantheon authors

#pragma once

#include "Parameters.h"
#include "Presets.h"
#include <juce_audio_processors/juce_audio_processors.h>
#include <pantheon/dsp/BypassDelay.h>

class AegisProcessor : public juce::AudioProcessor, private juce::AsyncUpdater
{
public:
    AegisProcessor();
    ~AegisProcessor() override { cancelPendingUpdate(); }

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported (const BusesLayout&) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    void processBlockBypassed (juce::AudioBuffer<float>&, juce::MidiBuffer&) override; // delayed by the latency, so the track does not jump when the host bypasses

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return JucePlugin_Name; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }

    int getNumPrograms() override { return programs.count(); }
    int getCurrentProgram() override { return programs.current(); }
    void setCurrentProgram (int index) override { programs.set (index); }
    const juce::String getProgramName (int index) override { return programs.name (index); }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock&) override;
    void setStateInformation (const void*, int) override;

    aegis::Engine engine;
    juce::AudioProcessorValueTreeState apvts;
    ui::Programs programs { apvts, aegis::presets() }; // after apvts

private:
    // The latency moves with the look-ahead; setLatencySamples belongs to the message thread, so the audio thread only asks for it.
    void handleAsyncUpdate() override;
    std::atomic<int> reportedLatency { 0 };
    pdsp::BypassDelay bypassDelay;
    std::vector<float> monoRight; // a mono host: the engine's second channel

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AegisProcessor)
};
