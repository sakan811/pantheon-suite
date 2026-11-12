// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 The Pantheon authors

#pragma once

#include "Roles.h"
#include <pantheon/dsp/Biquad.h>
#include <cmath>

namespace hestia
{
// The level of a signal in each of the thirty bands, measured in the time domain: a fourth-order band-pass (two sections, Q 4.3 each: a third of an
// octave wide) per band, and the mean square of its output followed by a one-pole of 50 ms. No FFT, so there is no block, no latency and nothing that
// depends on the host's buffer size. Read as the peak of a sine that fills the band: a sine of amplitude A on a band's centre reads 20 log10 A.
class LevelBank
{
public:
    void prepare (double sampleRate)
    {
        fs = sampleRate;
        smooth = 1.0 - std::exp (-1.0 / (0.05 * fs));
        for (int k = 0; k < kBands; ++k)
        {
            usable[(size_t) k] = bandCentreHz (k) < 0.49 * fs;
            for (auto& s : stage[(size_t) k])
                s.setBandpass (fs, std::fmin (bandCentreHz (k), 0.49 * fs), 4.3);
        }
        reset();
    }

    void reset() noexcept
    {
        for (int k = 0; k < kBands; ++k)
        {
            stage[(size_t) k][0].reset();
            stage[(size_t) k][1].reset();
            meanSquare[(size_t) k] = 0.0;
        }
    }

    inline void push (double x) noexcept
    {
        for (int k = 0; k < kBands; ++k)
        {
            if (! usable[(size_t) k])
                continue;
            const double y = stage[(size_t) k][1].process (stage[(size_t) k][0].process (x));
            meanSquare[(size_t) k] += smooth * (y * y - meanSquare[(size_t) k]);
        }
    }

    void read (float* levelDb) noexcept
    {
        for (int k = 0; k < kBands; ++k)
        {
            double& e = meanSquare[(size_t) k];
            if (e < 1e-30)
                e = 0.0; // no denormals in a tail of silence
            levelDb[k] = e > 1e-12 ? (float) std::fmax (10.0 * std::log10 (2.0 * e), (double) -120.0) : -120.0f;
        }
    }

private:
    double fs = 48000.0, smooth = 0.0;
    std::array<std::array<pdsp::Biquad, 2>, kBands> stage;
    std::array<double, kBands> meanSquare {};
    std::array<bool, kBands> usable {};
};
} // namespace hestia
