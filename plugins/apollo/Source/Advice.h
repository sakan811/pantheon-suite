// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 The Pantheon authors

#pragma once

#include "DSP/Engine.h"
#include <pantheon/ui/Note.h>
#include <string>
#include <vector>

namespace apollo
{
// What the editor tells the user about the bands, so that an equaliser is not used for what it cannot do and what is heard is not mistaken for the result.
// Plain C++, no JUCE (the notes are shared/ui Note.h); each rule restates a limit under "Limits" in the README, and UiCheck pins it.
struct AdviceInput
{
    EngineParams params;
    double latencyMs = 0.0;
    bool delta = false; // DELTA is on (the editor's monitor, not a parameter)
    int listen = -1;    // LISTEN is on for this band (0 to 7), or -1 (the same kind of monitor)
};

constexpr double kBigBoostDb = 9.0;
constexpr double kNarrowQ = 6.0;
constexpr double kLowLinearHz = 60.0;
constexpr double kSumBoostDb = 12.0;

inline std::vector<ui::Note> adviceFor (const AdviceInput& in)
{
    std::vector<ui::Note> notes;
    const auto warn = [&] (std::string t) { notes.push_back ({ ui::Severity::warning, std::move (t) }); };
    const auto info = [&] (std::string t) { notes.push_back ({ ui::Severity::info, std::move (t) }); };
    const auto& p = in.params;

    if (in.delta)
        warn ("DELTA is on: you hear only what the bands change, not your track. Switch it off to hear it.");
    if (in.listen >= 0)
        warn ("LISTEN is on: you hear only the region of band " + std::to_string (in.listen + 1) + ", not your track. Switch it off to hear it.");
    const bool anyRunning = std::any_of (p.bands.begin(), p.bands.end(), [] (const BandParams& b) { return ! isIdentity (b); });
    if (! anyRunning && std::fabs (p.outDb) < 0.05)
        info ("No band changes anything yet. Drag a node on the display, or raise a band's GAIN.");

    for (int i = 0; i < kBands; ++i)
    {
        const auto& b = p.bands[(size_t) i];
        if (isIdentity (b))
            continue;
        const auto name = "Band " + std::to_string (i + 1) + ": ";
        if (hasGain (b.type) && b.gainDb >= kBigBoostDb)
            warn (name + "a boost of " + std::to_string ((int) (b.gainDb + 0.5)) + " dB or more. Try cutting somewhere else instead.");
        if (b.type == Type::bell && b.q >= kNarrowQ && std::fabs (b.gainDb) >= 3.0)
            info (name + "a narrow bell (Q " + std::to_string ((int) (b.q + 0.5)) + " or more) rings in minimum phase: it is for a surgical cut.");
        if (p.phase == Phase::linear && b.freqHz < kLowLinearHz && b.type != Type::highPass && b.type != Type::lowPass)
            info (name + "linear phase resolves about 12 Hz: a band this low is softer than the curve shows.");
        if (p.phase == Phase::linear && b.type == Type::highPass && b.freqHz < kLowLinearHz && b.slope >= 2)
            info (name + "a steep high-pass this low is softer in linear phase (the kernel resolves 12 Hz).");
    }
    // The sum of the bells that overlap (a quick bound: the most boosted gain among bands within an octave of each other).
    double worst = 0.0;
    for (int i = 0; i < kBands; ++i)
    {
        double sum = 0.0;
        for (int j = 0; j < kBands; ++j)
            if (! isIdentity (p.bands[(size_t) j]) && hasGain (p.bands[(size_t) j].type) && p.bands[(size_t) j].gainDb > 0.0 && std::fabs (std::log2 (p.bands[(size_t) j].freqHz / p.bands[(size_t) i].freqHz)) <= 1.0)
                sum += p.bands[(size_t) j].gainDb;
        worst = std::fmax (worst, sum);
    }
    if (worst >= kSumBoostDb)
        warn ("Boosts within an octave of each other add up to " + std::to_string ((int) (worst + 0.5)) + " dB. Lower one, or the output may clip.");
    const bool bigBoost = std::any_of (p.bands.begin(), p.bands.end(), [] (const BandParams& b) { return ! isIdentity (b) && hasGain (b.type) && b.gainDb >= kBigBoostDb; });
    if (bigBoost && ! p.match)
        info ("A boost of 9 dB or more makes the track louder, which sounds better. Turn on GAIN MATCH to judge it fairly.");
    if (p.match)
        info ("GAIN MATCH is on: the level the bands add is matched away, so they cannot make the track louder.");
    if (p.phase == Phase::linear && in.latencyMs > 0.0)
        info ("Linear phase delays the sound by " + std::to_string ((int) (in.latencyMs + 0.5)) + " ms (the host compensates it). Not for a live monitor.");
    return ui::ordered (std::move (notes));
}

inline const char* guideTitle() { return "Apollo Curve"; }

inline const char* guideText()
{
    return "FOR\n"
           "Shaping the tone of a track or a master: eight bands, each a bell, a shelf, a notch, or a high or low-pass of 12 to 48 dB per octave. "
           "Drag a node on the display: sideways is the frequency, up and down the gain, the wheel the width (Q); double-click empty glass to add a band there. LISTEN plays the selected band's region alone. Each band can work on the whole sound, "
           "only the middle, or only the sides.\n\n"
           "PHASE\n"
           "MINIMUM is the usual equaliser: no delay, and the phase turns around each band. LINEAR has no phase shift at all, which keeps a master's "
           "transients and stereo image intact when several bands work together, at the price of a delay of about 48 ms (the host compensates) and of "
           "a softer low end (about 12 Hz resolution).\n\n"
           "NOT FOR\n"
           "It is not a dynamic equaliser (Iris is): it does what the curve says, always. The analyser behind the curve shows the sound before (dim) and after "
           "(bright) the bands; ANALYZER turns it off. It does not judge by loudness: a boost is louder, and louder sounds better. Use GAIN MATCH, and DELTA to hear what the "
           "bands change.\n\n"
           "WITH THEMIS\n"
           "In Themis, COPY on a plan item or Copy EQ, then PASTE here: the bands go into free slots, and the target curve is drawn dashed behind the analyser to follow. "
           "Right-click the display to clear it.";
}
} // namespace apollo
