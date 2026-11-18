// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 The Pantheon authors

#pragma once

#include "Advice.h"
#include "PluginProcessor.h"
#include "TipArea.h"
#include "UI/AegisUi.h"
#include <array>
#include <pantheon/ui/AbCompare.h>
#include <pantheon/ui/Advice.h>
#include <pantheon/ui/Undo.h>

// A window of glass with the reduction of the last seconds and the input level, the three knobs (GAIN, CEILING, RELEASE), the look-ahead and the
// true-peak switch. Everything drawn is the shared kit's (shared/ui).
class AegisEditor : public juce::AudioProcessorEditor, private juce::Timer
{
public:
    explicit AegisEditor (AegisProcessor&);
    ~AegisEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseDown (const juce::MouseEvent&) override; // a click on the MAX REDUCTION number clears it

    static constexpr int kHistory = 270; // reduction readings, one a timer tick: 9 seconds

    ui::Knob gainKnob, ceilingKnob, releaseKnob;
    ui::Choice lookaheadBox;
    ui::Toggle truePeakButton, autoReleaseButton, deltaButton, matchButton;
    juce::TextButton pasteButton { "PASTE" }, clearOutButton { "CLEAR" }, autoGainButton { "AUTO GAIN" };
    ui::PresetBox presetBox;
    ui::GuideButton guideButton { aegis::guideTitle(), aegis::guideText() };
    juce::TextButton& guide() noexcept { return guideButton; }
    ui::UndoButtons undoButtons; // UNDO / REDO, in the header strip (shared/ui Undo.h)
    ui::AbButtons abButtons;     // A / B, beside them (shared/ui AbCompare.h)
    ui::UndoButtons& undoControls() noexcept { return undoButtons; } // for UiCheck
    ui::AbButtons& abControls() noexcept { return abButtons; }
    // Tooltips over the painted parts of the display (see TipArea.h): the trace, the MAX number, OUT PEAK, the IN and OUT bars, and the delay.
    aegis::TipArea reductionTip { "The reduction of the last nine seconds, in dB: the deeper the trace, the more the limiter took off." },
        maxTip { "The most the limiter has taken off since CLEAR. Click this number to clear it alone." },
        outPeakTip { "The highest true peak that has left since CLEAR. Red when it is over CEILING (by more than 0.1 dB)." },
        levelTip { "Input (left) and output (right) level now, in dBFS. The mark on OUT is CEILING." },
        delayTip { "The delay the host compensates: LOOK-AHEAD plus the true-peak reader." };
    std::vector<ui::Note> notes() const { return aegis::adviceFor (adviceInput()); } // what the strip under the controls says, for the checks
    float heldReductionDb() const noexcept; // the most the limiter took off in the history
    float reductionHoldDb() const noexcept { return reductionHold; } // the most the limiter took off since CLEAR, a click on the number, or CEILING or TRUE PEAK changed
    juce::Rectangle<int> reductionHoldArea() const { return holdArea; }
    float outHoldDb() const noexcept { return outHold; } // the highest true peak of the output since CLEAR (or since CEILING or TRUE PEAK changed)
    std::vector<ui::Note> shownNotes() const; // the notes, with what the last PASTE did first while it is shown
    void pasteFromClipboard (const juce::String& text); // what PASTE does with the clipboard's text (a limiter setting from Themis' plan)
    void finishAutoGain(); // the end of AUTO GAIN's four seconds: sets GAIN from the loudest part heard (the timer calls it; UiCheck calls it directly)

private:
    void timerCallback() override;
    aegis::AdviceInput adviceInput() const;
    void startAutoGain(); // AUTO GAIN's button: listen for four seconds
    void setStripNote (std::string text, int ticks); // a line of the strip for `ticks` timer ticks, ahead of the notes (PASTE and AUTO GAIN use it)

    AegisProcessor& proc;
    HardwareLookAndFeel look;
    juce::TooltipWindow tooltips { this, 600 };
    juce::Rectangle<int> nameplate, presetLabel, display, lookaheadLabel, noteStrip, latencyLabel;
    std::array<float, kHistory> history {}; // the reduction in dB (positive), oldest first
    float shownReduction = 0.0f, inLevelDb = aegis::gui::kLevelFloorDb, outLevelDb = aegis::gui::kLevelFloorDb, outHold = aegis::gui::kLevelFloorDb;
    float reductionHold = 0.0f;
    juce::Rectangle<int> holdArea;
    float shownCeilingDb = -1.0f;
    bool shownTruePeak = true;
    std::string stripSaid;
    int stripTicks = 0;   // timer ticks left to show stripSaid
    int autoGainTicks = 0; // timer ticks left to listen for AUTO GAIN (0: not listening)

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AegisEditor)
};
