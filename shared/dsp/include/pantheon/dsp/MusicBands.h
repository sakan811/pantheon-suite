// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 The Pantheon authors

#pragma once

#include "Biquad.h"

#include <algorithm>
#include <array>

namespace pdsp
{
// The six main frequency bands of music, the one definition every plugin's solo strip uses (Themis' AUDITION ranges, Iris'
// regions by default, Hephaestus' band solo strip), so a band soloed in one plugin is the same part of the spectrum in another.
namespace bands
{
struct MusicBand
{
    const char* name;  // short, for a switch ("Low-Mid")
    const char* title; // the full name ("Low-Mids")
    double loHz, hiHz;
    const char* role;  // what lives there, one line for a tooltip
};

inline constexpr int kCount = 6;
inline constexpr int kNumCrossovers = kCount - 1;

inline constexpr std::array<MusicBand, kCount> kBands { {
    { "Sub", "Sub-Bass", 20.0, 60.0, "the deepest rumble and physical weight under the kick and the bass, felt more than heard" },
    { "Bass", "Bass", 60.0, 250.0, "warmth and punch: the fundamentals of the bass and the low drums" },
    { "Low-Mid", "Low-Mids", 250.0, 500.0, "body and thickness, and the boxiness or mud when it is crowded" },
    { "Midrange", "Midrange", 500.0, 2000.0, "the core of the melody and the harmony, where the ear is most sensitive" },
    { "Upper-Mid", "Upper-Mids / Presence", 2000.0, 6000.0, "attack, clarity and bite; too much is harsh and tiring" },
    { "High", "Highs / Air", 6000.0, 20000.0, "sparkle and air: cymbals, the top of acoustic guitars, vocal sibilance (around 6 to 8 kHz)" },
} };

// The edge between band k and band k + 1.
inline constexpr double crossoverHz (int k) noexcept { return kBands[(size_t) k].hiHz; }

inline constexpr std::array<double, kNumCrossovers> defaultCrossovers() noexcept
{
    std::array<double, kNumCrossovers> xo {};
    for (int k = 0; k < kNumCrossovers; ++k)
        xo[(size_t) k] = crossoverHz (k);
    return xo;
}

// The band a frequency falls in.
inline constexpr int bandAt (double hz) noexcept
{
    int b = 0;
    while (b < kNumCrossovers && hz >= crossoverHz (b))
        ++b;
    return b;
}

// Every band that overlaps lo .. hi Hz, as a mask (bit b = kBands[b]).
inline constexpr unsigned bandsOverlapping (double loHz, double hiHz) noexcept
{
    unsigned mask = 0;
    for (int b = 0; b < kCount; ++b)
        if (kBands[(size_t) b].loHz < hiHz && kBands[(size_t) b].hiHz > loHz)
            mask |= 1u << b;
    return mask;
}

inline constexpr unsigned kAllBands = (1u << kCount) - 1u;
} // namespace bands

// Linkwitz-Riley 4th order: two Butterworth sections per side. low + high is an all-pass, so the pieces add back flat.
struct LR4
{
    std::array<Biquad, 2> lp, hp;

    void set (double fs, double f) noexcept
    {
        f = std::clamp (f, 10.0, fs * 0.45);
        for (auto& b : lp)
            b.setLowpass (fs, f, 0.7071067811865476);
        for (auto& b : hp)
            b.setHighpass (fs, f, 0.7071067811865476);
    }

    void reset() noexcept
    {
        for (auto& b : lp)
            b.reset();
        for (auto& b : hp)
            b.reset();
    }

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

// Splits a stereo signal into the six bands at five crossovers (bands::defaultCrossovers() unless moved). A band below
// another is run through the all-pass of every crossover above it, so any combination of bands adds up to the input's
// own magnitude response: soloing all six is the same sound as soloing none. Do not "save" the compensation filters:
// soloing two bands at once would then bump at the crossover.
class BandSplitter
{
public:
    static constexpr int kChannels = 2;

    void setCrossovers (double fs, const std::array<double, bands::kNumCrossovers>& hz) noexcept
    {
        for (auto& ch : channel)
            for (int k = 0; k < bands::kNumCrossovers; ++k)
            {
                ch.split[(size_t) k].set (fs, hz[(size_t) k]);
                for (int j = 0; j < k; ++j)
                    ch.comp[(size_t) (j * bands::kNumCrossovers + k)].set (fs, hz[(size_t) k]);
            }
    }

    void prepare (double fs) noexcept
    {
        setCrossovers (fs, bands::defaultCrossovers());
        reset();
    }

    void reset() noexcept
    {
        for (auto& ch : channel)
        {
            for (auto& s : ch.split)
                s.reset();
            for (auto& c : ch.comp)
                c.reset();
        }
    }

    // One sample of channel `c`, split into the six bands (each in phase with the others).
    inline void split (int c, double x, std::array<double, bands::kCount>& part) noexcept
    {
        auto& ch = channel[(size_t) c];
        double rest = x;
        for (int k = 0; k < bands::kNumCrossovers; ++k)
        {
            double h;
            ch.split[(size_t) k].split (rest, part[(size_t) k], h);
            rest = h;
        }
        part[bands::kNumCrossovers] = rest;
        // Band j goes through the all-pass of crossovers j+1 ... the last.
        for (int j = 0; j < bands::kNumCrossovers; ++j)
            for (int k = j + 1; k < bands::kNumCrossovers; ++k)
                part[(size_t) j] = ch.comp[(size_t) (j * bands::kNumCrossovers + k)].allpass (part[(size_t) j]);
    }

    // The sum of the bands in `mask` (bit b = band b) for one sample of channel `c`.
    inline double process (int c, double x, unsigned mask) noexcept
    {
        std::array<double, bands::kCount> part;
        split (c, x, part);
        double sum = 0.0;
        for (int b = 0; b < bands::kCount; ++b)
            if (mask & (1u << b))
                sum += part[(size_t) b];
        return sum;
    }

private:
    struct Channel
    {
        std::array<LR4, bands::kNumCrossovers> split;
        std::array<LR4, bands::kNumCrossovers * bands::kNumCrossovers> comp; // [j * kNumCrossovers + k]: crossover k's all-pass for band j
    };
    std::array<Channel, kChannels> channel;
};
} // namespace pdsp
