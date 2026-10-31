// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 The Pantheon authors

#pragma once

#include "Common.h"
#include "FrameHistory.h"
#include "TruePeak.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <functional>

namespace themis
{
// What the clip meter saw since the last time it was asked (one analysis frame's worth of audio).
struct ClipWindow
{
    float samplePeak = 0.0f; // linear, largest |sample| of either channel
    float truePeak = 0.0f;   // linear, largest inter-sample (4x oversampled) peak of either channel
    uint32_t clipEvents = 0;  // flat-topped plateaus (see ClipMeter)
    uint32_t clipSamples = 0; // samples inside those plateaus
    uint32_t overSamples = 0; // samples at or above 0 dBFS
};

// Everything since the last reset, for the CLIPPING readout.
struct ClipTotals
{
    bool have = false;
    float samplePeakDb = kHistoryFloorDb;
    float truePeakDb = kHistoryFloorDb;
    uint64_t clipEvents = 0, clipSamples = 0, overSamples = 0;

    // Exact: only used to tell whether a readout has changed.
    bool operator== (const ClipTotals& o) const
    {
        const std::equal_to<float> same;
        return have == o.have && same (samplePeakDb, o.samplePeakDb) && same (truePeakDb, o.truePeakDb) && clipEvents == o.clipEvents
               && clipSamples == o.clipSamples && overSamples == o.overSamples;
    }
    bool operator!= (const ClipTotals& o) const { return !(*this == o); }
};

// A linear peak as a history level: dBFS, or the history floor for silence.
inline float peakToDb (float linear) { return linear > 1.0e-5f ? std::max (linToDb (linear), kHistoryFloorDb) : kHistoryFloorDb; }

// Clipping and peak detection on a stereo signal. Monitoring only: the samples are read, never written.
//
//   * Sample peak and true peak (4x oversampled, TruePeakDetector) in dB, per analysis frame and since the reset.
//   * OVER: samples at or above 0 dBFS. A float stream carries them without harm, but the converter or a lossy encoder
//     will clip them.
//   * CLIP: a flat top, i.e. a plateau of at least kMinRun consecutive samples with the same value, at least
//     kFlatMinLevel high, that the waveform reached with a step of at least kEntryStep (a hard clipper's signature at any
//     ceiling, not only 0 dBFS). The entry-step test is what keeps the quantised crest of a loud low sine from a 16-bit
//     source from counting. Limits: a synthesised square wave or a held DC level is a plateau too, and a clip that is
//     smoothed afterwards (a soft clipper, a lowpass) leaves no plateau to find.
//
// Threading: process() runs on the audio thread (a few multiplies per sample for the oversampler, no allocation).
// Everything it publishes is an atomic: the per-frame accumulators are drained by take() on the analysis thread and the
// totals are read by the UI. requestReset() is safe from any thread and is applied by the next process(); prepare() must
// not overlap with process().
class ClipMeter
{
public:
    static constexpr float kOverLevel = 1.0f;
    static constexpr float kFlatMinLevel = 0.5f;  // -6 dBFS
    static constexpr int kMinRun = 4;             // samples
    static constexpr float kSameSample = 1.0e-6f; // two samples closer than this are "the same value"
    static constexpr float kEntryStep = 1.0e-4f;  // the plateau must have been reached with a step at least this big

    void prepare()
    {
        reset();
        resetFlag.store (false, std::memory_order_relaxed);
    }

    void requestReset() { resetFlag.store (true, std::memory_order_release); }

    // Audio thread.
    void process (const float* l, const float* r, int n)
    {
        if (resetFlag.exchange (false, std::memory_order_acq_rel))
            reset();

        ClipWindow w;
        const float* channels[2] = { l, r };
        for (int c = 0; c < 2; ++c)
        {
            auto& s = state[c];
            for (int i = 0; i < n; ++i)
                step (s, channels[c][i], w);
        }
        publish (w, static_cast<uint64_t> (n));
    }

    // Analysis thread: everything seen since the previous call.
    ClipWindow take()
    {
        ClipWindow w;
        w.samplePeak = window.samplePeak.exchange (0.0f, std::memory_order_acq_rel);
        w.truePeak = window.truePeak.exchange (0.0f, std::memory_order_acq_rel);
        w.clipEvents = window.clipEvents.exchange (0, std::memory_order_acq_rel);
        w.clipSamples = window.clipSamples.exchange (0, std::memory_order_acq_rel);
        w.overSamples = window.overSamples.exchange (0, std::memory_order_acq_rel);
        return w;
    }

    // Any thread. A pending reset counts as "no data" right away, even if no audio is flowing to apply it.
    ClipTotals totals() const
    {
        ClipTotals t;
        t.have = totalSamples.load (std::memory_order_acquire) > 0 && !resetFlag.load (std::memory_order_acquire);
        if (!t.have)
            return t;
        t.samplePeakDb = peakToDb (totalSamplePeak.load (std::memory_order_relaxed));
        t.truePeakDb = peakToDb (totalTruePeak.load (std::memory_order_relaxed));
        t.clipEvents = totalClipEvents.load (std::memory_order_relaxed);
        t.clipSamples = totalClipSamples.load (std::memory_order_relaxed);
        t.overSamples = totalOverSamples.load (std::memory_order_relaxed);
        return t;
    }

private:
    struct Channel
    {
        TruePeakDetector oversampler;
        float previous = 0.0f;
        int plateau = 0;       // length of the current run of equal, high samples
        float entryStep = 0.0f; // how far the run's first sample was from the one before it
    };

    static void endPlateau (Channel& s, ClipWindow& w)
    {
        if (s.plateau >= kMinRun && s.entryStep >= kEntryStep)
        {
            ++w.clipEvents;
            w.clipSamples += static_cast<uint32_t> (s.plateau);
        }
        s.plateau = 0;
    }

    static void step (Channel& s, float x, ClipWindow& w)
    {
        const float a = std::abs (x);
        if (!std::isfinite (a)) // a glitched sample is not a peak: leave the detectors (and the totals) alone
            return;
        w.samplePeak = std::max (w.samplePeak, a);
        w.truePeak = std::max ({ w.truePeak, a, s.oversampler.process (x) });
        if (a >= kOverLevel)
            ++w.overSamples;

        if (s.plateau > 0 && std::abs (x - s.previous) <= kSameSample && a >= kFlatMinLevel)
            ++s.plateau;
        else
        {
            endPlateau (s, w);
            if (a >= kFlatMinLevel)
            {
                s.plateau = 1;
                s.entryStep = std::abs (x - s.previous);
            }
        }
        s.previous = x;
    }

    static void raiseTo (std::atomic<float>& target, float v)
    {
        float cur = target.load (std::memory_order_relaxed);
        while (v > cur && !target.compare_exchange_weak (cur, v, std::memory_order_relaxed))
        {
        }
    }

    void publish (const ClipWindow& w, uint64_t samples)
    {
        raiseTo (window.samplePeak, w.samplePeak);
        raiseTo (window.truePeak, w.truePeak);
        window.clipEvents.fetch_add (w.clipEvents, std::memory_order_relaxed);
        window.clipSamples.fetch_add (w.clipSamples, std::memory_order_relaxed);
        window.overSamples.fetch_add (w.overSamples, std::memory_order_relaxed);

        // The totals have this thread as their only writer, so a load + store is enough.
        totalSamplePeak.store (std::max (totalSamplePeak.load (std::memory_order_relaxed), w.samplePeak), std::memory_order_relaxed);
        totalTruePeak.store (std::max (totalTruePeak.load (std::memory_order_relaxed), w.truePeak), std::memory_order_relaxed);
        totalClipEvents.store (totalClipEvents.load (std::memory_order_relaxed) + w.clipEvents, std::memory_order_relaxed);
        totalClipSamples.store (totalClipSamples.load (std::memory_order_relaxed) + w.clipSamples, std::memory_order_relaxed);
        totalOverSamples.store (totalOverSamples.load (std::memory_order_relaxed) + w.overSamples, std::memory_order_relaxed);
        totalSamples.store (totalSamples.load (std::memory_order_relaxed) + samples, std::memory_order_release);
    }

    void reset()
    {
        for (auto& s : state)
        {
            s.oversampler.reset();
            s.previous = 0.0f;
            s.plateau = 0;
            s.entryStep = 0.0f;
        }
        take();
        totalSamples.store (0, std::memory_order_relaxed);
        totalSamplePeak.store (0.0f, std::memory_order_relaxed);
        totalTruePeak.store (0.0f, std::memory_order_relaxed);
        totalClipEvents.store (0, std::memory_order_relaxed);
        totalClipSamples.store (0, std::memory_order_relaxed);
        totalOverSamples.store (0, std::memory_order_relaxed);
    }

    Channel state[2];
    std::atomic<bool> resetFlag { false };

    struct Window
    {
        std::atomic<float> samplePeak { 0.0f }, truePeak { 0.0f };
        std::atomic<uint32_t> clipEvents { 0 }, clipSamples { 0 }, overSamples { 0 };
    } window;

    std::atomic<float> totalSamplePeak { 0.0f }, totalTruePeak { 0.0f };
    std::atomic<uint64_t> totalClipEvents { 0 }, totalClipSamples { 0 }, totalOverSamples { 0 };
    std::atomic<uint64_t> totalSamples { 0 };
};
} // namespace themis
