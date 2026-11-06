// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 The Pantheon authors

#pragma once

#include "BandGrid.h"
#include "Common.h"
#include "FrameHistory.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <vector>

namespace themis
{
// The level of every analysis band in one analysed frame (stereo: mid + side), mean-square dBFS, for the spectrogram.
// kHistoryFloorDb marks a band with no signal (digital silence, or above what the sample rate can carry).
using BandLevels = std::array<float, BandGrid::kNum>;

// Fixed-size ring of the most recent BandLevels, written by the analysis thread right next to the FrameHistory (same
// frame period, same capacity) and read by the UI. Every value is an atomic; the writer would have to lap the whole ring
// (~85 s) to touch a frame a reader is copying. Kept apart from FrameStats so the plan's copies of the history stay small.
class SpectrumHistory
{
public:
    static constexpr int kCapacity = FrameHistory::kCapacity;

    // Writer thread.
    void setFramePeriod (double seconds) { period.store (seconds, std::memory_order_relaxed); }
    void clear() { count.store (0, std::memory_order_release); }

    void push (const BandLevels& levels)
    {
        const uint64_t n = count.load (std::memory_order_relaxed);
        const size_t base = static_cast<size_t> (n % kCapacity) * BandGrid::kNum;
        for (size_t b = 0; b < levels.size(); ++b)
            data[base + b].store (levels[b], std::memory_order_relaxed);
        count.store (n + 1, std::memory_order_release);
    }

    // Any thread.
    double framePeriodSeconds() const { return period.load (std::memory_order_relaxed); }
    uint64_t sequence() const { return count.load (std::memory_order_acquire); }

    // Copies the newest frames, oldest first, into out[0..n); returns how many were available.
    int copyRecent (BandLevels* out, int n) const
    {
        const uint64_t total = count.load (std::memory_order_acquire);
        const int have = static_cast<int> (std::min<uint64_t> ({ total, static_cast<uint64_t> (kCapacity), static_cast<uint64_t> (std::max (n, 0)) }));
        for (int i = 0; i < have; ++i)
        {
            const size_t base = static_cast<size_t> ((total - static_cast<uint64_t> (have - i)) % kCapacity) * BandGrid::kNum;
            for (size_t b = 0; b < out[i].size(); ++b)
                out[i][b] = data[base + b].load (std::memory_order_relaxed);
        }
        return have;
    }

private:
    std::array<std::atomic<float>, static_cast<size_t> (kCapacity) * BandGrid::kNum> data {};
    std::atomic<uint64_t> count { 0 };
    std::atomic<double> period { 0.1 };
};

// What the spectrogram shows and what it says about the window. Every limit is a hand-authored rule of thumb.
namespace spectro
{
// The display is tilted like the spectrum displays of the other plugins (+4.5 dB per octave around 1 kHz), so a typical
// programme looks about flat instead of sloping down towards the highs.
inline constexpr float kTiltDbPerOctave = 4.5f;
inline constexpr double kTiltRefHz = 1000.0;
inline constexpr float kTopDb = -25.0f;   // tilted level shown as the brightest colour
inline constexpr float kRangeDb = 60.0f;  // and how far below it the darkest is

inline float tiltedDb (float levelDb, double hz) { return levelDb + kTiltDbPerOctave * static_cast<float> (std::log2 (hz / kTiltRefHz)); }

// 0 (nothing) to 1 (brightest) for a band level at this frequency.
inline float brightness (float levelDb, double hz)
{
    return hasLevel (levelDb) ? std::clamp ((tiltedDb (levelDb, hz) - (kTopDb - kRangeDb)) / kRangeDb, 0.0f, 1.0f) : 0.0f;
}

inline constexpr int kMinFrames = 12;     // ~1 s
inline constexpr float kBurstRangeDb = 50.0f; // a band whose usual level is this far under the loudest band's is noise floor, not a burst

// The sharpest burst in the window: the band and moment where the level rose furthest above that band's own usual (median)
// level. Drums make big bursts by nature, so this is a pointer to where to look, never a fault.
struct BurstReport
{
    bool have = false;
    int band = 0;          // index on the BandGrid
    int framesAgo = 0;     // 0 = the newest frame
    float excessDb = 0.0f; // how far above the band's usual level it rose
    float levelDb = 0.0f, usualDb = 0.0f;
};

inline BurstReport analyseBursts (const BandLevels* frames, int n, double usableMaxHz)
{
    BurstReport r;
    if (n < kMinFrames)
        return r;
    std::array<float, BandGrid::kNum> usual {};
    std::array<bool, BandGrid::kNum> counted {};
    float loudest = kHistoryFloorDb;
    std::vector<float> v;
    v.reserve (static_cast<size_t> (n));
    for (int b = 0; b < BandGrid::kNum; ++b)
    {
        if (BandGrid::centreHz (b) > usableMaxHz)
            continue;
        v.clear();
        for (int i = 0; i < n; ++i)
            if (const float x = frames[i][static_cast<size_t> (b)]; hasLevel (x))
                v.push_back (x);
        if (static_cast<int> (v.size()) < kMinFrames)
            continue;
        std::nth_element (v.begin(), v.begin() + static_cast<std::ptrdiff_t> (v.size() / 2), v.end());
        usual[static_cast<size_t> (b)] = v[v.size() / 2];
        counted[static_cast<size_t> (b)] = true;
        loudest = std::max (loudest, usual[static_cast<size_t> (b)]);
    }
    for (int b = 0; b < BandGrid::kNum; ++b)
    {
        if (!counted[static_cast<size_t> (b)] || usual[static_cast<size_t> (b)] < loudest - kBurstRangeDb)
            continue;
        for (int i = 0; i < n; ++i)
        {
            const float x = frames[i][static_cast<size_t> (b)];
            if (!hasLevel (x))
                continue;
            const float excess = x - usual[static_cast<size_t> (b)];
            if (!r.have || excess > r.excessDb)
            {
                r.have = true;
                r.band = b;
                r.framesAgo = n - 1 - i;
                r.excessDb = excess;
                r.levelDb = x;
                r.usualDb = usual[static_cast<size_t> (b)];
            }
        }
    }
    return r;
}
} // namespace spectro
} // namespace themis
