// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 The Pantheon authors

#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <pantheon/dsp/TruePeak.h>
#include <vector>

// The whole limiter: no GUI, no JUCE. Everything is sized in prepare() / configure(); process() allocates and locks nothing.
namespace aegis
{
inline constexpr int kNumLookahead = 4;
inline constexpr double kLookaheadMs[kNumLookahead] = { 0.5, 1.0, 2.0, 5.0 };
inline constexpr int kDefaultLookahead = 2;

inline constexpr int kDetectorDelay = pdsp::kTruePeakDelay; // samples the detector reads behind the newest input: the audio waits this much longer than the look-ahead
inline constexpr float kMaxGainDb = 24.0f;

// AUTO RELEASE: the gain comes back at RELEASE only after a sustained reduction; after a short peak it comes back faster. How much reduction counts as
// sustained (a mean of the gain over kAutoMeanMs, in linear gain: 0.4 is about 4.4 dB), and how much faster a short peak is let go (a fraction of RELEASE, never under 2 ms).
inline constexpr double kAutoMeanMs = 150.0;
inline constexpr double kAutoDepth = 0.4;
inline constexpr double kAutoFastShare = 0.2;
inline constexpr double kAutoFastFloorMs = 2.0;

struct EngineParams
{
    float gainDb = 0.0f;       // before the limiter
    float ceilingDb = -1.0f;   // the most the output reaches, in dBTP (dBFS with TRUE PEAK off)
    float releaseMs = 100.0f;  // the time the gain takes to come back 63% of the way
    int lookahead = kDefaultLookahead;
    bool truePeak = true;      // read the peaks between the samples too
    bool autoRelease = false;  // a short peak is let go faster than RELEASE, a sustained reduction at RELEASE
};

inline int lookaheadSamples (double fs, int choice) noexcept
{
    return std::max (1, (int) std::lround (kLookaheadMs[std::clamp (choice, 0, kNumLookahead - 1)] * 0.001 * fs));
}

// The smallest value of the last `window + 1` pushed: a monotonic queue, so each push costs a few comparisons, never a scan.
class SlidingMin
{
public:
    void prepare (int maxWindow)
    {
        int size = 16;
        while (size < maxWindow + 2)
            size *= 2;
        index.assign ((size_t) size, 0);
        value.assign ((size_t) size, 1.0);
        mask = (uint64_t) size - 1;
        reset();
    }

    void reset() noexcept { head = tail = 0; count = 0; }

    inline double push (double v, int window) noexcept
    {
        while (tail > head && value[(tail - 1) & mask] >= v)
            --tail;
        index[tail & mask] = count;
        value[tail & mask] = v;
        ++tail;
        while (index[head & mask] + (uint64_t) window < count)
            ++head;
        ++count;
        return value[head & mask];
    }

private:
    std::vector<uint64_t> index;
    std::vector<double> value;
    uint64_t mask = 15, head = 0, tail = 0, count = 0;
};

class Engine
{
public:
    void prepare (double sampleRate, int lookaheadChoice)
    {
        fs = sampleRate;
        const int longest = lookaheadSamples (fs, kNumLookahead - 1);
        mins.prepare (longest);
        ring = 1;
        while (ring < longest * 2 + kDetectorDelay + 8)
            ring *= 2;
        delayL.assign ((size_t) ring, 0.0);
        delayR.assign ((size_t) ring, 0.0);
        avg1.assign ((size_t) ring, 1.0);
        avg2.assign ((size_t) ring, 1.0);
        configure (lookaheadChoice);
    }

    void reset() noexcept
    {
        mins.reset();
        readerL.reset();
        readerR.reset();
        outReaderL.reset();
        outReaderR.reset();
        std::fill (delayL.begin(), delayL.end(), 0.0);
        std::fill (delayR.begin(), delayR.end(), 0.0);
        std::fill (avg1.begin(), avg1.end(), 1.0);
        std::fill (avg2.begin(), avg2.end(), 1.0);
        position = 0;
        clamped = 0;
        widthOne = (look + 2) / 2;
        widthTwo = look + 2 - widthOne;
        sum1 = (double) widthOne;
        sum2 = (double) widthTwo;
        release = 1.0;
        sustained = 1.0;
        gainNow = 1.0;
        gainStarted = false;
    }

    // A change of look-ahead restarts the limiter (the latency moves with it).
    void configure (int lookaheadChoice) noexcept
    {
        choice = std::clamp (lookaheadChoice, 0, kNumLookahead - 1);
        look = lookaheadSamples (fs, choice);
        latencySamples.store (look + kDetectorDelay, std::memory_order_relaxed);
        reset();
    }

    // In place. Everything that is not a number is silence.
    void process (float* left, float* right, int n, const EngineParams& p) noexcept
    {
        if (p.lookahead != choice)
            configure (p.lookahead);
        const double ceiling = std::pow (10.0, (double) std::min (p.ceilingDb, 0.0f) / 20.0);
        const double gainTarget = std::pow (10.0, (double) std::clamp (p.gainDb, 0.0f, kMaxGainDb) / 20.0);
        const double releaseStep = 1.0 - std::exp (-1.0 / std::max (0.001 * (double) p.releaseMs * fs, 1.0));
        const double gainSmooth = 1.0 - std::exp (-1.0 / (0.005 * fs));
        // AUTO RELEASE moves the step between a fast one and RELEASE's, by how long the reduction has lasted. Whatever the step, the envelope is never above `held`, so the ceiling does not depend on it.
        const double fastMs = std::min ((double) p.releaseMs, std::max (kAutoFastFloorMs, kAutoFastShare * (double) p.releaseMs));
        const double fastStep = 1.0 - std::exp (-1.0 / std::max (0.001 * fastMs * fs, 1.0));
        const double meanStep = 1.0 - std::exp (-1.0 / (0.001 * kAutoMeanMs * fs));
        if (! gainStarted)
        {
            gainNow = gainTarget;
            gainStarted = true;
        }
        const uint64_t mask = (uint64_t) (ring - 1), wait = (uint64_t) (look + kDetectorDelay), w1 = (uint64_t) widthOne, w2 = (uint64_t) widthTwo;
        float peakIn = 0.0f, peakOut = 0.0f;
        double deepest = 1.0;

        for (int i = 0; i < n; ++i)
        {
            gainNow += (gainTarget - gainNow) * gainSmooth;
            const double xl = finite (left[i]) * gainNow, xr = finite (right[i]) * gainNow;
            peakIn = std::max (peakIn, (float) std::fmax (std::fabs (finite (left[i])), std::fabs (finite (right[i]))));

            // What the limiter must do for the sound the detector is looking at now, kept for the whole look-ahead, then let go at the release.
            const double peak = std::fmax (readerL.push (xl, p.truePeak), readerR.push (xr, p.truePeak));
            const double required = peak > ceiling ? ceiling / peak : 1.0;
            const double held = mins.push (required, look + 1); // the two detector points either side of the sample that is heard now: see the README
            double step = releaseStep;
            if (p.autoRelease)
                step = fastStep + (releaseStep - fastStep) * std::clamp ((1.0 - sustained) / kAutoDepth, 0.0, 1.0); // fast after a short peak, RELEASE's once the reduction has lasted
            release = std::fmin (held, release + (1.0 - release) * step);
            sustained += (release - sustained) * meanStep;

            // Two means, each over about half the look-ahead (the window of both together is the look-ahead): a fall is spread over it with a rounded shape, so the
            // gain changes slowly next to a sample and the peaks between the samples are not made again by the gain itself. A mean never exceeds the smallest value it covers.
            const size_t slot = (size_t) (position & mask);
            sum1 += release - avg1[(size_t) ((position - w1) & mask)];
            avg1[slot] = release;
            const double stage1 = sum1 / (double) w1;
            sum2 += stage1 - avg2[(size_t) ((position - w2) & mask)];
            avg2[slot] = stage1;
            const double gain = std::fmin (1.0, sum2 / (double) w2);
            deepest = std::fmin (deepest, gain);

            delayL[slot] = xl;
            delayR[slot] = xr;
            const size_t read = (size_t) ((position - wait) & mask);
            const double yl = delayL[read] * gain, yr = delayR[read] * gain;
            // The gain already keeps the output under the ceiling; this is the end of the line for a sample that got past (and a counter, so a test can tell).
            if (std::fabs (yl) > ceiling * (1.0 + 1e-9) || std::fabs (yr) > ceiling * (1.0 + 1e-9))
                ++clamped;
            left[i] = (float) std::clamp (yl, -ceiling, ceiling);
            right[i] = (float) std::clamp (yr, -ceiling, ceiling);
            // What leaves, read the way the detector reads: the number OUT shows (and what a true-peak meter after the limiter should find).
            peakOut = std::max (peakOut, (float) std::fmax (outReaderL.push (left[i], p.truePeak), outReaderR.push (right[i], p.truePeak)));
            ++position;
        }

        const float reduction = (float) (-20.0 * std::log10 (std::fmax (deepest, 1e-6)));
        if (reduction > reductionMeter.load (std::memory_order_relaxed))
            reductionMeter.store (reduction, std::memory_order_relaxed);
        if (peakIn > inPeak.load (std::memory_order_relaxed))
            inPeak.store (peakIn, std::memory_order_relaxed);
        if (peakOut > outPeak.load (std::memory_order_relaxed))
            outPeak.store (peakOut, std::memory_order_relaxed);
    }

    uint64_t clamped = 0; // samples the final clamp had to touch (for the tests: with the limiter working it stays 0)

    std::atomic<int> latencySamples { 0 };
    std::atomic<float> reductionMeter { 0.0f }; // the deepest reduction since the editor last asked, in dB (positive)
    std::atomic<float> inPeak { 0.0f };         // the largest input sample since the editor last asked (linear)
    std::atomic<float> outPeak { 0.0f };        // the largest true peak (sample peak with TRUE PEAK off) of the output since the editor last asked (linear)

private:
    static inline double finite (float v) noexcept { return std::isfinite (v) ? (double) v : 0.0; }

    double fs = 48000.0;
    int choice = kDefaultLookahead, look = 96, ring = 1024;
    SlidingMin mins;
    pdsp::TruePeakReader readerL, readerR, outReaderL, outReaderR;
    std::vector<double> delayL, delayR, avg1, avg2;
    uint64_t position = 0;
    double sum1 = 1.0, sum2 = 1.0, release = 1.0, sustained = 1.0, gainNow = 1.0;
    int widthOne = 1, widthTwo = 1; // the two means: widthOne + widthTwo = look + 2, so together they reach back look samples
    bool gainStarted = false;
};
} // namespace aegis
