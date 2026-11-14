// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 The Pantheon authors

#pragma once

#include "Advice.h"
#include "PluginProcessor.h"
#include "UI/HestiaUi.h"
#include "UI/SpaceView.h"
#include <pantheon/ui/Advice.h>

// The display of the mix's space, the stem's role and mix in the header, the knobs that say how far back it sits and how it gives room, and the
// sidechain's role. Everything drawn is the
// shared kit's (shared/ui).
class HestiaEditor : public juce::AudioProcessorEditor, private juce::Timer
{
public:
    explicit HestiaEditor (HestiaProcessor&);
    ~HestiaEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

    ui::Choice roleBox, mixBox, scRoleBox;
    ui::Knob importance, distance, carve, reach, attack, release, outKnob, scImportance;
    ui::Toggle holdButton;
    juce::TextButton deltaButton { "DELTA" }, copyButton { "COPY TO MIX" };
    ui::PresetBox presetBox;
    ui::GuideButton guideButton { hestia::guideTitle(), hestia::guideText() };
    hestia::gui::SpaceView view;
    juce::TextButton& guide() noexcept { return guideButton; }
    void copyToMix(); // what the COPY TO MIX button does (the checks call it too)
    std::vector<ui::Note> notes() const { return hestia::adviceFor (adviceInput()); } // what the strip under the knobs says, for the checks

private:
    void timerCallback() override;
    hestia::AdviceInput adviceInput() const;

    HestiaProcessor& proc;
    HardwareLookAndFeel look;
    juce::TooltipWindow tooltips { this, 600 };
    juce::Rectangle<int> nameplate, presetLabel, roleLabel, mixLabel, scPanel, scLabel, knobPanel, noteStrip;
    float inLevel = 0.0f;
    int sentTo = -1, sentTicks = 0; // what COPY TO MIX did last, shown in the strip for a few seconds

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (HestiaEditor)
};
