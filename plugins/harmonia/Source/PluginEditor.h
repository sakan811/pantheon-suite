// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 The Pantheon authors

#pragma once

#include "Advice.h"
#include "PluginProcessor.h"
#include "ui/HarmoniaUi.h"
#include "ui/NoteKeyboard.h"
#include "ui/PitchGraph.h"

#include <pantheon/ui/AbCompare.h>
#include <pantheon/ui/Advice.h>
#include <pantheon/ui/Undo.h>
#include <juce_audio_processors/juce_audio_processors.h>

class HarmoniaEditor : public juce::AudioProcessorEditor, private juce::Timer
{
public:
    explicit HarmoniaEditor (HarmoniaProcessor&);
    ~HarmoniaEditor() override;

    void resized() override;

    juce::TextButton& guide() noexcept { return guideButton; }                          // for the selftest
    juce::TextButton& detect() noexcept { return detectButton; }                        // for the selftest
    ui::UndoButtons& undoControls() noexcept { return undoButtons; }                   // for the selftest
    ui::AbButtons& abControls() noexcept { return abButtons; }                          // for the selftest
    juce::String status() const { return statusLabel.getText(); }                       // for the selftest
    std::vector<ui::Note> notes() const { return harmonia::adviceFor (adviceInput()); } // what the strip under the controls says, for the selftest

    // The buttons under the piano keys, also callable from the selftest: all notes, none, the notes flipped, the scale of the key again.
    enum class NoteAction { all, none, invert, scale };
    void noteAction (NoteAction);

    static constexpr int kDesignW = 880, kDesignH = 584 + 6 + 36 + 0; // the editor is drawn at this size and scaled; the foot strip is the notes' (or GUIDE's reminder)

private:
    static constexpr int kHeaderH = 84;
    static inline const juce::Rectangle<int> kNameplateBounds { 12, 8, 230, 68 };
    static inline const juce::Rectangle<int> kLatencyBounds { 262, 36, 96, 28 };

    struct Content : juce::Component
    {
        std::function<void (juce::Graphics&)> onPaint;
        void paint (juce::Graphics& g) override
        {
            if (onPaint)
                onPaint (g);
        }
    };

    void paintContent (juce::Graphics&);
    harmonia::AdviceInput adviceInput() const;
    void timerCallback() override;

    // Sets any parameter from its plain value (choice index, bool as 0/1, or float), as one host gesture.
    void setParam (const juce::String& id, float plainValue);
    void toggleNote (int pitchClass);
    void setNotes (uint16_t mask); // switches to Custom and sets the twelve note switches
    void applyPreset (int index);
    void onDetectClicked();
    juce::String makeLatencyText() const;
    void setupKnob (int index, const char* id, const juce::String& title, const juce::String& tip);

    HarmoniaProcessor& proc;
    HardwareLookAndFeel laf;
    Content content;
    juce::TooltipWindow tooltips { this, 500 };

    PitchGraph graph;
    NoteKeyboard keyboard;
    ui::Choice rangeBox, keyBox, scaleBox;
    ui::Toggle bypassButton, autoKeyButton;
    juce::TextButton detectButton { "Detect key" };
    juce::TextButton resetButton { "RESET" };
    ui::UndoButtons undoButtons { proc.undo, *this };
    ui::AbButtons abButtons { proc.ab };
    ui::GuideButton guideButton { harmonia::guideTitle(), harmonia::guideText() };
    juce::TextButton presetButtons[3];
    juce::TextButton noteButtons[4];
    int lastRealScale = harmonia::kMajorScale; // the scale last chosen that was not Custom: what the SCALE button returns to
    juce::Label statusLabel;
    std::array<std::unique_ptr<ui::Knob>, 6> knobs;

    bool keysLocked = false; // the key follows the vocal: the piano keys cannot be edited
    bool awaitingKeyResult = false;
    bool awaitingListen = false; // Detect was clicked and the processor has not started listening yet
    juce::uint32 detectClickedAt = 0;
    juce::uint32 finishClickedAt = 0;
    juce::String detectedText, latencyText, shownNotes;
    bool singing = false; // the nameplate lamp: a voice is being tracked

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (HarmoniaEditor)
};
