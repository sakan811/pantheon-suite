// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 The Pantheon authors

#pragma once

#include "Common.h"
#include "LowCrestMeter.h"

#include <pantheon/dsp/MusicBands.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>

namespace themis
{
namespace bandcrest
{
// What a band reads before enough audio has been measured, and in silence (the same role as the meters' "no value" when
// hasData() is false). A crest factor is never below 0 dB (a peak is at least its RMS), so this is clearly no value.
inline constexpr double kNothingDb = -1.0;

using Crests = std::array<double, pdsp::bands::kCount>; // one value per band, in pdsp::bands::kBands order
} // namespace bandcrest

// Crest factor (peak / RMS in dB) of each of the six main bands of music (pdsp::bands) of a stereo signal, analysed as mid.
// It is LowCrestMeter's measure for every band instead of its one 20-250 Hz band: the same ~0.4 s windows, the same gate
// (a band whose RMS is under LowCrestMeter::kGateDb is skipped), the same warm-up window, the same averaging. The split is
// pdsp::BandSplitter, whose bands sum flat, so the bands are the mid signal's own parts.
//
// A readout only: nothing here sets a limit, a zone or advice. Band-limited noise reads higher than a sine, so a zone would
// need research first.
//
// Threading: process() runs on the audio thread and allocates nothing (fixed-size arrays and the splitter's filters); the
// published values are atomics. requestReset() is safe from any thread and is applied by process(). prepare() must not
// overlap with process().
class BandCrestMeter
{
public:
    static constexpr int kBands = pdsp::bands::kCount;

    void prepare (double sampleRate)
    {
        fs = sampleRate;
        windowSamples = std::max (1, static_cast<int> (LowCrestMeter::kWindowSeconds * fs));
        splitter.prepare (fs);
        setAveragingSeconds (kDefaultAveragingSeconds);
        reset();
    }

    // 0 = infinite (cumulative mean).
    void setAveragingSeconds (float s) { avgSeconds.store (s, std::memory_order_relaxed); }

    void requestReset() { resetFlag.store (true, std::memory_order_release); }

    // Audio thread.
    void process (const float* l, const float* r, int n)
    {
        if (resetFlag.exchange (false, std::memory_order_acq_rel))
            reset();

        for (int i = 0; i < n; ++i)
        {
            std::array<double, kBands> part {};
            splitter.split (0, static_cast<double> (midOf (l[i], r[i])), part);
            for (int b = 0; b < kBands; ++b)
            {
                const double x = part[static_cast<size_t> (b)];
                peak[static_cast<size_t> (b)] = std::max (peak[static_cast<size_t> (b)], std::abs (x));
                sumSquares[static_cast<size_t> (b)] += x * x;
            }
            if (++windowCount >= windowSamples)
                finishWindow();
        }
    }

    // True when at least one band has a value. A pending reset counts as "no data" right away, even if no audio is flowing.
    bool hasData() const
    {
        for (int b = 0; b < kBands; ++b)
            if (hasData (b))
                return true;
        return false;
    }

    // Averaged crest factor of band `b` in dB (valid when hasData (b)).
    bool hasData (int b) const
    {
        return published[static_cast<size_t> (b)].windows.load (std::memory_order_relaxed) >= LowCrestMeter::kMinWindows
               && !resetFlag.load (std::memory_order_acquire);
    }

    // The six averaged crest factors in dB; bandcrest::kNothingDb for a band that has no value yet.
    bandcrest::Crests crestDb() const
    {
        bandcrest::Crests out {};
        for (int b = 0; b < kBands; ++b)
            out[static_cast<size_t> (b)] = hasData (b) ? static_cast<double> (published[static_cast<size_t> (b)].crestDb.load (std::memory_order_relaxed))
                                                       : bandcrest::kNothingDb;
        return out;
    }

private:
    void finishWindow()
    {
        for (int b = 0; b < kBands; ++b)
            if (!std::isfinite (sumSquares[static_cast<size_t> (b)])) // a NaN/inf input would otherwise poison the filter state for good
            {
                resetFilters();
                clearWindow();
                return;
            }

        if (warmingUp)
            warmingUp = false; // the first window contains the filters' start-up transient
        else
        {
            const double avgTime = avgSeconds.load (std::memory_order_relaxed);
            for (size_t b = 0; b < kBands; ++b)
            {
                const double rms = std::sqrt (sumSquares[b] / windowCount);
                if (linToDb (static_cast<float> (rms)) < LowCrestMeter::kGateDb)
                    continue;
                const float crest = linToDb (static_cast<float> (peak[b] / rms));
                ++windows[b];
                const double alpha = runningMeanWeight (emaWeight (LowCrestMeter::kWindowSeconds, avgTime), windows[b]);
                avg[b] += alpha * (static_cast<double> (crest) - avg[b]);
                published[b].crestDb.store (static_cast<float> (avg[b]), std::memory_order_relaxed);
                published[b].windows.store (windows[b], std::memory_order_relaxed);
            }
        }
        clearWindow();
    }

    void clearWindow()
    {
        peak.fill (0.0);
        sumSquares.fill (0.0);
        windowCount = 0;
    }

    void resetFilters()
    {
        splitter.reset();
        warmingUp = true;
    }

    void reset()
    {
        resetFilters();
        clearWindow();
        windows.fill (0);
        avg.fill (0.0);
        for (auto& p : published)
        {
            p.windows.store (0, std::memory_order_relaxed);
            p.crestDb.store (0.0f, std::memory_order_relaxed);
        }
    }

    double fs = 48000.0;
    int windowSamples = 19200;
    pdsp::BandSplitter splitter;
    std::array<double, kBands> peak {};
    std::array<double, kBands> sumSquares {};
    int windowCount = 0;
    std::array<int, kBands> windows {};
    std::array<double, kBands> avg {};
    bool warmingUp = true;

    std::atomic<float> avgSeconds { kDefaultAveragingSeconds };
    std::atomic<bool> resetFlag { false };

    struct Published
    {
        std::atomic<float> crestDb { 0.0f };
        std::atomic<int> windows { 0 };
    };
    std::array<Published, kBands> published;
};
} // namespace themis
