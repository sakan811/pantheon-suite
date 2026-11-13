// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 The Pantheon authors

#pragma once

#include "DSP/Engine.h"
#include <pantheon/ui/Note.h>
#include <string>
#include <vector>

namespace nemesis
{
// What the editor tells the user about the settings, so that a de-esser is not used for what it cannot do and what is heard is not mistaken for the result.
// Plain C++, no JUCE (the notes are shared/ui Note.h); each rule restates a limit under "Limits" in the README, and UiCheck pins it.
struct AdviceInput
{
    EngineParams params;
    float reductionDb = 0.0f; // what it is taking off now
    float heldDb = 0.0f;      // the most in the last few seconds
};

constexpr float kHeavyDb = 10.0f;
constexpr float kTopThresholdAbsDb = -3.0f;

inline std::vector<ui::Note> adviceFor (const AdviceInput& in)
{
    std::vector<ui::Note> notes;
    const auto warn = [&] (std::string t) { notes.push_back ({ ui::Severity::warning, std::move (t) }); };
    const auto info = [&] (std::string t) { notes.push_back ({ ui::Severity::info, std::move (t) }); };
    const auto& p = in.params;

    if (p.listen)
        warn ("LISTEN is on: you hear only the band the detector hears, not your sound. Switch it off.");
    if (p.rangeDb <= 0.05f)
        warn ("RANGE is 0 dB: Nemesis takes nothing off. Raise it.");
    if (p.detection == Detection::absolute && p.thresholdDb >= kTopThresholdAbsDb)
        warn ("THRESH is at the top: the band hardly ever gets over it, so nothing is turned down. Lower it until the meter moves.");
    if (in.heldDb >= kHeavyDb)
        warn ("Taking off " + std::to_string ((int) (in.heldDb + 0.5f)) + " dB or more makes a lisp: the voice sounds dull. Raise THRESH or lower RANGE.");
    if (p.mode == Mode::wideband && in.heldDb >= 4.0f)
        info ("WIDEBAND turns the whole voice down with each s: it can sound as if it ducks. SPLIT-BAND does not.");
    if (p.detection == Detection::relative)
        info ("RELATIVE reads the band against the whole sound: THRESH is how loud the s may be next to the voice.");
    if (p.freqHz < 4500.0f)
        info ("Under 4.5 kHz you are on the voice's body, not its s (male 4-7 kHz, female 6-10). Find it with LISTEN.");
    return ui::ordered (std::move (notes));
}

inline const char* guideTitle() { return "Nemesis Hush"; }

inline const char* guideText()
{
    return "FOR\n"
           "Taming the s, sh and t of a voice (sibilance): it listens to a band (FREQ and WIDTH), and when the band is louder than THRESH it turns it down, "
           "by up to RANGE. SPLIT-BAND turns down only that band, so the rest of the voice is not touched; WIDEBAND turns the whole sound down.\n\n"
           "FINDING IT\n"
           "Switch LISTEN on and move FREQ until you hear the hiss of the s: that is the band the detector hears. RELATIVE detection compares the band with the "
           "whole sound, so one threshold works for a quiet and a loud passage.\n\n"
           "NOT FOR\n"
           "Not a cure for a harsh recording: taking off more than about 10 dB makes a lisp. With LOOK-AHEAD off, the very first millisecond of a loud s gets through. "
           "It is not a dynamic equaliser (Iris is).";
}
} // namespace nemesis
