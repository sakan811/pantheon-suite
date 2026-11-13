// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 The Pantheon authors

#pragma once

#include "Advice.h"
#include "PluginProcessor.h"
#include "UI/CastorUi.h"
#include <array>
#include <pantheon/ui/Advice.h>

// Six width knobs (one for each band of music), the mid, the side, the mono bass, the balance and the output, and a goniometer with a correlation bar.
// Everything drawn is the shared kit's (shared/ui).
class CastorEditor : public juce::AudioProcessorEditor, private juce::Timer
{
public:
    explicit CastorEditor (CastorProcessor&);
    ~CastorEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

    static constexpr int kPoints = 1536; // samples the goniometer keeps

    std::array<std::unique_ptr<ui::Knob>, castor::kBands> widths;
    ui::Knob midKnob, sideKnob, monoKnob, balanceKnob, outKnob;
    ui::PresetBox presetBox;
    ui::GuideButton guideButton { castor::guideTitle(), castor::guideText() };
    juce::TextButton& guide() noexcept { return guideButton; }
    std::vector<ui::Note> notes() const { return castor::adviceFor (adviceInput()); } // what the strip under the controls says, for the checks
    void tickDisplay() { timerCallback(); }

private:
    void timerCallback() override;
    castor::AdviceInput adviceInput() const;

    CastorProcessor& proc;
    HardwareLookAndFeel look;
    juce::TooltipWindow tooltips { this, 600 };
    juce::Rectangle<int> nameplate, presetLabel, gonio, correlationBar, noteStrip, bandsLabel, otherLabel;
    std::array<float, kPoints> pointL {}, pointR {};
    std::vector<float> scratchL, scratchR;
    float correlation = 1.0f;
    bool live = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (CastorEditor)
};
