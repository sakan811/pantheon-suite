// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 The Pantheon authors

#pragma once

#include "Band.h"
#include "Crossover.h"
#include "Dynamics.h"
#include "LinearPhase.h"
#include "Saturator.h"

#include <juce_audio_basics/juce_audio_basics.h>
#include <pantheon/dsp/AnalyzerTap.h>

#include <atomic>

namespace iris
{
struct EngineParams
{
    std::array<BandParams, maxBands> bands;
    bool sat = false;                 // CLEAN (false) or SAT
    SatStyle style = SatStyle::tape;
    double driveDb = 12.0;
    double outGainDb = 0.0;
    bool match = false;
    std::array<double, numCrossovers> crossover = defaultCrossoverHz;
    PhaseMode phase = PhaseMode::zeroLatency;
};

// What is being listened to instead of the plain output. These are monitoring aids, not settings: they are not parameters,
// are never saved with a project, and always start off.
enum class SoloKind { band, curve };

struct Monitor
{
    std::atomic<bool> delta { false };
    std::atomic<int> soloBand { -1 };                       // -1 = none
    std::atomic<int> soloKind { (int) SoloKind::band };
    std::atomic<unsigned> regions { 0 };                    // bit r = region r is soloed
};

// The equaliser. Zero Latency runs all bands in series in double precision on the left/right pair; the linear-phase modes (LinearPhase.h)
// filter the whole curve at once with a symmetric FIR and delay everything by `latencySamples`. In either, a band that
// works on Mid or Side takes that signal out of the pair, filters it, and puts only the *difference* back, so everything it
// does not touch is bit-for-bit what went in.
//
// Everything that changes with time (glides to new settings, the dynamics, coefficient updates, gain match) moves in steps
// of `tick` samples counted from the start of playback, not from the start of a host block, so the output does not depend on
// how the host happens to cut the audio into blocks.
class Engine
{
public:
    static constexpr int tick = 32;

    void prepare (double sampleRate, int maxBlockSize, PhaseMode initialPhase = PhaseMode::zeroLatency)
    {
        fs = sampleRate;
        linear.prepare();
        maxBlock = std::max (maxBlockSize, 1);

        const double tickSeconds = tick / fs;
        glideK = 1.0 - std::exp (-tickSeconds / 0.006);
        mixK = 1.0 - std::exp (-tickSeconds / 0.004);
        satK = 1.0 - std::exp (-tickSeconds / 0.010);
        outK = 1.0 - std::exp (-tickSeconds / 0.010);
        matchEnergyK = 1.0 - std::exp (-tickSeconds / 0.6);
        matchGainK = 1.0 - std::exp (-tickSeconds / 0.15);
        soloStep = 1.0 / (0.010 * fs);

        for (auto& b : bands)
        {
            b = {};
            b.det[0].prepare (fs);
            b.det[1].prepare (fs);
            for (auto& s : b.shaper)
                s.setSampleRate (fs);
        }
        reset();
        applyPhase (initialPhase);
        for (auto& a : dynamicDb)
            a.store (0.0f);
    }

    void reset()
    {
        for (auto& b : bands)
        {
            b.running = false;
            b.mix = b.mixPrev = b.mixStep = 0.0;
        }
        regions.reset();
        for (auto& f : soloFilters)
            for (auto& s : f)
                s.reset();
        phase = 0;
        appliedCrossover = {}; // the splitter's coefficients belong to the old sample rate
        satAmount = satPrev = 0.0;
        soloAmount = regionAmount = 0.0;
        shownIndex = -1;
        lastRegionMask = 0;
        matchGain = matchPrev = 1.0;
        linear.start (phaseMode);
        kernelsDirty = soloKernelDirty = true;
        outGain = outPrev = 0.0;
        outInitialised = false;
        msDry = msWet = 1.0e-6;
        eDry = eWet = 0.0;
    }

    void process (juce::AudioBuffer<float>& buffer, const EngineParams& p)
    {
        const int total = buffer.getNumSamples();
        const int nCh = std::min (buffer.getNumChannels(), 2);
        if (nCh == 0)
            return;

        const float* in[2] = { buffer.getReadPointer (0), buffer.getReadPointer (nCh > 1 ? 1 : 0) };
        tapIn.push (in, nCh, total);
        smoothPeak (inPeak, peakOf (buffer, nCh), total);

        params = p;
        for (int start = 0, n = 0; start < total; start += n)
        {
            // A segment never crosses a tick boundary, wherever the host's block starts.
            n = std::min (tick - phase, total - start);
            segment (buffer, start, n, nCh);
        }

        const float* out[2] = { buffer.getReadPointer (0), buffer.getReadPointer (nCh > 1 ? 1 : 0) };
        tapOut.push (out, nCh, total);
        smoothPeak (outPeak, peakOf (buffer, nCh), total);
    }

    std::atomic<float> inPeak { 0.0f }, outPeak { 0.0f };
    void clearMeters() noexcept { inPeak.store (0.0f, std::memory_order_relaxed); outPeak.store (0.0f, std::memory_order_relaxed); }

    // Per band: where the dynamics have moved its gain to right now (dB, relative to its own gain), and the level it is hearing.
    std::atomic<float> dynamicDb[maxBands];
    std::atomic<float> detectedDb[maxBands];
    std::atomic<float> matchDb { 0.0f };   // the gain Gain Match is applying
    Monitor monitor;
    std::atomic<int> latencySamples { 0 }; // what the host has to be told: 0 in Zero Latency, the linear modes' delay otherwise
    pdsp::StereoTap tapIn, tapOut;         // the signal before and after the equaliser, for the display

private:
    struct BandState
    {
        std::array<BandFilter, 2> filter;       // slot 0: left, mid, side (and the only slot of a one-channel placement); slot 1: right
        std::array<Detector, 2> det;
        std::array<Shaper, 2> shaper;
        bool running = false, active = false, saturating = false;
        BandType type = BandType::bell;
        Placement place = Placement::stereo;
        double logFreq = 0.0, gainDb = 0.0, logQ = 0.0;   // where the glide has got to
        double dynDb = 0.0;
        double appliedGain = 1.0e9, appliedFreq = 0.0, appliedQ = 0.0;
        int appliedOrder = -1;                            // a cut's slope: a change of it is a new filter too
        double mix = 0.0, mixPrev = 0.0, mixStep = 0.0;   // 0..1 fade when the band is switched on or off
        bool dynamic = false;
    };

    // ---- once per tick -------------------------------------------------------------------------
    void control()
    {
        const auto& p = params;
        if (p.phase != phaseMode)
            applyPhase (p.phase);

        const double wantSat = p.sat ? 1.0 : 0.0;
        satPrev = satAmount;
        satAmount += satK * (wantSat - satAmount);
        if (std::fabs (wantSat - satAmount) < 1.0e-4) satAmount = wantSat;
        satDrive = std::pow (10.0, std::clamp (p.driveDb, 0.0, 36.0) / 20.0);

        // The style, like everything else, changes at a tick. The shaper's memory (its last input and antiderivative) belongs to
        // the old curve, so it starts again.
        if (p.style != satStyle)
        {
            satStyle = p.style;
            for (auto& b : bands)
                for (auto& sh : b.shaper)
                    sh.reset (satStyle);
        }

        for (int b = 0; b < maxBands; ++b)
            controlBand (b, bands[(size_t) b], p.bands[(size_t) b]);

        // The crossovers stay in order and a little apart, so the regions cannot cross.
        auto xo = p.crossover;
        for (int k = 0; k < numCrossovers; ++k)
            xo[(size_t) k] = clampFreq (k == 0 ? xo[0] : std::max (xo[(size_t) k], xo[(size_t) k - 1] * 1.2), fs);
        if (xo != appliedCrossover)
        {
            regions.setCrossovers (fs, xo);
            appliedCrossover = xo;
        }

        // JUCE snaps a 0.1 dB parameter to a float grid, so "0 dB" can arrive as 0.000002: anything under 0.005 dB is exactly unity,
        // which is what keeps a plugin at its defaults bit-for-bit transparent.
        const double wantOut = std::fabs (p.outGainDb) < 0.005 ? 1.0 : std::pow (10.0, std::clamp (p.outGainDb, -48.0, 48.0) / 20.0);
        outPrev = outInitialised ? outGain : wantOut;
        outGain = outInitialised ? outGain + outK * (wantOut - outGain) : wantOut;
        outInitialised = true;

        // Gain match: slow RMS ratio of what came in to what the bands made of it, over the last tick.
        if (eCount > 0)
        {
            msDry += matchEnergyK * (eDry / eCount - msDry);
            msWet += matchEnergyK * (eWet / eCount - msWet);
            eDry = eWet = 0.0;
            eCount = 0;
        }
        matchPrev = matchGain;
        const double target = p.match ? std::clamp (std::sqrt ((msDry + 1.0e-9) / (msWet + 1.0e-9)), 0.25, 4.0) : 1.0;
        matchGain += matchGainK * (target - matchGain);
        matchDb.store ((float) (20.0 * std::log10 (matchGain)), std::memory_order_relaxed);

        // Band solo: which band, and in which of its two ways.
        soloIndex = monitor.soloBand.load (std::memory_order_relaxed);
        if (soloIndex < 0 || soloIndex >= maxBands || ! p.bands[(size_t) soloIndex].on)
            soloIndex = -1;
        soloKind = (SoloKind) monitor.soloKind.load (std::memory_order_relaxed);
        if (soloIndex >= 0)
        {
            if (soloAmount <= 0.0)
                for (auto& chain : soloFilters)
                    for (auto& bq : chain)
                        bq.reset();
            if (shownIndex != soloIndex || shownKind != soloKind)
                soloKernelDirty = true;
            shownIndex = soloIndex;
            shownKind = soloKind;
        }
        regionMask = monitor.regions.load (std::memory_order_relaxed) & ((1u << numRegions) - 1u);
        if (regionMask != 0 && regionAmount <= 0.0)
            regions.reset();
        deltaOn = monitor.delta.load (std::memory_order_relaxed);
        if (soloIndex >= 0 && soloKind == SoloKind::band)
            configureBandSolo (params.bands[(size_t) soloIndex]);
    }

    // A new phase mode starts every band from clean memory (the delay in front of the filters is a different length) and the
    // linear filter from silence.
    void applyPhase (PhaseMode m)
    {
        phaseMode = m;
        latencySamples.store (latencyFor (m), std::memory_order_relaxed);
        linear.start (m);
        for (auto& b : bands)
        {
            for (auto& f : b.filter) f.reset();
            for (auto& d : b.det) d.reset();
            for (auto& sh : b.shaper) sh.reset (satStyle);
            b.appliedGain = 1.0e9;
        }
        kernelsDirty = soloKernelDirty = true;
    }

    void startBand (BandState& s, const BandParams& t)
    {
        kernelsDirty = soloKernelDirty = true;
        s.running = true;
        s.type = t.type;
        s.place = t.place;
        s.logFreq = std::log (clampFreq (t.freq, fs));
        s.gainDb = t.gainDb;
        s.logQ = std::log (std::clamp (t.q, 0.1, 18.0));
        s.dynDb = 0.0;
        s.appliedGain = 1.0e9;
        s.mix = s.mixPrev = s.mixStep = 0.0;
        for (auto& f : s.filter) f.reset();
        for (auto& d : s.det) d.reset();
        for (auto& sh : s.shaper) sh.reset (satStyle);
        s.dynamic = false;
        s.appliedOrder = -1;
    }

    void controlBand (int index, BandState& s, const BandParams& t)
    {
        if (t.on && ! s.running)
            startBand (s, t);

        s.mixPrev = s.mix;
        if (! s.running)
        {
            s.mixStep = 0.0;
            s.active = false;
            dynamicDb[index].store (0.0f, std::memory_order_relaxed);
            return;
        }

        const double wantMix = t.on ? 1.0 : 0.0;
        s.mix += mixK * (wantMix - s.mix);
        if (! t.on && s.mix < 1.0e-4)
        {
            s.mix = 0.0;
            s.running = s.active = false;
            s.mixStep = -s.mixPrev / tick;
            kernelsDirty = soloKernelDirty = true;
            return;
        }
        s.mixStep = (s.mix - s.mixPrev) / tick;
        if (std::fabs (s.mixStep) > 0.0)
            kernelsDirty = soloKernelDirty = true;

        // A placement or type change would leave the filters' memory meaningless: start them again.
        if (t.type != s.type || t.place != s.place)
        {
            s.type = t.type;
            s.place = t.place;
            for (auto& f : s.filter) f.reset();
            for (auto& d : s.det) d.reset();
            s.appliedGain = 1.0e9;
        }

        s.logFreq += glideK * (std::log (clampFreq (t.freq, fs)) - s.logFreq);
        s.gainDb += glideK * (t.gainDb - s.gainDb);
        s.logQ += glideK * (std::log (std::clamp (t.q, 0.1, 18.0)) - s.logQ);

        const bool wasDynamic = s.dynamic;
        s.dynamic = t.isDynamic();
        if (s.dynamic && ! wasDynamic)
        {
            // The detector has not been fed (or configured) while the band was not dynamic: it starts from silence, on the band's
            // current frequency and width.
            for (auto& d : s.det) d.reset();
            s.appliedGain = 1.0e9;
        }
        if (s.dynamic)
        {
            const double level = std::max (s.det[0].level(), s.det[1].level());
            const double levelDb = 20.0 * std::log10 (std::max (level, 1.0e-6));

            // Attack and release act on the gain, in dB: attack while the gain is moving further from rest, release while it comes back.
            const double wanted = dynamicOffsetDb (t.dyn, levelDb);
            const double ms = std::fabs (wanted) > std::fabs (s.dynDb) ? t.dyn.attackMs : t.dyn.releaseMs;
            s.dynDb += (1.0 - std::exp (-(double) tick / (std::max (ms, 0.01) * 0.001 * fs))) * (wanted - s.dynDb);
            detectedDb[index].store ((float) levelDb, std::memory_order_relaxed);
        }
        else
        {
            s.dynDb = 0.0;
            detectedDb[index].store (-120.0f, std::memory_order_relaxed);
        }
        dynamicDb[index].store ((float) s.dynDb, std::memory_order_relaxed);

        const double f = std::exp (s.logFreq), q = std::exp (s.logQ);
        const double g = hasGain (t.type) ? std::clamp (s.gainDb + s.dynDb, -36.0, 36.0) : 0.0;
        const int order = orderForSlope (t.slope);
        if (std::fabs (g - s.appliedGain) > 1.0e-5 || std::fabs (f - s.appliedFreq) > f * 1.0e-6 || std::fabs (q - s.appliedQ) > q * 1.0e-6
            || order != s.appliedOrder)
        {
            for (auto& filter : s.filter)
                filter.configure (t.type, fs, f, q, g, order);
            if (s.dynamic)
                for (auto& d : s.det)
                    d.setFilter (t.type, f, q);
            s.appliedGain = g;
            s.appliedFreq = f;
            s.appliedQ = q;
            s.appliedOrder = order;
            kernelsDirty = soloKernelDirty = true;
        }

        // A bell or shelf at 0 dB is the identity, so it costs nothing until it has something to do.
        s.active = ! hasGain (t.type) || s.dynamic || std::fabs (g) > 0.004;
        s.saturating = s.active && hasGain (t.type) && g > 0.05;
    }

    // ---- one segment: up to a tick, never across a boundary -------------------------------------
    void segment (juce::AudioBuffer<float>& buffer, int start, int n, int nCh)
    {
        if (phase == 0)
            control();

        double L[tick], R[tick], dryL[tick], dryR[tick], soloL[tick] = {}, soloR[tick] = {};
        const float* in0 = buffer.getReadPointer (0, start);
        const float* in1 = buffer.getReadPointer (nCh > 1 ? 1 : 0, start);
        for (int i = 0; i < n; ++i)
        {
            // An inf or NaN would stay in a filter's memory for good: it is replaced by silence.
            L[i] = dryL[i] = std::isfinite (in0[i]) ? (double) in0[i] : 0.0;
            R[i] = dryR[i] = std::isfinite (in1[i]) ? (double) in1[i] : 0.0;
        }

        // While a band solo fades out the band that was soloed is still the one shown.
        const bool curveShown = shownIndex >= 0 && shownKind == SoloKind::curve;
        if (phaseMode != PhaseMode::zeroLatency)
            runLinear (L, R, dryL, dryR, soloL, soloR, n);
        else
            for (int b = 0; b < maxBands; ++b)
            {
                auto& s = bands[(size_t) b];
                const bool capture = b == shownIndex && curveShown;
                if (s.running && (s.active || s.dynamic))
                    runBand (s, L, R, n, capture ? soloL : nullptr, capture ? soloR : nullptr);
            }

        const bool bandSolo = soloIndex >= 0;
        const bool regionSolo = regionMask != 0;

        for (int i = 0; i < n; ++i)
        {
            const double t = (double) (phase + i + 1) / tick;
            const double l = L[i], r = R[i];

            eDry += dryL[i] * dryL[i] + dryR[i] * dryR[i];
            eWet += l * l + r * r;
            ++eCount;
            const double match = matchPrev + (matchGain - matchPrev) * t;

            // Normal output: gain matched, or the difference to the input.
            double outL = l * match, outR = r * match;
            if (deltaOn)
            {
                outL -= dryL[i];
                outR -= dryR[i];
            }

            // Band solo takes over (it wins over delta), with a short fade so it does not click.
            if (soloAmount > 0.0 || bandSolo)
            {
                soloAmount = std::clamp (soloAmount + (bandSolo ? soloStep : -soloStep), 0.0, 1.0);
                double sl = 0.0, sr = 0.0;
                if (curveShown)
                {
                    sl = soloL[i];
                    sr = soloR[i];
                }
                else
                    bandRegion (l, r, sl, sr);
                outL += soloAmount * (sl - outL);
                outR += soloAmount * (sr - outR);
            }

            // Output gain, then the frequency regions.
            const double g = outPrev + (outGain - outPrev) * t;
            outL *= g;
            outR *= g;

            if (regionAmount > 0.0 || regionSolo)
            {
                regionAmount = std::clamp (regionAmount + (regionSolo ? soloStep : -soloStep), 0.0, 1.0);
                const unsigned mask = regionSolo ? regionMask : lastRegionMask;
                const double rl = regions.process (0, outL, mask), rr = regions.process (1, outR, mask);
                outL += regionAmount * (rl - outL);
                outR += regionAmount * (rr - outR);
            }

            L[i] = outL;
            R[i] = outR;
        }
        if (regionSolo)
            lastRegionMask = regionMask;

        float* out0 = buffer.getWritePointer (0, start);
        float* out1 = nCh > 1 ? buffer.getWritePointer (1, start) : nullptr;
        for (int i = 0; i < n; ++i)
        {
            out0[i] = (float) L[i];
            if (out1 != nullptr)
                out1[i] = (float) R[i];
        }

        phase = (phase + n) % tick;
    }

    // The linear-phase modes over a segment: the input goes into the filter and what comes out is the sound of `latencySamples` ago, so
    // the dry signal (for Delta and Gain Match) is replaced by its delayed copy. The detectors listen to the input, which is
    // ahead of the sound they act on: a dynamic band reacts before the level it has measured arrives (look-ahead, by the latency).
    void runLinear (double* L, double* R, double* dryL, double* dryR, double* soloL, double* soloR, int n)
    {
        for (int i = 0; i < n; ++i)
        {
            for (auto& s : bands)
                if (s.running && s.dynamic)
                    feedDetectors (s, L[i], R[i]);

            const auto f = linear.step (L[i], R[i]);
            if (linear.hopDue())
                runLinearHop();
            L[i] = f.l;
            R[i] = f.r;
            dryL[i] = f.dryL;
            dryR[i] = f.dryR;
            soloL[i] = f.soloL;
            soloR[i] = f.soloR;
        }
    }

    static void feedDetectors (BandState& s, double l, double r) noexcept
    {
        switch (s.place)
        {
            case Placement::stereo: s.det[0].push (l); s.det[1].push (r); break;
            case Placement::left:   s.det[0].push (l); break;
            case Placement::right:  s.det[0].push (r); break;
            case Placement::mid:    s.det[0].push (0.5 * (l + r)); break;
            case Placement::side:   s.det[0].push (0.5 * (l - r)); break;
        }
    }

    // A block of the linear filter has been collected: rebuild its curve if anything in it moved since the last one.
    void runLinearHop()
    {
        const auto working = [] (const BandState& s) { return s.running && (s.active || s.dynamic); };
        const bool filtering = std::any_of (bands.begin(), bands.end(), working);
        const bool curve = shownIndex >= 0 && shownKind == SoloKind::curve && (soloIndex >= 0 || soloAmount > 0.0);
        if (filtering && kernelsDirty)
        {
            linear.beginKernels();
            for (const auto& s : bands)
                if (working (s))
                    linear.addBand (s.filter[0], touchesLeft (s.place), touchesRight (s.place), touchesMid (s.place), touchesSide (s.place), s.mix);
            linear.finishKernels();
            kernelsDirty = false;
        }
        if (curve && soloKernelDirty)
        {
            linear.setSoloBand (bands[(size_t) shownIndex].filter[0], bands[(size_t) shownIndex].mix);
            soloKernelDirty = false;
        }
        linear.processHop (filtering, curve, curve ? bands[(size_t) shownIndex].place : Placement::stereo);
    }

    // One band over a segment. It adds its difference to L and R, so what it does not touch stays exactly as it was.
    void runBand (BandState& s, double* L, double* R, int n, double* soloL, double* soloR)
    {
        const bool sat = satAmount > 0.0 && s.saturating;
        const SatStyle style = satStyle;
        const double drive = satDrive;
        const bool dyn = s.dynamic;
        const int base = phase;

        const auto shape = [&] (int slot, double d, double satMix)
        {
            return sat ? d + satMix * s.shaper[(size_t) slot].residual (style, d, drive) : d;
        };

        for (int i = 0; i < n; ++i)
        {
            const double m = s.mixPrev + s.mixStep * (double) (base + i + 1);
            const double satMix = satPrev + (satAmount - satPrev) * (double) (base + i + 1) / tick;
            double dL = 0.0, dR = 0.0;
            switch (s.place) // the placement the filters were set up for, which changes at a tick (not `t.place`, which may be newer)
            {
                case Placement::stereo:
                {
                    const double x0 = L[i], x1 = R[i];
                    if (dyn) { s.det[0].push (x0); s.det[1].push (x1); }
                    dL = shape (0, s.filter[0].process (x0) - x0, satMix);
                    dR = shape (1, s.filter[1].process (x1) - x1, satMix);
                    break;
                }
                case Placement::left:
                {
                    const double x = L[i];
                    if (dyn) s.det[0].push (x);
                    dL = shape (0, s.filter[0].process (x) - x, satMix);
                    break;
                }
                case Placement::right:
                {
                    const double x = R[i];
                    if (dyn) s.det[0].push (x);
                    dR = shape (0, s.filter[0].process (x) - x, satMix);
                    break;
                }
                case Placement::mid:
                {
                    const double x = 0.5 * (L[i] + R[i]);
                    if (dyn) s.det[0].push (x);
                    dL = dR = shape (0, s.filter[0].process (x) - x, satMix);
                    break;
                }
                case Placement::side:
                {
                    const double x = 0.5 * (L[i] - R[i]);
                    if (dyn) s.det[0].push (x);
                    dL = shape (0, s.filter[0].process (x) - x, satMix);
                    dR = -dL;
                    break;
                }
            }
            dL *= m;
            dR *= m;
            L[i] += dL;
            R[i] += dR;
            if (soloL != nullptr)
            {
                soloL[i] = dL;
                soloR[i] = dR;
            }
        }
    }

    // ---- band solo, region flavour: the output heard through a filter that matches the band ----------
    void configureBandSolo (const BandParams& t)
    {
        const double f = clampFreq (t.freq, fs), q = std::clamp (t.q, 0.7, 12.0);
        for (auto& chain : soloFilters)
            for (auto& bq : chain)
            {
                if (t.type == BandType::lowShelf || t.type == BandType::lowCut) bq.setLowpass (fs, f, 0.7071);
                else if (t.type == BandType::highShelf || t.type == BandType::highCut) bq.setHighpass (fs, f, 0.7071);
                else bq.setBandpass (fs, f, q);
            }
        soloPlace = t.place;
    }

    void bandRegion (double l, double r, double& outL, double& outR)
    {
        const auto run = [&] (int slot, double x) { return soloFilters[(size_t) slot][1].process (soloFilters[(size_t) slot][0].process (x)); };
        switch (soloPlace)
        {
            case Placement::stereo: outL = run (0, l); outR = run (1, r); break;
            case Placement::left:   outL = outR = run (0, l); break;
            case Placement::right:  outL = outR = run (0, r); break;
            case Placement::mid:    outL = outR = run (0, 0.5 * (l + r)); break;
            case Placement::side:   outL = run (0, 0.5 * (l - r)); outR = -outL; break;
        }
    }

    static float peakOf (const juce::AudioBuffer<float>& b, int nCh) noexcept
    {
        float peak = 0.0f;
        for (int c = 0; c < nCh; ++c)
        {
            const float* d = b.getReadPointer (c);
            for (int i = 0; i < b.getNumSamples(); ++i)
            {
                const float a = std::fabs (d[i]);
                if (std::isfinite (a)) // an inf or NaN sample must not pin the meter
                    peak = std::max (peak, a);
            }
        }
        return peak;
    }

    void smoothPeak (std::atomic<float>& meter, float peak, int n) noexcept
    {
        const float decay = std::exp (-(float) n / (0.3f * (float) fs)); // ~300 ms fall-back
        const float held = meter.load (std::memory_order_relaxed);
        meter.store (std::max (std::isfinite (held) ? held * decay : 0.0f, peak), std::memory_order_relaxed);
    }

    double fs = 48000.0;
    int maxBlock = 512;
    EngineParams params;
    int phase = 0;
    PhaseMode phaseMode = PhaseMode::zeroLatency;
    LinearFilter linear;
    bool kernelsDirty = true, soloKernelDirty = true;

    std::array<BandState, maxBands> bands;
    double glideK = 0.1, mixK = 0.1, satK = 0.1, outK = 0.1, matchEnergyK = 0.001, matchGainK = 0.01, soloStep = 0.002;

    double satAmount = 0.0, satPrev = 0.0, satDrive = 4.0;
    SatStyle satStyle = SatStyle::tape;
    double outGain = 1.0, outPrev = 1.0;
    bool outInitialised = false;

    double msDry = 1.0e-6, msWet = 1.0e-6, eDry = 0.0, eWet = 0.0, matchGain = 1.0, matchPrev = 1.0;
    int eCount = 0;

    int soloIndex = -1, shownIndex = -1;
    SoloKind soloKind = SoloKind::band, shownKind = SoloKind::band;
    Placement soloPlace = Placement::stereo;
    std::array<std::array<pdsp::Biquad, 2>, 2> soloFilters; // [slot][stage]
    double soloAmount = 0.0, regionAmount = 0.0;

    bool deltaOn = false;
    unsigned regionMask = 0, lastRegionMask = 0;
    RegionSplitter regions;
    std::array<double, numCrossovers> appliedCrossover {};
};
} // namespace iris
