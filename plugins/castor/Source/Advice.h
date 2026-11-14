// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 The Pantheon authors

#pragma once

#include "DSP/Engine.h"
#include <pantheon/ui/Note.h>
#include <string>
#include <vector>

namespace castor
{
// What the editor tells the user about the image, so that a widener is not used for what it cannot do and what is heard is not mistaken for the result.
// Plain C++, no JUCE (the notes are shared/ui Note.h); each rule restates a limit under "Limits" in the README, and UiCheck pins it.
struct AdviceInput
{
    EngineParams params;
    float correlation = 1.0f; // of the output: +1 mono .. -1 out of phase
};

constexpr float kWideWidth = 1.5f;
constexpr double kBassTopHz = 250.0;
constexpr float kNegativeCorrelation = -0.1f;

inline bool isNeutral (const EngineParams& p) noexcept
{
    return std::all_of (p.width.begin(), p.width.end(), [] (float w) { return std::fabs (w - 1.0f) < 0.005f; }) && std::fabs (p.midDb) < 0.05f && std::fabs (p.sideDb) < 0.05f &&
           p.monoBelowHz <= kMonoOffHz + 0.5f && std::fabs (p.balance) < 0.005f && std::fabs (p.outDb) < 0.05f;
}

inline std::vector<ui::Note> adviceFor (const AdviceInput& in)
{
    std::vector<ui::Note> notes;
    const auto warn = [&] (std::string t) { notes.push_back ({ ui::Severity::warning, std::move (t) }); };
    const auto info = [&] (std::string t) { notes.push_back ({ ui::Severity::info, std::move (t) }); };
    const auto& p = in.params;

    if (p.listen != Listen::stereo)
        warn ("LISTEN is not Stereo: you hear a check, not your sound. Set it back to Stereo before you bounce.");
    if (isNeutral (p))
        info ("Nothing is changed yet: turn a width, MID, SIDE or MONO BELOW.");
    if (in.correlation < kNegativeCorrelation)
        warn ("The sound is out of phase (correlation below 0): it will thin out or vanish in mono. Lower a width or SIDE.");
    for (int b = 0; b < kBands; ++b)
        if (p.width[(size_t) b] > kWideWidth && pdsp::bands::kBands[(size_t) b].hiHz <= kBassTopHz && p.monoBelowHz <= kMonoOffHz + 0.5f)
        {
            warn ("A wide " + std::string (pdsp::bands::kBands[(size_t) b].name) + " is the first thing to vanish on a mono or club system. Use MONO BELOW instead.");
            break;
        }
    if (std::all_of (p.width.begin(), p.width.end(), [] (float w) { return w < 0.005f; }))
        info ("Every width is 0%: the output is mono.");
    if (p.sideDb > 6.0f)
        warn ("SIDE above +6 dB: the image can fall apart in mono. Check the correlation bar.");
    if (std::fabs (p.balance) > 0.005f)
        info ("BALANCE turns one channel down: it is not a pan, and the loudness changes.");
    if (! isNeutral (p) && std::any_of (p.width.begin(), p.width.end(), [] (float w) { return std::fabs (w - 1.0f) > 0.005f; }))
        info ("Widths turn the phase around the band edges (the level stays flat); at 100% everywhere nothing is touched.");
    return ui::ordered (std::move (notes));
}

inline const char* guideTitle() { return "Castor Pair"; }

inline const char* guideText()
{
    return "FOR\n"
           "The width of a stereo sound, band by band: what is the same in both channels is the middle, what differs is the sides, and each of the six bands "
           "of music (Sub, Bass, Low-Mid, Midrange, Upper-Mid, High) has its own WIDTH for its sides: 0% is mono, 100% as it is, 200% twice the sides. "
           "MID and SIDE set their levels as a whole, MONO BELOW takes the sides away under a frequency so the bass stays in the middle, "
           "BALANCE turns one channel down.\n\n"
           "READING IT\n"
           "The goniometer draws the left and right as a cloud: a vertical line is mono, a horizontal one is out of phase. The correlation bar reads +1 for mono, "
           "0 for wide, and below 0 for a sound that will cancel in mono; MIN is the lowest it has read since you last clicked it. "
           "LISTEN checks the result: Mono plays the sum of both channels (what a mono speaker plays), Side only what differs. Set it back to Stereo before you bounce.\n\n"
           "NOT FOR\n"
           "It adds nothing: it does not make a mono recording stereo (there are no sides to widen), and no widener fixes a bad mix. Always listen in mono.";
}
} // namespace castor
