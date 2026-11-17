// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 The Pantheon authors

#pragma once

#include "Compressor.h"
#include "Eq.h"
#include "Limiter.h"
#include "Loudness.h"
#include "StereoTools.h"
#include "TonalAnalyzer.h"

#include <pantheon/dsp/TruePeak.h> // the true-peak reader of tpOut

#include <array>
#include <atomic>
#include <vector>

namespace themis
{
struct EngineParams
{
    float inGainDb = 0.0f;
    float outGainDb = 0.0f;

    bool eqOn = true;
    std::array<EqBandParams, MasteringEq::kNumBands> eq {};

    bool compOn = false;
    CompParams comp;

    float widthPct = 100.0f;
    float monoBelowHz = 0.0f; // 0 = off

    bool limOn = true;
    LimiterParams lim;

    // Loudness-matched "hear the original" switch (for honest A/B).
    bool hearOriginal = false;

    float analyzerAvgSeconds = 30.0f; // 0 = infinite
};

struct EngineMeters
{
    float momentaryLufs = LoudnessMeter::kSilence;
    float shortTermLufs = LoudnessMeter::kSilence;
    float integratedLufs = LoudnessMeter::kSilence;
    float inputShortTermLufs = LoudnessMeter::kSilence;
    float truePeakDb = -120.0f; // held until resetMeters()
    float compGrDb = 0.0f;
    float limiterGrDb = 0.0f;
    float correlation = 1.0f;
    float abMatchDb = 0.0f; // gain applied to the original when hearOriginal is on
};

// The full mastering chain:
//   input -> [taps: input loudness + tonal] -> in gain -> EQ -> glue comp -> width -> limiter -> out gain
//         -> [taps: output loudness + tonal + true peak + correlation]
// All taps are read before anything downstream, so processing placed AFTER this chain never influences the
// measurements.
class Engine
{
public:
    void prepare (double sampleRate, int maxBlockSize)
    {
        fs = sampleRate;
        maxBlock = std::max (1, maxBlockSize);
        eq.prepare (fs);
        comp.prepare (fs);
        stereo.prepare (fs);
        limiter.prepare (fs);
        loudIn.prepare (fs);
        loudOut.prepare (fs);
        tonalIn.prepare (fs);
        tonalOut.prepare (fs, 0.5);

        const size_t len = static_cast<size_t> (limiter.latencySamples()) + 1;
        dryDelay[0].assign (len, 0.0f);
        dryDelay[1].assign (len, 0.0f);
        dryScratch[0].assign (static_cast<size_t> (maxBlock), 0.0f);
        dryScratch[1].assign (static_cast<size_t> (maxBlock), 0.0f);
        xfadeCoeff = onePoleCoeff (0.02, fs);
        reset();
    }

    int latencySamples() const { return limiter.latencySamples(); }

    void reset()
    {
        eq.reset();
        comp.reset();
        stereo.reset();
        limiter.reset();
        for (auto& d : dryDelay)
            std::fill (d.begin(), d.end(), 0.0f);
        dryPos = 0;
        inGain = dbToLin (params.inGainDb);
        outGain = dbToLin (params.outGainDb);
        xfade = params.hearOriginal ? 1.0 : 0.0;
        abMatchDb = 0.0;
        resetMetersNow();
    }

    void setParams (const EngineParams& p)
    {
        params = p;
        eq.setBands (p.eq);
        tonalIn.setAveragingSeconds (p.analyzerAvgSeconds);
        tonalOut.setAveragingSeconds (p.analyzerAvgSeconds);
    }

    // Thread-safe: applied at the start of the next process() call.
    void requestMeterReset() { meterResetFlag.store (true, std::memory_order_release); }
    void requestAnalyzerReset()
    {
        tonalIn.requestReset();
        tonalOut.requestReset();
    }

    const TonalAnalyzer& inputTonal() const { return tonalIn; }
    const TonalAnalyzer& outputTonal() const { return tonalOut; }
    const EngineMeters& meters() const { return m; }

    void process (float* l, float* r, int n)
    {
        for (int pos = 0; pos < n; pos += maxBlock)
            processChunk (l + pos, r + pos, std::min (maxBlock, n - pos));
    }

private:
    void processChunk (float* l, float* r, int n)
    {
        if (meterResetFlag.exchange (false, std::memory_order_acq_rel))
            resetMetersNow();

        // --- input taps + dry path (delayed by the chain latency for glitch-free A/B)
        loudIn.process (l, r, n);
        tonalIn.process (l, r, n);
        {
            const size_t len = dryDelay[0].size();
            for (int i = 0; i < n; ++i)
            {
                dryDelay[0][dryPos] = l[i];
                dryDelay[1][dryPos] = r[i];
                const size_t rd = (dryPos + 1) % len; // oldest sample == latency samples ago
                dryScratch[0][static_cast<size_t> (i)] = dryDelay[0][rd];
                dryScratch[1][static_cast<size_t> (i)] = dryDelay[1][rd];
                dryPos = (dryPos + 1) % len;
            }
        }

        // --- processing
        rampGain (l, r, n, inGain, dbToLin (params.inGainDb));
        if (params.eqOn)
            eq.process (l, r, n);
        if (params.compOn)
        {
            comp.process (l, r, n, params.comp);
            m.compGrDb = comp.gainReductionDb();
        }
        else
            m.compGrDb = 0.0f;
        stereo.process (l, r, n, params.widthPct, params.monoBelowHz);
        limiter.process (l, r, n, params.lim, params.limOn);
        m.limiterGrDb = params.limOn ? limiter.gainReductionDb() : 0.0f;
        rampGain (l, r, n, outGain, dbToLin (params.outGainDb));

        // --- output taps (measure the processed signal, whatever A/B is doing)
        loudOut.process (l, r, n);
        tonalOut.process (l, r, n);
        measurePeaksAndCorrelation (l, r, n);

        m.momentaryLufs = loudOut.momentary();
        m.shortTermLufs = loudOut.shortTerm();
        m.integratedLufs = loudOut.integrated();
        m.inputShortTermLufs = loudIn.shortTerm();

        // --- loudness-matched A/B
        if (!params.hearOriginal && xfade < 1.0e-3)
        {
            const float a = loudIn.shortTerm(), b = loudOut.shortTerm();
            if (a > -60.0f && b > -60.0f)
            {
                const double target = std::clamp (static_cast<double> (b - a), -24.0, 24.0);
                const double k = 1.0 - std::exp (-static_cast<double> (n) / (fs * 2.0));
                abMatchDb += (target - abMatchDb) * k;
            }
        }
        m.abMatchDb = static_cast<float> (abMatchDb);

        const double xfTarget = params.hearOriginal ? 1.0 : 0.0;
        const float matchGain = dbToLin (static_cast<float> (abMatchDb));
        for (int i = 0; i < n; ++i)
        {
            xfade = xfTarget + (xfade - xfTarget) * xfadeCoeff;
            if (xfade > 1.0e-5)
            {
                const float x = static_cast<float> (xfade);
                l[i] = l[i] * (1.0f - x) + dryScratch[0][static_cast<size_t> (i)] * matchGain * x;
                r[i] = r[i] * (1.0f - x) + dryScratch[1][static_cast<size_t> (i)] * matchGain * x;
            }
        }
    }

    static void rampGain (float* l, float* r, int n, float& current, float target)
    {
        if (std::abs (target - current) < 1.0e-7f)
        {
            current = target;
            if (std::abs (target - 1.0f) > 1.0e-7f)
                for (int i = 0; i < n; ++i)
                {
                    l[i] *= target;
                    r[i] *= target;
                }
            return;
        }
        const float step = (target - current) / static_cast<float> (n);
        for (int i = 0; i < n; ++i)
        {
            current += step;
            l[i] *= current;
            r[i] *= current;
        }
        current = target;
    }

    void measurePeaksAndCorrelation (const float* l, const float* r, int n)
    {
        const double a = 1.0 - onePoleCoeff (0.3, fs);
        float peak = 0.0f;
        for (int i = 0; i < n; ++i)
        {
            peak = std::max ({ peak, static_cast<float> (tpOut[0].push (l[i])), static_cast<float> (tpOut[1].push (r[i])) });
            const double xl = l[i], xr = r[i];
            sumLL += (xl * xl - sumLL) * a;
            sumRR += (xr * xr - sumRR) * a;
            sumLR += (xl * xr - sumLR) * a;
        }
        tpMaxLin = std::max (tpMaxLin, peak);
        m.truePeakDb = linToDb (tpMaxLin);
        const double den = std::sqrt (sumLL * sumRR);
        m.correlation = den > 1.0e-12 ? static_cast<float> (std::clamp (sumLR / den, -1.0, 1.0)) : 1.0f;
    }

    void resetMetersNow()
    {
        loudIn.reset();
        loudOut.reset();
        tpMaxLin = 0.0f;
        tpOut[0].reset();
        tpOut[1].reset();
        sumLL = sumRR = sumLR = 0.0;
        m = EngineMeters {};
    }

    double fs = 48000.0;
    int maxBlock = 512;
    EngineParams params;
    MasteringEq eq;
    Compressor comp;
    StereoTools stereo;
    Limiter limiter;
    LoudnessMeter loudIn, loudOut;
    TonalAnalyzer tonalIn, tonalOut;
    pdsp::TruePeakReader tpOut[2];

    std::vector<float> dryDelay[2], dryScratch[2];
    size_t dryPos = 0;
    float inGain = 1.0f, outGain = 1.0f;
    double xfade = 0.0, xfadeCoeff = 0.99;
    double abMatchDb = 0.0;

    float tpMaxLin = 0.0f;
    double sumLL = 0.0, sumRR = 0.0, sumLR = 0.0;
    EngineMeters m;
    std::atomic<bool> meterResetFlag { false };
};
} // namespace themis
