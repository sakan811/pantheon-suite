// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 The Pantheon authors

#pragma once

#include "Advice.h"
#include "PluginProcessor.h"
#include "UI/HeraUi.h"
#include <array>
#include <pantheon/ui/AbCompare.h>
#include <pantheon/ui/Advice.h>
#include <pantheon/ui/Undo.h>

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
    ui::Choice detector, lookahead;
    ui::PresetBox presetBox;
    ui::Toggle autoRelease, autoMakeup, key, levelMatch;
    juce::TextButton deltaButton { "DELTA" }, listenButton { "SC LISTEN" };
    ui::GuideButton guideButton { hera::guideTitle(), hera::guideText() };
    juce::TextButton& guide() noexcept { return guideButton; }
    ui::UndoButtons undoButtons; // UNDO / REDO, in the header strip (shared/ui Undo.h)
    ui::AbButtons abButtons;     // A / B, beside them (shared/ui AbCompare.h)
    ui::UndoButtons& undoControls() noexcept { return undoButtons; } // for UiCheck
    ui::AbButtons& abControls() noexcept { return abButtons; }
    std::vector<ui::Note> notes() const { return hera::adviceFor (adviceInput()); } // what the strip under the controls says, for the checks
    float heldReductionDb() const noexcept { return *std::max_element (history.begin(), history.end()); }
    juce::TextButton& autoThresh() noexcept { return autoThreshButton; } // for UiCheck

    // AUTO THRESH: one press listens to four seconds of the sound and sets THRESH once (one undo step); see Engine.h.
    juce::TextButton autoThreshButton { "AUTO THRESH" };

private:
    void timerCallback() override;
    hera::AdviceInput adviceInput() const;
    void startAutoThresh();
    void finishAutoThresh();

    hera::AutoThresh autoAnswer;           // the last answer, shown in the strip until the next press
    bool autoAsked = false;                // whether there is an answer to show
    int autoTicks = 0;                     // timer ticks left in the listening (a timeout when no sound plays, as with a stopped transport)
    int autoStatusTicks = 0;               // ticks left before the button's result text goes back to its name

    HeraProcessor& proc;
    HardwareLookAndFeel look;
    juce::TooltipWindow tooltips { this, 600 };
    juce::Rectangle<int> nameplate, curveArea, historyArea, detectorLabel, presetLabel, lookaheadLabel, latencyLabel, noteStrip;
    std::array<float, kHistory> history {}; // the reduction in dB (positive), oldest first
    float shownReduction = 0.0f, inDb = -90.0f;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (HeraEditor)
};
