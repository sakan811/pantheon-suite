// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 The Pantheon authors

#pragma once

#include <array>
#include <cmath>

// Linear-phase half-band oversampling, by stages of two: plain C++, no JUCE. Every stage is a windowed-sinc half-band filter, so every delay is a whole number of samples
// and the dry sound can be delayed to match exactly.
namespace dionysus
{
inline constexpr int kNumOversampling = 4; // 1x, 2x, 4x, 8x
inline constexpr int kOversamplingFactor[kNumOversampling] = { 1, 2, 4, 8 };

// One doubling: up (one sample in, two out) and down (two in, one out), each with the filter of `taps` (odd, and (taps - 1) / 2 even) taps. The delay of a round trip is (taps - 1) / 2
// samples at the lower rate.
//
// The filter is a half-band: every tap at an even distance from the centre is exactly 0 (taps 2, 4, ... of the sinc), the centre is 1/2, and it is symmetric. So the work is a
// fraction of the plain convolution: `up` gives its first output as the input delayed (the centre tap alone) and its second from the odd taps in pairs; `down` is the centre plus
// the odd taps in pairs. No allocation (the largest filter is a fixed array), no modulo (each history is written twice, `taps` apart, so a window is always contiguous).
class HalfbandStage
{
public:
    static constexpr int kMaxTaps = 65;

    void prepare (int taps) noexcept
    {
        n = taps;
        m = (taps - 1) / 2;
        // Tap k of the half-band: 0.5 sinc (k / 2) times a Blackman window. Computed on the distance from the centre so the two halves are the same bit for bit.
        const auto tap = [this] (int d)
        {
            const double sinc = d == 0 ? 1.0 : std::sin (M_PI * d / 2.0) / (M_PI * d / 2.0);
            const double w = 0.42 + 0.5 * std::cos (M_PI * d / (m + 1.0)) + 0.08 * std::cos (2.0 * M_PI * d / (m + 1.0));
            return 0.5 * sinc * w;
        };
        centre = tap (0);
        // Odd taps at the distances 1, 3, 5 ... from the centre; the filter's tap index is k = m +- d, with k odd (m is even).
        pairs = m / 2;
        for (int j = 0; j < pairs; ++j)
            odd[(size_t) j] = tap (2 * j + 1);
        reset();
    }

    void reset() noexcept
    {
        upHistory.fill (0.0);
        downHistory.fill (0.0);
        upPos = downPos = 0;
        pending = 0.0;
    }

    // One sample in at the lower rate; two out at the higher (the zero-stuffed signal through the filter, with the gain of 2 that makes up for the zeros).
    inline void up (double x, double& first, double& second) noexcept
    {
        // Newest sample at upHistory[upPos], older ones at the higher indices: m samples, so the window upHistory[upPos .. upPos + m - 1].
        upPos = upPos == 0 ? (size_t) m - 1 : upPos - 1;
        upHistory[upPos] = upHistory[upPos + (size_t) m] = x;
        const double* w = upHistory.data() + upPos;
        // Output 2n (even taps: only the centre, which sits m / 2 samples back) and 2n + 1 (the odd taps h[2k + 1], k = 0 .. m - 1, which are symmetric: k and m - 1 - k).
        first = 2.0 * centre * w[m / 2];
        double b = 0.0;
        for (int k = 0; k < pairs; ++k)
            b += odd[(size_t) (pairs - 1 - k)] * (w[k] + w[m - 1 - k]); // the pair k is m - 1 - 2k from the centre
        second = 2.0 * b;
    }

    // Two samples in at the higher rate, one out at the lower: call with the first (false), then the second (true), which returns the output. The output is the filter's value at the
    // first of the two (the even sample), so that a round trip delays by exactly (taps - 1) / 2 samples of the lower rate.
    inline double down (double x, bool second) noexcept
    {
        downPos = downPos == 0 ? (size_t) n - 1 : downPos - 1;
        downHistory[downPos] = downHistory[downPos + (size_t) n] = x;
        if (second)
            return pending;
        const double* w = downHistory.data() + downPos; // w[k]: the sample k back
        // The odd taps are at k = m - d and m + d (d odd): the pair index j has d = 2j + 1.
        double sum = centre * w[m];
        for (int j = 0; j < pairs; ++j)
            sum += odd[(size_t) j] * (w[m - 1 - 2 * j] + w[m + 1 + 2 * j]);
        pending = sum;
        return 0.0;
    }

private:
    int n = 33, m = 16, pairs = 8;
    double centre = 0.5;
    std::array<double, kMaxTaps / 4 + 1> odd {};
    std::array<double, 2 * (kMaxTaps / 2)> upHistory {};
    std::array<double, 2 * kMaxTaps> downHistory {};
    size_t upPos = 0, downPos = 0;
    double pending = 0.0;
};

// The chain for a factor: stage 0 is the widest (65 taps), the rest 33. Latency in samples at the lower rate.
inline constexpr int stageTaps (int stage) noexcept { return stage == 0 ? 65 : 33; }
inline constexpr int oversamplingLatency (int index) noexcept
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
