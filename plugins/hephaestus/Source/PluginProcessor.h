// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 The Pantheon authors

#pragma once

#include "Parameters.h"
#include "UserPresets.h"
#include <pantheon/dsp/BypassDelay.h>
#include <pantheon/ui/AbCompare.h>
#include <juce_audio_processors/juce_audio_processors.h>

class HephaestusProcessor : public juce::AudioProcessor,
                            private juce::AsyncUpdater
{
public:
    HephaestusProcessor();
    ~HephaestusProcessor() override = default;

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    void reset() override { engine.reset(); } // a host locate, loop or stop: nothing of the old position may sound at the new one
    bool isBusesLayoutSupported (const BusesLayout&) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    // A host that bypasses the plugin itself (the AU path) still hears it delayed by the latency it reported, so the track does not jump early.
    void processBlockBypassed (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return JucePlugin_Name; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    // The output is the input delayed by the latency, so that delay is also what is left to play after the input stops.
    double getTailLengthSeconds() const override { return currentSampleRate > 0.0 ? (double) getLatencySamples() / currentSampleRate : 0.0; }

    int getNumPrograms() override { return (int) hep::presets().size(); }
    int getCurrentProgram() override { return currentProgram; }
    void setCurrentProgram (int index) override;
    const juce::String getProgramName (int index) override;
    void changeProgramName (int, const juce::String&) override {}

    // The user's own presets (files in the user's folder); the host's program list is the factory presets only. The name of the one last chosen is kept
    // with the session so the box shows it again, until a factory preset is chosen.
    void applyUserPreset (const juce::String& name)
    {
        if (userPresets.apply (apvts, name))
            currentUserPreset = name;
    }
    bool saveUserPreset (const juce::String& name)
    {
        if (! userPresets.save (apvts, name))
            return false;
        currentUserPreset = name.trim().substring (0, 40);
        return true;
    }
    void deleteUserPreset (const juce::String& name)
    {
        userPresets.remove (name);
        if (name == currentUserPreset)
            currentUserPreset = {};
    }
    const juce::String& getCurrentUserPreset() const { return currentUserPreset; }
    hep::UserPresets userPresets { hep::UserPresets::defaultFolder() };

    void getStateInformation (juce::MemoryBlock&) override;
    void setStateInformation (const void*, int) override;

    hep::Engine engine;
    juce::UndoManager undo; // UNDO / REDO of the parameters (shared/ui Undo.h); declared before the state that records into it
    juce::AudioProcessorValueTreeState apvts;
    ui::AbCompare ab { apvts }; // A/B compare (shared/ui AbCompare.h): two settings, one live
    double currentSampleRate = 48000.0;

private:
    void handleAsyncUpdate() override;

    int currentProgram = 0;
    juce::String currentUserPreset;
    std::atomic<int> reportedLatency { -1 };  // what setLatencySamples last got (message thread), read by the audio thread
    std::atomic<int> requestedLatency { -1 }; // what the audio thread last asked the message thread for: one trigger per change
    pdsp::BypassDelay bypassDelay;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (HephaestusProcessor)
};
