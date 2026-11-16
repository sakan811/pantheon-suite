// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 The Pantheon authors

#pragma once

#include "Advice.h"
#include "PluginProcessor.h"
#include "UI/AtlasUi.h"
#include "UI/BandView.h"
#include <pantheon/ui/Advice.h>

// The display (the spectrogram the bands are made on) and a strip for each band, with its compressor, its solo and its delta; the split's phase mode, the oversampling
// and the output gain in the header. Everything drawn is the shared kit's (shared/ui).
class AtlasEditor : public juce::AudioProcessorEditor, private juce::Timer
{
public:
    explicit AtlasEditor (AtlasProcessor&);
    ~AtlasEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

    // One band's controls. Solo and delta are monitoring (Engine::monitor), not parameters: plain buttons, lit from the timer.
    struct Strip
    {
        Strip (AtlasProcessor&, int band);
        ui::Toggle on, match, autoThr, autoRel; // AUTO T: THRESH is the distance under the band's own slow level; AUTO R: the release adapts to the programme
        juce::TextButton solo { "SOLO" }, delta { "DELTA" };
        juce::TextButton learn { "LEARN" }, toAll { "TO ALL" }; // set THRESH, RATIO, ATTACK and RELEASE from four seconds of the band; copy THRESH, RATIO, ATTACK, RELEASE to every band
        ui::Choice channels, listen, direction; // what the compressor works on; what it listens to (its own band or the sidechain's); whether it turns down or lifts
        ui::Knob threshold, ratio, attack, release, makeup;
        std::unique_ptr<ui::Knob> crossover; // the edge to the next band; the top band has none
        std::vector<juce::Component*> components()
        {
            std::vector<juce::Component*> all { &on, &match, &solo, &delta, &learn, &toAll, &autoThr, &autoRel, &channels, &listen, &direction, &threshold, &ratio, &attack, &release, &makeup };
            if (crossover != nullptr)
                all.push_back (crossover.get());
            return all;
        }
        juce::Rectangle<int> bounds, meter, histogram;
        float shownReduction = 0.0f;
        std::array<float, atlas::Engine::kHistogramBins> shownLevels {}; // how often the band's level was in each dB lately (drawn under the meter)
    };
    std::array<std::unique_ptr<Strip>, atlas::kBands> strips;
    juce::TextButton clearButton { "ALL BANDS" };
    juce::TextButton learnAllButton { "LEARN ALL" }, targetButton { "TARGET" };
    // TARGET with the text of the clipboard (a curve another plugin of the repository copied): false, and a message on the display, when it holds none.
    bool useTarget (const juce::String& clipboardText);
    ui::GuideButton guideButton { atlas::guideTitle(), atlas::guideText() };
    atlas::gui::BandView view;
    juce::TextButton& guide() noexcept { return guideButton; }                    // for the checks
    std::vector<ui::Note> notes() const { return atlas::adviceFor (adviceInput()); } // what the strip under the bands says, for the checks

private:
    void timerCallback() override;
    void layoutStrips();
    int layoutSignature() const;
    atlas::AdviceInput adviceInput() const;

    AtlasProcessor& proc;
    HardwareLookAndFeel look;
    juce::TooltipWindow tooltips { this, 600 };
    ui::Choice phaseBox, oversamplingBox, lookaheadBox;
    juce::ComboBox presetBox;
    ui::Knob outKnob, mixKnob;
    juce::Rectangle<int> nameplate, phaseLabel, oversamplingLabel, lookaheadLabel, presetLabel, latencyLabel;
    int lastProgram = -1;
    void setPresetTooltip();
    float inLevel = 0.0f;
    juce::Rectangle<int> stripArea, noteStrip, toolbar;
    int shownLayout = -1;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AtlasEditor)
};
