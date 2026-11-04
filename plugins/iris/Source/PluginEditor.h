// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 The Pantheon authors

#pragma once

#include "PluginProcessor.h"
#include "UI/EqView.h"
#include "UI/IrisUi.h"
#include <juce_gui_basics/juce_gui_basics.h>

class IrisEditor : public juce::AudioProcessorEditor, private juce::Timer
{
public:
    explicit IrisEditor (IrisProcessor&);
    ~IrisEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseDown (const juce::MouseEvent&) override;

    void selectBand (int band);

private:
    enum class Panel { band, dynamics, output, count };

    void refreshBandPanel();
    void layoutBandPanel();
    int bandSignature() const;
    juce::String bandHint() const;
    juce::String dynamicsHint() const;
    void timerCallback() override;

    IrisProcessor& proc;
    HardwareLookAndFeel look;
    iris::gui::EqView view;
    juce::TooltipWindow tooltips { this, 600 };

    // The selected band's controls. The knobs are re-bound to whichever band is selected; a choice or a switch is bound for
    // good, so each band has its own and only the selected band's is shown.
    ui::Knob freqKnob, gainKnob, qKnob;
    ui::Knob thresholdKnob, ratioKnob, rangeKnob, attackKnob, releaseKnob;
    std::array<std::unique_ptr<ui::Choice>, iris::maxBands> typeBoxes, placeBoxes, slopeBoxes, dynModeBoxes;
    std::array<std::unique_ptr<ui::Toggle>, iris::maxBands> dynToggles;
    juce::TextButton removeButton { "REMOVE" }, addButton { "ADD BAND" }, soloBandButton { "BAND" }, soloCurveButton { "CURVE" };
    std::array<juce::Rectangle<int>, iris::maxBands> chipBounds;
    int shownSignature = -1;

    // Output and saturation.
    ui::Knob driveKnob, outKnob;
    ui::Choice styleBox, modeBox;

    juce::ComboBox presetBox;
    juce::TextButton deltaButton { "DELTA" }, clearMetersButton { "CLEAR" };
    ui::Toggle matchButton;

    float inLevel = 0.0f, outLevel = 0.0f;
    std::array<juce::Rectangle<int>, (size_t) Panel::count> panelBounds;
    juce::Rectangle<int> nameplate, meterArea, dynReadout;
    int lastProgram = -1;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (IrisEditor)
};
