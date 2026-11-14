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
    ui::Choice mode, detection, lookahead;
    ui::PresetBox presetBox;
    ui::Toggle listen, delta;
    juce::TextButton findButton { "FIND" }; // listens for a moment and sets FREQ to the strongest place between 4 and 10 kHz (LISTEN on)
    void tickDisplay() { timerCallback(); } // one reading of the engine (the checks and a snapshot call it as the timer would)
    bool findPending() const noexcept { return awaiting; }
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
    juce::Rectangle<int> nameplate, bandArea, historyArea, modeLabel, detectionLabel, presetLabel, lookaheadLabel, latencyLabel, noteStrip;
    std::array<float, kHistory> history {}; // the reduction in dB (positive), oldest first
    float shownReduction = 0.0f;
    bool awaiting = false;      // FIND was pressed and its answer has not come
    unsigned findCountAtClick = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (NemesisEditor)
};
