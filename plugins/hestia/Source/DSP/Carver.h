// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 The Pantheon authors

#pragma once

#include "Roles.h"
#include <pantheon/dsp/Biquad.h>
#include <array>
#include <cmath>

namespace hestia
{
// The cut: one peaking bell on each of the thirty bands, in series, minimum phase, no delay. The bells overlap (the third-octave neighbours of a
// bell are still a few tenths of its gain down), so setting each bell to the gain wanted at its own band would cut more than asked wherever
// neighbouring bands are cut too. The target is therefore taken as the curve wanted *at the band centres*, and the bell gains are what produces
// it: `gain = inverse (overlap) x target`, the inverse made once in prepare(). A flat target of -6 dB is -6 dB at every centre, and a single deep
// notch is that deep at its own band and nothing a few bands away. Between centres the curve is what the bells draw: `responseDb` reads it back from
// the coefficients, for the display and the tests.
class Carver
{
public:
    static constexpr double kQ = 3.6;
    static constexpr double kMaxBellDb = 36.0; // what a bell may be asked for, either way, whatever the inverse says

    void prepare (double sampleRate)
    {
        fs = sampleRate;
        // The overlap: the response in dB at centre k of bell j set to -6 dB, per dB of its gain. A band the sample rate cannot reach is left alone.
        constexpr double probe = -6.0;
        std::array<std::array<double, kBands>, kBands> a {};
        for (int j = 0; j < kBands; ++j)
        {
            pdsp::Biquad probeBell;
            if (reachable (j))
                probeBell.setPeak (fs, bandCentreHz (j), kQ, probe);
            for (int k = 0; k < kBands; ++k)
                a[(size_t) k][(size_t) j] = (reachable (j) && reachable (k)) ? 20.0 * std::log10 (probeBell.magnitudeAt (fs, bandCentreHz (k))) / probe : (j == k ? 1.0 : 0.0);
        }
        invert (a);
        for (int c = 0; c < 2; ++c)
            for (auto& b : bell[(size_t) c])
                b = pdsp::Biquad();
        gainDb.fill (0.0);
        active.fill (false);
        reset();
    }

    void reset() noexcept
    {
        for (auto& channel : bell)
            for (auto& b : channel)
                b.reset();
    }

    // The curve wanted at the centres, in dB. Cheap when nothing changed.
    void setTarget (const float* targetDb) noexcept
    {
        for (int j = 0; j < kBands; ++j)
        {
            double g = 0.0;
            for (int k = 0; k < kBands; ++k)
                g += inverse[(size_t) j][(size_t) k] * (double) targetDb[k];
            g = std::fmax (std::fmin (g, kMaxBellDb), -kMaxBellDb);
            const bool none = std::fabs (g) < 0.005;
            if (none)
                g = 0.0;
            if (std::fabs (g - gainDb[(size_t) j]) < (none ? 1e-12 : 0.01))
                continue; // nothing to change (a bell is not redesigned for a hundredth of a dB)
            gainDb[(size_t) j] = g;
            if (! reachable (j))
                continue;
            if (none)
            {
                // No bell: it is not run, so the band is the input exactly. A bell of 0 dB is the identity on paper (b == a), but a compiler that contracts
                // b1*x - a1*y into a fused multiply-add (any build for a machine with FMA) leaves rounding noise in it: so the bell is left out, not run at 0.
                // A bell that is the identity has no state, so it is reset here and starts from rest when it is wanted again.
                bell[0][(size_t) j] = pdsp::Biquad();
                bell[1][(size_t) j] = pdsp::Biquad();
                active[(size_t) j] = false;
                continue;
            }
            active[(size_t) j] = true;
            bell[0][(size_t) j].setPeak (fs, bandCentreHz (j), kQ, g);
            // Both channels get the same coefficients and keep their own state.
            auto& l = bell[0][(size_t) j];
            auto& r = bell[1][(size_t) j];
            r.b0 = l.b0; r.b1 = l.b1; r.b2 = l.b2; r.a1 = l.a1; r.a2 = l.a2;
        }
    }

    inline void process (double& left, double& right) noexcept
    {
        for (int j = 0; j < kBands; ++j)
            if (active[(size_t) j])
            {
                left = bell[0][(size_t) j].process (left);
                right = bell[1][(size_t) j].process (right);
            }
    }

    // The response of the whole cascade at a frequency, in dB.
    double responseDb (double hz) const noexcept
    {
        double m = 1.0;
        for (const auto& b : bell[0])
            m *= b.magnitudeAt (fs, hz);
        return 20.0 * std::log10 (m);
    }

    const std::array<double, kBands>& bellGains() const noexcept { return gainDb; }

private:
    bool reachable (int k) const noexcept { return bandCentreHz (k) < 0.49 * fs; }

    // Gauss-Jordan with partial pivoting.
    void invert (std::array<std::array<double, kBands>, kBands> a)
    {
        for (int i = 0; i < kBands; ++i)
            for (int j = 0; j < kBands; ++j)
                inverse[(size_t) i][(size_t) j] = i == j ? 1.0 : 0.0;
        for (int col = 0; col < kBands; ++col)
        {
            int pivot = col;
            for (int r = col + 1; r < kBands; ++r)
                if (std::fabs (a[(size_t) r][(size_t) col]) > std::fabs (a[(size_t) pivot][(size_t) col]))
                    pivot = r;
            std::swap (a[(size_t) col], a[(size_t) pivot]);
            std::swap (inverse[(size_t) col], inverse[(size_t) pivot]);
            const double d = a[(size_t) col][(size_t) col];
            for (int j = 0; j < kBands; ++j)
            {
                a[(size_t) col][(size_t) j] /= d;
                inverse[(size_t) col][(size_t) j] /= d;
            }
            for (int r = 0; r < kBands; ++r)
            {
                if (r == col)
                    continue;
                const double f = a[(size_t) r][(size_t) col];
                for (int j = 0; j < kBands; ++j)
                {
                    a[(size_t) r][(size_t) j] -= f * a[(size_t) col][(size_t) j];
                    inverse[(size_t) r][(size_t) j] -= f * inverse[(size_t) col][(size_t) j];
                }
            }
        }
    }

    double fs = 48000.0;
    std::array<std::array<pdsp::Biquad, kBands>, 2> bell;
    std::array<std::array<double, kBands>, kBands> inverse {};
    std::array<double, kBands> gainDb {};
    std::array<bool, kBands> active {}; // the bells that are run: those with a gain
};
} // namespace hestia
