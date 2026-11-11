// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 The Pantheon authors

#pragma once

#include "Advice.h"
#include "PluginProcessor.h"
#include "UI/HephaestusUi.h"
#include "UI/SpectrumView.h"
#include <pantheon/ui/Advice.h>
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
    juce::TextButton& regionButton (int band) { return regionButtons[(size_t) band]; } // the band solo strip, for UiCheck
    juce::TextButton& guide() { return guideButton; }                                  // the GUIDE button, for UiCheck
    std::vector<ui::Note> notes() const { return hep::adviceFor (adviceInput()); }                // what the band panel tells the user, for UiCheck
    juce::ComboBox& characterSelector() { return characterBox; }                     // Warm / Clean / Octave of the shown fill band, for UiCheck

private:
    enum class Panel { band, character, master, count };

    void addKnob (const char* paramId, const char* caption, const char* tooltip);
    void layoutPanelKnobs (Panel);
    void layoutBandPanel();
    void refreshBandPanel();
    int bandSignature() const;
    juce::String bandHint() const;
    hep::AdviceInput adviceInput() const;
    void setPresetTooltip();
    juce::String notesSignature() const;
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
    ui::GuideButton guideButton { hep::guideTitle(), hep::guideText() };

    // How the selected fill band makes its harmonics: bound to that band's parameter while one is shown, and to nothing otherwise.
    juce::ComboBox characterBox;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> characterAttachment;

    juce::TextButton saturateModeButton, fillModeButton, removeButton, addSaturateButton, addFillButton, pasteButton { "PASTE" }, soloBandButton { "SOLO BAND" }, soloAddsButton { "SOLO ADDS" };
    std::array<juce::Rectangle<int>, hep::maxBands> chipBounds;
    int shownBandSignature = -1, shownSolo = -1;
    void toggleSolo (hep::SoloKind kind);

    // The band solo strip: one switch per main band of music (pdsp::bands), under the display and lined up with its axis.
    std::array<juce::TextButton, pdsp::bands::kCount> regionButtons;
    void toggleRegion (int band, bool alone);

    float inLevel = 0.0f, outLevel = 0.0f;
    std::array<juce::Rectangle<int>, (size_t) Panel::count> panelBounds;
    juce::Rectangle<int> nameplate, meterArea;
    int lastProgram = -1;
    juce::String shownNotes;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (HephaestusEditor)
};
