// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 The Pantheon authors

#pragma once

#include "PluginProcessor.h"
#include "UI/HephaestusUi.h"
#include "UI/SpectrumView.h"
#include <juce_gui_basics/juce_gui_basics.h>

class HephaestusEditor : public juce::AudioProcessorEditor, private juce::Timer
{
public:
    explicit HephaestusEditor (HephaestusProcessor&);
    ~HephaestusEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseDown (const juce::MouseEvent&) override;

    void selectBand (int band);

private:
    enum class Panel { band, character, master, count };

    void addKnob (const char* paramId, const char* caption, const char* tooltip);
    void layoutPanelKnobs (Panel);
    void layoutBandPanel();
    void refreshBandPanel();
    int bandSignature() const;
    juce::String bandHint() const;
    void timerCallback() override;

    HephaestusProcessor& proc;
    HardwareLookAndFeel look;
    hep::gui::SpectrumView spectrum;
    juce::TooltipWindow tooltips { this, 600 };

    std::vector<std::unique_ptr<ui::Knob>> knobs; // character and master knobs, same order as the table in the .cpp
    std::array<std::unique_ptr<ui::Knob>, hep::maxBands> bandKnobs; // re-attached to whichever band is selected
    int bandKnobCount = 0;

    juce::ComboBox presetBox;
    ui::Choice stereoBox, osBox, curveBox;
    ui::Toggle deltaButton, matchButton;
    juce::TextButton clearMetersButton { "CLEAR" };

    juce::TextButton saturateModeButton, fillModeButton, removeButton, addSaturateButton, addFillButton;
    std::array<juce::Rectangle<int>, hep::maxBands> chipBounds;
    int shownBandSignature = -1;

    float inLevel = 0.0f, outLevel = 0.0f;
    std::array<juce::Rectangle<int>, (size_t) Panel::count> panelBounds;
    juce::Rectangle<int> nameplate, meterArea;
    int lastProgram = -1;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (HephaestusEditor)
};
