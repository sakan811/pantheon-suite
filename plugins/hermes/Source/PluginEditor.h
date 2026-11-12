// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 The Pantheon authors

#pragma once

#include "Advice.h"
#include "PluginProcessor.h"
#include "UI/HermesUi.h"
#include <array>
#include <pantheon/ui/Advice.h>

// The loudness readings and a graph of the last minute on glass, the trim, the listening mode, and the reference (load, A/B, level match, restart).
// Everything drawn is the shared kit's (shared/ui).
class HermesEditor : public juce::AudioProcessorEditor, private juce::Timer
{
public:
    explicit HermesEditor (HermesProcessor&);
    ~HermesEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

    static constexpr int kHistory = 600; // readings at 10 Hz: a minute

    ui::Knob trim, referenceTrim;
    ui::Choice listen;
    ui::Toggle polarity, reference, levelMatch;
    juce::TextButton loadButton { "LOAD" }, restartButton { "RESTART" }, resetButton { "RESET" };
    ui::GuideButton guideButton { hermes::guideTitle(), hermes::guideText() };
    juce::TextButton& guide() noexcept { return guideButton; }
    std::vector<ui::Note> notes() const { return hermes::adviceFor (adviceInput()); } // what the strip under the controls says, for the checks
    void loadFile (const juce::File&);                                              // what LOAD does with the file that was picked (the checks call it too)
    void tickDisplay() { timerCallback(); }                                         // one reading of the meters (a snapshot calls it as the timer would)
    juce::String status;                                                            // what the last LOAD said, if it failed

private:
    void timerCallback() override;
    hermes::AdviceInput adviceInput() const;

    HermesProcessor& proc;
    HardwareLookAndFeel look;
    juce::TooltipWindow tooltips { this, 600 };
    std::unique_ptr<juce::FileChooser> chooser;
    juce::Rectangle<int> nameplate, readouts, graph, listenLabel, referenceArea, noteStrip;
    std::array<float, kHistory> momentaryHistory, shortTermHistory;
    float momentary = -200.0f, shortTerm = -200.0f, integrated = -200.0f, range = 0.0f, truePeak = -200.0f, samplePeak = -200.0f, position = 0.0f;
    int ticks = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (HermesEditor)
};
