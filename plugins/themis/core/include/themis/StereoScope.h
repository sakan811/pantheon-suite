// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 The Pantheon authors

#pragma once

#include "Common.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>

namespace themis
{
// Data source for a vectorscope (goniometer) and a phase-correlation meter.
//
// push() runs on the audio thread: it copies the raw L/R samples into a lock-free ring and updates a smoothed
// correlation estimate (a few multiply-adds per sample, no allocation, nothing written to the audio). UI
// threads call copyRecent() / correlation(). The ring is a fixed-size member, so prepare() never reallocates
// anything a reader could be looking at.
//
// A reader copies the newest samples within microseconds; the writer would need to wrap the whole ring
// (kRingSize samples, ~340 ms at 48 kHz) to touch what is being read, so the copy is not guarded further.
class StereoScope
{
public:
    static constexpr size_t kRingSize = 1 << 14; // power of two: indices wrap with a mask
    static constexpr double kCorrelationSeconds = 0.4;

    void prepare (double sampleRate)
    {
        smoothing = emaWeight (1.0 / std::max (sampleRate, 1.0), kCorrelationSeconds);
        requestReset();
    }

    // Safe from any thread; applied by the next push().
    void requestReset() { resetFlag.store (true, std::memory_order_release); }

    // Audio thread.
    void push (const float* l, const float* r, int n)
    {
        if (resetFlag.exchange (false, std::memory_order_acq_rel))
        {
            sumLL = sumRR = sumLR = 0.0;
            correlationValue.store (0.0f, std::memory_order_relaxed);
            valid.store (false, std::memory_order_relaxed);
        }

        const uint64_t w = writeCount.load (std::memory_order_relaxed);
        for (int i = 0; i < n; ++i)
        {
            const size_t pos = static_cast<size_t> ((w + static_cast<uint64_t> (i)) & (kRingSize - 1));
            const float xl = std::isfinite (l[i]) ? l[i] : 0.0f, xr = std::isfinite (r[i]) ? r[i] : 0.0f; // a glitched sample is read as silence, or the correlation would freeze at NaN
            ringL[pos] = xl;
            ringR[pos] = xr;
            const double a = xl, b = xr;
            sumLL += smoothing * (a * a - sumLL);
            sumRR += smoothing * (b * b - sumRR);
            sumLR += smoothing * (a * b - sumLR);
        }
        writeCount.store (w + static_cast<uint64_t> (n), std::memory_order_release);

        // Normalised cross-correlation of the two channels; a silent signal keeps the last value.
        const double den = std::sqrt (sumLL * sumRR);
        if (den > 1.0e-10)
        {
            correlationValue.store (static_cast<float> (std::clamp (sumLR / den, -1.0, 1.0)), std::memory_order_relaxed);
            valid.store (true, std::memory_order_relaxed);
        }
    }

    // Increases whenever audio has been pushed; lets a UI skip repaints while the transport is stopped.
    uint64_t sequence() const { return writeCount.load (std::memory_order_acquire); }

    // True once a correlation value has been measured since the last reset.
    bool hasData() const { return valid.load (std::memory_order_relaxed); }

    // Smoothed inter-channel correlation: +1 mono, 0 uncorrelated / wide, -1 out of phase.
    float correlation() const { return correlationValue.load (std::memory_order_relaxed); }

    // Copies the newest n samples (oldest first) of each channel; returns how many were available.
    int copyRecent (float* l, float* r, int n) const
    {
        const uint64_t w = writeCount.load (std::memory_order_acquire);
        const int count = static_cast<int> (std::min<uint64_t> ({ static_cast<uint64_t> (std::max (n, 0)), w, kRingSize / 2 }));
        for (int i = 0; i < count; ++i)
        {
            const size_t pos = static_cast<size_t> ((w - static_cast<uint64_t> (count - i)) & (kRingSize - 1));
            l[i] = ringL[pos];
            r[i] = ringR[pos];
        }
        return count;
    }

    // Goniometer coordinates: vertical = mid, horizontal = side, so mono is a vertical line, left-only
    // points up-left and right-only up-right. A full-scale one-channel signal has radius 1; full-scale mono
    // reaches 1.414 on the vertical axis.
    static void toXY (float l, float r, float& x, float& y)
    {
        constexpr float k = 0.70710678f;
        x = (r - l) * k;
        y = (l + r) * k;
    }

private:
    std::array<float, kRingSize> ringL {}, ringR {};
    std::atomic<uint64_t> writeCount { 0 };
    std::atomic<float> correlationValue { 0.0f };
    std::atomic<bool> valid { false };
    std::atomic<bool> resetFlag { true };
    double smoothing = 1.0e-3;
    double sumLL = 0.0, sumRR = 0.0, sumLR = 0.0;
};
} // namespace themis
