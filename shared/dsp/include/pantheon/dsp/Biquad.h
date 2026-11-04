// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 The Pantheon authors

#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>

namespace pdsp
{
// Double-precision RBJ biquad. Low-frequency filters (tens of Hz) put poles very close
// to z = 1, which single precision cannot resolve cleanly, so everything here is double.
struct Biquad
{
    double b0 = 1.0, b1 = 0.0, b2 = 0.0, a1 = 0.0, a2 = 0.0;
    double z1 = 0.0, z2 = 0.0;

    void reset() noexcept { z1 = z2 = 0.0; }

    inline double process (double x) noexcept
    {
        const double y = b0 * x + z1;
        z1 = b1 * x - a1 * y + z2;
        z2 = b2 * x - a2 * y;
        return y;
    }

    void setLowpass (double fs, double f, double q) noexcept
    {
        const double w0 = 2.0 * M_PI * f / fs, c = std::cos (w0), alpha = std::sin (w0) / (2.0 * q);
        set ((1.0 - c) * 0.5, 1.0 - c, (1.0 - c) * 0.5, 1.0 + alpha, -2.0 * c, 1.0 - alpha);
    }

    void setHighpass (double fs, double f, double q) noexcept
    {
        const double w0 = 2.0 * M_PI * f / fs, c = std::cos (w0), alpha = std::sin (w0) / (2.0 * q);
        set ((1.0 + c) * 0.5, -(1.0 + c), (1.0 + c) * 0.5, 1.0 + alpha, -2.0 * c, 1.0 - alpha);
    }

    // Constant 0 dB peak-gain band-pass.
    void setBandpass (double fs, double f, double q) noexcept
    {
        const double w0 = 2.0 * M_PI * f / fs, c = std::cos (w0), alpha = std::sin (w0) / (2.0 * q);
        set (alpha, 0.0, -alpha, 1.0 + alpha, -2.0 * c, 1.0 - alpha);
    }

    // ---- parametric-EQ designs -----------------------------------------------------------------
    // Peaking bell: `gainDb` at the centre, 0 dB far away, bandwidth set by q.
    void setPeak (double fs, double f, double q, double gainDb) noexcept
    {
        const double A = std::pow (10.0, gainDb / 40.0);
        const double w0 = 2.0 * M_PI * f / fs, c = std::cos (w0), alpha = std::sin (w0) / (2.0 * q);
        set (1.0 + alpha * A, -2.0 * c, 1.0 - alpha * A, 1.0 + alpha / A, -2.0 * c, 1.0 - alpha / A);
    }

    // Shelves: `gainDb` well beyond the corner, 0 dB on the other side; q = 0.7071 is the maximally flat shelf.
    void setLowShelf (double fs, double f, double q, double gainDb) noexcept
    {
        const double A = std::pow (10.0, gainDb / 40.0);
        const double w0 = 2.0 * M_PI * f / fs, c = std::cos (w0), alpha = std::sin (w0) / (2.0 * q), r = 2.0 * std::sqrt (A) * alpha;
        set (A * ((A + 1.0) - (A - 1.0) * c + r), 2.0 * A * ((A - 1.0) - (A + 1.0) * c), A * ((A + 1.0) - (A - 1.0) * c - r),
             (A + 1.0) + (A - 1.0) * c + r, -2.0 * ((A - 1.0) + (A + 1.0) * c), (A + 1.0) + (A - 1.0) * c - r);
    }

    void setHighShelf (double fs, double f, double q, double gainDb) noexcept
    {
        const double A = std::pow (10.0, gainDb / 40.0);
        const double w0 = 2.0 * M_PI * f / fs, c = std::cos (w0), alpha = std::sin (w0) / (2.0 * q), r = 2.0 * std::sqrt (A) * alpha;
        set (A * ((A + 1.0) + (A - 1.0) * c + r), -2.0 * A * ((A - 1.0) + (A + 1.0) * c), A * ((A + 1.0) + (A - 1.0) * c - r),
             (A + 1.0) - (A - 1.0) * c + r, 2.0 * ((A - 1.0) - (A + 1.0) * c), (A + 1.0) - (A - 1.0) * c - r);
    }

    void setNotch (double fs, double f, double q) noexcept
    {
        const double w0 = 2.0 * M_PI * f / fs, c = std::cos (w0), alpha = std::sin (w0) / (2.0 * q);
        set (1.0, -2.0 * c, 1.0, 1.0 + alpha, -2.0 * c, 1.0 - alpha);
    }

    // |H| at a frequency, from the coefficients (for drawing a curve and for tests).
    double magnitudeAt (double fs, double f) const noexcept
    {
        const double w = 2.0 * M_PI * f / fs;
        const std::complex<double> z1i = std::polar (1.0, -w), z2i = std::polar (1.0, -2.0 * w);
        return std::abs ((b0 + b1 * z1i + b2 * z2i) / (1.0 + a1 * z1i + a2 * z2i));
    }

    void set (double nb0, double nb1, double nb2, double a0, double na1, double na2) noexcept
    {
        b0 = nb0 / a0; b1 = nb1 / a0; b2 = nb2 / a0; a1 = na1 / a0; a2 = na2 / a0;
    }
};

// Butterworth low/high-pass of even order 2..8 (one to four cascaded biquads): 12 to 48 dB per octave.
class Butterworth
{
public:
    enum class Type { lowpass, highpass };
    static constexpr int maxSections = 4;

    void setup (Type t, int order, double fs, double freq) noexcept
    {
        sections = std::clamp (order / 2, 1, maxSections);
        freq = std::fmin (std::fmax (freq, 1.0), fs * 0.45);

        for (int i = 0; i < sections; ++i)
        {
            // The poles of an order-2n Butterworth sit at angles (2k+1) pi / (4n): each pair is a biquad with this Q.
            const double q = 1.0 / (2.0 * std::cos (M_PI * (2.0 * i + 1.0) / (4.0 * sections)));
            if (t == Type::lowpass) bq[(size_t) i].setLowpass (fs, freq, q);
            else                    bq[(size_t) i].setHighpass (fs, freq, q);
        }
    }

    void reset() noexcept { for (auto& b : bq) b.reset(); }

    inline double process (double x) noexcept
    {
        for (int i = 0; i < sections; ++i)
            x = bq[(size_t) i].process (x);
        return x;
    }

    double magnitudeAt (double fs, double f) const noexcept
    {
        double m = 1.0;
        for (int i = 0; i < sections; ++i)
            m *= bq[(size_t) i].magnitudeAt (fs, f);
        return m;
    }

private:
    std::array<Biquad, maxSections> bq;
    int sections = 1;
};

// High-pass + low-pass in series; the building block for every "range" in the fundamental engine.
struct Bandpass
{
    Butterworth hp, lp;

    void setup (double fs, int hpOrder, double hpHz, int lpOrder, double lpHz) noexcept
    {
        hp.setup (Butterworth::Type::highpass, hpOrder, fs, hpHz);
        lp.setup (Butterworth::Type::lowpass, lpOrder, fs, lpHz);
    }

    void reset() noexcept { hp.reset(); lp.reset(); }
    inline double process (double x) noexcept { return lp.process (hp.process (x)); }
};
}
