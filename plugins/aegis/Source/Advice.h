// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 The Pantheon authors

#pragma once

#include "DSP/Engine.h"
#include <pantheon/ui/Note.h>
#include <string>
#include <vector>

namespace aegis
{
// What the editor tells the user about the settings, so that a limiter is not used for what it cannot do and what is heard is not mistaken for the
// result. Plain C++, no JUCE (the notes are shared/ui Note.h); each rule restates a limit under "Limits" in the README, and UiCheck pins it.
struct AdviceInput
{
    EngineParams params;
    float reductionDb = 0.0f; // what the limiter is taking off now (positive)
    float heldDb = 0.0f;      // the most it took off in the last few seconds
    double latencyMs = 0.0;
};

constexpr float kHeavyDb = 6.0f;       // from this much reduction a limiter is no longer transparent
constexpr float kHotCeilingDb = -0.3f; // above this a codec or a converter can still go over
constexpr float kFastReleaseMs = 40.0f;

inline std::vector<ui::Note> adviceFor (const AdviceInput& in)
{
    std::vector<ui::Note> notes;
    const auto warn = [&] (std::string t) { notes.push_back ({ ui::Severity::warning, std::move (t) }); };
    const auto info = [&] (std::string t) { notes.push_back ({ ui::Severity::info, std::move (t) }); };
    const auto& p = in.params;

    if (! p.truePeak)
        warn ("TRUE PEAK is off: only the samples are held; the sound between them can go over. Switch it on for a master.");
    if (p.delta)
        warn ("DELTA is on: you hear what the limiter takes away, not the sound. Switch it off before you bounce.");
    if (p.matchLevel && ! p.delta)
        warn ("MATCH is on: the output is turned down to the input's loudness. Switch it off before you bounce.");
    if (p.ceilingDb > kHotCeilingDb)
        warn ("A ceiling above -0.3 dBTP leaves no room for a codec or converter, which can add peaks. -1 dBTP is usual.");
    if (in.heldDb >= kHeavyDb)
        warn ("Taking off " + std::to_string ((int) (in.heldDb + 0.5f)) + " dB or more: it will sound squashed. Lower GAIN, or compress before the limiter.");
    if (p.releaseMs < kFastReleaseMs && in.heldDb >= 3.0f)
        info ("A RELEASE under 40 ms with this much reduction follows the waveform of the bass: it can distort. Try 100 ms or more.");
    if (p.gainDb <= 0.0f && in.heldDb <= 0.0f)
        info ("Nothing is being limited. Raise GAIN until the meter moves: the ceiling is where the output stops.");
    if (in.latencyMs > 0.0)
        info ("The limiter delays the sound by " + std::to_string ((int) (in.latencyMs + 0.5)) + " ms (the host compensates it). Do not put it on a send you monitor live.");
    return ui::ordered (std::move (notes));
}

inline const char* guideTitle() { return "Aegis Ceiling"; }

inline const char* guideText()
{
    return "FOR\n"
           "The last stage of a master: it raises the level (GAIN) and keeps every peak under a ceiling, including the peaks between the samples "
           "(true peak), so a converter or a codec after it cannot go over.\n\n"
           "HOW\n"
           "It looks ahead (LOOK-AHEAD) and turns the sound down before a peak arrives, so there is no click, and lets go at RELEASE. "
           "Both channels share one gain, so the stereo image does not move. AUTO RELEASE lets a short peak go sooner than RELEASE. "
           "OUT PEAK is the highest true peak that leaves; MAX under REDUCTION is the most it took off (click it, or CLEAR, to start again).\n\n"
           "LISTENING\n"
           "DELTA plays what the limiter takes away: if you hear the music and not just the peaks, it is working too hard. "
           "MATCH turns the output down to the input's loudness, so a higher GAIN does not win the comparison only by being louder. Switch both off before you bounce.\n\n"
           "WITH THEMIS\n"
           "Put Themis after it. COPY on its plan's ceiling or loudness item, then PASTE here: it sets CEILING and moves GAIN by the amount "
           "Themis measured. Aegis does not measure loudness; Themis does.\n\n"
           "NOT FOR\n"
           "Not a substitute for a compressor: taking off more than about 6 dB squashes the sound, and a fast RELEASE distorts bass. "
           "It does not clip, and does not measure loudness.";
}
} // namespace aegis
