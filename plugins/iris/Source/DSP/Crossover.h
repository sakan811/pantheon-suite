// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 The Pantheon authors

#pragma once

#include "Band.h"

namespace iris
{
// Linkwitz-Riley 4th order: two Butterworth sections per side. low + high is an all-pass, so the pieces add back flat.
struct LR4
{
    std::array<pdsp::Biquad, 2> lp, hp;

    void set (double fs, double f) noexcept
    {
        f = clampFreq (f, fs);
        for (auto& b : lp) b.setLowpass (fs, f, 0.7071067811865476);
        for (auto& b : hp) b.setHighpass (fs, f, 0.7071067811865476);
    }

    void reset() noexcept { for (auto& b : lp) b.reset(); for (auto& b : hp) b.reset(); }

    inline void split (double x, double& low, double& high) noexcept
    {
        low = lp[1].process (lp[0].process (x));
        high = hp[1].process (hp[0].process (x));
    }

    inline double allpass (double x) noexcept
    {
        double l, h;
        split (x, l, h);
        return l + h;
    }
};

// Six regions of the spectrum for soloing (sub, bass, low-mid, mid, high-mid, high), cut at five crossovers. A region below another
// is run through the all-pass of every crossover above it, so any combination of regions adds up to the input's own magnitude
// response: soloing all six is the same sound as soloing none.
class RegionSplitter
{
public:
    void setCrossovers (double fs, const std::array<double, numCrossovers>& hz) noexcept
    {
        for (auto& ch : channel)
            for (int k = 0; k < numCrossovers; ++k)
            {
                ch.split[(size_t) k].set (fs, hz[(size_t) k]);
                for (int j = 0; j < k; ++j)
                    ch.comp[(size_t) (j * numCrossovers + k)].set (fs, hz[(size_t) k]);
            }
    }

    void reset() noexcept
    {
        for (auto& ch : channel)
        {
            for (auto& s : ch.split) s.reset();
            for (auto& c : ch.comp) c.reset();
        }
    }

    // The sum of the regions in `mask` (bit 0 = sub ... bit 5 = high) for one sample of channel `c`.
    inline double process (int c, double x, unsigned mask) noexcept
    {
        auto& ch = channel[(size_t) c];
        double low[numCrossovers], rest = x;
        for (int k = 0; k < numCrossovers; ++k)
        {
            double h;
            ch.split[(size_t) k].split (rest, low[k], h);
            rest = h;
        }
        // low[k] is region k; `rest` is the top region. Region j goes through the all-pass of crossovers j+1 ... the last.
        double sum = 0.0;
        for (int j = 0; j < numCrossovers; ++j)
        {
            double v = low[j];
            for (int k = j + 1; k < numCrossovers; ++k)
                v = ch.comp[(size_t) (j * numCrossovers + k)].allpass (v);
            if (mask & (1u << j))
                sum += v;
        }
        if (mask & (1u << numCrossovers))
            sum += rest;
        return sum;
    }

private:
    struct Channel
    {
        std::array<LR4, numCrossovers> split;
        std::array<LR4, numCrossovers * numCrossovers> comp; // [j * numCrossovers + k]: crossover k's all-pass for region j
    };
    std::array<Channel, 2> channel;
};

inline const char* regionName (int region) noexcept
{
    static constexpr const char* names[numRegions] = { "Sub", "Bass", "Low-Mid", "Mid", "High-Mid", "High" };
    return names[std::clamp (region, 0, numRegions - 1)];
}
} // namespace iris
