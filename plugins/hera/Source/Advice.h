// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 The Pantheon authors

#pragma once

#include "DSP/Engine.h"
#include <pantheon/ui/Note.h>
#include <string>
#include <vector>

namespace hera
{
// What the editor tells the user about the settings, so that a compressor is not used for what it cannot do and what is heard is not mistaken for the result.
// Plain C++, no JUCE (the notes are shared/ui Note.h); each rule restates a limit under "Limits" in the README, and UiCheck pins it.
struct AdviceInput
{
    EngineParams params;
    float reductionDb = 0.0f;     // what it is taking off now
    float heldDb = 0.0f;          // the most in the last few seconds
    bool sidechainConnected = false;
    bool delta = false;    // DELTA is on (the editor's monitor, not a parameter)
    bool scListen = false; // SC LISTEN is on (the same kind of monitor)
};

constexpr float kHeavyDb = 10.0f;
constexpr float kFastAttackMs = 2.0f;
constexpr float kThresholdAtTopDb = -1.0f;

inline std::vector<ui::Note> adviceFor (const AdviceInput& in)
{
    std::vector<ui::Note> notes;
    const auto warn = [&] (std::string t) { notes.push_back ({ ui::Severity::warning, std::move (t) }); };
    const auto info = [&] (std::string t) { notes.push_back ({ ui::Severity::info, std::move (t) }); };
    const auto& p = in.params;

    if (in.scListen)
        warn ("SC LISTEN is on: you hear the detector's key, not your track. Switch it off to hear it.");
    else if (in.delta)
        warn ("DELTA is on: you hear only what the compressor changes, not your track. Switch it off to hear it.");
    if (p.ratio <= 1.05f)
        warn ("RATIO is 1:1, which does nothing. Raise it.");
    if (p.thresholdDb >= kThresholdAtTopDb)
        warn ("THRESH is at the top, so almost nothing is compressed. Lower it until the meter moves.");
    if (p.external && ! in.sidechainConnected)
        warn ("KEY is on, but no sidechain is connected, so it listens to itself. Route a track to the sidechain.");
    if (in.heldDb >= kHeavyDb)
        warn ("Taking off " + std::to_string ((int) (in.heldDb + 0.5f)) + " dB or more: it will pump. Raise THRESH or lower RATIO.");
    if (p.ratio >= 10.0f && p.attackMs <= kFastAttackMs)
        info ("A high RATIO with a fast ATTACK is limiting, but Hera reads no true peak. Use a limiter after it.");
    if (p.attackMs <= kFastAttackMs && p.detector == Detector::peak)
        info ("A very fast ATTACK on the peak detector takes the transient away: slower keeps the punch.");
    if (p.mix < 0.999f && p.mix > 0.001f)
        info ("MIX under 100% blends the compressed sound with the dry one; the meter shows the compressed path only.");
    if (p.autoRelease)
        info ("AUTO release follows the programme: RELEASE is not used.");
    if (p.match)
        info ("LEVEL MATCH is on: the loudness it adds or takes away is matched away, so louder cannot win.");
    if (p.autoMakeup && p.makeupDb > 0.0f)
        info ("AUTO makeup is on: MAKEUP adds to it.");
    return ui::ordered (std::move (notes));
}

inline const char* guideTitle() { return "Hera Bond"; }

inline const char* guideText()
{
    return "FOR\n"
           "Compressing a whole track or a bus: it turns the sound down while it is over THRESH, by RATIO, so the loud and the quiet come closer. "
           "Slow ATTACK keeps the punch, fast takes it away; RELEASE sets how it lets go (AUTO follows the programme). With MIX under 100% "
           "the compressed sound is blended with the dry one (parallel compression).\n\n"
           "SIDECHAIN\n"
           "The detector can listen to a high-passed copy (SC HIGH-PASS) so the bass does not trigger it, or to another track routed to the sidechain input (KEY). SC LISTEN plays what the detector hears (after the high-pass) instead of the track.\n\n"
           "JUDGING IT\n"
           "A compressor that is louder sounds better, whatever it did. LEVEL MATCH takes the level difference off, slowly, and DELTA plays only what the compressor changes.\n\n"
           "NOT FOR\n"
           "Not a limiter: it reads no true peak, and its look-ahead is a few milliseconds. Not a multiband compressor (Atlas is) and not a dynamic equaliser (Iris is).";
}
} // namespace hera
