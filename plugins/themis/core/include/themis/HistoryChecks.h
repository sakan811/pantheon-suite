// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 The Pantheon authors

#pragma once

#include "Common.h"
#include "FrameHistory.h"
#include "Fundamental.h"
#include "TonalAnalyzer.h"
#include "Translation.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

namespace themis
{
// Verdicts for the time graphs: each reads a window of FrameHistory frames (oldest first) and answers the question the
// graph is there for. Pure functions on the frames, so they are tested without a UI. Every limit is a hand-authored
// rule of thumb, not measured data.
namespace history
{
inline constexpr int kMinFrames = 12; // ~1 s of frames: less than this is not worth a verdict

// How much a range's level moves over the window (90th minus 10th percentile): beyond this the low end, say, is
// not holding steady from section to section.
inline constexpr float kSwingWarnDb = 6.0f, kSwingBadDb = 10.0f;

// Left/right level difference.
inline constexpr float kBalanceWarnDb = 0.5f, kBalanceBadDb = 1.5f;

// Bass fundamentals under this are beyond what phones, laptops and small monitors reproduce; a master whose bass
// lives there for more than kSubFloorWarnShare of the time depends on systems that can.
inline constexpr double kSubFloorHz = 40.0;
inline constexpr float kSubFloorWarnShare = 0.25f;

// Values of one series over a window, frames without a level left out.
struct Stats
{
    int n = 0;
    float mean = kHistoryFloorDb; // mean of the dB values
    float lo = kHistoryFloorDb, hi = kHistoryFloorDb;
    float p10 = kHistoryFloorDb, p90 = kHistoryFloorDb;
    float swingDb() const { return p90 - p10; }
};

template <typename Value> // Value: float (const FrameStats&)
Stats summarise (const FrameStats* frames, int n, Value&& value)
{
    std::vector<float> v;
    v.reserve (static_cast<size_t> (std::max (n, 0)));
    for (int i = 0; i < n; ++i)
        if (const float x = value (frames[i]); hasLevel (x))
            v.push_back (x);
    Stats s;
    s.n = static_cast<int> (v.size());
    if (v.empty())
        return s;
    std::sort (v.begin(), v.end());
    double sum = 0.0;
    for (const float x : v)
        sum += x;
    const auto at = [&] (double q) { return v[static_cast<size_t> (std::lround (q * static_cast<double> (v.size() - 1)))]; };
    s.mean = static_cast<float> (sum / static_cast<double> (v.size()));
    s.lo = v.front();
    s.hi = v.back();
    s.p10 = at (0.1);
    s.p90 = at (0.9);
    return s;
}

// Mean power of a series over the window, as dB (loud passages count for more than quiet ones, like a meter's
// long-term average). kHistoryFloorDb when no frame has a level.
template <typename Value>
float meanPowerDb (const FrameStats* frames, int n, Value&& value)
{
    double sum = 0.0;
    int count = 0;
    for (int i = 0; i < n; ++i)
        if (const float x = value (frames[i]); hasLevel (x))
        {
            sum += dbToPower (x);
            ++count;
        }
    return count > 0 ? static_cast<float> (powerToDb (sum / count, 1.0e-12)) : kHistoryFloorDb;
}

// ---- level over time (BANDS) -----------------------------------------------------------------
struct SwingReport
{
    bool have = false;
    int widest = 0;       // range that moves most
    float swingDb = 0.0f; // its swing
    int steadiest = 0;
    float steadiestDb = 0.0f;
    Level level = Level::good;
};

inline SwingReport analyseSwing (const FrameStats* frames, int n)
{
    SwingReport r;
    if (n < kMinFrames)
        return r;
    for (int c = 0; c < kNumEnergyRanges; ++c)
    {
        const auto s = summarise (frames, n, [c] (const FrameStats& f) { return f.energy (Signal::stereo, c); });
        if (s.n < kMinFrames)
            continue;
        if (!r.have || s.swingDb() > r.swingDb)
        {
            r.widest = c;
            r.swingDb = s.swingDb();
        }
        if (!r.have || s.swingDb() < r.steadiestDb)
        {
            r.steadiest = c;
            r.steadiestDb = s.swingDb();
        }
        r.have = true;
    }
    r.level = translation::classify (r.swingDb, kSwingWarnDb, kSwingBadDb);
    return r;
}

// ---- mid / side (width) ----------------------------------------------------------------------
struct WidthReport
{
    bool have = false;
    float sideMinusMidDb = 0.0f; // mean side level against mean mid level: very negative is narrow, 0 dB is as wide as it is centred
    float monoLossDb = 0.0f;     // what a sum to mono loses of the signal's power: 10 log10 (1 + S / M)
    Level level = Level::good;
};

// The mono-loss limits are the ones the mono-fold readouts use: the low range is held to the tighter low-end limits, the
// whole spectrum to the overall ones, and the other ranges to the single-band ones.
inline float monoWarnDb (int column)
{
    return column == 0 ? translation::kLowMonoWarnDb : column == kAllRanges ? translation::kMonoWarnDb : translation::kBandMonoWarnDb;
}
inline float monoBadDb (int column)
{
    return column == 0 ? translation::kLowMonoBadDb : column == kAllRanges ? translation::kMonoBadDb : translation::kBandMonoBadDb;
}
inline Level monoLossLevel (int column, float lossDb) { return translation::classify (lossDb, monoWarnDb (column), monoBadDb (column)); }

inline WidthReport analyseWidth (const FrameStats* frames, int n, int column)
{
    WidthReport r;
    const float mid = meanPowerDb (frames, n, [column] (const FrameStats& f) { return f.energy (Signal::mid, column); });
    const float side = meanPowerDb (frames, n, [column] (const FrameStats& f) { return f.energy (Signal::side, column); });
    if (n < kMinFrames || !hasLevel (mid) || !hasLevel (side))
        return r;
    r.have = true;
    r.sideMinusMidDb = side - mid;
    r.monoLossDb = static_cast<float> (10.0 * std::log10 (1.0 + dbToPower (r.sideMinusMidDb)));
    r.level = monoLossLevel (column, r.monoLossDb);
    return r;
}

// How far the side signal of this column has to come down for a sum to mono to lose no more than the warning limit: the
// amount a width item asks for, computed from the measurement (0 when it is already inside the limit).
inline float sideCutToMonoOkDb (const WidthReport& r, int column)
{
    if (!r.have)
        return 0.0f;
    const double okRatio = std::pow (10.0, static_cast<double> (monoWarnDb (column)) / 10.0) - 1.0; // the side-to-mid power ratio at the limit
    return std::max (0.0f, r.sideMinusMidDb - static_cast<float> (10.0 * std::log10 (okRatio)));
}

// ---- left / right (balance) ------------------------------------------------------------------
struct BalanceReport
{
    bool have = false;
    float leftMinusRightDb = 0.0f;
    Level level = Level::good;
};

inline BalanceReport analyseBalance (const FrameStats* frames, int n, int column)
{
    BalanceReport r;
    const float left = meanPowerDb (frames, n, [column] (const FrameStats& f) { return f.energy (Signal::left, column); });
    const float right = meanPowerDb (frames, n, [column] (const FrameStats& f) { return f.energy (Signal::right, column); });
    if (n < kMinFrames || !hasLevel (left) || !hasLevel (right))
        return r;
    r.have = true;
    r.leftMinusRightDb = left - right;
    r.level = translation::classify (std::abs (r.leftMinusRightDb), kBalanceWarnDb, kBalanceBadDb);
    return r;
}

// ---- peaks and clipping ----------------------------------------------------------------------
namespace clip
{
inline constexpr float kTruePeakWarnDb = -1.0f; // the usual streaming-delivery ceiling: above it lossy encoding can clip
inline constexpr float kTruePeakBadDb = 0.0f;

// A master with flat-topped clips, samples at 0 dBFS or inter-sample peaks over 0 dBTP is bad; one that is merely closer
// than 1 dB to full scale is a warning.
inline Level level (float truePeakDb, uint64_t clipEvents, uint64_t overSamples)
{
    if (clipEvents > 0 || overSamples > 0)
        return Level::bad;
    return translation::classify (truePeakDb, kTruePeakWarnDb, kTruePeakBadDb);
}
} // namespace clip

struct PeakReport
{
    bool have = false;
    float samplePeakDb = kHistoryFloorDb, truePeakDb = kHistoryFloorDb;
    uint64_t clipEvents = 0, clipSamples = 0, overSamples = 0;
    int clippedFrames = 0;
    Level level = Level::good;
};

inline PeakReport analysePeaks (const FrameStats* frames, int n)
{
    PeakReport r;
    for (int i = 0; i < n; ++i)
    {
        const auto& f = frames[i];
        r.samplePeakDb = std::max (r.samplePeakDb, f.samplePeakDb);
        r.truePeakDb = std::max (r.truePeakDb, f.truePeakDb);
        r.clipEvents += static_cast<uint64_t> (f.clipEvents);
        r.clipSamples += static_cast<uint64_t> (f.clipSamples);
        r.overSamples += static_cast<uint64_t> (f.overSamples);
        r.clippedFrames += f.clipped() ? 1 : 0;
    }
    r.have = n >= kMinFrames && hasLevel (r.truePeakDb);
    r.level = clip::level (r.truePeakDb, r.clipEvents, r.overSamples);
    return r;
}

// ---- bass fundamental ------------------------------------------------------------------------
struct BassReport
{
    bool have = false;
    int tracked = 0;                     // frames with a tonal bass
    double lowHz = 0.0, medianHz = 0.0, highHz = 0.0; // 5th, 50th and 95th percentile of the fundamental (note onsets mis-track now and then, so not min / max)
    std::array<float, 3> share {};       // share of tracked frames per fundamental::Balance
    fundamental::Balance dominant = fundamental::Balance::balanced;
    float medianTiltDb = 0.0f;
    float belowFloorShare = 0.0f;        // share of tracked frames with a fundamental under kSubFloorHz
    Level balanceLevel = Level::good;    // balanced is good, anything else is worth a look
    Level floorLevel = Level::good;
};

inline BassReport analyseBass (const FrameStats* frames, int n)
{
    BassReport r;
    std::vector<double> hz;
    std::vector<float> tilt;
    for (int i = 0; i < n; ++i)
    {
        if (!frames[i].hasFundamental())
            continue;
        hz.push_back (frames[i].f0Hz);
        const float t = fundamental::tiltDb (frames[i]);
        tilt.push_back (t);
        ++r.share[static_cast<size_t> (fundamental::balanceOf (t))];
    }
    r.tracked = static_cast<int> (hz.size());
    if (n < kMinFrames || r.tracked < kMinFrames / 2)
        return r;
    r.have = true;
    std::sort (hz.begin(), hz.end());
    std::sort (tilt.begin(), tilt.end());
    const auto percentile = [&] (double q) { return hz[static_cast<size_t> (std::lround (q * static_cast<double> (hz.size() - 1)))]; };
    r.lowHz = percentile (0.05);
    r.medianHz = percentile (0.5);
    r.highHz = percentile (0.95);
    r.medianTiltDb = tilt[tilt.size() / 2];
    for (auto& s : r.share)
        s /= static_cast<float> (r.tracked);
    r.dominant = static_cast<fundamental::Balance> (std::max_element (r.share.begin(), r.share.end()) - r.share.begin());
    r.belowFloorShare = static_cast<float> (std::count_if (hz.begin(), hz.end(), [] (double f) { return f < kSubFloorHz; })) / static_cast<float> (hz.size());
    r.balanceLevel = r.dominant == fundamental::Balance::balanced ? Level::good : Level::warn;
    r.floorLevel = r.belowFloorShare > kSubFloorWarnShare ? Level::warn : Level::good;
    return r;
}
} // namespace history
} // namespace themis
