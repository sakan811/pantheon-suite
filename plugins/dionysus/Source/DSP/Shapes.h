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

// The antiderivative F of each curve (F' = shape (s, x)), for the ADAA of the 1x path. Each one is checked against its curve by DionysusTests [10].
// ln cosh is written so that it does not overflow for a large input.
inline double lnCosh (double x) noexcept
{
    const double a = std::fabs (x);
    return a + std::log1p (std::exp (-2.0 * a)) - std::log (2.0);
}

inline double shapeIntegral (Shape s, double x) noexcept
{
    switch (s)
    {
        case Shape::tape: return lnCosh (x);
        case Shape::tube: return std::fabs (x) - std::log1p (std::fabs (x));
        case Shape::transformer: return std::fabs (x) < 1.0 ? x * x / 2.0 - x * x * x * x / 12.0 : 2.0 / 3.0 * std::fabs (x) - 0.25;
        // The negative side is 0.7 tanh (x / 0.7). Its antiderivative is 0.49 ln cosh (x / 0.7): the chain rule divides by 0.7 and the 0.49 leaves 0.7 (test [10] checks it).
        case Shape::diode: return x >= 0.0 ? lnCosh (x) : 0.49 * lnCosh (x / 0.7);
        case Shape::fold: return -std::cos (x);
        case Shape::clip: return std::fabs (x) <= 1.0 ? x * x / 2.0 : std::fabs (x) - 0.5;
    }
    return x * x / 2.0;
}

// First-order antiderivative anti-aliasing (Parker, Zavalishin, Le Bivic, DAFx 2016): the curve is averaged over each step of its input, not sampled at the sample, which
// keeps the aliasing of a hard curve down at 1x. The state is the previous input (the working point, BIAS included) and the antiderivative there.
struct AdaaState
{
    double uPrev = 0.0, fPrev = 0.0;
    bool primed = false;
};

// Like saturate(), with ADAA: the same BIAS offset is taken off after. A stream that started from silence (the first sample continues from the silence's working point)
// gives exactly 0 for silence, and a step smaller than 1e-5 uses the curve at the midpoint (the limit of the average as the step goes to 0).
inline double saturateAdaa (Shape s, AdaaState& st, double x, double bias) noexcept
{
    const double b = bias * kBiasScale;
    if (! st.primed)
    {
        st.uPrev = b;
        st.fPrev = shapeIntegral (s, b);
        st.primed = true;
    }
    const double u = x + b;
    const double fNow = shapeIntegral (s, u);
    const double du = u - st.uPrev;
    const double y = std::fabs (du) < 1e-5 ? shape (s, 0.5 * (u + st.uPrev)) : (fNow - st.fPrev) / du;
    st.uPrev = u;
    st.fPrev = fNow;
    return b == 0.0 ? y : y - shape (s, b);
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
