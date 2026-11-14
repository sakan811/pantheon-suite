// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 The Pantheon authors

#pragma once

#include <atomic>

#include "DSP/Peers.h"
#include <pantheon/dsp/BypassDelay.h>
#include <pantheon/ui/AbCompare.h>
#include "Parameters.h"
#include <juce_audio_processors/juce_audio_processors.h>

class IrisProcessor : public juce::AudioProcessor, private juce::AsyncUpdater
{
public:
    IrisProcessor();
    ~IrisProcessor() override { cancelPendingUpdate(); }

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    void reset() override { engine.reset(); } // a host locate, loop or stop: nothing of the old position may sound at the new one
    bool isBusesLayoutSupported (const BusesLayout&) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    void processBlockBypassed (juce::AudioBuffer<float>&, juce::MidiBuffer&) override; // delayed by the reported latency, so a bypassed track stays in time

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
    juce::UndoManager undo; // UNDO / REDO of the parameters (shared/ui Undo.h); declared before the state that records into it
    juce::AudioProcessorValueTreeState apvts;
    ui::AbCompare ab { apvts }; // A/B compare (shared/ui AbCompare.h): two settings, one live
    // What arrives at the sidechain input (another track), for the masking view only: it never reaches the audio.
    pdsp::StereoTap sidechainTap;
    // Whether the host handed the last block a sidechain: the editor says so next to a band set to listen to it.
    std::atomic<bool> sidechainConnected { false };
    // This instance as the other Iris instances in the host see it: they can mask against what it sends out (see DSP/Peers.h).
    iris::Peer peer;
    double currentSampleRate = 48000.0;

    // The host's name for the track, when it tells: it is what the list of other instances shows.
    void updateTrackProperties (const TrackProperties& properties) override
    {
        AudioProcessor::updateTrackProperties (properties);
        peer.setTrackName (properties.name.has_value() ? properties.name->toStdString() : std::string());
    }

private:
    // The latency changes with the phase mode, which a host or the editor can change at any time; setLatencySamples belongs to the
    // message thread, so the audio thread only asks for it.
    void handleAsyncUpdate() override;
    std::atomic<int> reportedLatency { 0 };
    pdsp::BypassDelay bypassDelay;

    int currentProgram = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (IrisProcessor)
};
