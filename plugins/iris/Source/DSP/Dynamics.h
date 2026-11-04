// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 The Pantheon authors

#pragma once

#include "Band.h"

namespace iris
{
// Measures how loud a band's own frequency region is: a filter that matches the band (band-pass for a bell or notch,
// low-pass for a low shelf, high-pass for a high shelf), then the mean square of what comes out, smoothed over about two
// cycles of the band's own frequency by two one-pole stages (which also takes the ripple at twice the signal frequency out
// of it). It reads as the peak level of a sine of the same power, so "-20 dB" means a sine whose peak is at -20 dBFS.
// Attack and release are not here: they act on the gain (see Engine), where the time they ask for is the time they take.
// It listens to what goes *into* the band, so the band's own boost cannot feed back into its detector.
class Detector
{
public:
    void prepare (double sampleRate) noexcept { fs = sampleRate; reset(); }
    void reset() noexcept { filter.reset(); filter2.reset(); stage1 = stage2 = 0.0; }

    void setFilter (BandType type, double f, double q) noexcept
    {
        f = clampFreq (f, fs);
        k = 1.0 - std::exp (-f / fs); // each stage: a time constant of one cycle
        if (type == BandType::lowShelf)
        {
            filter.setLowpass (fs, f, 0.7071);
            filter2.setLowpass (fs, f, 0.7071);
        }
        else if (type == BandType::highShelf)
        {
            filter.setHighpass (fs, f, 0.7071);
            filter2.setHighpass (fs, f, 0.7071);
        }
        else
        {
            const double dq = std::clamp (q, 0.7, 6.0);
            filter.setBandpass (fs, f, dq);
            filter2.setBandpass (fs, f, dq);
        }
    }

    inline void push (double x) noexcept
    {
        const double y = filter2.process (filter.process (x));
        stage1 += k * (y * y - stage1);
        stage2 += k * (stage1 - stage2);
    }

    double level() const noexcept { return std::sqrt (2.0 * std::max (stage2, 0.0)); }

private:
    double fs = 48000.0, k = 0.02, stage1 = 0.0, stage2 = 0.0;
    pdsp::Biquad filter, filter2; // two in series: a steeper skirt, so a neighbouring band does not trigger this one
};

// The gain computer: how far the dynamics move the band's gain at a given level (dB, signed like `rangeDb`).
// Past the threshold the gain moves by (1 - 1/ratio) dB per dB, through a 6 dB soft knee, and stops at the range.
inline double dynamicOffsetDb (const DynamicsParams& p, double levelDb) noexcept
{
    constexpr double knee = 6.0;
    const double over = p.mode == DynMode::above ? levelDb - p.thresholdDb : p.thresholdDb - levelDb;
    double x = 0.0;
    if (over >= knee * 0.5)        x = over;
    else if (over > -knee * 0.5)   x = (over + knee * 0.5) * (over + knee * 0.5) / (2.0 * knee);

    const double moved = x * (1.0 - 1.0 / std::max (p.ratio, 1.0));
    const double limit = std::fabs (p.rangeDb);
    return std::copysign (std::min (moved, limit), p.rangeDb);
}
} // namespace iris
