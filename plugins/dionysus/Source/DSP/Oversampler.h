// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 The Pantheon authors

#pragma once

#include <array>
#include <cmath>
#include <vector>

// Linear-phase half-band oversampling, by stages of two: plain C++, no JUCE. Every stage is a windowed-sinc half-band filter, so every delay is a whole number of samples
// and the dry sound can be delayed to match exactly.
namespace dionysus
{
inline constexpr int kNumOversampling = 4; // 1x, 2x, 4x, 8x
inline constexpr int kOversamplingFactor[kNumOversampling] = { 1, 2, 4, 8 };

// One doubling: up (one sample in, two out) and down (two in, one out), each with the filter of `taps` (odd) taps. The delay of a round trip is (taps - 1) / 2 samples at the lower rate.
class HalfbandStage
{
public:
    void prepare (int taps)
    {
        n = taps;
        const int m = (taps - 1) / 2;
        h.assign ((size_t) taps, 0.0);
        for (int i = 0; i < taps; ++i)
        {
            const double k = i - m;
            const double sinc = std::fabs (k) < 1e-12 ? 1.0 : std::sin (M_PI * k / 2.0) / (M_PI * k / 2.0);
            const double w = 0.42 + 0.5 * std::cos (M_PI * k / (m + 1.0)) + 0.08 * std::cos (2.0 * M_PI * k / (m + 1.0)); // Blackman
            h[(size_t) i] = 0.5 * sinc * w;
        }
        upHistory.assign ((size_t) taps, 0.0);
        downHistory.assign ((size_t) taps, 0.0);
        reset();
    }

    void reset() noexcept
    {
        std::fill (upHistory.begin(), upHistory.end(), 0.0);
        std::fill (downHistory.begin(), downHistory.end(), 0.0);
        upPos = downPos = 0;
        pending = 0.0;
    }

    // One sample in at the lower rate; two out at the higher (the zero-stuffed signal through the filter, with the gain of 2 that makes up for the zeros).
    inline void up (double x, double& first, double& second) noexcept
    {
        upPos = (upPos + 1) % (size_t) n;
        upHistory[upPos] = x;
        double a = 0.0, b = 0.0;
        for (int k = 0; 2 * k < n; ++k)
            a += h[(size_t) (2 * k)] * upHistory[(upPos + (size_t) n - (size_t) k) % (size_t) n];
        for (int k = 0; 2 * k + 1 < n; ++k)
            b += h[(size_t) (2 * k + 1)] * upHistory[(upPos + (size_t) n - (size_t) k) % (size_t) n];
        first = 2.0 * a;
        second = 2.0 * b;
    }

    // Two samples in at the higher rate, one out at the lower: call with the first (false), then the second (true), which returns the output. The output is the filter's value at the
    // first of the two (the even sample), so that a round trip delays by exactly (taps - 1) / 2 samples of the lower rate.
    inline double down (double x, bool second) noexcept
    {
        downPos = (downPos + 1) % (size_t) n;
        downHistory[downPos] = x;
        if (second)
            return pending;
        double sum = 0.0;
        for (int k = 0; k < n; ++k)
            sum += h[(size_t) k] * downHistory[(downPos + (size_t) n - (size_t) k) % (size_t) n];
        pending = sum;
        return 0.0;
    }

private:
    int n = 33;
    std::vector<double> h, upHistory, downHistory;
    size_t upPos = 0, downPos = 0;
    double pending = 0.0;
};

// The chain for a factor: stage 0 is the widest (65 taps), the rest 33. Latency in samples at the lower rate.
inline int stageTaps (int stage) noexcept { return stage == 0 ? 65 : 33; }
inline int oversamplingLatency (int index) noexcept
{
    int total = 0;
    for (int s = 0; s < index; ++s)
        total += (stageTaps (s) - 1) / 2 >> s; // a stage at the rate 2^s: its delay in samples of that rate, in samples of the first
    return total;
}

// One channel: `process` takes one sample, runs `curve` on every sample at the highest rate, and returns the sample at the original rate.
class Oversampler
{
public:
    void prepare (int index)
    {
        stages = index;
        for (int s = 0; s < index; ++s)
            stage[(size_t) s].prepare (stageTaps (s));
    }

    void reset() noexcept
    {
        for (auto& s : stage)
            s.reset();
    }

    template <class Curve>
    inline double process (double x, Curve&& curve) noexcept
    {
        if (stages == 0)
            return curve (x);
        return run (0, x, curve);
    }

private:
    template <class Curve>
    inline double run (int s, double x, Curve& curve) noexcept
    {
        double a, b;
        stage[(size_t) s].up (x, a, b);
        double ya, yb;
        if (s + 1 == stages)
        {
            ya = curve (a);
            yb = curve (b);
        }
        else
        {
            ya = run (s + 1, a, curve);
            yb = run (s + 1, b, curve);
        }
        stage[(size_t) s].down (ya, false);
        return stage[(size_t) s].down (yb, true);
    }

    int stages = 0;
    std::array<HalfbandStage, 3> stage;
};
} // namespace dionysus
