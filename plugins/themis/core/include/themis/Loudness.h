// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 The Pantheon authors

#pragma once

#include "Biquad.h"

#include <array>
#include <vector>

namespace themis
{
// ITU-R BS.1770-4 K-weighting, derived from the analogue prototype so it is valid at any sample rate.
inline std::pair<BiquadCoeffs, BiquadCoeffs> makeKWeighting (double fs)
{
    // Stage 1: high-shelf "head" filter.
    BiquadCoeffs shelf;
    {
        const double G = 3.999843853973347, Q = 0.7071752369554196, fc = 1681.974450955533;
        const double K = std::tan (kPi * fc / fs);
        const double Vh = std::pow (10.0, G / 20.0);
        const double Vb = std::pow (Vh, 0.4996667741545416);
        const double a0 = 1.0 + K / Q + K * K;
        shelf.b0 = (Vh + Vb * K / Q + K * K) / a0;
        shelf.b1 = 2.0 * (K * K - Vh) / a0;
        shelf.b2 = (Vh - Vb * K / Q + K * K) / a0;
        shelf.a1 = 2.0 * (K * K - 1.0) / a0;
        shelf.a2 = (1.0 - K / Q + K * K) / a0;
    }
    // Stage 2: RLB high-pass. Numerator is deliberately not divided by a0 (matches the ITU table).
    BiquadCoeffs hp;
    {
        const double Q = 0.5003270373238773, fc = 38.13547087602444;
        const double K = std::tan (kPi * fc / fs);
        const double a0 = 1.0 + K / Q + K * K;
        hp.b0 = 1.0;
        hp.b1 = -2.0;
        hp.b2 = 1.0;
        hp.a1 = 2.0 * (K * K - 1.0) / a0;
        hp.a2 = (1.0 - K / Q + K * K) / a0;
    }
    return { shelf, hp };
}

// Momentary (400 ms), short-term (3 s) and gated integrated loudness for a stereo signal.
// Realtime-safe after prepare(): the integrated gate uses a fixed 0.1 LU histogram.
class LoudnessMeter
{
public:
    static constexpr float kSilence = -120.0f;

    void prepare (double sampleRate)
    {
        fs = sampleRate;
        subBlockLen = std::max (1, static_cast<int> (std::lround (0.1 * fs)));
        const auto kw = makeKWeighting (fs);
        for (int c = 0; c < 2; ++c)
        {
            stage1[c].setCoeffs (kw.first);
            stage2[c].setCoeffs (kw.second);
        }
        reset();
    }

    void reset()
    {
        for (int c = 0; c < 2; ++c)
        {
            stage1[c].reset();
            stage2[c].reset();
        }
        ring.fill (0.0);
        ringPos = 0;
        filled = 0;
        subCount = 0;
        subSum = 0.0;
        hist.fill ({});
        shortHist.fill (0);
        momentaryLufs = shortTermLufs = integratedLufs = kSilence;
        rangeLu = 0.0f;
    }

    void process (const float* l, const float* r, int n)
    {
        for (int i = 0; i < n; ++i)
        {
            // A NaN or inf would stay in the filter state for good: every later value NaN, the short-term reading dead and
            // the integrated one frozen at what it was. A glitched sample counts as silence instead.
            const float xl = std::isfinite (l[i]) ? l[i] : 0.0f, xr = std::isfinite (r[i]) ? r[i] : 0.0f;
            const float kl = stage2[0].process (stage1[0].process (xl));
            const float kr = stage2[1].process (stage1[1].process (xr));
            subSum += static_cast<double> (kl) * kl + static_cast<double> (kr) * kr;
            if (++subCount >= subBlockLen)
                finishSubBlock();
        }
    }

    float momentary() const { return momentaryLufs; }
    float shortTerm() const { return shortTermLufs; }
    float integrated() const { return integratedLufs; }
    // Loudness range (EBU Tech 3342): the spread of the short-term loudness, 10th to 95th percentile of the 3 s values above
    // -70 LUFS and no more than 20 LU under their mean. 0 until there is a short-term value to rank.
    float loudnessRange() const { return rangeLu; }

private:
    static constexpr int kRing = 30; // 3 s of 100 ms sub-blocks
    static constexpr int kBins = 900; // -70 .. +20 LUFS in 0.1 LU steps
    struct Bin
    {
        double energy = 0.0;
        int count = 0;
    };

    static float toLufs (double meanSquare)
    {
        return meanSquare <= 1.0e-12 ? kSilence : static_cast<float> (-0.691 + 10.0 * std::log10 (meanSquare));
    }

    double meanOfLast (int count) const
    {
        count = std::min (count, filled);
        if (count <= 0)
            return 0.0;
        double s = 0.0;
        for (int i = 0; i < count; ++i)
            s += ring[static_cast<size_t> ((ringPos - 1 - i + kRing) % kRing)];
        return s / count;
    }

    void finishSubBlock()
    {
        ring[static_cast<size_t> (ringPos)] = subSum / subBlockLen;
        ringPos = (ringPos + 1) % kRing;
        filled = std::min (filled + 1, kRing);
        subSum = 0.0;
        subCount = 0;

        const double m = meanOfLast (4);
        momentaryLufs = toLufs (m);
        shortTermLufs = toLufs (meanOfLast (kRing));

        if (filled >= 4)
        {
            const float lk = toLufs (m);
            if (lk > -70.0f)
            {
                const int idx = std::clamp (static_cast<int> ((lk + 70.0f) * 10.0f), 0, kBins - 1);
                hist[static_cast<size_t> (idx)].energy += m;
                hist[static_cast<size_t> (idx)].count++;
                updateIntegrated();
            }
        }
        if (filled >= kRing && shortTermLufs > -70.0f)
        {
            shortHist[static_cast<size_t> (std::clamp (static_cast<int> ((shortTermLufs + 70.0f) * 10.0f), 0, kBins - 1))]++;
            updateRange();
        }
    }

    // Walks the 900-bin histogram once per 100 ms, like the integrated gate.
    void updateRange()
    {
        double energy = 0.0;
        long n = 0;
        for (int i = 0; i < kBins; ++i)
        {
            const auto c = shortHist[static_cast<size_t> (i)];
            energy += static_cast<double> (c) * std::pow (10.0, (binCentre (i) + 0.691) / 10.0);
            n += c;
        }
        if (n == 0)
            return;
        const float gate = toLufs (energy / static_cast<double> (n)) - 20.0f;
        long gated = 0;
        for (int i = 0; i < kBins; ++i)
            if (binCentre (i) >= gate)
                gated += shortHist[static_cast<size_t> (i)];
        const auto percentile = [&] (double share) {
            const double want = share * static_cast<double> (gated);
            long seen = 0;
            for (int i = 0; i < kBins; ++i)
            {
                if (binCentre (i) < gate)
                    continue;
                seen += shortHist[static_cast<size_t> (i)];
                if (static_cast<double> (seen) >= want)
                    return binCentre (i);
            }
            return binCentre (kBins - 1);
        };
        rangeLu = gated > 0 ? std::max (0.0f, percentile (0.95) - percentile (0.10)) : 0.0f;
    }

    static float binCentre (int i) { return -70.0f + (static_cast<float> (i) + 0.5f) * 0.1f; }

    void updateIntegrated()
    {
        double e = 0.0;
        long n = 0;
        for (const auto& b : hist)
        {
            e += b.energy;
            n += b.count;
        }
        if (n == 0)
            return;
        const float relGate = toLufs (e / static_cast<double> (n)) - 10.0f;
        double e2 = 0.0;
        long n2 = 0;
        for (int i = 0; i < kBins; ++i)
        {
            const float binLufs = -70.0f + static_cast<float> (i) * 0.1f;
            if (binLufs >= relGate)
            {
                e2 += hist[static_cast<size_t> (i)].energy;
                n2 += hist[static_cast<size_t> (i)].count;
            }
        }
        if (n2 > 0)
            integratedLufs = toLufs (e2 / static_cast<double> (n2));
    }

    double fs = 48000.0;
    int subBlockLen = 4800, subCount = 0;
    double subSum = 0.0;
    Biquad stage1[2], stage2[2];
    std::array<double, kRing> ring {};
    int ringPos = 0, filled = 0;
    std::array<Bin, kBins> hist {};
    std::array<long, kBins> shortHist {}; // short-term values, for the loudness range
    float momentaryLufs = kSilence, shortTermLufs = kSilence, integratedLufs = kSilence, rangeLu = 0.0f;
};
} // namespace themis
