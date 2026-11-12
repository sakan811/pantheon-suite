// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 The Pantheon authors

#pragma once

#include "Advice.h"
#include "PluginProcessor.h"
#include "UI/AegisUi.h"
#include <array>
#include <pantheon/ui/Advice.h>

// A window of glass with the reduction of the last seconds and the input level, the three knobs (GAIN, CEILING, RELEASE), the look-ahead and the
// true-peak switch. Everything drawn is the shared kit's (shared/ui).
class AegisEditor : public juce::AudioProcessorEditor, private juce::Timer
{
public:
    explicit AegisEditor (AegisProcessor&);
    ~AegisEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

    static constexpr int kHistory = 270; // reduction readings, one a timer tick: 9 seconds

    ui::Knob gainKnob, ceilingKnob, releaseKnob;
    ui::Choice lookaheadBox;
    ui::Toggle truePeakButton;
    ui::GuideButton guideButton { aegis::guideTitle(), aegis::guideText() };
    juce::TextButton& guide() noexcept { return guideButton; }
    std::vector<ui::Note> notes() const { return aegis::adviceFor (adviceInput()); } // what the strip under the controls says, for the checks
    float heldReductionDb() const noexcept; // the most the limiter took off in the history

private:
    void timerCallback() override;
    aegis::AdviceInput adviceInput() const;

    AegisProcessor& proc;
    HardwareLookAndFeel look;
    juce::TooltipWindow tooltips { this, 600 };
    juce::Rectangle<int> nameplate, display, lookaheadLabel, noteStrip, latencyLabel;
    std::array<float, kHistory> history {}; // the reduction in dB (positive), oldest first
    float shownReduction = 0.0f, inLevelDb = aegis::gui::kLevelFloorDb;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AegisEditor)
};
