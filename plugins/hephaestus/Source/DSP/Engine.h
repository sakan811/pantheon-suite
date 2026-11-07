// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 The Pantheon authors

#pragma once

#include <pantheon/dsp/AnalyzerTap.h>
#include <pantheon/dsp/MusicBands.h>
#include "FundamentalEngine.h"
#include "Saturator.h"
#include <atomic>
#include <cmath>
#include <vector>

namespace hep
{
// Whole-number sample delay (power-of-two ring buffer). Latency compensation needs exact
// integer delays; no interpolation, no smoothing.
class IntDelay
{
public:
    void prepare (int maxDelay)
    {
        int size = 16;
        while (size < maxDelay + 2) size <<= 1;
        buffer.assign ((size_t) size, 0.0f);
        mask = size - 1;
        reset();
    }

    void reset() noexcept { std::fill (buffer.begin(), buffer.end(), 0.0f); pos = 0; }
    void setDelay (int d) noexcept { delay = std::clamp (d, 0, mask - 1); }

    inline float process (float x) noexcept
    {
        buffer[(size_t) pos] = x;
        const float y = buffer[(size_t) ((pos - delay) & mask)];
        pos = (pos + 1) & mask;
        return y;
    }

private:
    std::vector<float> buffer;
    int mask = 15, pos = 0, delay = 0;
};

// One user-placed band. `lo`/`hi` are the single source of truth for its range; `fill.lo/hi` are
// overwritten from them before the fill engine runs.
struct BandParams
{
    bool on = false;
    BandMode mode = BandMode::saturate;
    double lo = 100.0, hi = 1000.0;
    double drive = 1.0;             // saturate: linear
    FundamentalEngine::Params fill; // fill: everything but the range
};

// What is listened to instead of the plain output: a monitoring aid, not a setting. It is not a parameter, so a host cannot
// record, save or restore it, and the editor ends it when it closes.
//   band: the band's own region, the signal through a band-pass over its range (what the band works on)
//   adds: only what the band adds (its distortion, or its filled harmonics), nothing else
// and, on top of whatever that is, the main frequency bands of music (pdsp::bands: sub, bass, low-mid, midrange, upper-mid,
// high), the same bands the other plugins of the suite solo.
enum class SoloKind { band, adds };

struct Monitor
{
    std::atomic<int> soloBand { -1 }; // -1 = none
    std::atomic<int> soloKind { (int) SoloKind::band };
    std::atomic<unsigned> regions { 0 }; // bit b = pdsp::bands::kBands[b] is soloed; 0 = none
};

struct EngineParams
{
    float inGainDb = 0.0f, outGainDb = 0.0f, mix = 1.0f;
    bool midSide = false, delta = false, match = false;
    int osMode = 2;

    int curve = 0;      // saturate bands: 0 tanh, 1 atan, 2 algebraic
    double bias = 0.0;  // saturate bands: pre-shaper offset; > 0 adds even harmonics
    std::array<BandParams, maxBands> bands;
};

class Engine
{
public:
    static constexpr int maxChannels = 2;

    // `initialOsMode` (if given) sets the reported latency right away; otherwise it is only known after the first block.
    void prepare (double sampleRate, int maxBlockSize, int initialOsMode = -1)
    {
        fs = sampleRate;
        maxBlock = maxBlockSize;

        saturator.prepare (sampleRate, maxBlockSize);
        for (auto& f : fillEngine)
            f.prepare (sampleRate);

        int maxLatency = 0;
        for (int m = 0; m < Saturator::numOsModes; ++m)
        {
            latencyTable[(size_t) m] = saturator.latencyFor (m);
            maxLatency = std::max (maxLatency, latencyTable[(size_t) m]);
        }

        forEachDelay ([&] (IntDelay& d) { d.prepare (maxLatency + 8); });
        for (auto* b : { &dryBuf, &wetBuf, &fundBuf, &satBuf, &tmpBuf, &soloBuf, &soloFillBuf, &soloSatBuf })
            b->setSize (maxChannels, maxBlockSize, false, false, true);

        fillGain.fill (0.0f);
        fillRunning.fill (false);
        for (auto& a : bandActivity)
            a.store (0.0f);
        for (auto& r : bandReachDb)
            r.store ((float) FundamentalEngine::kReachFloorDb);

        currentOs = -1; // forces a latency refresh on the first block
        msDry = msWet = 1.0e-6;
        matchGain = 1.0;
        soloStep = 1.0 / (0.010 * fs);
        soloAmount = 0.0;
        shownBand = -1;
        regionSplitter.setCrossovers (fs, pdsp::bands::defaultCrossovers());
        inGainLast = outGainLast = 1.0f;
        reset();

        if (initialOsMode >= 0)
            applyLatencyFor (std::clamp (initialOsMode, 0, Saturator::numOsModes - 1));
    }

    void reset()
    {
        saturator.reset();
        for (auto& f : fillEngine)
            f.reset();
        fillGain.fill (0.0f);
        fillRunning.fill (false);
        forEachDelay ([] (IntDelay& d) { d.reset(); });
        for (auto& f : soloFilter)
            f.reset();
        soloAmount = 0.0;
        regionSplitter.reset();
        regionAmount = 0.0;
        lastRegionMask = 0;
    }

    int getLatency() const noexcept { return latency; }

    void process (juce::AudioBuffer<float>& buffer, const EngineParams& p)
    {
        const int total = buffer.getNumSamples();
        const int nCh = std::min (buffer.getNumChannels(), maxChannels);

        for (int start = 0; start < total; start += maxBlock)
            processChunk (buffer, start, std::min (maxBlock, total - start), nCh, p);
    }

    std::atomic<float> inPeak { 0.0f }, outPeak { 0.0f };
    void clearMeters() noexcept { inPeak.store (0.0f, std::memory_order_relaxed); outPeak.store (0.0f, std::memory_order_relaxed); }
    std::atomic<float> bandActivity[maxBands]; // fill bands: how much of its Amount the adaptive fill is adding now (0..1)
    std::atomic<float> bandReachDb[maxBands];  // fill bands: energy already above the band re the band's own (dB, the Amount scale)
    Monitor monitor;
    pdsp::StereoTap tapIn, tapOut; // pre (delay-aligned dry) and post signal, for the spectrum display

private:
    using Ptrs = std::array<float*, maxChannels>; // a mono buffer repeats channel 0, so no entry is null

    static Ptrs pointersOf (juce::AudioBuffer<float>& b, int start = 0) noexcept
    {
        const int second = b.getNumChannels() > 1 ? 1 : 0;
        return { b.getWritePointer (0, start), b.getWritePointer (second, start) };
    }

    template <class Fn>
    void forEachDelay (Fn&& fn)
    {
        for (auto* set : { &dryDelay, &fundDelay, &bypassDelay, &soloDelay })
            for (auto& d : *set)
                fn (d);
    }

    template <class Fn>
    static void forEachSample (int nCh, int n, Fn&& fn)
    {
        for (int c = 0; c < nCh; ++c)
            for (int i = 0; i < n; ++i)
                fn (c, i);
    }

    // Ramps the gain across the chunk and returns the peak of the result.
    static float rampGain (const Ptrs& ch, int nCh, int n, float from, float to) noexcept
    {
        float peak = 0.0f;
        const float step = (to - from) / (float) n;
        for (int c = 0; c < nCh; ++c)
        {
            float g = from;
            for (int i = 0; i < n; ++i)
            {
                g += step;
                ch[(size_t) c][i] *= g;
                const float a = std::fabs (ch[(size_t) c][i]);
                if (std::isfinite (a)) // an inf or NaN sample must not pin the meter (inf decays to inf)
                    peak = std::max (peak, a);
            }
        }
        return peak;
    }

    // Mid = (L+R)/2, Side = (L-R)/2: unity through an encode/decode round trip.
    static void midSideEncode (const Ptrs& ch, int n) noexcept
    {
        for (int i = 0; i < n; ++i)
        {
            const float l = ch[0][i], r = ch[1][i];
            ch[0][i] = 0.5f * (l + r);
            ch[1][i] = 0.5f * (l - r);
        }
    }

    static void midSideDecode (const Ptrs& ch, int n) noexcept
    {
        for (int i = 0; i < n; ++i)
        {
            const float m = ch[0][i], s = ch[1][i];
            ch[0][i] = m + s;
            ch[1][i] = m - s;
        }
    }

    void applyLatencyFor (int osMode)
    {
        currentOs = osMode;
        latency = latencyTable[(size_t) osMode];
        saturator.onOversamplingChanged();
        forEachDelay ([&] (IntDelay& d) { d.setDelay (latency); d.reset(); });
    }

    // Slow RMS ratio of dry to wet, so loudness differences don't masquerade as tone.
    double matchGainFor (const Ptrs& dry, const Ptrs& wet, int nCh, int n)
    {
        const double tc = 1.0 - std::exp (-(double) n / (0.6 * fs));
        double eDry = 0.0, eWet = 0.0;
        forEachSample (nCh, n, [&] (int c, int i)
        {
            eDry += (double) dry[(size_t) c][i] * dry[(size_t) c][i];
            eWet += (double) wet[(size_t) c][i] * wet[(size_t) c][i];
        });
        msDry += tc * (eDry / (n * nCh) - msDry);
        msWet += tc * (eWet / (n * nCh) - msWet);

        const double target = std::clamp (std::sqrt ((msDry + 1.0e-9) / (msWet + 1.0e-9)), 0.25, 4.0);
        matchGain += 0.02 * (target - matchGain);
        return matchGain;
    }

    void processChunk (juce::AudioBuffer<float>& buffer, int start, int n, int nCh, const EngineParams& p)
    {
        const int os = std::clamp (p.osMode, 0, Saturator::numOsModes - 1);
        if (os != currentOs)
            applyLatencyFor (os);

        const auto io = pointersOf (buffer, start);
        const auto dry = pointersOf (dryBuf), wet = pointersOf (wetBuf), add = pointersOf (fundBuf);
        const auto shaped = pointersOf (satBuf);
        const bool ms = p.midSide && nCh == 2;

        // 1. Input gain, input meter.
        const float inGain = juce::Decibels::decibelsToGain (p.inGainDb);
        smoothPeak (inPeak, rampGain (io, nCh, n, inGainLast, inGain), n);
        inGainLast = inGain;

        // 2. Dry reference, delayed to line up with the oversampled wet path.
        forEachSample (nCh, n, [&] (int c, int i)
        {
            wet[(size_t) c][i] = io[(size_t) c][i];
            dry[(size_t) c][i] = dryDelay[(size_t) c].process (io[(size_t) c][i]);
        });

        if (ms) midSideEncode (wet, n);

        // The band solo: which band is heard and in which way. A band that is switched off, or deselected, fades out as it was.
        int soloWant = monitor.soloBand.load (std::memory_order_relaxed);
        if (soloWant < 0 || soloWant >= maxBands || ! p.bands[(size_t) soloWant].on)
            soloWant = -1;
        if (soloWant >= 0)
        {
            if (soloAmount <= 0.0)
                for (auto& f : soloFilter)
                    f.reset();
            shownBand = soloWant;
            shownKind = (SoloKind) monitor.soloKind.load (std::memory_order_relaxed);
        }
        const bool soloShown = shownBand >= 0 && (soloWant >= 0 || soloAmount > 0.0);
        captureBand = soloShown && shownKind == SoloKind::adds ? shownBand : -1;
        const auto solo = pointersOf (soloBuf), soloFill = pointersOf (soloFillBuf), soloSat = pointersOf (soloSatBuf);
        for (auto& c : { 0, 1 })
            std::fill_n (soloFill[(size_t) c], n, 0.0f);

        // 3. Fill bands on the clean signal. In mid/side only the mid channel feeds them, so bass
        //    stays centred.
        for (auto& c : { 0, 1 })
            std::fill_n (add[(size_t) c], n, 0.0f);
        for (int b = 0; b < maxBands; ++b)
            runFillBand (b, wet, add, soloFill, nCh, n, ms, p.bands[(size_t) b]);

        // Always run the delay so its ring buffer keeps flowing when a fill band is switched on later.
        forEachSample (nCh, n, [&] (int c, int i)
        {
            add[(size_t) c][i] = fundDelay[(size_t) c].process (add[(size_t) c][i]);
            soloFill[(size_t) c][i] = soloDelay[(size_t) c].process (soloFill[(size_t) c][i]); // the soloed fill band, aligned the same way
        });

        // 4. Saturate bands add their distortion to the clean signal, which is delayed to match.
        Saturator::Params sat;
        sat.curve = p.curve;
        sat.bias = p.bias;
        for (int b = 0; b < maxBands; ++b)
        {
            const auto& band = p.bands[(size_t) b];
            sat.band[(size_t) b] = { band.on && band.mode == BandMode::saturate, band.lo, band.hi, band.drive };
        }

        const float* satIn[maxChannels] = { wet[0], wet[1] };
        saturator.process (satIn, shaped.data(), nCh, n, os, sat, captureBand, soloSat.data());

        forEachSample (nCh, n, [&] (int c, int i)
        {
            wet[(size_t) c][i] = bypassDelay[(size_t) c].process (wet[(size_t) c][i])
                                 + shaped[(size_t) c][i] + add[(size_t) c][i];
        });

        if (ms) midSideDecode (wet, n);

        // What the solo plays, in the output's own channels.
        if (soloShown)
        {
            if (shownKind == SoloKind::band)
            {
                const auto& band = p.bands[(size_t) shownBand];
                const auto range = effectiveRange (band.mode, band.lo, band.hi);
                const double hiHz = std::min (range.hi, fs * 0.45), loHz = std::min (range.lo, hiHz / minRangeRatio);
                for (auto& f : soloFilter)
                    f.setup (fs, 4, loHz, 4, hiHz);
                forEachSample (nCh, n, [&] (int c, int i) { solo[(size_t) c][i] = (float) soloFilter[(size_t) c].process ((double) dry[(size_t) c][i]); });
            }
            else
            {
                forEachSample (nCh, n, [&] (int c, int i) { solo[(size_t) c][i] = soloFill[(size_t) c][i] + soloSat[(size_t) c][i]; });
                if (ms)
                    midSideDecode (solo, n);
            }
        }

        // 5. Mix / delta, then the solo (it wins over both), then the band solo strip, then output gain.
        float gMatch = 1.0f;
        if (p.match) gMatch = (float) matchGainFor (dry, wet, nCh, n);
        else         matchGain = 1.0;

        forEachSample (nCh, n, [&] (int c, int i)
        {
            const float w = wet[(size_t) c][i] * gMatch, d = dry[(size_t) c][i];
            io[(size_t) c][i] = p.delta ? (w - d) : d + p.mix * (w - d);
        });

        if (soloShown)
            for (int i = 0; i < n; ++i)
            {
                soloAmount = std::clamp (soloAmount + (soloWant >= 0 ? soloStep : -soloStep), 0.0, 1.0);
                for (int c = 0; c < nCh; ++c)
                    io[(size_t) c][i] += (float) soloAmount * (solo[(size_t) c][i] - io[(size_t) c][i]);
            }

        // The band solo strip: the main bands of music, out of whatever is heard (so it combines with Delta and a band solo).
        // Every band goes through the all-pass of the crossovers above it, so any combination sums flat; it fades like the
        // band solo, and starts from clean filters.
        const unsigned regionMask = monitor.regions.load (std::memory_order_relaxed) & pdsp::bands::kAllBands;
        if (regionMask != 0 && regionAmount <= 0.0)
            regionSplitter.reset();
        if (regionMask != 0 || regionAmount > 0.0)
        {
            const unsigned mask = regionMask != 0 ? regionMask : lastRegionMask;
            for (int i = 0; i < n; ++i)
            {
                regionAmount = std::clamp (regionAmount + (regionMask != 0 ? soloStep : -soloStep), 0.0, 1.0);
                for (int c = 0; c < nCh; ++c)
                {
                    const double x = io[(size_t) c][i];
                    io[(size_t) c][i] = (float) (x + regionAmount * (regionSplitter.process (c, x, mask) - x));
                }
            }
        }
        if (regionMask != 0)
            lastRegionMask = regionMask;

        const float outGain = juce::Decibels::decibelsToGain (p.outGainDb);
        smoothPeak (outPeak, rampGain (io, nCh, n, outGainLast, outGain), n);
        outGainLast = outGain;

        // 6. Spectrum taps.
        const float* dryOut[maxChannels] = { dry[0], dry[1] };
        const float* ioOut[maxChannels] = { io[0], io[1] };
        tapIn.push (dryOut, nCh, n);
        tapOut.push (ioOut, nCh, n);
    }

    // Runs one fill band and accumulates its (faded) output into `add`. A band that is off and has
    // faded out is left alone, and starts from clean history when it comes back.
    void runFillBand (int b, const Ptrs& wet, const Ptrs& add, const Ptrs& soloFill, int nCh, int n, bool ms, const BandParams& band)
    {
        auto& engine = fillEngine[(size_t) b];
        auto& gain = fillGain[(size_t) b];
        const bool want = band.on && band.mode == BandMode::fill && (band.fill.extGain > 0.0 || band.fill.resGain > 0.0);

        if (! want && gain <= 0.0f)
        {
            if (fillRunning[(size_t) b])
                engine.reset();
            fillRunning[(size_t) b] = false;
            bandActivity[b].store (0.0f, std::memory_order_relaxed);
            bandReachDb[b].store ((float) FundamentalEngine::kReachFloorDb, std::memory_order_relaxed);
            return;
        }
        fillRunning[(size_t) b] = true;

        auto params = band.fill;
        params.lo = band.lo;
        params.hi = band.hi;

        const auto tmp = pointersOf (tmpBuf);
        const float* in[maxChannels] = { wet[0], wet[1] };
        float* out[maxChannels] = { tmp[0], tmp[1] };
        engine.process (in, out, ms ? 1 : nCh, n, params);
        if (ms)
            std::fill_n (tmp[1], n, 0.0f);

        const float target = want ? 1.0f : 0.0f, step = (target - gain) / (float) n;
        forEachSample (nCh, n, [&] (int c, int i)
        {
            const float v = tmp[(size_t) c][i] * (gain + step * (float) (i + 1));
            add[(size_t) c][i] += v;
            if (b == captureBand)
                soloFill[(size_t) c][i] = v;
        });
        gain = target;
        bandActivity[b].store (want && band.fill.extGain > 0.0 ? (float) engine.currentAdaptGain() : 0.0f, std::memory_order_relaxed);
        bandReachDb[b].store ((float) engine.currentReachDb(), std::memory_order_relaxed);
    }

    void smoothPeak (std::atomic<float>& meter, float peak, int n) noexcept
    {
        const float decay = std::exp (-(float) n / (0.3f * (float) fs)); // ~300 ms fall-back
        const float held = meter.load (std::memory_order_relaxed);
        meter.store (std::max (std::isfinite (held) ? held * decay : 0.0f, peak), std::memory_order_relaxed);
    }

    double fs = 48000.0;
    int maxBlock = 512, currentOs = -1, latency = 0;
    std::array<int, Saturator::numOsModes> latencyTable {};

    Saturator saturator;
    std::array<FundamentalEngine, maxBands> fillEngine;
    std::array<float, maxBands> fillGain {};
    std::array<bool, maxBands> fillRunning {};
    std::array<IntDelay, maxChannels> dryDelay, fundDelay, bypassDelay, soloDelay;
    juce::AudioBuffer<float> dryBuf, wetBuf, fundBuf, satBuf, tmpBuf, soloBuf, soloFillBuf, soloSatBuf;

    // Band solo state (audio thread): the band shown (kept while it fades out), its way, how far it has faded in, and the
    // band whose additions are being captured this chunk.
    std::array<pdsp::Bandpass, maxChannels> soloFilter;
    int shownBand = -1, captureBand = -1;
    SoloKind shownKind = SoloKind::band;
    double soloAmount = 0.0, soloStep = 0.002;

    // Band solo strip state (audio thread): how far it has faded in, and the bands it was playing (kept while it fades out).
    pdsp::BandSplitter regionSplitter;
    double regionAmount = 0.0;
    unsigned lastRegionMask = 0;

    double msDry = 1.0e-6, msWet = 1.0e-6, matchGain = 1.0;
    float inGainLast = 1.0f, outGainLast = 1.0f;
};
}
