// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 The Pantheon authors

#pragma once

#include "Band.h"
#include "OctaveGenerator.h"
#include "PartialTracker.h"
#include <pantheon/dsp/Biquad.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

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
// How a fill band makes its harmonics. Warm: a waveshaper (Chebyshev polynomials of the band over its envelope). Clean: sines locked to
// the one strongest note in the band (PartialTracker). Octave: sines locked to every resolved partial in the band, made in the spectrum
// (OctaveGenerator), which is the only one with a latency of its own.
enum class Character { warm = 0, clean = 1, octave = 2 };

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
        Character character = Character::warm; // how EXTEND makes its harmonics (see Character)
    };

    void prepare (double sampleRate)
    {
        fs = sampleRate;
        lastLo = lastHi = lastCeiling = -1.0;
        octaveGen.prepare (sampleRate);
        restoreRing.assign ((size_t) maxChannels * (size_t) (OctaveGenerator::latencyFor (sampleRate) + 1), 0.0);
        restorePos = 0;
        alignSize = (size_t) (OctaveGenerator::latencyFor (sampleRate) + 1);
        alignRing.assign ((size_t) maxChannels * alignSize, 0.0);
        alignPos = 0;
        guardCoeff = 1.0 - std::exp (-1.0 / (0.008 * sampleRate));
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
        tracker.reset();
        octaveGen.reset();
        std::fill (restoreRing.begin(), restoreRing.end(), 0.0);
        std::fill (alignRing.begin(), alignRing.end(), 0.0);
        guardIn.fill (0.0);
        guardOut.fill (0.0);
        octaveRunning = false;
        lastCharacter = Character::warm;
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

        // A generator that has not been running starts from nothing, not from what it last heard.
        const bool octave = p.character == Character::octave, clean = p.character == Character::clean;
        if (clean && lastCharacter != Character::clean)
            tracker.reset();
        const bool octaveWanted = octave && p.extGain > 0.0;
        if (octaveWanted && ! octaveRunning)
        {
            octaveGen.reset();
            std::fill (alignRing.begin(), alignRing.end(), 0.0);
            guardIn.fill (0.0);
            guardOut.fill (0.0);
        }
        if (octave && lastCharacter != Character::octave)
            std::fill (restoreRing.begin(), restoreRing.end(), 0.0);
        octaveRunning = octaveWanted;
        lastCharacter = p.character;

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

            Frame extended {};
            if (p.extGain > 0.0)
                extended = clean ? extendClean (b, nCh, p) : octave ? extendOctave (x, b, nCh, p) : extend (b, envelope, nCh, p);
            Frame restored = p.resGain > 0.0 ? restore (x, b, nCh, p) : Frame {};
            if (octave)
                restored = delayRestored (restored, nCh); // the spectral generator is late; what it is summed with must be too

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

    // Octave EXTEND: a sine at k times the frequency and phase of every resolved partial in the band, made in the spectrum
    // (OctaveGenerator), with the same weights, Amount and adaptive gain as the others. Its output is latency() samples late.
    Frame extendOctave (const Frame& x, const Frame& b, int nCh, const Params& p) noexcept
    {
        const double weights[OctaveGenerator::kHarmonics] = { p.harmonic[0] * usable[0], p.harmonic[1] * usable[1],
                                                              p.harmonic[2] * usable[2], p.harmonic[3] * usable[3] };
        Frame made {}, result {};
        octaveGen.process (x, made, nCh, curLo, curHi, weights);

        // A frame's sines last the whole frame, so before a note starts and after it stops there is a short echo of it. What the band
        // holds at that moment (its own signal, held back by the same latency) bounds what may be added: the harmonics, which carry
        // at most the band's energy, are not let past twice its level (a margin for the band's edges, where its filter lets less
        // through than the spectrum sees). In steady playing the bound is never reached.
        for (int c = 0; c < nCh; ++c)
        {
            const size_t at = (size_t) c * alignSize + alignPos;
            const double held = alignRing[at];
            alignRing[at] = b[(size_t) c];
            guardIn[(size_t) c] += guardCoeff * (held * held - guardIn[(size_t) c]);
            guardOut[(size_t) c] += guardCoeff * (made[(size_t) c] * made[(size_t) c] - guardOut[(size_t) c]);
            const double allowed = kGuardMargin * kGuardMargin * guardIn[(size_t) c];
            const double g = guardOut[(size_t) c] > allowed ? std::sqrt (allowed / guardOut[(size_t) c]) : 1.0;
            result[(size_t) c] = made[(size_t) c] * g * p.extGain * adaptGain;
        }
        alignPos = (alignPos + 1) % alignSize;
        return result;
    }

    // Rebuild's output, held back by the octave generator's latency so that the two sum in step.
    Frame delayRestored (const Frame& v, int nCh) noexcept
    {
        Frame out {};
        for (int c = 0; c < nCh; ++c)
        {
            auto& ring = restoreRing;
            const size_t size = ring.size() / (size_t) maxChannels, base = (size_t) c * size;
            ring[base + restorePos] = v[(size_t) c];
            out[(size_t) c] = ring[base + (restorePos + 1) % size];
        }
        restorePos = (restorePos + 1) % (restoreRing.size() / (size_t) maxChannels);
        return out;
    }

    // Clean EXTEND: the strongest partial of the band is followed (PartialTracker) and each harmonic is a sine of its
    // amplitude at k times its phase, so nothing the band holds besides that partial is bent into new frequencies, and
    // the level and the dynamics of the input cannot change what is added but its amplitude.
    Frame extendClean (const Frame& b, int nCh, const Params& p) noexcept
    {
        tracker.process (b, nCh);
        const double gain = tracker.gate();

        Frame result {};
        for (int c = 0; c < nCh; ++c)
        {
            const auto& t = tracker.partial (c);
            double powerRe = t.re * t.re - t.im * t.im, powerIm = 2.0 * t.re * t.im; // the 2nd power of the phase vector
            double h = 0.0;
            for (int k = 0; k < 4; ++k)
            {
                h += p.harmonic[k] * usable[k] * powerRe;
                const double next = powerRe * t.re - powerIm * t.im;
                powerIm = powerRe * t.im + powerIm * t.re;
                powerRe = next;
            }
            result[(size_t) c] = ext[(size_t) c].process (t.amp * h) * p.extGain * adaptGain * gain;
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
        tracker.setup (fs, lo, hi);

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
    PartialTracker<maxChannels> tracker;
    OctaveGenerator octaveGen;
    Character lastCharacter = Character::warm;
    bool octaveRunning = false;
    std::vector<double> restoreRing; // maxChannels rings of latency + 1
    size_t restorePos = 0;
    std::vector<double> alignRing;   // the band signal held back by the octave generator's latency, per channel
    size_t alignSize = 1, alignPos = 0;
    std::array<double, maxChannels> guardIn {}, guardOut {};
    double guardCoeff = 0.0;
    static constexpr double kGuardMargin = 2.0;
    EnvelopeFollower envBand, envRestore;
};
}
