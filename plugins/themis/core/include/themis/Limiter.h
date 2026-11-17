// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 The Pantheon authors

#pragma once

#include <pantheon/dsp/TruePeak.h> // the true peak the Pantheon plugins read (8x, 64 taps); plain C++

#include <vector>

namespace themis
{
struct LimiterParams
{
    float driveDb = 0.0f;    // gain into the limiter
    float ceilingDb = -1.0f; // true-peak ceiling
    float releaseMs = 120.0f;
};

// Stereo-linked look-ahead true-peak limiter.
//
// Gain is computed as: required gain -> sliding-window minimum (W = N + 4) -> release envelope
// -> N-sample boxcar. The window-min / boxcar pair guarantees the applied gain never exceeds the
// required gain for the sample it is applied to. Audio is delayed by N + kTruePeakDelay + 1 so the true-peak
// reader's own delay is covered with +/- 2 samples of margin.
class Limiter
{
public:
    static constexpr double kLookaheadMs = 1.5;
    // The reader under-reads a pure tone near Nyquist by a fraction of a dB (README "Limitations"); the guard covers it.
    static constexpr float kDetectGuard = 1.02f; // +0.17 dB

    void prepare (double sampleRate)
    {
        fs = sampleRate;
        N = std::max (2, static_cast<int> (std::lround (kLookaheadMs * 0.001 * fs)));
        W = N + 4;
        D = N + pdsp::kTruePeakDelay + 1;
        delay[0].assign (static_cast<size_t> (D + 1), 0.0f);
        delay[1].assign (static_cast<size_t> (D + 1), 0.0f);
        minVals.assign (static_cast<size_t> (W), 1.0f);
        minIdx.assign (static_cast<size_t> (W + 1), 0);
        boxRing.assign (static_cast<size_t> (N), 1.0f);
        reset();
    }

    int latencySamples() const { return D; }

    void reset()
    {
        std::fill (delay[0].begin(), delay[0].end(), 0.0f);
        std::fill (delay[1].begin(), delay[1].end(), 0.0f);
        tp[0].reset();
        tp[1].reset();
        dPos = 0;
        std::fill (minVals.begin(), minVals.end(), 1.0f);
        dqHead = dqTail = 0;
        sampleCounter = 0;
        std::fill (boxRing.begin(), boxRing.end(), 1.0f);
        boxPos = 0;
        boxSum = static_cast<double> (N);
        relState = 1.0;
        grDb = 0.0f;
    }

    // enabled == false still delays the signal so latency stays constant.
    void process (float* l, float* r, int n, const LimiterParams& p, bool enabled)
    {
        const float drive = enabled ? dbToLin (p.driveDb) : 1.0f;
        const float ceiling = dbToLin (p.ceilingDb);
        const double rel = onePoleCoeff (p.releaseMs * 0.001, fs);
        float minGain = 1.0f;

        for (int i = 0; i < n; ++i)
        {
            const float xl = l[i] * drive, xr = r[i] * drive;

            // delay line (circular, length D + 1)
            const size_t len = static_cast<size_t> (D + 1);
            delay[0][static_cast<size_t> (dPos)] = xl;
            delay[1][static_cast<size_t> (dPos)] = xr;
            const size_t rd = (static_cast<size_t> (dPos) + len - static_cast<size_t> (D)) % len;
            const float dl = delay[0][rd], dr = delay[1][rd];
            dPos = static_cast<int> ((static_cast<size_t> (dPos) + 1) % len);

            if (!enabled)
            {
                l[i] = dl;
                r[i] = dr;
                continue;
            }

            // kDetectGuard covers the interpolator's small under-read on content near Nyquist
            const float peak = static_cast<float> (std::max (tp[0].push (xl), tp[1].push (xr))) * kDetectGuard;
            const float required = peak > ceiling ? ceiling / peak : 1.0f;

            const float w = slidingMin (required);

            // release envelope: instant attack (the boxcar provides the ramp), exponential release
            if (static_cast<double> (w) < relState)
                relState = w;
            else
                relState = rel * relState + (1.0 - rel) * w;

            // boxcar over N samples
            boxSum += relState - boxRing[static_cast<size_t> (boxPos)];
            boxRing[static_cast<size_t> (boxPos)] = static_cast<float> (relState);
            boxPos = (boxPos + 1) % N;
            const float gain = static_cast<float> (std::min (1.0, boxSum / N));

            l[i] = dl * gain;
            r[i] = dr * gain;
            minGain = std::min (minGain, gain);
        }
        grDb = linToDb (minGain);
    }

    // Largest gain reduction (negative dB) applied in the last block.
    float gainReductionDb() const { return grDb; }

private:
    // Monotonic-deque sliding-window minimum over the last W values.
    float slidingMin (float v)
    {
        const size_t cap = static_cast<size_t> (W + 1);
        while (dqTail != dqHead && minVals[minIdx[(dqTail + cap - 1) % cap] % static_cast<size_t> (W)] >= v)
            dqTail = (dqTail + cap - 1) % cap;
        minVals[sampleCounter % static_cast<size_t> (W)] = v;
        minIdx[dqTail] = sampleCounter;
        dqTail = (dqTail + 1) % cap;
        while (minIdx[dqHead] + static_cast<size_t> (W) <= sampleCounter)
            dqHead = (dqHead + 1) % cap;
        const float result = minVals[minIdx[dqHead] % static_cast<size_t> (W)];
        ++sampleCounter;
        return result;
    }

    double fs = 48000.0;
    int N = 72, W = 76, D = 85;
    std::vector<float> delay[2];
    pdsp::TruePeakReader tp[2];
    int dPos = 0;

    std::vector<float> minVals;
    std::vector<size_t> minIdx;
    size_t dqHead = 0, dqTail = 0, sampleCounter = 0;

    std::vector<float> boxRing;
    int boxPos = 0;
    double boxSum = 0.0;
    double relState = 1.0;
    float grDb = 0.0f;
};
} // namespace themis
