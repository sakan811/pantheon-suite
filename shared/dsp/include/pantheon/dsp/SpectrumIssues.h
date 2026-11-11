// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 The Pantheon authors

#pragma once

#include <algorithm>
#include <array>
#include <cmath>

namespace pdsp
{
// The grid a spectrogram and its helpers share: 1/24 octave from 20 Hz to 20 kHz. Plain C++.
struct SpectrumGrid
{
    static constexpr int kPerOctave = 24;
    static constexpr int kSteps = 240;
    static constexpr double kLowHz = 20.0;
    using Column = std::array<float, kSteps>; // one moment: the level of every step, dB

    static double hz (double step) noexcept { return kLowHz * std::pow (2.0, step / kPerOctave); }
    static double stepAt (double freqHz) noexcept { return kPerOctave * std::log2 (std::max (freqHz, 1.0) / kLowHz); }
};

// Reads a stream of spectrogram columns and points at two kinds of trouble, for a display to mark:
//
//   peaks   narrow regions that stand out of their neighbourhood and stay there (a ringing mode, a harsh formant): what an
//           equaliser cut or a narrow band is for;
//   swings  regions whose level keeps jumping far above its own average (a kick that pokes out, a vocal that leaps, sibilance):
//           what a compressor band is for. Each comes with the level its jumps reach, so a threshold can be suggested.
//
// Fed from a display's timer, never the audio thread. The limits are rules of thumb, not measured data.
class IssueFinder
{
public:
    static constexpr int kSteps = SpectrumGrid::kSteps;
    static constexpr float kAverageSeconds = 3.0f;     // the long-term level a region is compared with
    static constexpr float kMinSeconds = 2.0f;         // nothing is reported before this much has been heard
    static constexpr float kHoldFallDbPerSecond = 4.0f; // how fast the remembered jump lets go
    static constexpr float kRangeDb = 40.0f;           // anything this far under the loudest region is ignored
    static constexpr float kSilentDb = -100.0f;        // a column whose loudest step is under this says nothing (transport stopped)

    static constexpr int kNear = 4, kFar = 12;         // peaks: 1/6 and 1/2 octave, in steps
    static constexpr float kMinProminenceDb = 6.0f;
    static constexpr int kMaxPeaks = 4;

    static constexpr float kMinSwingDb = 10.0f;        // swings: the jump over the average that counts
    static constexpr int kMinSwingSteps = 6;           // at least 1/4 octave wide
    static constexpr int kSwingGapSteps = 4;           // regions closer than 1/6 octave are one
    static constexpr int kMaxSwings = 3;

    struct Peak
    {
        double freqHz = 0.0;
        float levelDb = 0.0f, prominenceDb = 0.0f;
    };

    struct Swing
    {
        double loHz = 0.0, hiHz = 0.0;
        float swingDb = 0.0f;   // how far the jumps go above the average
        float peakDb = 0.0f;    // the level they reach
        float thresholdDb() const noexcept { return peakDb - 0.5f * swingDb; } // half way: the jumps are caught, the average is not
    };

    void reset() noexcept { seconds = 0.0f; }

    void push (const SpectrumGrid::Column& levelDb, float dt) noexcept
    {
        if (*std::max_element (levelDb.begin(), levelDb.end()) < kSilentDb)
            return;
        if (seconds <= 0.0f)
        {
            mean = levelDb;
            hold = levelDb;
        }
        const float a = 1.0f - std::exp (-dt / kAverageSeconds);
        for (size_t i = 0; i < (size_t) kSteps; ++i)
        {
            mean[i] += a * (levelDb[i] - mean[i]);
            hold[i] = std::max (levelDb[i], hold[i] - kHoldFallDbPerSecond * dt);
        }
        seconds += dt;
    }

    int peaks (std::array<Peak, kMaxPeaks>& found) const noexcept
    {
        if (seconds < kMinSeconds)
            return 0;
        const float loudest = *std::max_element (mean.begin(), mean.end());
        int count = 0;
        for (int i = kFar; i < kSteps - kFar; ++i)
        {
            const float level = mean[(size_t) i];
            if (level < loudest - kRangeDb)
                continue;
            bool highest = true;
            for (int k = -kNear; k <= kNear && highest; ++k)
                highest = mean[(size_t) (i + k)] <= level;
            if (! highest)
                continue;
            float around = 0.0f;
            for (int k = kNear + 1; k <= kFar; ++k)
                around += mean[(size_t) (i - k)] + mean[(size_t) (i + k)];
            const float prominence = level - around / (float) (2 * (kFar - kNear));
            if (prominence < kMinProminenceDb)
                continue;
            const Peak p { SpectrumGrid::hz (i), level, prominence };
            insert (found, count, p, [] (const Peak& x, const Peak& y) { return x.prominenceDb > y.prominenceDb; });
            i += kNear; // the next one cannot be inside this one
        }
        return count;
    }

    int swings (std::array<Swing, kMaxSwings>& found) const noexcept
    {
        if (seconds < kMinSeconds)
            return 0;
        const float loudest = *std::max_element (hold.begin(), hold.end());
        const auto jumps = [&] (int i) { return hold[(size_t) i] - mean[(size_t) i] >= kMinSwingDb && hold[(size_t) i] >= loudest - kRangeDb; };
        int count = 0;
        for (int i = 0; i < kSteps; ++i)
        {
            if (! jumps (i))
                continue;
            int last = i;
            for (int k = last + 1; k < kSteps && k <= last + kSwingGapSteps; ++k)
                if (jumps (k))
                    last = k;
            if (last - i + 1 >= kMinSwingSteps)
            {
                Swing s { SpectrumGrid::hz (i - 0.5), SpectrumGrid::hz (last + 0.5), 0.0f, -1.0e9f };
                for (int k = i; k <= last; ++k)
                {
                    s.swingDb += (hold[(size_t) k] - mean[(size_t) k]) / (float) (last - i + 1);
                    s.peakDb = std::max (s.peakDb, hold[(size_t) k]);
                }
                insert (found, count, s, [] (const Swing& x, const Swing& y) { return x.swingDb > y.swingDb; });
            }
            i = last;
        }
        return count;
    }

private:
    // Keeps the best `N` in order.
    template <typename T, size_t N, typename Better>
    static void insert (std::array<T, N>& list, int& count, const T& item, Better better) noexcept
    {
        int at = count;
        while (at > 0 && better (item, list[(size_t) (at - 1)]))
            --at;
        if (at >= (int) N)
            return;
        count = std::min (count + 1, (int) N);
        for (int k = count - 1; k > at; --k)
            list[(size_t) k] = list[(size_t) (k - 1)];
        list[(size_t) at] = item;
    }

    SpectrumGrid::Column mean {}, hold {};
    float seconds = 0.0f;
};
} // namespace pdsp
