// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 The Pantheon authors

#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>

namespace themis
{
// Fixed analysis grid shared by the analyzer, target curves and EQ fitting:
// 1/6-octave constant-Q bands from 20 Hz upward (61 bands, 20 Hz .. ~20.5 kHz).
struct BandGrid
{
    static constexpr int kPerOctave = 6;
    static constexpr int kNum = 61;
    static constexpr double kF0 = 20.0;
    static constexpr double kMaxHz = 20000.0; // top of the audible range: displays and device ranges span [kF0, kMaxHz]

    // Bands above this share of the sampling rate sit at the analysis' roll-off: displays and checks leave them out.
    static double usableMaxHz (double sampleRate) { return sampleRate * 0.47; }

    static double centreHz (int i) { return kF0 * std::pow (2.0, static_cast<double> (i) / kPerOctave); }
    static double lowEdgeHz (int i) { return centreHz (i) * std::pow (2.0, -0.5 / kPerOctave); }
    static double highEdgeHz (int i) { return centreHz (i) * std::pow (2.0, 0.5 / kPerOctave); }

    // How many bands have their centre at or below `hz` (at least 2, at most kNum): the bands a sample rate can carry.
    static int bandsUpTo (double hz)
    {
        int n = 0;
        while (n < kNum && centreHz (n) <= hz)
            ++n;
        return std::max (n, 2);
    }

    // Fractional band index for a frequency (clamped to the grid).
    static double indexForHz (double f)
    {
        return std::clamp (std::log2 (std::max (f, 1.0) / kF0) * kPerOctave, 0.0, static_cast<double> (kNum - 1));
    }

    // Normalisation region: curves are shifted so their mean over this range is 0 dB.
    static constexpr double kAnchorLoHz = 200.0;
    static constexpr double kAnchorHiHz = 5000.0;
};

using BandArray = std::array<float, BandGrid::kNum>;

// The tilt of a band curve: the least-squares slope of its dB against octaves (log frequency). It is a shape, so a level
// offset does not change it. Positive is brighter (more high end per octave), negative darker.
namespace tilt
{
// Two tilts closer than this read as the same. A rule of thumb: 0.25 dB per octave is about 2.5 dB from one end of the
// ten-octave grid to the other.
inline constexpr float kSameDbPerOct = 0.25f;

// The slope over the first `bands` bands (the ones the sample rate can carry, see BandGrid::bandsUpTo).
inline double slopeDbPerOctave (const BandArray& db, int bands = BandGrid::kNum)
{
    const int n = std::clamp (bands, 2, BandGrid::kNum);
    double mx = 0.0, my = 0.0;
    for (int i = 0; i < n; ++i)
    {
        mx += static_cast<double> (i) / BandGrid::kPerOctave;
        my += db[static_cast<size_t> (i)];
    }
    mx /= n;
    my /= n;
    double sxy = 0.0, sxx = 0.0;
    for (int i = 0; i < n; ++i)
    {
        const double dx = static_cast<double> (i) / BandGrid::kPerOctave - mx;
        sxy += dx * (db[static_cast<size_t> (i)] - my);
        sxx += dx * dx;
    }
    return sxy / sxx;
}

// The measured curve's tilt against the target's, over the bands that the sample rate carries.
struct Report
{
    bool have = false;
    float measuredDbPerOct = 0.0f, targetDbPerOct = 0.0f;
    float differenceDbPerOct = 0.0f; // measured - target: negative is darker than the target
};

inline Report compare (const BandArray& measured, const BandArray& target, double sampleRate)
{
    Report r;
    const int bands = BandGrid::bandsUpTo (BandGrid::usableMaxHz (sampleRate));
    r.measuredDbPerOct = static_cast<float> (slopeDbPerOctave (measured, bands));
    r.targetDbPerOct = static_cast<float> (slopeDbPerOctave (target, bands));
    r.differenceDbPerOct = r.measuredDbPerOct - r.targetDbPerOct;
    r.have = true;
    return r;
}

enum class Side { same, darker, brighter };
inline Side sideOf (float differenceDbPerOct)
{
    return differenceDbPerOct <= -kSameDbPerOct ? Side::darker : differenceDbPerOct >= kSameDbPerOct ? Side::brighter : Side::same;
}
} // namespace tilt
} // namespace themis
