// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 The Pantheon authors

#pragma once

#include "Advice.h"
#include "PluginProcessor.h"
#include "UI/DionysusUi.h"
#include <pantheon/ui/Advice.h>

// The curve and the harmonics it makes on glass, five knobs, the curve and oversampling selectors and AUTO GAIN. Everything drawn is the shared kit's (shared/ui).
class DionysusEditor : public juce::AudioProcessorEditor, private juce::Timer
{
public:
    explicit DionysusEditor (DionysusProcessor&);
    ~DionysusEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

    ui::Knob drive, bias, tone, mix, out;
    ui::Choice shape, oversampling;
    ui::Toggle autoGain;
    ui::PresetBox presetBox;
    ui::GuideButton guideButton { dionysus::guideTitle(), dionysus::guideText() };
    juce::TextButton& guide() noexcept { return guideButton; }
    std::vector<ui::Note> notes() const { return dionysus::adviceFor (adviceInput()); } // what the strip under the controls says, for the checks

private:
    void timerCallback() override;
    dionysus::AdviceInput adviceInput() const;

    DionysusProcessor& proc;
    HardwareLookAndFeel look;
    juce::TooltipWindow tooltips { this, 600 };
    juce::Rectangle<int> nameplate, presetLabel, curveArea, harmonicArea, shapeLabel, oversamplingLabel, latencyLabel, noteStrip;
    float level = 0.0f;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (DionysusEditor)
};
