// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 The Pantheon authors

#pragma once

#include "Advice.h"
#include "PluginProcessor.h"
#include "UI/ApolloUi.h"
#include "UI/CurveView.h"
#include <pantheon/ui/Advice.h>

// The curve of the eight bands on glass, and a strip under it for each; the phase mode and the output gain in the header. Everything drawn is the
// shared kit's (shared/ui).
class ApolloEditor : public juce::AudioProcessorEditor, private juce::Timer
{
public:
    explicit ApolloEditor (ApolloProcessor&);
    ~ApolloEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

    // One band's controls. A gain exists for a bell and the shelves; a slope for a high-pass and a low-pass; a Q for the rest.
    struct Strip
    {
        Strip (ApolloProcessor&, int band);
        ui::Toggle on;
        ui::Choice type, target, slope;
        ui::Knob freq, gain, q;
        juce::Rectangle<int> bounds;
    };
    std::array<std::unique_ptr<Strip>, apollo::kBands> strips;
    ui::Choice phaseBox;
    ui::Knob outKnob;
    apollo::gui::CurveView view;
    ui::PresetBox presetBox;
    ui::GuideButton guideButton { apollo::guideTitle(), apollo::guideText() };
    juce::TextButton& guide() noexcept { return guideButton; }
    std::vector<ui::Note> notes() const { return apollo::adviceFor (adviceInput()); } // what the strip under the bands says, for the checks
    void showTypes(); // which controls each strip shows (the timer calls it; the checks too)

private:
    void timerCallback() override;
    apollo::AdviceInput adviceInput() const;

    ApolloProcessor& proc;
    HardwareLookAndFeel look;
    juce::TooltipWindow tooltips { this, 600 };
    juce::Rectangle<int> nameplate, presetLabel, phaseLabel, latencyLabel, noteStrip, stripArea;
    int shownTypes = -1;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ApolloEditor)
};
