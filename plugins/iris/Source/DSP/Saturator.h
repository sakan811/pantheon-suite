// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 The Pantheon authors

#pragma once

#include "Band.h"

namespace iris
{
// The SAT mode's shaper: what a boosting band adds is pushed through a soft curve, so a boost that gets loud warms up and
// rounds off instead of staying perfectly linear. It works on the *residual* of the curve (the curve minus its straight
// line), with first-order antiderivative anti-aliasing, and the residual is added to the untouched band output. That keeps
// small signals exactly linear (the residual is cubic) and keeps the anti-aliasing from low-passing the wanted signal.
// Runs in double; the tube curve is asymmetric (even harmonics), so its residual is DC-blocked.
class Shaper
{
public:
    void reset (SatStyle style) noexcept
    {
        x1 = 0.0;
        G1 = antiderivative (style, 0.0);
        dcX = dcY = 0.0;
    }

    void setSampleRate (double fs) noexcept { dcR = 1.0 - 2.0 * M_PI * 8.0 / fs; }

    // The residual (shaped minus unshaped) for input `x` driven by `drive` (linear gain).
    inline double residual (SatStyle style, double x, double drive) noexcept
    {
        const double u = x * drive;
        const double G = antiderivative (style, u);
        const double du = u - x1;
        const double g = std::fabs (du) < 1.0e-4 ? curve (style, 0.5 * (u + x1)) - 0.5 * (u + x1)
                                                 : (G - G1) / du;
        x1 = u;
        G1 = G;

        const double r = g / drive;
        const double y = r - dcX + dcR * dcY; // one-pole DC blocker
        dcX = r;
        dcY = y;
        return y;
    }

    // f(u): the curve; every one has f'(0) = 1.
    static double curve (SatStyle style, double u) noexcept
    {
        switch (style)
        {
            case SatStyle::tape:    return std::tanh (u);
            case SatStyle::tube:    return (std::tanh (u + tubeBias) - tubeTanh) / tubeNorm;
            case SatStyle::console: return std::atan (u);
        }
        return u;
    }

private:
    static constexpr double tubeBias = 0.35;
    static inline const double tubeTanh = std::tanh (tubeBias);
    static inline const double tubeNorm = 1.0 - std::tanh (tubeBias) * std::tanh (tubeBias);

    static double logCosh (double x) noexcept
    {
        const double a = std::fabs (x);
        return a + std::log1p (std::exp (-2.0 * a)) - 0.6931471805599453;
    }

    // G(u): the antiderivative of f(u) - u.
    static double antiderivative (SatStyle style, double u) noexcept
    {
        const double lin = 0.5 * u * u;
        switch (style)
        {
            case SatStyle::tape:    return logCosh (u) - lin;
            case SatStyle::tube:    return (logCosh (u + tubeBias) - u * tubeTanh) / tubeNorm - lin;
            case SatStyle::console: return u * std::atan (u) - 0.5 * std::log1p (u * u) - lin;
        }
        return 0.0;
    }

    double x1 = 0.0, G1 = 0.0, dcX = 0.0, dcY = 0.0, dcR = 0.9990;
};
} // namespace iris
