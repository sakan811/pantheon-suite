// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 The Pantheon authors

#pragma once

#include "Eq.h"
#include "EqMove.h"
#include "TargetCurve.h"

namespace themis
{
struct EqFitResult
{
    // Indices follow MasteringEq: 1 = low shelf, 2..4 = bells, 5 = high shelf. Index 0 (HPF) is untouched.
    std::array<EqBandParams, MasteringEq::kNumBands> bands {};
    double residualRmsBefore = 0.0, residualRmsAfter = 0.0;
};

// Fits the 5 tonal EQ bands so that measured + EQ ~= target.
// `measuredDb` and `target.medianDb` must both be anchor-normalised. `strength` scales the correction
// (1 = full), `maxGainDb` caps each band.
inline EqFitResult fitEqToTarget (const BandArray& measuredDb, const TargetCurve& target, double fs, float strength = 0.8f,
                                  float maxGainDb = 6.0f)
{
    constexpr int N = BandGrid::kNum;
    std::array<double, N> diff {};
    for (int i = 0; i < N; ++i)
        diff[static_cast<size_t> (i)] = target.medianDb[static_cast<size_t> (i)] - measuredDb[static_cast<size_t> (i)];

    // Smooth (+/- 1/2 octave) so we correct broad tonal balance, not individual notes.
    std::array<double, N> d {};
    for (int i = 0; i < N; ++i)
    {
        double s = 0;
        int n = 0;
        for (int k = -3; k <= 3; ++k)
            if (i + k >= 0 && i + k < N)
            {
                s += diff[static_cast<size_t> (i + k)];
                ++n;
            }
        d[static_cast<size_t> (i)] = s / n * strength;
    }

    auto rms = [&] (const std::array<double, N>& v) {
        double s = 0;
        for (double x : v)
            s += x * x;
        return std::sqrt (s / N);
    };

    EqFitResult res;
    res.bands[0].enabled = false; // HPF is never touched by the fit
    res.bands[0].freq = 20.0f;
    res.residualRmsBefore = rms (d);
    std::array<double, N> resid = d;
    auto clampGain = [&] (double g) { return static_cast<float> (std::clamp (g, -static_cast<double> (maxGainDb), static_cast<double> (maxGainDb))); };
    auto subtract = [&] (const BiquadCoeffs& c) {
        for (int i = 0; i < N; ++i)
            resid[static_cast<size_t> (i)] -= c.magnitudeDb (fs, BandGrid::centreHz (i));
    };

    // Low shelf @ 80 Hz fitted to the mean error under 60 Hz, high shelf @ 9 kHz to the mean above 12 kHz.
    {
        double s = 0;
        int n = 0;
        for (int i = 0; BandGrid::centreHz (i) <= 60.0; ++i, ++n)
            s += resid[static_cast<size_t> (i)];
        auto& b = res.bands[1];
        b.enabled = true;
        b.freq = 80.0f;
        b.q = 0.7071f;
        b.gainDb = clampGain (s / std::max (n, 1));
        subtract (MasteringEq::coeffsFor (1, b, fs));
    }
    {
        double s = 0;
        int n = 0;
        for (int i = 0; i < N; ++i)
            if (BandGrid::centreHz (i) >= 12000.0 && BandGrid::centreHz (i) <= fs * 0.45)
            {
                s += resid[static_cast<size_t> (i)];
                ++n;
            }
        auto& b = res.bands[5];
        b.enabled = true;
        b.freq = 9000.0f;
        b.q = 0.7071f;
        b.gainDb = clampGain (n ? s / n : 0.0);
        subtract (MasteringEq::coeffsFor (5, b, fs));
    }

    // Greedy bells on the largest remaining error between 50 Hz and 12 kHz.
    for (int band = 2; band <= 4; ++band)
    {
        int best = -1;
        double bestAbs = 0;
        for (int i = 0; i < N; ++i)
        {
            const double f = BandGrid::centreHz (i);
            if (f < 50.0 || f > 12000.0)
                continue;
            if (std::abs (resid[static_cast<size_t> (i)]) > bestAbs)
            {
                bestAbs = std::abs (resid[static_cast<size_t> (i)]);
                best = i;
            }
        }
        auto& b = res.bands[static_cast<size_t> (band)];
        b.enabled = true;
        b.q = 0.9f;
        if (best < 0 || bestAbs < 0.25)
        {
            b.freq = band == 2 ? 200.0f : band == 3 ? 1000.0f : 4000.0f;
            b.gainDb = 0.0f;
            continue;
        }
        b.freq = static_cast<float> (BandGrid::centreHz (best));
        b.gainDb = clampGain (resid[static_cast<size_t> (best)] * 0.9);
        subtract (MasteringEq::coeffsFor (band, b, fs));
    }

    res.residualRmsAfter = rms (resid);
    return res;
}
// The fit as bands for an equaliser: the low shelf, the bells and the high shelf that move more than `minGainDb`, on both
// channels. What the tonal view's COPY EQ hands over: a difference from the target, i.e. a suggestion to judge by ear.
inline EqMoves tonalMoves (const BandArray& measuredDb, const TargetCurve& target, double fs, float minGainDb = 0.5f)
{
    const auto fit = fitEqToTarget (measuredDb, target, fs);
    EqMoves moves;
    for (int i = 1; i <= 5; ++i)
    {
        const auto& b = fit.bands[static_cast<size_t> (i)];
        if (!b.enabled || std::abs (b.gainDb) < minGainDb)
            continue;
        EqMove m;
        m.kind = i == 1 ? EqMove::Kind::lowShelf : i == 5 ? EqMove::Kind::highShelf : EqMove::Kind::bell;
        m.freqHz = b.freq;
        m.gainDb = b.gainDb;
        m.q = b.q;
        moves.push_back (m);
    }
    return moves;
}
} // namespace themis
