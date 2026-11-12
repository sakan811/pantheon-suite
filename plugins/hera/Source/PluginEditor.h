// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 The Pantheon authors

#pragma once

#include "Advice.h"
#include "PluginProcessor.h"
#include "UI/HeraUi.h"
#include <array>
#include <pantheon/ui/Advice.h>

// The transfer curve and the reduction of the last seconds on glass, nine knobs and four switches. Everything drawn is the shared kit's (shared/ui).
class HeraEditor : public juce::AudioProcessorEditor, private juce::Timer
{
public:
    explicit HeraEditor (HeraProcessor&);
    ~HeraEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

    static constexpr int kHistory = 300;

    ui::Knob threshold, ratio, attack, release, knee, makeup, mix, link, hpf;
    ui::Choice detector;
    ui::Toggle autoRelease, autoMakeup, key;
    ui::GuideButton guideButton { hera::guideTitle(), hera::guideText() };
    juce::TextButton& guide() noexcept { return guideButton; }
    std::vector<ui::Note> notes() const { return hera::adviceFor (adviceInput()); } // what the strip under the controls says, for the checks
    float heldReductionDb() const noexcept { return *std::max_element (history.begin(), history.end()); }

private:
    void timerCallback() override;
    hera::AdviceInput adviceInput() const;

    HeraProcessor& proc;
    HardwareLookAndFeel look;
    juce::TooltipWindow tooltips { this, 600 };
    juce::Rectangle<int> nameplate, curveArea, historyArea, detectorLabel, noteStrip;
    std::array<float, kHistory> history {}; // the reduction in dB (positive), oldest first
    float shownReduction = 0.0f, inDb = -90.0f;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (HeraEditor)
};
