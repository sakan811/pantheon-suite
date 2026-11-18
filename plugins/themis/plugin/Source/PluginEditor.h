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
#include <pantheon/ui/Resize.h>

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
    juce::TextButton& closest() noexcept { return closestButton; }                   // for the smoke test
    juce::String statusText() const { return statusLabel.getText(); }                 // for the smoke test
    std::vector<ui::Note> notes() const { return usage::notesFor (proc.usageState()); }   // what the status window says when idle, for the smoke test

private:
    void timerCallback() override;
    void paintHeaderGroups (juce::Graphics&) const;
    void paintVerdictPlate (juce::Graphics&) const;
    void setStatus (const juce::String& text, bool isError);
    void showIdleStatus();
    void copyTonalEq();
    void chooseClosestTarget();
    void toggleHeldCurve();

    void launchChooser (const juce::String& title, const juce::File& start, const juce::String& patterns, int chooserFlags,
                        std::function<void (ThemisEditor&, const juce::FileChooser&)> onChosen);
    void chooseReferenceTracks();
    void saveCurve();
    void loadCurve();

    static constexpr const char* kCurveExtension = ".themiscurve";
    static constexpr int kMinWidth = 1400;     // the smallest (and opening) width: the header's two rows of controls need it
    static constexpr int kMinHeight = ui::editorHeightFor (kMinWidth), kMaxWidth = 2080, kMaxHeight = ui::editorHeightFor (kMaxWidth); // 16:10 all the way (1400 x 875 to 2080 x 1300)
    static constexpr int kHistoryHeight = 240; // the two time graphs along the bottom
    // The control panel along the top: two rows, each with its group captions above. The left column holds the nameplate and the verdict plate.
    static constexpr int kHeaderHeight = 112;
    static constexpr int kTitleWidth = 196;    // the left column (nameplate and verdict plate); the header rows start after it
    static constexpr int kCaptionHeight = 12, kRowHeight = 28, kKnobRowHeight = 40, kRowGap = 8; // a caption line, a row of buttons and combos, the knobs, the gap between the rows
    static constexpr int kVerdictHeight = 30;  // the verdict plate under the nameplate
    static constexpr int kTargetWidth = 748, kJudgeWidth = 984, kSessionWidth = 172; // the groups' widths (their controls, and the gaps between them)
    static constexpr int kKnobWidth = 150;     // a knob with its read-out box, in the JUDGE group
    static constexpr int kGroupGap = 12;       // between a group and the next, with an engraved rule in it
    static constexpr int kSideWidth = 450;  // the right-hand column: master plan on top, stereo scope and audition below
    static constexpr int kScopeHeight = 246, kAuditionWidth = 226; // the stereo scope and the audition strip along the bottom of that column
    static constexpr int kPlanRefreshTicks = 15; // the plan is re-read twice a second

    ThemisProcessor& proc;
    HardwareLookAndFeel laf;

    TonalBalanceView view;
    StereoScopeView scopeView;
    gui::LevelView levelView;
    gui::FundamentalView fundamentalView;
    AdvicePanel advicePanel;
    AuditionPanel auditionPanel;

    ui::Choice targetBox, deviceBox, loudnessBox;
    juce::Label deviceLabel, loudnessLabel;
    juce::TextButton learnButton { "Learn from tracks..." }, closestButton { "Closest" }, saveButton { "Save curve" }, loadButton { "Load curve" },
        resetButton { "Reset" }, copyEqButton { "Copy EQ" }, holdButton { "Hold curve" };
    juce::Label statusLabel;
    ui::GuideButton guideButton { usage::guideTitle(), juce::String (usage::guideText()) };
    ui::Knob avgKnob, toleranceKnob, ceilingKnob;
    ui::Toggle resetOnPlayButton;
    juce::TooltipWindow tooltipWindow { this, 600 };

    std::unique_ptr<juce::FileChooser> chooser;
    std::unique_ptr<ui::ResizeGrips> grips; // the four corners; added last, so it is on top
    juce::Rectangle<int> nameplate, verdictPlate, statusWindow;
    juce::Rectangle<int> captionTarget, captionJudge, captionSession; // the group captions above the header's groups
    juce::Line<float> ruleTarget, ruleJudge, ruleRows;                 // the engraved rules between groups, and between the two rows
    int statusTicks = 0, planTicks = 0;
    float shownCeilingDb = 0.0f; // the true-peak ceiling the graphs were last judged against
    bool signalPresent = false; // the SIGNAL lamp: audio is reaching the plugin

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ThemisEditor)
};
