// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 The Pantheon authors

#pragma once

#include "Advice.h"
#include "PluginProcessor.h"
#include "UI/CastorUi.h"
#include <array>
#include <pantheon/ui/Advice.h>
#include <pantheon/ui/Undo.h>

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
    ui::Choice listen;
    juce::TextButton minButton { "MIN --" }; // the lowest correlation since it was last clicked
    float correlationMinHold() const noexcept { return minHold; }
    ui::PresetBox presetBox;
    ui::GuideButton guideButton { castor::guideTitle(), castor::guideText() };
    juce::TextButton& guide() noexcept { return guideButton; }
    juce::TextButton pasteButton { "PASTE" }; // the width a Themis plan suggests (COPY on a width item), from the clipboard
    ui::UndoButtons undoButtons;               // UNDO / REDO, in the header strip (shared/ui Undo.h)
    ui::UndoButtons& undoControls() noexcept { return undoButtons; } // for UiCheck
    std::vector<ui::Note> notes() const { return castor::adviceFor (adviceInput()); } // what the strip under the controls says, for the checks
    std::vector<ui::Note> shownNotes() const; // the notes, with what the last PASTE did first while it is shown
    void pasteFromClipboard (const juce::String& text); // what PASTE does with the clipboard's text (a width suggestion from Themis' plan)
    void tickDisplay() { timerCallback(); }

private:
    void timerCallback() override;
    void drawWidthGlass (juce::Graphics&) const;
    castor::AdviceInput adviceInput() const;
    void setStripNote (std::string text, int ticks); // a line of the strip for `ticks` timer ticks, ahead of the notes (PASTE uses it)

    CastorProcessor& proc;
    HardwareLookAndFeel look;
    juce::TooltipWindow tooltips { this, 600 };
    juce::Rectangle<int> nameplate, presetLabel, gonio, widthGlass, correlationBar, noteStrip, bandsLabel, otherLabel;
    std::array<float, kPoints> pointL {}, pointR {};
    std::vector<float> scratchL, scratchR;
    float correlation = 1.0f, minHold = 1.0f;
    bool live = false;
    std::string stripSaid;
    int stripTicks = 0; // timer ticks left to show stripSaid

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (CastorEditor)
};
