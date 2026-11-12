// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 The Pantheon authors

#pragma once

#include "DSP/Engine.h"
#include <pantheon/ui/Note.h>
#include <string>
#include <vector>

namespace dionysus
{
// What the editor tells the user about the settings, so that a saturator is not used for what it cannot do and what is heard is not mistaken for the result.
// Plain C++, no JUCE (the notes are shared/ui Note.h); each rule restates a limit under "Limits" in the README, and UiCheck pins it.
struct AdviceInput
{
    EngineParams params;
    double latencyMs = 0.0;
};

constexpr float kHeavyDriveDb = 24.0f;
constexpr float kAliasDriveDb = 12.0f;

inline std::vector<ui::Note> adviceFor (const AdviceInput& in)
{
    std::vector<ui::Note> notes;
    const auto warn = [&] (std::string t) { notes.push_back ({ ui::Severity::warning, std::move (t) }); };
    const auto info = [&] (std::string t) { notes.push_back ({ ui::Severity::info, std::move (t) }); };
    const auto& p = in.params;

    if (p.mix <= 0.001f)
        warn ("MIX is 0%: you hear only the dry sound. Raise it.");
    if (p.oversampling == 0 && p.driveDb >= kAliasDriveDb && (p.shape == Shape::clip || p.shape == Shape::fold || p.shape == Shape::transformer))
        warn ("This curve at this DRIVE without oversampling aliases harshly (tones that were never there). Use 4x or 8x.");
    if (p.driveDb >= kHeavyDriveDb)
        info ("A DRIVE of " + std::to_string ((int) (p.driveDb + 0.5f)) + " dB or more is distortion, not warmth: the sound will lose its dynamics.");
    if (! p.autoGain)
        info ("AUTO GAIN is off: more DRIVE is also louder, and loud sounds better. Compare at the same level.");
    if (std::fabs (p.bias) > 0.005f)
        info ("BIAS makes even harmonics (a rounder, thicker sound); a DC blocker at 10 Hz takes away the offset it makes.");
    if (p.mix > 0.001f && p.mix < 0.999f && p.oversampling > 0)
        info ("With MIX under 100% the dry sound is delayed to match the oversampler: nothing combs.");
    if (p.oversampling > 0 && in.latencyMs > 0.0)
        info ("Oversampling delays the sound by " + std::to_string ((int) (in.latencyMs + 0.5)) + " ms (the host compensates it). It is off at 1x.");
    return ui::ordered (std::move (notes));
}

inline const char* guideTitle() { return "Dionysus Warmth"; }

inline const char* guideText()
{
    return "FOR\n"
           "Adding harmonics: it pushes the sound through a curve (CURVE) that rounds off the peaks, so the sound gets denser, thicker or harsher "
           "the more DRIVE it gets. TAPE and TUBE are gentle, TRANSFORMER is thicker, DIODE is uneven (it adds even harmonics, a rounder sound), FOLD turns "
           "peaks back on themselves, CLIP cuts them. BIAS moves the working point to add even harmonics, TONE tilts the result bright or dark.\n\n"
           "THE DISPLAY\n"
           "The curve shows what each level in becomes; the bars show the harmonics a -12 dBFS sine gets, in dB under the sound that went in.\n\n"
           "NOT FOR\n"
           "Not a limiter or a clipper for loudness (Aegis limits), and not a tool to fix a bad recording. Use OVERSAMPLING for hard curves: without it the "
           "harmonics above half the sample rate fold back as tones that were never there.";
}
} // namespace dionysus
