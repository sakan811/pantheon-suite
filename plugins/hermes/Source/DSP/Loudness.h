// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 The Pantheon authors

#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <pantheon/dsp/Biquad.h>

// Loudness as ITU-R BS.1770-4 and EBU R128 define it: K-weighting, momentary (400 ms), short-term (3 s), integrated (gated) and loudness range. Plain C++, no JUCE, nothing
// allocated after construction (the histograms are fixed), so the audio thread can run it and a loader can run it on a file with the same code.
namespace hermes
{
inline constexpr double kAbsoluteGateLufs = -70.0;
inline constexpr double kRelativeGateLu = -10.0;      // integrated
inline constexpr double kRangeRelativeGateLu = -20.0; // loudness range
inline constexpr double kHistogramBottom = -70.0, kHistogramStep = 0.1;
inline constexpr int kHistogramBins = 801;            // -70.0 to +10.0 LUFS in steps of 0.1
inline constexpr int kSubBlocksMomentary = 4, kSubBlocksShortTerm = 30; // 100 ms sub-blocks: 400 ms with 75% overlap, 3 s

inline double toLufs (double power) noexcept { return power > 1e-14 ? -0.691 + 10.0 * std::log10 (power) : -200.0; }
inline double fromLufs (double lufs) noexcept { return std::pow (10.0, (lufs + 0.691) / 10.0); }

// The two filters of BS.1770's K-weighting for any sample rate (the analog prototypes through the bilinear transform; the coefficients of the standard at 48 kHz).
inline void kWeighting (double fs, pdsp::Biquad& shelf, pdsp::Biquad& highpass) noexcept
{
    {
        const double f0 = 1681.974450955533, gain = 3.999843853973347, q = 0.7071752369554196;
        const double k = std::tan (M_PI * f0 / fs), vh = std::pow (10.0, gain / 20.0), vb = std::pow (vh, 0.4996667741545416);
        const double a0 = 1.0 + k / q + k * k;
        shelf.set (vh + vb * k / q + k * k, 2.0 * (k * k - vh), vh - vb * k / q + k * k, a0, 2.0 * (k * k - 1.0), 1.0 - k / q + k * k);
    }
    {
        const double f0 = 38.13547087602444, q = 0.5003270373238773;
        const double k = std::tan (M_PI * f0 / fs);
        const double a0 = 1.0 + k / q + k * k;
        highpass.set (1.0, -2.0, 1.0, a0, 2.0 * (k * k - 1.0), 1.0 - k / q + k * k);
    }
}

class LoudnessMeter
{
public:
    void prepare (double sampleRate)
    {
        fs = sampleRate;
        subLength = std::max (1, (int) std::lround (0.1 * fs));
        for (auto& c : ch)
            kWeighting (fs, c.shelf, c.highpass);
        reset();
    }

    void reset() noexcept
    {
        for (auto& c : ch)
        {
            c.shelf.reset();
            c.highpass.reset();
        }
        ring.fill (0.0);
        filled = 0;
        head = 0;
        accumulated = 0.0;
        counted = 0;
        gated = {};
        ranged = {};
        momentaryNow = shortTermNow = -200.0;
    }

    // One stereo sample. (Left and right weigh 1.0 each; a mono sound is both.)
    inline void push (double l, double r) noexcept
    {
        const double a = ch[0].highpass.process (ch[0].shelf.process (l)), b = ch[1].highpass.process (ch[1].shelf.process (r));
        accumulated += a * a + b * b;
        if (++counted < subLength)
            return;
        ring[(size_t) head] = accumulated / (double) subLength;
        head = (head + 1) % kSubBlocksShortTerm;
        filled = std::min (filled + 1, kSubBlocksShortTerm);
        accumulated = 0.0;
        counted = 0;

        momentaryNow = toLufs (meanOfLast (kSubBlocksMomentary));
        shortTermNow = toLufs (meanOfLast (kSubBlocksShortTerm));
        if (filled >= kSubBlocksMomentary)
            add (gated, momentaryNow, fromLufs (momentaryNow)); // the gating blocks of the integrated loudness: 400 ms, every 100 ms
        if (filled >= kSubBlocksShortTerm)
            add (ranged, shortTermNow, fromLufs (shortTermNow));
    }

    double momentary() const noexcept { return momentaryNow; }
    double shortTerm() const noexcept { return shortTermNow; }

    // The gated mean of the blocks since the last reset: blocks over -70 LUFS, then over 10 LU under the mean of those.
    double integrated() const noexcept
    {
        double power = 0.0, count = 0.0;
        for (int i = 0; i < kHistogramBins; ++i)
        {
            power += gated.power[(size_t) i];
            count += gated.count[(size_t) i];
        }
        if (count < 1.0)
            return -200.0;
        const double threshold = toLufs (power / count) + kRelativeGateLu;
        double p = 0.0, c = 0.0;
        for (int i = 0; i < kHistogramBins; ++i)
            if (centre (i) >= threshold)
            {
                p += gated.power[(size_t) i];
                c += gated.count[(size_t) i];
            }
        return c < 1.0 ? -200.0 : toLufs (p / c);
    }

    // The spread of the short-term loudness: between the 10th and the 95th percentile of the values over -70 LUFS and over 20 LU under their mean.
    double range() const noexcept
    {
        double power = 0.0, count = 0.0;
        for (int i = 0; i < kHistogramBins; ++i)
        {
            power += ranged.power[(size_t) i];
            count += ranged.count[(size_t) i];
        }
        if (count < 1.0)
            return 0.0;
        const double threshold = toLufs (power / count) + kRangeRelativeGateLu;
        double total = 0.0;
        for (int i = 0; i < kHistogramBins; ++i)
            if (centre (i) >= threshold)
                total += ranged.count[(size_t) i];
        if (total < 1.0)
            return 0.0;
        double seen = 0.0, low = 0.0, high = 0.0;
        bool gotLow = false;
        for (int i = 0; i < kHistogramBins; ++i)
            if (centre (i) >= threshold)
            {
                seen += ranged.count[(size_t) i];
                if (! gotLow && seen >= 0.10 * total)
                {
                    low = centre (i);
                    gotLow = true;
                }
                if (seen >= 0.95 * total)
                {
                    high = centre (i);
                    break;
                }
            }
        return std::fmax (high - low, 0.0);
    }

private:
    struct Histogram
    {
        std::array<double, kHistogramBins> count {}, power {};
    };
    struct Channel
    {
        pdsp::Biquad shelf, highpass;
    };

    static double centre (int bin) noexcept { return kHistogramBottom + kHistogramStep * bin; }

    double meanOfLast (int blocks) const noexcept
    {
        const int n = std::min (blocks, filled);
        double sum = 0.0;
        for (int i = 1; i <= n; ++i)
            sum += ring[(size_t) ((head + kSubBlocksShortTerm - i) % kSubBlocksShortTerm)];
        return n > 0 ? sum / n : 0.0;
    }

    static void add (Histogram& h, double lufs, double power) noexcept
    {
        if (lufs < kAbsoluteGateLufs)
            return;
        const int bin = std::clamp ((int) std::lround ((lufs - kHistogramBottom) / kHistogramStep), 0, kHistogramBins - 1);
        h.count[(size_t) bin] += 1.0;
        h.power[(size_t) bin] += power;
    }

    double fs = 48000.0, accumulated = 0.0, momentaryNow = -200.0, shortTermNow = -200.0;
    int subLength = 4800, counted = 0, filled = 0, head = 0;
    std::array<double, kSubBlocksShortTerm> ring {};
    std::array<Channel, 2> ch;
    Histogram gated, ranged;
};
} // namespace hermes
