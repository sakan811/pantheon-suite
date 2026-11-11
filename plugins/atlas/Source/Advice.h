// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 The Pantheon authors

#pragma once

#include "DSP/Engine.h"
#include <pantheon/ui/Note.h>
#include <string>
#include <vector>

namespace atlas
{
// What the editor tells the user about the bands and the session, so that a compressor is not used for what it cannot do and what is
// heard is not mistaken for the result. Plain C++, no JUCE (the notes are shared/ui Note.h); each rule restates a limit under
// "Limits" in the README or a well-known pitfall, and UiCheck pins it.
struct BandAdvice
{
    int number = 1;                // the band as the user counts them, from the bottom of the spectrum
    double loHz = 20.0, hiHz = 20000.0;
    BandParams params;
    double reductionDb = 0.0;      // what its compressor is taking off now
};

struct AdviceInput
{
    std::vector<BandAdvice> bands; // the bands there are, in order
    bool delta = false, solo = false;
    PhaseMode phase = PhaseMode::zeroLatency;
    double latencyMs = 0.0;
    double lookaheadMs = 0.0;
    bool sidechainConnected = false;
};

constexpr double kBassTopHz = 250.0;     // the top of the Bass band: below it a fast attack follows single cycles
constexpr double kFastAttackMs = 5.0;
constexpr double kLimiterRatio = 10.0;
constexpr double kHeavyReductionDb = 12.0;
constexpr double kThresholdAtTopDb = -0.5;

inline std::vector<ui::Note> adviceFor (const AdviceInput& in)
{
    std::vector<ui::Note> notes;
    const auto warn = [&] (std::string t) { notes.push_back ({ ui::Severity::warning, std::move (t) }); };
    const auto info = [&] (std::string t) { notes.push_back ({ ui::Severity::info, std::move (t) }); };

    if (in.delta)
        warn ("DELTA is on: you hear only what a compressor changes, not your track. ALL BANDS ends it.");
    if (in.solo)
        warn ("A SOLO is on: the output is the soloed bands, not the whole track. ALL BANDS ends it.");

    const bool anyOn = std::any_of (in.bands.begin(), in.bands.end(), [] (const BandAdvice& b) { return b.params.on; });
    if (! anyOn)
        warn ("Every band is off: Atlas passes the sound as it is. Switch a band ON.");

    for (const auto& b : in.bands)
    {
        if (! b.params.on)
            continue;
        const auto name = "Band " + std::to_string (b.number) + ": ";

        if (b.params.thresholdDb >= kThresholdAtTopDb)
            warn (name + "THRESH is at the top, so it hardly compresses. Lower it until the meter moves.");
        if (b.params.ratio <= 1.05)
            warn (name + "RATIO is 1:1, which does nothing. Raise it.");
        if (b.reductionDb >= kHeavyReductionDb)
            warn (name + "taking off " + std::to_string ((int) (b.reductionDb + 0.5)) + " dB or more. Raise THRESH or lower RATIO, or it will pump.");
        if (b.params.ratio >= kLimiterRatio)
            warn (name + "RATIO 10:1 or more is limiting, but Atlas reads no true peak. Use a limiter after it.");
        if (b.params.sidechain && ! in.sidechainConnected)
            warn (name + "listens to the SIDECHAIN, but none is connected, so it listens to itself. Route a track in.");
        if (in.lookaheadMs > 0.0 && b.params.attackMs > in.lookaheadMs + 0.5)
            info (name + "ATTACK (" + std::to_string (static_cast<int> (b.params.attackMs + 0.5)) + " ms) is longer than the look-ahead: it is still turning down at the peak.");
        if (b.hiHz <= kBassTopHz && b.params.attackMs < kFastAttackMs)
            info (name + "a fast ATTACK on bass follows single cycles and distorts the notes. Try 10 ms or more.");
        if (b.params.channels == Channels::dual)
            info (name + "L/R gives each side its own gain, so the image can drift. Stereo keeps it still.");
        if (b.params.match)
            info (name + "MATCH brings it back to its old loudness, so it will not seem quieter. MAKEUP adds on top.");
    }

    if (in.lookaheadMs > 0.0)
        info ("Look-ahead delays the sound by " + std::to_string (static_cast<int> (in.lookaheadMs + 0.5)) + " ms (reported to the host). Changing it restarts the bands.");
    if (in.phase == PhaseMode::linear)
        info ("Linear phase delays the sound by " + std::to_string ((int) (in.latencyMs + 0.5)) + " ms (reported to the host). Changing it restarts the bands.");

    return ui::ordered (std::move (notes));
}

inline const char* guideTitle() { return "Atlas Span"; }

inline const char* guideText()
{
    return "IT IS FOR controlling dynamics band by band:\n"
           "  - One to six bands made on a spectrogram: double-click to split, drag an edge, double-click an edge to join.\n"
           "  - Taming what pokes out in one part of the spectrum: a boomy low end, a harsh upper-mid, a kick that leaps. PEAK and JUMPS "
           "mark where to look, and a click on a JUMPS marker makes that region a band.\n"
           "  - Making room: a band can listen to the SIDECHAIN input instead of itself (route the kick there and the bass bands duck when it hits).\n"
           "  - Gluing a sound that moves around: Stereo keeps the image still, M/S compresses the centre and the sides apart.\n"
           "\n"
           "IT IS NOT FOR:\n"
           "  - Limiting peaks: LOOK-AHEAD lets a fast peak be turned down as it arrives, but the level is not a true-peak reading and nothing is clipped. Use a limiter after it.\n"
           "  - Fixing the tonal balance: it turns a band down only while it is loud. For a constant cut or lift use an equaliser (Iris).\n"
           "  - Adding what is missing: it only takes away. Use Hephaestus to add harmonics.\n"
           "  - Doing the work by itself: PEAK and JUMPS are rules of thumb on the input, and a threshold from a marker is a starting point.\n"
           "\n"
           "GOOD TO KNOW: the presets are starting points: their thresholds suit a master around -14 LUFS, so set THRESH where the meter first "
           "moves. Use a low RATIO (2:1) and slow-ish times, and DELTA to hear what a "
           "band takes. MATCH keeps a band from sounding better only because it is quieter. Splitting or joining bands while they compress "
           "can click, and changing Phase, Oversampling or Look-ahead while playing restarts the bands (a short gap).";
}
}
