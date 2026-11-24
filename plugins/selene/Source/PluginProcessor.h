// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 The Pantheon authors

#pragma once

#include "Parameters.h"
#include "Presets.h"
#include <juce_audio_processors/juce_audio_processors.h>
#include <pantheon/ui/AbCompare.h>
#include <pantheon/ui/Presets.h>
#include <pantheon/ui/Undo.h>

class SeleneProcessor : public juce::AudioProcessor
{
public:
    SeleneProcessor();
    ~SeleneProcessor() override = default;

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported (const BusesLayout&) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    void processBlockBypassed (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    void reset() override { engine.reset(); keyboardState.reset(); }

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return JucePlugin_Name; }
    bool acceptsMidi() const override { return true; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 4.0; }

    int getNumPrograms() override { return programs.count(); }
    int getCurrentProgram() override { return programs.current(); }
    void setCurrentProgram (int index) override { ui::beginUndoStep (apvts, "Preset"); programs.set (index); }
    const juce::String getProgramName (int index) override { return programs.name (index); }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock&) override;
    void setStateInformation (const void*, int) override;

    selene::Engine engine;
    juce::UndoManager undo; // declared before apvts so it outlives the state that records into it
    juce::AudioProcessorValueTreeState apvts;
    ui::Programs programs { apvts, selene::presets() };
    ui::AbCompare ab { apvts };
    juce::MidiKeyboardState keyboardState;

private:
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SeleneProcessor)
};
