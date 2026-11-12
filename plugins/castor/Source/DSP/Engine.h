// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 The Pantheon authors

#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <pantheon/dsp/Biquad.h>
#include <pantheon/dsp/MusicBands.h>

// The whole stereo imager: no GUI, no JUCE. Nothing is allocated in process().
namespace castor
{
inline constexpr int kBands = pdsp::bands::kCount;
inline constexpr int kCrossovers = pdsp::bands::kNumCrossovers;
inline constexpr float kMaxWidth = 2.0f;     // 200%
inline constexpr float kMonoOffHz = 20.0f;   // MONO BELOW at or under this is off

struct EngineParams
{
    std::array<float, kBands> width { 1, 1, 1, 1, 1, 1 }; // the side of each band, as a share of what it is (1 = as it is)
    float midDb = 0.0f, sideDb = 0.0f;                     // the centre and the sides, after the bands
    float monoBelowHz = kMonoOffHz;                        // below this the sides are taken away: the bass is mono
    float balance = 0.0f;                                  // -1 (left only) .. +1 (right only)
    float outDb = 0.0f;
};

class Engine
{
public:
    void prepare (double sampleRate)
    {
        fs = sampleRate;
        splitter.prepare (fs);
        corrCoef = 1.0 - std::exp (-1.0 / (0.3 * fs));
        reset();
    }

    void reset() noexcept
    {
        splitter.reset();
        mono.reset();
        engaged = 0.0;
        monoFor = -1.0;
        started = false;
        ell = err = elr = 0.0;
    }

    // In place. Everything that is not a number is silence.
    void process (float* left, float* right, int n, const EngineParams& p) noexcept
    {
        const bool monoOn = p.monoBelowHz > kMonoOffHz + 0.5f;
        if (monoOn && p.monoBelowHz != monoFor)
        {
            monoFor = p.monoBelowHz;
            mono.setup (pdsp::Butterworth::Type::highpass, 4, fs, std::clamp ((double) p.monoBelowHz, 20.0, 0.45 * fs));
        }
        std::array<double, kBands> widthTarget;
        for (int b = 0; b < kBands; ++b)
            widthTarget[(size_t) b] = std::clamp ((double) p.width[(size_t) b], 0.0, (double) kMaxWidth);
        const double midT = std::pow (10.0, std::clamp (p.midDb, -24.0f, 24.0f) / 20.0), sideT = std::pow (10.0, std::clamp (p.sideDb, -24.0f, 24.0f) / 20.0);
        const double leftT = p.balance > 0.0f ? 1.0 - (double) std::min (p.balance, 1.0f) : 1.0, rightT = p.balance < 0.0f ? 1.0 + (double) std::max (p.balance, -1.0f) : 1.0;
        const double outT = std::pow (10.0, std::clamp (p.outDb, -24.0f, 24.0f) / 20.0);
        if (! started)
        {
            width = widthTarget;
            midGain = midT; sideGain = sideT; leftGain = leftT; rightGain = rightT; outGain = outT;
            started = true;
        }
        const double smooth = 1.0 - std::exp (-1.0 / (0.01 * fs));
        bool wide = false; // some width is not 100%
        for (int b = 0; b < kBands; ++b)
            wide = wide || std::fabs (widthTarget[(size_t) b] - 1.0) > 1e-9 || std::fabs (width[(size_t) b] - 1.0) > 1e-9;
        const double engagedTarget = wide ? 1.0 : 0.0;

        for (int i = 0; i < n; ++i)
        {
            for (int b = 0; b < kBands; ++b)
            {
                width[(size_t) b] += (widthTarget[(size_t) b] - width[(size_t) b]) * smooth;
                if (std::fabs (widthTarget[(size_t) b] - width[(size_t) b]) < 1e-9)
                    width[(size_t) b] = widthTarget[(size_t) b]; // at rest exactly: 100% is exactly 100%
            }
            engaged += (engagedTarget - engaged) * smooth;
            if (std::fabs (engagedTarget - engaged) < 1e-9)
                engaged = engagedTarget;
            midGain += (midT - midGain) * smooth;
            sideGain += (sideT - sideGain) * smooth;
            leftGain += (leftT - leftGain) * smooth;
            rightGain += (rightT - rightGain) * smooth;
            outGain += (outT - outGain) * smooth;

            const double l0 = std::isfinite (left[i]) ? (double) left[i] : 0.0, r0 = std::isfinite (right[i]) ? (double) right[i] : 0.0;

            // The widths: each channel is split into the six bands of music (they are in phase with each other, so they add up to the sound with its magnitude
            // unchanged); each band's side is scaled, and the bands are added again.
            double l = l0, r = r0;
            splitter.split (0, l0, partL);
            splitter.split (1, r0, partR);
            double wl = 0.0, wr = 0.0;
            for (int b = 0; b < kBands; ++b)
            {
                const double mb = 0.5 * (partL[(size_t) b] + partR[(size_t) b]), sb = 0.5 * (partL[(size_t) b] - partR[(size_t) b]) * width[(size_t) b];
                wl += mb + sb;
                wr += mb - sb;
            }
            // The split has its own phase (the all-pass of the crossovers), so with every width at 100% the plug-in does not use it: the sound goes through as it is. The two
            // are crossfaded over 10 ms when a width leaves 100% or comes back.
            if (engaged > 0.0)
            {
                l = l0 + engaged * (wl - l0);
                r = r0 + engaged * (wr - r0);
            }
            const double m = 0.5 * (l + r), s = 0.5 * (l - r);
            double dS = 0.0;
            dS += (sideGain - 1.0) * s;
            if (monoOn)
                dS = mono.process (s + dS) - s; // the sides through a high-pass: their bass is taken away, so the bass is mono
            else
                mono.reset();
            const double dM = (midGain - 1.0) * m;

            double yl = l + dM + dS, yr = r + dM - dS;
            yl *= leftGain * outGain;
            yr *= rightGain * outGain;
            left[i] = (float) yl;
            right[i] = (float) yr;

            ell += corrCoef * (yl * yl - ell);
            err += corrCoef * (yr * yr - err);
            elr += corrCoef * (yl * yr - elr);
        }
        const double denom = std::sqrt (ell * err);
        correlation.store (denom > 1e-12 ? (float) std::clamp (elr / denom, -1.0, 1.0) : 1.0f, std::memory_order_relaxed);
    }

    std::atomic<float> correlation { 1.0f }; // of the output, over 0.3 s: +1 mono, 0 wide, -1 out of phase

private:
    double fs = 48000.0, corrCoef = 0.0001, midGain = 1.0, sideGain = 1.0, leftGain = 1.0, rightGain = 1.0, outGain = 1.0, monoFor = -1.0;
    double ell = 0.0, err = 0.0, elr = 0.0;
    bool started = false;
    std::array<double, kBands> width { 1, 1, 1, 1, 1, 1 };
    pdsp::BandSplitter splitter;
    std::array<double, kBands> partL {}, partR {};
    double engaged = 0.0;
    pdsp::Butterworth mono;
};
} // namespace castor
