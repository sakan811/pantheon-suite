// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 The Pantheon authors

#pragma once

#include "LinearPhase.h"
#include <atomic>
#include <vector>

// The whole equaliser: no GUI, no JUCE. Everything is sized in prepare(); process() allocates and locks nothing.
namespace apollo
{
enum class Phase { minimum, linear };
inline constexpr int kNumPhases = 2;

struct EngineParams
{
    std::array<BandParams, kBands> bands;
    Phase phase = Phase::minimum;
    double outDb = 0.0;
    bool match = false; // GAIN MATCH: the bands' effect on the level is taken off again (slowly), so a boost that is merely louder is not mistaken for better
};

class Engine
{
public:
    // Everything that changes with time (the smoothing of the bands' settings, the output gain, the linear kernels) steps every kTick samples, counted from the
    // start: that is what makes the output the same for any host block size.
    static constexpr int kTick = 64;

    void prepare (double sampleRate, Phase phase)
    {
        fs = sampleRate;
        sizes = linearSizesFor (fs);
        for (auto& c : convolver)
            c.prepare (sizes);
        smoothing = 1.0 - std::exp (-(double) kTick / (0.02 * fs));
        matchEnergyK = 1.0 - std::exp (-(double) kTick / (0.6 * fs));
        matchGainK = 1.0 - std::exp (-(double) kTick / (0.15 * fs));
        dryL.assign ((size_t) std::max (sizes.latency(), 1), 0.0);
        dryR.assign (dryL.size(), 0.0);
        configure (phase);
    }

    void reset() noexcept
    {
        for (auto& b : band)
        {
            b.running = false;
            b.mid.reset();
            b.side.reset();
            b.now = BandParams();
        }
        for (auto& c : convolver)
            c.reset();
        tickCount = 0;
        std::fill (dryL.begin(), dryL.end(), 0.0);
        std::fill (dryR.begin(), dryR.end(), 0.0);
        dryPos = 0;
        matchNow = 1.0;
        msDry = msWet = 1.0e-6;
        eDry = eWet = 0.0;
        eCount = 0;
        matchDb.store (0.0f, std::memory_order_relaxed);
        outNow = -1.0; // snaps to the target at the first tick
        listenL.reset();
        listenR.reset();
        listenBand = -1;
        linearDirty = true;
        sinceDesign = 1000;
    }

    // A change of phase mode restarts the equaliser (the latency moves with it).
    void configure (Phase phase) noexcept
    {
        mode = phase;
        latencySamples.store (mode == Phase::linear ? sizes.latency() : 0, std::memory_order_relaxed);
        reset();
    }

    // In place. Everything that is not a number is silence.
    void process (float* left, float* right, int n, const EngineParams& p) noexcept
    {
        if (p.phase != mode)
            configure (p.phase);
        int done = 0;
        while (done < n)
        {
            if (tickCount == 0)
                tick (p);
            const int m = std::min (n - done, kTick - tickCount);
            for (int i = done; i < done + m; ++i)
            {
                const double l = std::isfinite (left[i]) ? (double) left[i] : 0.0, r = std::isfinite (right[i]) ? (double) right[i] : 0.0;
                double yl, yr, dl = l, dr = r; // dl, dr: the input as the output hears it (in linear phase, the input of `latency` samples ago)
                if (mode == Phase::minimum)
                    sample (l, r, yl, yr);
                else
                {
                    sampleLinear (l, r, yl, yr);
                    std::swap (dl, dryL[dryPos]);
                    std::swap (dr, dryR[dryPos]);
                    dryPos = dryPos + 1 == dryL.size() ? 0 : dryPos + 1;
                }
                eDry += dl * dl + dr * dr;
                eWet += yl * yl + yr * yr;
                ++eCount;
                yl *= matchNow; // exactly 1.0 unless GAIN MATCH is on
                yr *= matchNow;
                if (deltaOn)
                {
                    yl -= dl;
                    yr -= dr;
                }
                if (listenBand >= 0)
                {
                    yl = listenL.process (yl);
                    yr = listenR.process (yr);
                }
                left[i] = (float) (yl * outNow);
                right[i] = (float) (yr * outNow);
            }
            done += m;
            tickCount = (tickCount + m) % kTick;
        }
    }

    // What the editor draws: the bands as the engine has them now (smoothed), for the curve while a knob moves.
    std::atomic<int> latencySamples { 0 };
    std::atomic<float> inPeak { 0.0f }, outPeak { 0.0f };
    std::atomic<bool> delta { false };   // DELTA: only what the bands change (output minus input). Monitoring, not a setting: never saved, never a parameter
    std::atomic<int> listen { -1 };      // LISTEN: the band (0 to 7) whose region is heard alone, after the bands, through a band-pass at its frequency and width; -1 is off. Monitoring, like delta
    std::atomic<float> matchDb { 0.0f }; // the gain GAIN MATCH is applying now, for the display

private:
    struct BandState
    {
        bool running = false;
        BandParams now;             // what the band is now: the target's settings, moved toward it
        double mix = 1.0;           // a band without a gain (high-pass, low-pass, notch) fades in and out through this: 1 is the filter, 0 is the plain input
        Section mid, side;          // the filters: `mid` is the left or the mid signal, `side` the right or the side signal
    };

    void tick (const EngineParams& p) noexcept
    {
        deltaOn = delta.load (std::memory_order_relaxed);
        {
            const int want = std::clamp (listen.load (std::memory_order_relaxed), -1, kBands - 1);
            if (want != listenBand)
            {
                listenL.reset();
                listenR.reset();
                listenBand = want;
            }
            if (listenBand >= 0)
            {
                const auto& b = p.bands[(size_t) listenBand];
                const double f = std::clamp (b.freqHz, kMinHz, std::min (kMaxHz, 0.45 * fs)), q = std::clamp (b.q, 0.3, 10.0);
                listenL.setBandpass (fs, f, q); // the coefficients move, the states stay
                listenR.setBandpass (fs, f, q);
            }
        }
        // GAIN MATCH: the RMS of what came in against what the bands made of it, over about 0.6 s; the gain that equals them follows at 0.15 s, within 12 dB.
        if (eCount > 0)
        {
            msDry += matchEnergyK * (eDry / eCount - msDry);
            msWet += matchEnergyK * (eWet / eCount - msWet);
            eDry = eWet = 0.0;
            eCount = 0;
        }
        const double matchTarget = p.match ? std::clamp (std::sqrt ((msDry + 1.0e-9) / (msWet + 1.0e-9)), 0.25, 4.0) : 1.0;
        matchNow = std::fabs (matchTarget - matchNow) < 1e-9 ? matchTarget : matchNow + (matchTarget - matchNow) * matchGainK;
        matchDb.store ((float) (20.0 * std::log10 (matchNow)), std::memory_order_relaxed);
        const double outTarget = std::pow (10.0, std::clamp (p.outDb, -24.0, 24.0) / 20.0);
        outNow = outNow < 0.0 ? outTarget : (std::fabs (outTarget - outNow) < 1e-9 ? outTarget : outNow + (outTarget - outNow) * smoothing);
        for (int i = 0; i < kBands; ++i)
        {
            auto& b = band[(size_t) i];
            const auto& t = p.bands[(size_t) i];
            const bool sameKind = t.type == b.now.type && t.target == b.now.target && t.slope == b.now.slope;
            // A band that is being taken out is run until it has got there: it fades, it does not cut. A bell or shelf fades its gain; a high-pass, low-pass or notch (no gain to take to 0)
            // fades its `mix`, from the filter to the plain input.
            const bool fading = b.running && sameKind && (hasGain (b.now.type) ? std::fabs (b.now.gainDb) >= kIdentityDb : b.mix > 0.0);
            if (isIdentity (t) && ! fading)
            {
                if (b.running)
                {
                    b.running = false;
                    b.mid.reset();
                    b.side.reset();
                    linearDirty = true;
                }
                b.now = t;
                continue;
            }
            if (! b.running || ! sameKind)
            {
                // Starts from 0 dB (a gain moves to its value like any other change) with clean filters.
                b.now = t;
                b.mix = 1.0;
                if (hasGain (t.type))
                    b.now.gainDb = 0.0;
                b.mid.reset();
                b.side.reset();
                b.mid.design (b.now, fs);
                b.side.design (b.now, fs);
                b.running = true;
                linearDirty = true;
                continue;
            }
            // Smoothed, so a knob turned does not click: the frequency in the log domain, the gain and the Q linearly; the last bit is snapped to the target.
            const double targetGain = t.on ? t.gainDb : 0.0;
            double f = std::exp (std::log (b.now.freqHz) + (std::log (t.freqHz) - std::log (b.now.freqHz)) * smoothing);
            double g = b.now.gainDb + (targetGain - b.now.gainDb) * smoothing;
            double q = b.now.q + (t.q - b.now.q) * smoothing;
            if (std::fabs (std::log (t.freqHz / f)) < 1e-4) f = t.freqHz;
            if (std::fabs (targetGain - g) < 1e-3) g = targetGain;
            if (std::fabs (t.q - q) < 1e-3) q = t.q;
            double mix = b.mix;
            if (! hasGain (t.type))
            {
                const double mixTarget = t.on ? 1.0 : 0.0;
                mix += (mixTarget - mix) * smoothing;
                if (std::fabs (mixTarget - mix) < 1e-3) mix = mixTarget;
            }
            if (f != b.now.freqHz || g != b.now.gainDb || q != b.now.q || mix != b.mix)
            {
                b.mix = mix;
                b.now.freqHz = f;
                b.now.gainDb = g;
                b.now.q = q;
                b.mid.design (b.now, fs); // the coefficients move, the states stay
                b.side.design (b.now, fs);
                linearDirty = true;
            }
        }
        // The linear kernels are made at most once a hop: four FFTs, and the sound only needs them as often as the hop produces output.
        if (mode == Phase::linear && linearDirty && ++sinceDesign >= std::max (1, sizes.hop / kTick))
        {
            designLinear();
            sinceDesign = 0;
        }
    }

    inline void sample (double l, double r, double& yl, double& yr) noexcept
    {
        for (auto& b : band)
        {
            if (! b.running)
                continue;
            switch (b.now.target)
            {
                case Target::stereo:
                {
                    const double fl = b.mid.process (l), fr = b.side.process (r);
                    l = b.mix == 1.0 ? fl : l + b.mix * (fl - l);
                    r = b.mix == 1.0 ? fr : r + b.mix * (fr - r);
                    break;
                }
                case Target::mid:
                {
                    const double m = 0.5 * (l + r), d = b.mix * (b.mid.process (m) - m);
                    l += d;
                    r += d;
                    break;
                }
                case Target::side:
                {
                    const double s = 0.5 * (l - r), d = b.mix * (b.mid.process (s) - s);
                    l += d;
                    r -= d;
                    break;
                }
            }
        }
        yl = l;
        yr = r;
    }

    inline void sampleLinear (double l, double r, double& yl, double& yr) noexcept
    {
        const double m = convolver[0].process (0.5 * (l + r)), s = convolver[1].process (0.5 * (l - r));
        yl = m + s;
        yr = m - s;
    }

    void designLinear() noexcept
    {
        for (int path = 0; path < 2; ++path)
        {
            std::array<Section, kBands> sections;
            std::array<bool, kBands> running {};
            std::array<double, kBands> mix {};
            for (int i = 0; i < kBands; ++i)
            {
                const auto& b = band[(size_t) i];
                running[(size_t) i] = b.running && reaches (b.now.target, path == 0 ? Target::mid : Target::side);
                sections[(size_t) i] = b.mid;
                mix[(size_t) i] = b.mix;
            }
            bool identity = false;
            convolver[(size_t) path].design (sections, running, mix, identity);
        }
        linearDirty = false;
    }

    double fs = 48000.0, smoothing = 0.1, outNow = -1.0;
    double matchEnergyK = 0.001, matchGainK = 0.01, matchNow = 1.0, msDry = 1.0e-6, msWet = 1.0e-6, eDry = 0.0, eWet = 0.0;
    int eCount = 0;
    bool deltaOn = false;
    int listenBand = -1;
    pdsp::Biquad listenL, listenR;
    std::vector<double> dryL, dryR; // the input, delayed by the linear phase's latency, to compare the output with
    size_t dryPos = 0;
    LinearSizes sizes;
    Phase mode = Phase::minimum;
    int tickCount = 0, sinceDesign = 1000;
    bool linearDirty = true;
    std::array<BandState, kBands> band;
    std::array<Convolver, 2> convolver; // the mid and the side
};
} // namespace apollo
