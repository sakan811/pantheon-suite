// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 The Pantheon authors

#pragma once

#include "Advice.h"
#include "PluginProcessor.h"
#include "UI/NemesisUi.h"
#include <array>
#include <pantheon/ui/Advice.h>

// The band the detector listens to and the reduction of the last seconds on glass, five knobs, two selectors and LISTEN. Everything drawn is the shared kit's (shared/ui).
class NemesisEditor : public juce::AudioProcessorEditor, private juce::Timer
{
public:
    explicit NemesisEditor (NemesisProcessor&);
    ~NemesisEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

    static constexpr int kHistory = 240;

    ui::Knob freq, width, threshold, range, release;
    ui::Choice mode, detection;
    ui::Toggle listen;
    ui::GuideButton guideButton { nemesis::guideTitle(), nemesis::guideText() };
    juce::TextButton& guide() noexcept { return guideButton; }
    std::vector<ui::Note> notes() const { return nemesis::adviceFor (adviceInput()); } // what the strip under the controls says, for the checks
    float heldReductionDb() const noexcept { return *std::max_element (history.begin(), history.end()); }

private:
    void timerCallback() override;
    nemesis::AdviceInput adviceInput() const;

    NemesisProcessor& proc;
    HardwareLookAndFeel look;
    juce::TooltipWindow tooltips { this, 600 };
    juce::Rectangle<int> nameplate, bandArea, historyArea, modeLabel, detectionLabel, noteStrip;
    std::array<float, kHistory> history {}; // the reduction in dB (positive), oldest first
    float shownReduction = 0.0f;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (NemesisEditor)
};
