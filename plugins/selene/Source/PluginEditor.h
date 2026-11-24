// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 The Pantheon authors

#pragma once

#include "Advice.h"
#include "PluginProcessor.h"
#include "UI/SeleneUi.h"
#include <array>
#include <pantheon/ui/AbCompare.h>
#include <pantheon/ui/Advice.h>
#include <pantheon/ui/Undo.h>

// Lightweight 2-octave (C3-B4, MIDI 48-71) hardware-styled audition keyboard driving MidiKeyboardState.
class AuditionKeyboard : public juce::Component, public juce::SettableTooltipClient
{
public:
    static constexpr int kFirstNote = 48; // C3
    static constexpr int kNumNotes = 24;  // C3..B4
    static constexpr int kNumWhiteKeys = 14;

    explicit AuditionKeyboard (juce::MidiKeyboardState& state);
    ~AuditionKeyboard() override;

    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;

private:
    int noteAtPoint (juce::Point<int>) const noexcept;
    juce::Rectangle<float> whiteKeyRect (int whiteIdx) const noexcept;
    juce::Rectangle<float> blackKeyRect (int midiNote) const noexcept;
    static bool isBlack (int midiNote) noexcept;

    juce::MidiKeyboardState& keyboardState;
    int pressedNote = -1;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AuditionKeyboard)
};

// Selene Poly editor: filter response & oscilloscope on glass, three hardware module rows, audition keyboard, and note strip.
class SeleneEditor : public juce::AudioProcessorEditor, private juce::Timer
{
public:
    explicit SeleneEditor (SeleneProcessor&);
    ~SeleneEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

    void tickDisplay() { timerCallback(); }
    std::vector<ui::Note> notes() const { return selene::adviceFor (adviceInput()); }
    ui::UndoButtons& undoControls() noexcept { return undoButtons; }
    ui::AbButtons& abControls() noexcept { return abButtons; }
    juce::TextButton& guide() noexcept { return guideButton; }

    // Header controls
    ui::UndoButtons undoButtons;
    ui::AbButtons abButtons;
    ui::PresetBox presetBox;
    juce::TextButton prevPresetButton { "<" };
    juce::TextButton nextPresetButton { ">" };
    ui::Choice voiceMode;
    ui::GuideButton guideButton { selene::guideTitle(), selene::guideText() };

    // Row 1: OSC 1, OSC 2, FILTER
    ui::Choice osc1Wave;
    ui::Knob osc1Pwm, subLevel;

    ui::Choice osc2Wave;
    ui::Knob osc2Semi, osc2Detune, osc2Fm, oscMix, noiseLevel;

    ui::Choice filterMode;
    ui::Knob cutoff, resonance, filterEnv, keytrack, drive;

    // Row 2: AMP ENV, FILTER ENV, LFO
    ui::Knob ampA, ampD, ampS, ampR, velAmp;
    ui::Knob fltA, fltD, fltS, fltR, velFlt;

    ui::Choice lfoWave;
    ui::Knob lfoRate, lfoPitch, lfoFilter, lfoPwm;

    // Row 3: ANALOG, CHORUS & ECHO, AUDITION
    ui::Knob glide, drift, masterGain;

    ui::Choice chorusMode;
    ui::Knob echoTime, echoFeedback, echoMix;

    AuditionKeyboard auditionKeyboard;

private:
    void timerCallback() override;
    selene::AdviceInput adviceInput() const;

    SeleneProcessor& proc;
    HardwareLookAndFeel look;
    juce::TooltipWindow tooltips { this, 600 };

    juce::Rectangle<int> nameplate, presetLabel, voiceModeLabel;
    juce::Rectangle<int> filterArea, scopeArea;
    juce::Rectangle<int> osc1Mod, osc2Mod, filterMod;
    juce::Rectangle<int> ampEnvMod, fltEnvMod, lfoMod;
    juce::Rectangle<int> analogMod, fxMod, auditionMod;
    juce::Rectangle<int> osc1WaveLabel, osc2WaveLabel, filterModeLabel, lfoWaveLabel, chorusModeLabel;
    juce::Rectangle<int> noteStrip;

    std::array<float, selene::kScopeSamples> scopeData {};
    int shownVoices = 0;
    float shownPeak = 0.0f;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SeleneEditor)
};
