// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 The Pantheon authors

#pragma once

#include "Parameters.h"
#include "Presets.h"
#include <juce_audio_processors/juce_audio_processors.h>

class HestiaProcessor : public juce::AudioProcessor, private juce::AsyncUpdater
{
public:
    HestiaProcessor();
    ~HestiaProcessor() override { cancelPendingUpdate(); }

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    void reset() override { engine.reset(); } // a host locate, loop or stop: nothing of the old position may sound at the new one
    bool isBusesLayoutSupported (const BusesLayout&) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

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

    hestia::Engine engine; // makes its place on the shared bus when the instance is made and gives it back when it goes
    juce::AudioProcessorValueTreeState apvts;
    ui::Programs programs { apvts, hestia::presets(), hestia::keepsParameter }; // after apvts
    // The host's name for this track, when it tells (the VST3 and AU hosts that do): it is what the other instances' lists and depth maps show for this stem.
    void updateTrackProperties (const TrackProperties& properties) override;
    // COPY TO MIX: CARVE, REACH, ATTACK and RELEASE go to the other instances of this mix, which take them on their next block. Returns how many instances are in the mix.
    int copyToMix();
    // Whether the host handed the last block a sidechain: the editor says so.
    std::atomic<bool> sidechainConnected { false };

private:
    // The audio thread saw a broadcast for this mix; the parameters are set on the message thread.
    void handleAsyncUpdate() override;
    std::vector<float> monoRight; // a mono host: the engine's second channel

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (HestiaProcessor)
};
