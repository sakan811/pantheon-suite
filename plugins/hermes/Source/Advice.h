// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 The Pantheon authors

#pragma once

#include "DSP/Engine.h"
#include <pantheon/ui/Note.h>
#include <string>
#include <vector>

namespace hermes
{
// What the editor tells the user about the settings and the readings, so that what is heard is not mistaken for the sound and a meter is not misread.
// Plain C++, no JUCE (the notes are shared/ui Note.h); each rule restates a limit under "Limits" in the README, and UiCheck pins it.
struct AdviceInput
{
    EngineParams params;
    bool referenceLoaded = false;
    double integratedLufs = -200.0;
    double truePeakDb = -200.0;
    double rangeLu = 0.0;
};

constexpr double kLoudTargetLufs = -9.0;      // above this a master is louder than any streaming service plays it
constexpr double kStreamingLufs = -14.0;
constexpr double kPeakLimitDb = -1.0;

inline std::vector<ui::Note> adviceFor (const AdviceInput& in)
{
    std::vector<ui::Note> notes;
    const auto warn = [&] (std::string t) { notes.push_back ({ ui::Severity::warning, std::move (t) }); };
    const auto info = [&] (std::string t) { notes.push_back ({ ui::Severity::info, std::move (t) }); };
    const auto& p = in.params;

    if (p.listen != Listen::stereo)
        warn ("LISTEN is not Stereo: you hear a check, not your sound. Set it back to Stereo before you bounce.");
    if (p.reference && ! in.referenceLoaded)
        warn ("REF is on, but no reference is loaded: you hear the sound. Press LOAD.");
    if (p.reference && in.referenceLoaded)
        warn ("REF is on: you hear the reference, not your sound. The meters still read your sound.");
    if (p.polarity)
        info ("POLARITY inverts both channels: it sounds the same, but sums differently with other tracks.");
    if (in.truePeakDb > kPeakLimitDb && in.truePeakDb > -150.0)
        warn ("True peak over -1 dBTP: a lossy codec or a converter can clip it. Limit it first, or lower TRIM.");
    if (in.integratedLufs > kLoudTargetLufs && in.integratedLufs > -150.0)
        warn ("Louder than -9 LUFS: streaming turns it down to about -14, and a hard-limited master loses what it gained.");
    else if (in.integratedLufs > -150.0 && in.integratedLufs < -24.0 && in.integratedLufs > -69.0)
        info ("Under -24 LUFS integrated: quiet for a finished master (broadcast is -23, streaming about -14).");
    if (in.rangeLu > 0.0 && in.rangeLu < 3.0)
        info ("A loudness range under 3 LU is very flat: heavily compressed, or a single steady sound.");
    if (p.levelMatch && in.referenceLoaded)
        info ("LEVEL MATCH brings the reference to your sound's integrated loudness: let the meter run a while first.");
    return ui::ordered (std::move (notes));
}

inline const char* guideTitle() { return "Hermes Level"; }

inline const char* guideText()
{
    return "FOR\n"
           "Gain staging and checking: TRIM sets the level (and POLARITY the sign), LISTEN checks the sound in mono, one channel, the sides only, or with the channels swapped. "
           "The meters read BS.1770 / EBU R128 loudness: MOMENTARY (400 ms), SHORT-TERM (3 s), INTEGRATED (gated, since RESET) and the loudness range, and the true peak "
           "(the peaks between the samples, in dBTP). AUTO TRIM sets TRIM so the integrated loudness reads -14 LUFS (play the passage first); AUTO RESET starts the meters again whenever the host starts playing.\n\n"
           "REFERENCE\n"
           "LOAD a finished track and press REF to hear it instead of yours; with LEVEL MATCH it is brought to the loudness of your sound, so you compare the tone and not the level. "
           "RESTART takes it to its start, a click on its bar plays from there; it loops.\n\n"
           "NOT FOR\n"
           "It changes nothing but the level and the listening: no compression, no limiting, no EQ. The meters are not a limiter: they tell you what to do, they do not do it. "
           "Remember to set LISTEN back to Stereo and REF off before you bounce.";
}
} // namespace hermes
