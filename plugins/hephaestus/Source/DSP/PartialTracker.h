// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 The Pantheon authors

#pragma once

#include <pantheon/dsp/Biquad.h>
#include <algorithm>
#include <array>
#include <cmath>

namespace hep
{
// Follows the one strongest partial inside a band (the bass note), for the Clean fill: it reports that
// partial's amplitude and phase sample by sample, so the fill can build its harmonics as pure sines
// locked to it instead of running the signal through a waveshaper.
//
// Three parts, none of which touches the audio path (the fill adds, it never replaces):
//
//  PITCH  - a bank of narrow resonators over the band (eight per octave) is read for the strongest one;
//           interpolating the three energies around it gives the partial's frequency to a fraction of a
//           bank step. It only has to be close: the phase comes from the signal, not from this number.
//  PHASE  - one second-order generalised integrator per channel, tuned to that frequency, returns the
//           partial as a pair (v, q) in quadrature. (v, q) / |(v, q)| is the partial's phase as a unit
//           vector, and its k-th power is the k-th harmonic's phase, exactly. A frequency-locked loop on
//           its own was tried first and does not lock to either of two notes, which is why the pitch is
//           measured separately.
//  CONFIDENCE - how much of the band's energy the tracked partial holds. One note: close to 1. Two notes of
//           equal level: about a half. The fill fades out below kGateLow and is fully on above kGateHigh,
//           so where the band holds more than one thing it adds nothing, instead of intermodulation.
template <int maxChannels>
class PartialTracker
{
public:
    static constexpr int maxBank = 64;
    static constexpr double kStepsPerOctave = 8.0;
    static constexpr double kGateLow = 0.5, kGateHigh = 0.8;

    struct Partial { double amp = 0.0, re = 1.0, im = 0.0; }; // amplitude and unit phase vector

    void setup (double sampleRate, double lo, double hi) noexcept
    {
        fs = sampleRate;
        loHz = lo;
        hiHz = hi;
        const double octaves = std::log2 (hi / lo);
        bankSize = std::clamp ((int) std::ceil (octaves * kStepsPerOctave) + 1, 5, maxBank);
        logStep = std::log (hi / lo) / (double) (bankSize - 1);

        for (int j = 0; j < bankSize; ++j)
        {
            const double f = lo * std::exp (logStep * (double) j);
            bank[(size_t) j].setBandpass (fs, f, kBankQ);
            energyCoeff[(size_t) j] = 1.0 - std::exp (-f / (1.5 * fs));
        }

        powerCoeff = 1.0 - std::exp (-1.0 / (0.01 * fs));
        gateCoeff = 1.0 - std::exp (-1.0 / (0.02 * fs));
        hopLength = std::max (8, (int) (fs / 1500.0));
        glide = 1.0 - std::exp (-(double) hopLength / (0.004 * fs));
        retune (std::sqrt (lo * hi) * 2.0 * kPi);
    }

    void reset() noexcept
    {
        for (auto& b : bank) b.reset();
        energy.fill (0.0);
        for (auto& s : slave) s = {};
        master = {};
        bandPower = partialPower = 0.0;
        gateNow = 0.0;
        hop = 0;
        retune (std::sqrt (loHz * hiHz) * 2.0 * kPi);
    }

    // One frame of the band-passed input. Afterwards partial (c) and gate() describe this sample.
    void process (const std::array<double, maxChannels>& b, int nCh) noexcept
    {
        double mono = 0.0;
        for (int c = 0; c < nCh; ++c)
            mono += b[(size_t) c];
        mono /= (double) nCh;

        for (int j = 0; j < bankSize; ++j)
        {
            const double y = bank[(size_t) j].process (mono);
            energy[(size_t) j] += energyCoeff[(size_t) j] * (y * y - energy[(size_t) j]);
        }
        if (++hop >= hopLength)
        {
            hop = 0;
            pickPitch();
        }

        step (master, mono);
        for (int c = 0; c < nCh; ++c)
        {
            step (slave[(size_t) c], b[(size_t) c]);
            const double a = std::hypot (slave[(size_t) c].v, slave[(size_t) c].q);
            partials[(size_t) c] = a > 1.0e-12 ? Partial { a, slave[(size_t) c].v / a, slave[(size_t) c].q / a } : Partial { 0.0, 1.0, 0.0 };
        }

        bandPower += powerCoeff * (mono * mono - bandPower);
        partialPower += powerCoeff * (0.5 * (master.v * master.v + master.q * master.q) - partialPower);
        const double confidence = bandPower > 1.0e-14 ? partialPower / bandPower : 0.0;
        const double x = std::clamp ((confidence - kGateLow) / (kGateHigh - kGateLow), 0.0, 1.0);
        gateNow += gateCoeff * (x * x * (3.0 - 2.0 * x) - gateNow);
    }

    const Partial& partial (int c) const noexcept { return partials[(size_t) c]; }
    double gate() const noexcept { return gateNow; }
    double frequencyHz() const noexcept { return omega / (2.0 * kPi); }

private:
    static constexpr double kPi = 3.14159265358979323846;
    static constexpr double kBankQ = 6.0;
    static constexpr double kSogiDamping = 0.25; // k of the integrator: bandwidth k * f. Narrower rejects a neighbour better but is slower to follow level

    struct Sogi { double v = 0.0, q = 0.0; };

    // Semi-implicit Euler: the pair rotates at omega without growing or shrinking, and q lags v by 90 degrees.
    void step (Sogi& s, double in) const noexcept
    {
        s.v += dt * omega * (kSogiDamping * (in - s.v) - s.q);
        s.q += dt * omega * s.v;
    }

    void retune (double w) noexcept
    {
        omega = w;
        dt = 1.0 / fs;
    }

    void pickPitch() noexcept
    {
        int best = 0;
        for (int j = 1; j < bankSize; ++j)
            if (energy[(size_t) j] > energy[(size_t) best])
                best = j;
        if (energy[(size_t) best] < 1.0e-14)
            return; // nothing there: hold the last pitch

        double offset = 0.0;
        if (best > 0 && best < bankSize - 1)
        {
            const double lm = std::log (energy[(size_t) best - 1] + 1.0e-30), l0 = std::log (energy[(size_t) best] + 1.0e-30),
                         lp = std::log (energy[(size_t) best + 1] + 1.0e-30);
            const double curve = lm - 2.0 * l0 + lp;
            if (curve < -1.0e-9)
                offset = std::clamp (0.5 * (lm - lp) / curve, -0.5, 0.5);
        }
        const double f = std::clamp (loHz * std::exp (logStep * ((double) best + offset)), loHz, hiHz);
        omega += glide * (2.0 * kPi * f - omega);
    }

    double fs = 48000.0, loHz = 40.0, hiHz = 120.0, logStep = 0.1, dt = 1.0 / 48000.0, omega = 2.0 * kPi * 70.0;
    double powerCoeff = 0.0, gateCoeff = 0.0, glide = 0.0, bandPower = 0.0, partialPower = 0.0, gateNow = 0.0;
    int bankSize = 5, hop = 0, hopLength = 32;

    std::array<pdsp::Biquad, maxBank> bank;
    std::array<double, maxBank> energy {}, energyCoeff {};
    std::array<Sogi, maxChannels> slave {};
    std::array<Partial, maxChannels> partials {};
    Sogi master;
};
}
