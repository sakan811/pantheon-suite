// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 The Pantheon authors

#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <pantheon/dsp/BandLink.h>

namespace atlas
{
// TARGET: a spectrum shape (the curve another plugin of the repository copies, Themis' Copy EQ) says which bands stand out of it.
// Each band's level in the shape is the power of the curve's points inside it (points are evenly spaced in octaves, so this is the power per octave added up over the band's
// octaves, roughly the way a band's own level adds up what is in it: exact for edges that hold the same number of points, a few tenths of a dB off otherwise). A band's *excess* is how far its typical level is over its
// level in the shape, once the whole is taken to the same loudness (the mean over the bands that were heard). A band over the shape
// is turned down more: its THRESH goes down by its excess, at most kMaxShiftDb; a band under it is left as it was (a compressor cannot lift
// what is missing, and upward is the player's choice). A starting point: it compares a band's level with a curve's, which differ in how they are measured.
inline constexpr double kMaxShiftDb = 12.0;

struct TargetBand
{
    double loHz = 20.0, hiHz = 20000.0;
    double typicalDb = -120.0; // what LEARN heard
    bool heard = false;
};

// The dB the curve holds inside a band (the power of its points between the edges, with no point between the edges, the curve read at the middle of the band).
inline double curveBandDb (const pdsp::LinkCurve& c, double loHz, double hiHz)
{
    double sum = 0.0;
    int n = 0;
    for (size_t i = 0; i < c.db.size(); ++i)
    {
        const double f = c.frequency (i);
        if (f >= loHz && f < hiHz)
        {
            sum += std::pow (10.0, c.db[i] / 10.0);
            ++n;
        }
    }
    if (n == 0)
        return c.at (std::sqrt (loHz * hiHz));
    return 10.0 * std::log10 (std::max (sum, 1.0e-12));
}

// How many dB each band's THRESH goes down (0 for a band that was not heard or is not over the shape). Fewer than two bands heard: all 0, there is nothing to compare.
template <size_t N>
inline std::array<double, N> targetShifts (const pdsp::LinkCurve& curve, const std::array<TargetBand, N>& bands, int count)
{
    std::array<double, N> shift {}, diff {};
    double mean = 0.0;
    int heard = 0;
    for (int i = 0; i < count; ++i)
        if (bands[(size_t) i].heard)
        {
            diff[(size_t) i] = bands[(size_t) i].typicalDb - curveBandDb (curve, bands[(size_t) i].loHz, bands[(size_t) i].hiHz);
            mean += diff[(size_t) i];
            ++heard;
        }
    if (heard < 2)
        return shift;
    mean /= heard;
    for (int i = 0; i < count; ++i)
        if (bands[(size_t) i].heard)
            shift[(size_t) i] = std::clamp (diff[(size_t) i] - mean, 0.0, kMaxShiftDb);
    return shift;
}
} // namespace atlas
