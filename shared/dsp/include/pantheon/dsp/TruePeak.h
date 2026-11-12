// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 The Pantheon authors

#pragma once

#include <algorithm>
#include <array>
#include <cmath>

// The true peak of a stream: the level it reaches between its samples, as a converter or a codec will reconstruct it. Plain C++, no JUCE; used by Aegis (the limiter's detector) and
// Hermes (the true-peak meter). Both want the same thing, so it is read the same way.
namespace pdsp
{
inline constexpr int kTruePeakTaps = 64;                                // the interpolator: sixty-four taps for each of the points between two samples
inline constexpr int kTruePeakRatio = 8;                                // 8x: seven points between two samples (a 4x reader can read a peak 0.7 dB low; an 8x one 0.2 dB)
inline constexpr int kTruePeakCentre = kTruePeakTaps / 2 - 1;           // the history slot the interpolation is centred on (the newest is kTruePeakTaps - 1)
inline constexpr int kTruePeakDelay = kTruePeakTaps - 1 - kTruePeakCentre; // samples the reader reports behind the newest input

// The newest kTruePeakTaps samples, and seven interpolated points between the two in the middle (a 64-point Hann-windowed sinc). The true peak is the largest of those and the samples themselves
// (BS.1770 reads it at 4x at least; a 12-tap reader missed 1.5 dB on noise to Nyquist, 32 taps 0.7 dB, 64 taps 0.01 dB). The peak it reports is kTruePeakDelay samples behind the input.
class TruePeakReader
{
public:
    TruePeakReader()
    {
        for (int phase = 0; phase < kTruePeakRatio - 1; ++phase)
            for (int j = 0; j < kTruePeakTaps; ++j)
            {
                const double t = (double) (phase + 1) / (double) kTruePeakRatio, x = (double) kTruePeakCentre + t - (double) j; // the distance from the tap to the point
                const double sinc = std::fabs (x) < 1e-12 ? 1.0 : std::sin (M_PI * x) / (M_PI * x);
                const double window = 0.5 + 0.5 * std::cos (M_PI * x / (0.5 * kTruePeakTaps + 0.5)); // Hann, one more sample wide than the taps
                kernel[(size_t) phase][(size_t) j] = sinc * window;
            }
        reset();
    }

    void reset() noexcept { history.fill (0.0); }

    // Push a sample; returns the peak around the middle of the history. `between`: read the points between the samples too.
    inline double push (double x, bool between = true) noexcept
    {
        for (int j = 0; j < kTruePeakTaps - 1; ++j)
            history[(size_t) j] = history[(size_t) (j + 1)];
        history[kTruePeakTaps - 1] = x;
        double peak = std::fabs (history[kTruePeakCentre]);
        if (between)
            for (const auto& k : kernel)
            {
                double sum = 0.0;
                for (int j = 0; j < kTruePeakTaps; ++j)
                    sum += k[(size_t) j] * history[(size_t) j];
                peak = std::fmax (peak, std::fabs (sum));
            }
        return peak;
    }

private:
    std::array<std::array<double, kTruePeakTaps>, kTruePeakRatio - 1> kernel {};
    std::array<double, kTruePeakTaps> history {};
};
} // namespace pdsp
