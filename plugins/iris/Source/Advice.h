// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 The Pantheon authors

#pragma once

#include "DSP/Band.h"
#include <pantheon/ui/Note.h>
#include <string>
#include <vector>

namespace iris
{
// What the editor tells the user about the band it shows and the session, so that the equaliser is not used for a job it cannot
// do and what is heard is not mistaken for the result. Plain C++, no JUCE (the notes are shared/ui Note.h); the rules restate
// limits the README measured or well-known pitfalls, and UiCheck pins each one.
struct AdviceInput
{
    // The band shown (ignored while `bandOn` is false).
    bool bandOn = false;
    BandType type = BandType::bell;
    Placement place = Placement::stereo;
    double freqHz = 1000.0, gainDb = 0.0, q = 1.0;

    // The session.
    bool delta = false, soloActive = false, gainMatch = false;
    bool satChosen = false;                 // mode Sat
    PhaseMode phase = PhaseMode::zeroLatency;
    double latencyMs = 0.0;                 // of the phase mode, at the current sample rate
    bool anyBoostOver = false;              // some band boosts by at least kBigBoostDb
};

constexpr double kBigBoostDb = 9.0;
constexpr double kSideBassHz = 150.0;

inline std::vector<ui::Note> adviceFor (const AdviceInput& in)
{
    std::vector<ui::Note> notes;
    const auto warn = [&] (std::string t) { notes.push_back ({ ui::Severity::warning, std::move (t) }); };
    const auto info = [&] (std::string t) { notes.push_back ({ ui::Severity::info, std::move (t) }); };

    if (in.delta)
        warn ("DELTA is on: you hear only what the equaliser changes, not your track. Switch it off to hear it.");
    if (in.soloActive)
        warn ("A SOLO is on: the output is a solo, not the mix. Press it again to release.");
    if (in.satChosen && in.phase != PhaseMode::zeroLatency)
        warn ("SAT does nothing in the linear phase modes (the shaper is not a linear filter). Use Zero Latency for SAT.");

    if (in.bandOn && in.place == Placement::side && hasGain (in.type) && in.gainDb > 0.5 && in.freqHz < kSideBassHz)
        warn ("Boosting the Side below 150 Hz widens the bass and weakens it in mono. Keep the low end in the centre.");
    if (in.bandOn && hasGain (in.type) && in.gainDb >= kBigBoostDb && ! in.gainMatch)
        warn ("A boost of 9 dB or more makes the track louder, which sounds better. Turn on GAIN MATCH to judge it fairly.");
    else if (in.anyBoostOver && ! in.gainMatch)
        info ("Some band boosts by 9 dB or more: GAIN MATCH is off, so louder may be taken for better.");
    if (in.bandOn && in.type == BandType::bell && in.q > 8.0 && in.gainDb > 6.0)
        info ("A narrow boost this strong rings like a tone. A wider Q, or a cut elsewhere, is usually gentler.");

    if (in.phase != PhaseMode::zeroLatency)
        info ("Linear phase delays the sound by " + std::to_string ((int) (in.latencyMs + 0.5)) + " ms (reported to the host): not for monitoring while recording.");
    if (in.gainMatch)
        info ("GAIN MATCH is on: level changes are matched away, so the equaliser cannot make the track louder.");

    return ui::ordered (std::move (notes));
}

inline const char* guideTitle() { return "Iris Prism"; }

inline const char* guideText()
{
    return "IT IS FOR shaping the tonal balance:\n"
           "  - Cutting a resonance or mud, lifting air or presence, with each band on Stereo, Left, Right, Mid or Side.\n"
           "  - A band that follows the level of its own region (DYNAMIC): a de-esser, a mud ducker, a resonance tamer. A band can listen to a "
           "sidechain, so one track can make room for another.\n"
           "  - Matching the long-term shape of a reference (LEARN REF, then MATCH EQ), and keeping the low end centred with Mid/Side.\n"
           "\n"
           "IT IS NOT FOR:\n"
           "  - Controlling the dynamics of a whole sound: a dynamic band follows one region only. Use a compressor (Atlas).\n"
           "  - Adding harmonics that are missing: SAT only rounds off boosts. Use Hephaestus.\n"
           "  - Fixing a bad mix in one move: MATCH EQ copies a long-term shape, not dynamics or stereo width, and what it adds is a start to "
           "check with DELTA, not an answer. The resonance and masking marks are rules of thumb on an analyser: your ears decide.\n"
           "  - Judging by loudness: a boost is louder, and louder sounds better. Use GAIN MATCH, and DELTA to hear what you changed.\n"
           "\n"
           "BEFORE YOU START: double-click the display where you want a band. Press BAND to hear the region a band covers, and CURVE to hear "
           "only what it adds or takes away. The strip under the panels warns when a setting does not suit its job.";
}
}
