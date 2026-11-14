// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 The Pantheon authors

#pragma once

#include "AdvicePanel.h"
#include "AuditionPanel.h"
#include "HistoryViews.h"
#include "PluginProcessor.h"
#include "StereoScopeView.h"
#include "ThemisUi.h"
#include "TonalBalanceView.h"
#include "UsageNotes.h"

#include <pantheon/ui/Advice.h>

#include <functional>
#include <memory>

class ThemisEditor : public juce::AudioProcessorEditor, private juce::Timer
{
public:
    explicit ThemisEditor (ThemisProcessor&);
    ~ThemisEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

    juce::TextButton& guide() noexcept { return guideButton; }                       // for the smoke test
    std::vector<ui::Note> notes() const { return usage::notesFor (usageInput()); }   // what the status window says when idle, for the smoke test

private:
    void timerCallback() override;
    void setStatus (const juce::String& text, bool isError);
    void showIdleStatus();
    usage::Input usageInput() const;
    void copyTonalEq();
    void toggleHeldCurve();

    void launchChooser (const juce::String& title, const juce::File& start, const juce::String& patterns, int chooserFlags,
                        std::function<void (ThemisEditor&, const juce::FileChooser&)> onChosen);
    void chooseReferenceTracks();
    void saveCurve();
    void loadCurve();

    static constexpr const char* kCurveExtension = ".themiscurve";
    static constexpr int kMinWidth = 1400;     // the smallest (and opening) width: the header's two rows of controls need it
    static constexpr int kHistoryHeight = 240; // the two time graphs along the bottom
    static constexpr int kHeaderHeight = 84;   // the control panel along the top
    static constexpr int kTitleWidth = 196;    // the nameplate; the header rows start after it
    static constexpr int kSideWidth = 450;  // the right-hand column: master plan on top, stereo scope and audition below
    static constexpr int kScopeHeight = 246, kAuditionWidth = 226; // the stereo scope and the audition strip along the bottom of that column
    static constexpr int kPlanRefreshTicks = 15; // the plan is re-read twice a second

    ThemisProcessor& proc;
    HardwareLookAndFeel laf;

    TonalBalanceView view;
    StereoScopeView scopeView;
    ui::LevelView levelView;
    ui::FundamentalView fundamentalView;
    AdvicePanel advicePanel;
    AuditionPanel auditionPanel;

    ui::Choice targetBox, deviceBox, loudnessBox;
    juce::Label deviceLabel, loudnessLabel;
    juce::TextButton learnButton { "Learn from tracks..." }, saveButton { "Save curve" }, loadButton { "Load curve" },
        resetButton { "Reset" }, copyEqButton { "Copy EQ" }, holdButton { "Hold curve" };
    juce::Label statusLabel;
    ui::GuideButton guideButton { usage::guideTitle(), usage::guideText() };
    ui::Knob avgKnob, toleranceKnob, ceilingKnob;
    ui::Toggle resetOnPlayButton;
    juce::TooltipWindow tooltipWindow { this, 600 };

    std::unique_ptr<juce::FileChooser> chooser;
    juce::Rectangle<int> nameplate, statusWindow;
    int statusTicks = 0, planTicks = 0;
    float shownCeilingDb = 0.0f; // the true-peak ceiling the graphs were last judged against
    bool signalPresent = false; // the SIGNAL lamp: audio is reaching the plugin

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ThemisEditor)
};
