// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 The Pantheon authors

#pragma once

#include "PluginProcessor.h"
#include "UI/AtlasUi.h"

// Six strips, one per band of music, each with its compressor, its solo and its delta; the split's phase mode, the oversampling
// and the output gain in the header. Everything drawn is the shared kit's (shared/ui).
class AtlasEditor : public juce::AudioProcessorEditor, private juce::Timer
{
public:
    explicit AtlasEditor (AtlasProcessor&);
    ~AtlasEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

    // One band's controls. Solo and delta are monitoring (Engine::monitor), not parameters: plain buttons, lit from the timer.
    struct Strip
    {
        Strip (AtlasProcessor&, int band);
        ui::Toggle on, match;
        juce::TextButton solo { "SOLO" }, delta { "DELTA" };
        ui::Choice channels;
        ui::Knob threshold, ratio, attack, release, makeup;
        std::unique_ptr<ui::Knob> crossover; // the edge to the next band; the top band has none
        juce::Rectangle<int> bounds, meter;
        float shownReduction = 0.0f;
    };
    std::array<std::unique_ptr<Strip>, atlas::kBands> strips;
    juce::TextButton clearButton { "ALL BANDS" };

private:
    void timerCallback() override;

    AtlasProcessor& proc;
    HardwareLookAndFeel look;
    juce::TooltipWindow tooltips { this, 600 };
    ui::Choice phaseBox, oversamplingBox;
    ui::Knob outKnob;
    juce::Rectangle<int> nameplate, phaseLabel, oversamplingLabel, latencyLabel;
    float inLevel = 0.0f;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AtlasEditor)
};
