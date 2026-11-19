// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 The Pantheon authors

#pragma once

#include "DSP/Engine.h"
#include <cstdio>
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
    bool autoThreshAsked = false;     // AUTO THRESH was pressed and answered (the editor's, kept until the next press)
    AutoThresh autoThresh;            // its answer
    AutoSetup autoSet;                // the RATIO, ATTACK and RELEASE it set (with a set answer)
    double autoCrestDb = 0.0;         // how peaky the listened sound was (dB of peak over RMS)
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
    if (in.autoThreshAsked) // first among the infos: the answer to the last press is what the user looks for
    {
        char text[256];
        switch (in.autoThresh.result)
        {
            case AutoThresh::Result::set:
                std::snprintf (text, sizeof text, "AUTO SETUP set RATIO %.1f:1, ATTACK %.1f ms, RELEASE %d ms and THRESH %.1f dB (peaks %d dB over RMS).",
                               (double) in.autoSet.ratio, (double) in.autoSet.attackMs, (int) in.autoSet.releaseMs, (double) in.autoThresh.thresholdDb,
                               (int) (in.autoCrestDb + 0.5));
                info (text);
                info ("A starting point, about 3.5 dB of reduction on average: change any control, or UNDO takes all four back.");
                break;
            case AutoThresh::Result::silent:
                info ("AUTO SETUP heard no sound in four seconds: nothing is changed. Play the track, then press it again.");
                break;
            case AutoThresh::Result::flat:
                info ("AUTO SETUP: RATIO 1:1 reduces nothing, so THRESH could not be set from it: nothing is changed.");
                break;
            case AutoThresh::Result::quiet:
                info ("AUTO SETUP: the sound is too quiet to reach 3.5 dB of reduction, even at -60 dB: nothing is changed.");
                break;
        }
    }
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
    if (p.topology == Topology::feedback)
    {
        char text[160];
        std::snprintf (text, sizeof text, "Feedback: each dB over THRESH takes %.2f dB off at %.1f:1, Feed-forward %.2f; it never reaches 0.5 dB.",
                       steadyFeedbackReductionDb (p.thresholdDb + 1.0, p.thresholdDb, p.ratio, 0.0), (double) p.ratio,
                       reductionDb (p.thresholdDb + 1.0, p.thresholdDb, p.ratio, 0.0));
        info (text);
        if (p.lookahead > 0)
            info ("Feedback does not use LOOK-AHEAD: the output is not known early. The setting is kept for Feed-forward.");
        if (p.external && in.sidechainConnected)
            info ("Feedback is for the track's own sound: with KEY and a sidechain connected the detector reads the key.");
    }
    return ui::ordered (std::move (notes));
}

inline const char* guideTitle() { return "Hera Bond"; }

inline const char* guideText()
{
    return "FOR\n"
           "Compressing a whole track or a bus: it turns the sound down while it is over THRESH, by RATIO, so the loud and the quiet come closer. "
           "Slow ATTACK keeps the punch, fast takes it away; RELEASE sets how it lets go (AUTO follows the programme), after HOLD has kept the gain down for a set time. With MIX under 100% "
           "the compressed sound is blended with the dry one (parallel compression). TOPOLOGY Feedback takes the gain from the sound after the compression, as many classic compressors do: "
           "the same settings reduce less (at most 0.5 dB per dB over THRESH), and LOOK-AHEAD is not used.\n\n"
           "SIDECHAIN\n"
           "The detector can listen to a high-passed copy (SC HIGH-PASS) so the bass does not trigger it, or to another track routed to the sidechain input (KEY). SC LISTEN plays what the detector hears (after the high-pass) instead of the track.\n\n"
           "JUDGING IT\n"
           "A compressor that is louder sounds better, whatever it did. LEVEL MATCH takes the level difference off, slowly, and DELTA plays only what the compressor changes.\n\n"
           "NOT FOR\n"
           "Not a limiter: it reads no true peak, and its look-ahead is a few milliseconds. Not a multiband compressor (Atlas is) and not a dynamic equaliser (Iris is).";
}
} // namespace hera
