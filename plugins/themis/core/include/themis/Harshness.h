// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 The Pantheon authors

#pragma once

#include "BandGrid.h"
#include "Common.h"
#include "FrameHistory.h"
#include "Spectrogram.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace themis
{
// Harshness and sibilance: a region of the spectrum whose level keeps jumping well above its own average over the window the
// spectrogram keeps (about a minute and a half). A steady region is not a fault, and neither is a region that is loud all the
// time; what catches the ear is a level that reaches far above its usual on the loud part of the programme, again and again.
// Pure functions on the spectrogram's band history, tested without a UI. Every limit is a rule of thumb.
namespace harshness
{
inline constexpr double kLoHz = 4000.0, kHiHz = 10000.0; // the region: the bands whose centre is inside it (sibilance and the harsh top)
inline constexpr float kJumpDb = 6.0f;      // the level reaches this far above its own average on its loud tenth of frames
inline constexpr float kRangeDb = 60.0f;    // a region this far under the loudest band of the window is below what the spectrogram shows (its span): not a fault

struct Report
{
    bool have = false;
    float averageDb = 0.0f; // the region's level, averaged in dB over the frames that carry it
    float reachedDb = 0.0f; // the level it reaches on its loud tenth of frames (90th percentile)
    float jumpDb() const { return reachedDb - averageDb; }
};

// `frames` oldest first; `usableMaxHz` is the top the sample rate carries (bands above it are left out).
inline Report analyse (const BandLevels* frames, int n, double usableMaxHz)
{
    Report r;
    if (n < spectro::kMinFrames)
        return r;

    std::vector<int> region;
    for (int b = 0; b < BandGrid::kNum; ++b)
        if (BandGrid::centreHz (b) >= kLoHz && BandGrid::centreHz (b) <= kHiHz && BandGrid::centreHz (b) <= usableMaxHz)
            region.push_back (b);
    if (region.size() < 2)
        return r;

    // The region's level per frame: the mean power of its bands, as dB.
    std::vector<float> series;
    series.reserve (static_cast<size_t> (n));
    for (int i = 0; i < n; ++i)
    {
        double sum = 0.0;
        int count = 0;
        for (const int b : region)
            if (const float x = frames[i][static_cast<size_t> (b)]; hasLevel (x))
            {
                sum += dbToPower (x);
                ++count;
            }
        if (count > 0)
            series.push_back (static_cast<float> (powerToDb (sum / count, 1.0e-12)));
    }
    if (static_cast<int> (series.size()) < spectro::kMinFrames)
        return r;

    // The loudest band of the window, by its average level, for the noise-floor limit.
    float loudest = kHistoryFloorDb;
    for (int b = 0; b < BandGrid::kNum; ++b)
    {
        if (BandGrid::centreHz (b) > usableMaxHz)
            continue;
        double sum = 0.0;
        int count = 0;
        for (int i = 0; i < n; ++i)
            if (const float x = frames[i][static_cast<size_t> (b)]; hasLevel (x))
            {
                sum += dbToPower (x);
                ++count;
            }
        if (count >= spectro::kMinFrames)
            loudest = std::max (loudest, static_cast<float> (powerToDb (sum / count, 1.0e-12)));
    }

    double sumDb = 0.0;
    for (const float x : series)
        sumDb += x;
    r.averageDb = static_cast<float> (sumDb / static_cast<double> (series.size()));
    std::sort (series.begin(), series.end());
    r.reachedDb = series[static_cast<size_t> (std::lround (0.9 * static_cast<double> (series.size() - 1)))];
    r.have = r.averageDb >= loudest - kRangeDb;
    return r;
}
} // namespace harshness
} // namespace themis
