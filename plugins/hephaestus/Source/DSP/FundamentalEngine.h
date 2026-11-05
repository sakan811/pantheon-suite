// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 The Pantheon authors

#pragma once

#include "Band.h"
#include <pantheon/dsp/Biquad.h>
#include <algorithm>
#include <array>
#include <cmath>

namespace hep
{
// Relative levels of H2..H5 for the Tone control, blended in dB between three anchors (dark: H2
// with a trace of H3; mid: the classic virtual-bass roll-off; bright: H3 leads, H4/H5 present) and
// normalised so the four weights carry unit total energy. That makes Amount "how much harmonic
// energy, relative to the band" whatever the tone.
inline void harmonicsForTone (double tone, double (&w)[4]) noexcept
{
    static constexpr double dark[4] = { 0.0, -15.0, -36.0, -48.0 };
    static constexpr double mid[4] = { 0.0, -3.0, -9.0, -16.0 };
    static constexpr double bright[4] = { -6.0, 0.0, -3.0, -6.0 };

    const double t = std::clamp (tone, 0.0, 1.0);
    double energy = 0.0;
    for (int k = 0; k < 4; ++k)
    {
        const double db = t < 0.5 ? dark[k] + (mid[k] - dark[k]) * (t * 2.0)
                                  : mid[k] + (bright[k] - mid[k]) * ((t - 0.5) * 2.0);
        w[k] = std::pow (10.0, db / 20.0);
        energy += w[k] * w[k];
    }
    for (auto& v : w)
        v /= std::sqrt (energy);
}

// Attack/release peak follower.
struct EnvelopeFollower
{
    double y = 0.0, aA = 0.0, aR = 0.0;

    void setTimes (double fs, double attackSec, double releaseSec) noexcept
    {
        aA = std::exp (-1.0 / (attackSec * fs));
        aR = std::exp (-1.0 / (releaseSec * fs));
    }

    void reset() noexcept { y = 0.0; }

    inline double process (double x) noexcept
    {
        const double a = x > y ? aA : aR;
        y = a * y + (1.0 - a) * x;
        return y;
    }
};

// Answers "how much of the restored fundamental is already present in the input?", per frequency.
//
// A bank of narrow resonators is spread over the bass range; for each one the energy of the input
// and of the restored signal is tracked, and the result is the restored-energy-weighted fraction of
// the restored amplitude that the input already supplies (0 = a complete gap, 1 = fully present).
// Comparing against the whole bass band instead would mistake a 2nd harmonic for an existing
// fundamental, and correlating with the restored signal fails because a difference tone is in phase
// quadrature with the true fundamental of e.g. a saw-like waveform.
template <int maxChannels>
class GapDetector
{
public:
    static constexpr int numBands = 8;

    void setup (double fs, double lo, double hi) noexcept
    {
        energyCoeff = 1.0 - std::exp (-1.0 / (0.04 * fs));
        for (int k = 0; k < numBands; ++k)
        {
            const double f = lo * std::pow (hi / lo, (double) k / (numBands - 1));
            for (size_t c = 0; c < (size_t) maxChannels; ++c)
            {
                existing[c][(size_t) k].setBandpass (fs, f, 4.0);
                restored[c][(size_t) k].setBandpass (fs, f, 4.0);
            }
        }
    }

    void reset() noexcept
    {
        for (auto& ch : existing) for (auto& b : ch) b.reset();
        for (auto& ch : restored) for (auto& b : ch) b.reset();
        energyExisting.fill (0.0);
        energyRestored.fill (0.0);
    }

    double process (const std::array<double, maxChannels>& inputBand,
                    const std::array<double, maxChannels>& restoredSignal, int nCh) noexcept
    {
        for (int k = 0; k < numBands; ++k)
        {
            double eIn = 0.0, eRes = 0.0;
            for (int c = 0; c < nCh; ++c)
            {
                const double a = existing[(size_t) c][(size_t) k].process (inputBand[(size_t) c]);
                const double d = restored[(size_t) c][(size_t) k].process (restoredSignal[(size_t) c]);
                eIn += a * a;
                eRes += d * d;
            }
            energyExisting[(size_t) k] += energyCoeff * (eIn - energyExisting[(size_t) k]);
            energyRestored[(size_t) k] += energyCoeff * (eRes - energyRestored[(size_t) k]);
        }

        double weighted = 0.0, total = 1.0e-12;
        for (int k = 0; k < numBands; ++k)
        {
            const double er = energyRestored[(size_t) k];
            weighted += er * std::min (1.0, std::sqrt (energyExisting[(size_t) k] / (er + 1.0e-12)));
            total += er;
        }
        return weighted / total;
    }

private:
    std::array<std::array<pdsp::Biquad, numBands>, maxChannels> existing, restored;
    std::array<double, numBands> energyExisting {}, energyRestored {};
    double energyCoeff = 0.0;
};

// Low-frequency "fill" engine. Two independent generators feed one additive output:
//
//  EXTEND  - virtual bass / missing-fundamental effect. The bass range is band-passed,
//            divided by its own envelope, run through Chebyshev polynomials T2..T5, and
//            multiplied by the envelope again. For a sine of unit amplitude, T_k produces
//            exactly the k-th harmonic, so each harmonic gets an exact, level-independent
//            weight relative to the fundamental (the precision of a phase vocoder), while the
//            time-domain structure keeps transients intact (the strength of a plain NLD).
//
//  RESTORE - physically regenerates a fundamental that is absent while its harmonics are
//            present. Squaring a signal containing nf and (n+1)f yields their difference
//            tone f. The squared signal is normalised by its envelope (a raw squarer would
//            expand level 2:1), band-limited to the target range, and ducked by the amount
//            of fundamental that is already there (see GapDetector) so it only fills gaps.
//
// ADAPT   - closes the loop on EXTEND. The energy of the band itself is compared with the energy
//           already present just above it (harmonicReach); only the shortfall to the requested
//           amount is generated, so the harmonic-to-fundamental balance holds from one moment to
//           the next instead of piling harmonics onto passages that already have them. The
//           measurement is made on the input, so there is no feedback to destabilise.
//
// Everything runs at the host sample rate: a fill band's range stops at fillMaxHz, so its
// harmonics stay clear of Nyquist (the 5th is dropped if it would not) and there is nothing to
// alias and no need to oversample.
class FundamentalEngine
{
public:
    static constexpr int maxChannels = 2;
    using Frame = std::array<double, maxChannels>;

    struct Params
    {
        double lo = 40.0, hi = 120.0;        // target range of the fundamental, Hz
        double extGain = 0.0;                // linear; 0 disables the EXTEND path
        double harmonic[4] = { 0, 0, 0, 0 }; // linear weights of H2..H5 relative to the fundamental
        double ceiling = 1200.0;             // low-pass applied to the generated harmonics, Hz
        double resGain = 0.0;                // linear; 0 disables the RESTORE path
        double fill = 0.7;                   // 0..1 how strongly an existing fundamental ducks RESTORE
        double target = 0.0;                 // energy ratio (harmonic reach : band) ADAPT aims to keep
        double adapt = 0.0;                  // 0 = EXTEND at its fixed level, 1 = generate only the shortfall to `target`
    };

    void prepare (double sampleRate)
    {
        fs = sampleRate;
        lastLo = lastHi = lastCeiling = -1.0;
        reset();
    }

    void reset() noexcept
    {
        for (size_t c = 0; c < (size_t) maxChannels; ++c)
            for (auto* b : { &band[c], &ext[c], &restoreIn[c], &restoreOut[c] })
                b->reset();

        for (auto& r : region)
            r.reset();

        envBand.reset();
        envRestore.reset();
        gaps.reset();
        duck = 1.0;
        energyBand = energyReach = 0.0;
        adaptGain = 1.0;
        curLo = curHi = -1.0; // the next call snaps to the requested range instead of gliding there
    }

    // Current level of the adaptive fill (1 = full amount, 0 = the reach above the band is already full).
    double currentAdaptGain() const noexcept { return adaptGain; }

    // What the adaptive fill measures, as a level: the energy already above the band (harmonicReach) relative to the
    // band's own, in dB (the scale Amount is set on). Below kReachFloorDb, or with a silent band, it reads kReachFloorDb.
    static constexpr double kReachFloorDb = -60.0;
    double currentReachDb() const noexcept
    {
        if (energyBand < 1.0e-12)
            return kReachFloorDb;
        return std::max (kReachFloorDb, 10.0 * std::log10 (energyReach / energyBand + 1.0e-30));
    }

    // in: nCh read pointers; out: nCh write pointers that receive the additive signal.
    void process (const float* const* in, float* const* out, int nCh, int n, const Params& p) noexcept
    {
        updateCoefficients (p);
        nCh = std::min (nCh, maxChannels);

        for (int i = 0; i < n; ++i)
        {
            Frame x {}, b {};
            double peak = 0.0;
            for (int c = 0; c < nCh; ++c)
            {
                x[(size_t) c] = (double) in[c][i];
                b[(size_t) c] = band[(size_t) c].process (x[(size_t) c]);
                peak = std::max (peak, std::fabs (b[(size_t) c]));
            }
            const double envelope = std::max (envBand.process (peak), 1.0e-6);
            trackAdaptiveGain (x, b, nCh, p);

            const Frame extended = p.extGain > 0.0 ? extend (b, envelope, nCh, p) : Frame {};
            const Frame restored = p.resGain > 0.0 ? restore (x, b, nCh, p) : Frame {};

            for (int c = 0; c < nCh; ++c)
                out[c][i] = (float) (extended[(size_t) c] + restored[(size_t) c]);
        }
    }

private:
    // Chebyshev harmonics of the normalised bass band.
    Frame extend (const Frame& b, double envelope, int nCh, const Params& p) noexcept
    {
        Frame result {};
        for (int c = 0; c < nCh; ++c)
        {
            const double x = std::clamp (b[(size_t) c] / envelope, -1.0, 1.0);
            const double t2 = 2.0 * x * x - 1.0;
            const double t3 = x * (4.0 * x * x - 3.0);
            const double t4 = 2.0 * x * t3 - t2;
            const double t5 = 2.0 * x * t4 - t3;
            const double h = envelope * (p.harmonic[0] * usable[0] * t2 + p.harmonic[1] * usable[1] * t3
                                         + p.harmonic[2] * usable[2] * t4 + p.harmonic[3] * usable[3] * t5);

            // Removes the DC / sub terms that appear whenever |x| < 1, and band-limits.
            result[(size_t) c] = ext[(size_t) c].process (h) * p.extGain * adaptGain;
        }
        return result;
    }

    // Difference-tone regeneration, ducked by what the input already contains.
    Frame restore (const Frame& x, const Frame& b, int nCh, const Params& p) noexcept
    {
        Frame u {}, r {}, result {};
        double uPeak = 0.0;
        for (int c = 0; c < nCh; ++c)
        {
            u[(size_t) c] = restoreIn[(size_t) c].process (x[(size_t) c]);
            uPeak = std::max (uPeak, std::fabs (u[(size_t) c]));
        }

        const double eU = std::max (envRestore.process (uPeak), 1.0e-7);
        for (int c = 0; c < nCh; ++c)
        {
            // u^2 / env(u) has amplitude proportional to the input level (not its square). The
            // factor 2 calibrates 0 dB so two equal harmonics of amplitude a restore a
            // fundamental of amplitude a.
            r[(size_t) c] = 2.0 * restoreOut[(size_t) c].process (u[(size_t) c] * u[(size_t) c] / eU);
        }

        const double present = gaps.process (b, r, nCh);
        duck += 0.003 * ((1.0 - p.fill * present) - duck);

        for (int c = 0; c < nCh; ++c)
            result[(size_t) c] = r[(size_t) c] * duck * p.resGain;
        return result;
    }

    // Band energy vs. the energy already above it; the shortfall decides how much to generate.
    void trackAdaptiveGain (const Frame& x, const Frame& b, int nCh, const Params& p) noexcept
    {
        double inBand = 0.0, inReach = 0.0;
        for (int c = 0; c < nCh; ++c)
        {
            const double r = region[(size_t) c].process (x[(size_t) c]);
            inBand += b[(size_t) c] * b[(size_t) c];
            inReach += r * r;
        }
        energyBand += energyCoeff * (inBand / nCh - energyBand);
        energyReach += energyCoeff * (inReach / nCh - energyReach);

        const double wanted = energyBand * p.target + 1.0e-10;
        const double shortfall = 1.0 - std::min (1.0, energyReach / wanted);
        const double goal = 1.0 - p.adapt + p.adapt * std::sqrt (shortfall);
        adaptGain += adaptCoeff * (goal - adaptGain);
    }

    void updateCoefficients (const Params& p) noexcept
    {
        // Glide towards the requested range so dragging an edge doesn't zipper.
        const auto target = effectiveRange (BandMode::fill, p.lo, p.hi);
        const auto glide = [] (double& current, double goal)
        {
            current = current < 0.0 || std::fabs (goal - current) < 1.0e-4 * goal ? goal : current + 0.25 * (goal - current);
        };
        glide (curLo, target.lo);
        glide (curHi, target.hi);

        const double ceilingHz = std::min (p.ceiling, 0.45 * fs);
        const auto same = [] (double a, double b) { return std::fabs (a - b) < 1.0e-9; };
        if (same (curLo, lastLo) && same (curHi, lastHi) && same (ceilingHz, lastCeiling))
            return;

        lastLo = curLo; lastHi = curHi; lastCeiling = ceilingHz;
        const double lo = curLo, hi = curHi;
        const auto reach = harmonicReach (hi);

        for (size_t c = 0; c < (size_t) maxChannels; ++c)
        {
            band[c].setup (fs, 4, lo, 4, hi);                  // the band itself
            ext[c].setup (fs, 4, lo * 1.4, 2, ceilingHz);      // generated harmonics: above the range, below the ceiling
            restoreIn[c].setup (fs, 2, lo * 1.8, 2, hi * 3.6); // where the harmonics of the missing fundamentals live
            restoreOut[c].setup (fs, 4, lo, 4, hi);            // the regenerated fundamental
            region[c].setup (fs, 2, reach.lo, 2, reach.hi);    // what is already there, above the band
        }
        gaps.setup (fs, lo, hi);

        // A harmonic that would land too close to Nyquist would alias, so it is dropped.
        const int highest = highestHarmonic (hi, fs);
        for (int k = 0; k < 4; ++k)
            usable[k] = k + 2 <= highest ? 1.0 : 0.0;

        // Release spans several periods of the lowest frequency: shorter and the envelope ripples
        // at the bass period, which amplitude-modulates the generated harmonics.
        const double release = std::clamp (6.0 / lo, 0.04, 0.3);
        envBand.setTimes (fs, 0.0001, release);
        envRestore.setTimes (fs, 0.0005, release);

        // Short-term energy (what "unbalanced over time" is measured against) and a slower gain.
        energyCoeff = 1.0 - std::exp (-1.0 / (0.1 * fs));
        adaptCoeff = 1.0 - std::exp (-1.0 / (0.05 * fs));
    }

    double fs = 48000.0;
    double lastLo = -1.0, lastHi = -1.0, lastCeiling = -1.0, curLo = -1.0, curHi = -1.0;
    double duck = 1.0;
    double energyBand = 0.0, energyReach = 0.0, energyCoeff = 0.0, adaptGain = 1.0, adaptCoeff = 0.0;
    double usable[4] = { 1.0, 1.0, 1.0, 1.0 };

    std::array<pdsp::Bandpass, maxChannels> band, ext, restoreIn, restoreOut, region;
    GapDetector<maxChannels> gaps;
    EnvelopeFollower envBand, envRestore;
};
}
