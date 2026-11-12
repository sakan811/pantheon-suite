// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 The Pantheon authors

#pragma once

#include <algorithm>
#include <array>
#include <cmath>

// The curves of the saturator and what they do to a sine: plain C++, no GUI, no JUCE. The engine, the editor's curve and harmonic display and the tests all read them from here.
namespace dionysus
{
enum class Shape { tape, tube, transformer, diode, fold, clip };
inline constexpr int kNumShapes = 6;
inline constexpr float kMaxDriveDb = 36.0f;
inline constexpr double kBiasScale = 0.5;       // BIAS 100% moves the working point by this much (in units of the curve)
inline constexpr double kReferenceDb = -12.0;   // the level AUTO GAIN matches the loudness at: a sine at -12 dBFS
inline constexpr int kHarmonics = 9;

// Each curve has a slope of 1 at 0, so a small signal passes unchanged, and bounded output (about 1, the fold's is 1 exactly, a hard clip's is 1).
inline double shape (Shape s, double x) noexcept
{
    switch (s)
    {
        case Shape::tape: return std::tanh (x);
        case Shape::tube: return x / (1.0 + std::fabs (x));
        case Shape::transformer:
            if (x >= 1.0) return 2.0 / 3.0;
            if (x <= -1.0) return -2.0 / 3.0;
            return x - x * x * x / 3.0;
        case Shape::diode: return x >= 0.0 ? std::tanh (x) : 0.7 * std::tanh (x / 0.7);
        case Shape::fold: return std::sin (x);
        case Shape::clip: return std::clamp (x, -1.0, 1.0);
    }
    return x;
}

// The working point: BIAS (-1..1) moves the input before the curve; what the curve gives for that offset alone is taken off after, so silence stays silence.
inline double saturate (Shape s, double x, double bias) noexcept
{
    const double b = bias * kBiasScale;
    return b == 0.0 ? shape (s, x) : shape (s, x + b) - shape (s, b);
}

// What a sine at `levelDb` (peak, dBFS) turns into, per harmonic: the level of harmonic 1 .. kHarmonics in dB relative to the sound that went in, taken from one cycle through
// the curve (no oversampling: the curve itself, not the engine's aliasing).
inline std::array<double, kHarmonics> harmonicsDb (Shape s, double driveDb, double bias, double levelDb = kReferenceDb) noexcept
{
    constexpr int n = 512;
    const double a = std::pow (10.0, levelDb / 20.0), g = std::pow (10.0, driveDb / 20.0);
    std::array<double, kHarmonics> out {};
    for (int h = 1; h <= kHarmonics; ++h)
    {
        double re = 0.0, im = 0.0;
        for (int i = 0; i < n; ++i)
        {
            const double y = saturate (s, g * a * std::sin (2.0 * M_PI * i / n), bias);
            re += y * std::cos (2.0 * M_PI * h * i / n);
            im += y * std::sin (2.0 * M_PI * h * i / n);
        }
        out[(size_t) (h - 1)] = 20.0 * std::log10 (std::fmax (2.0 * std::sqrt (re * re + im * im) / n / a, 1e-9));
    }
    return out;
}

// The gain that brings the loudness of a sine at kReferenceDb back to what it was before the curve (AUTO GAIN): the ratio of the RMS in to the RMS out.
inline double compensation (Shape s, double driveDb, double bias) noexcept
{
    constexpr int n = 256;
    const double a = std::pow (10.0, kReferenceDb / 20.0), g = std::pow (10.0, driveDb / 20.0);
    double in = 0.0, out = 0.0, mean = 0.0;
    for (int i = 0; i < n; ++i)
    {
        const double x = a * std::sin (2.0 * M_PI * i / n), y = saturate (s, g * x, bias);
        in += x * x;
        out += y * y;
        mean += y;
    }
    mean /= n;
    out -= n * mean * mean; // the engine takes the DC away after an asymmetric curve: what is left is the AC
    return out > 1e-18 ? std::sqrt (in / out) : 1.0;
}
} // namespace dionysus
