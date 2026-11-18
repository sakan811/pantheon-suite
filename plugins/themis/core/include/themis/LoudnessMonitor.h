// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 The Pantheon authors

#pragma once

#include "BlockDynamics.h"
#include "FrameHistory.h"
#include "Loudness.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>

namespace themis
{
// Mono translation: the integrated loudness of the mono sum (L+R)/2 (played on both speakers, so fed to a second
// LoudnessMonitor as (m, m)) against the stereo one. For identical channels the two are equal. Uncorrelated channels of equal
// level already lose half their power in a mono sum (about 3 dB, the baseline the mono fold strip uses too), so the rule
// counts only the loss beyond that: a master whose channels cancel in a mono sum is the translation risk.
namespace monoloud
{
// What two uncorrelated channels of equal level lose in a mono sum: 10 log10 (2), half their power.
inline constexpr float kUncorrelatedLossDb = 3.0103f;
// Rule of thumb, no source claimed: a mono sum that loses this many LU more than uncorrelated channels do is worth a look.
inline constexpr float kWorthALookLu = 1.0f;

struct Comparison
{
    bool have = false;        // both measured (kMinSeconds each) and the stereo programme is not silent
    bool monoSilent = false;  // the mono sum is silent while the stereo is not (the channels cancel)
    float monoLufs = LoudnessMeter::kSilence; // the mono sum's integrated loudness
    float deltaLu = 0.0f;     // mono minus stereo, LU: negative = the mono sum reads lower (a floor when monoSilent)
    bool worthALook = false;  // the mono sum loses kWorthALookLu more than uncorrelated channels do, or is silent
};

inline Comparison compare (float stereoLufs, bool stereoMeasured, float monoLufs, bool monoMeasured)
{
    Comparison c;
    const float cleared = LoudnessMeter::kSilence + 1.0f;
    c.have = stereoMeasured && monoMeasured && stereoLufs > cleared;
    if (!c.have)
        return c;
    c.monoLufs = monoLufs;
    c.monoSilent = monoLufs <= cleared;
    c.deltaLu = c.monoSilent ? LoudnessMeter::kSilence - stereoLufs : monoLufs - stereoLufs;
    c.worthALook = c.monoSilent || c.deltaLu <= -(kUncorrelatedLossDb + kWorthALookLu);
    return c;
}
} // namespace monoloud

// How far the loudest 3 s since the reset sit above the integrated loudness, in LU: a readout (the loudest section of a
// track against its average). Nothing is judged on it, so there is no limit here.
struct LoudestSection
{
    bool have = false;
    float aboveAverageLu = 0.0f;
};

inline LoudestSection loudestSection (float maxShortTermLufs, float integratedLufs)
{
    LoudestSection s;
    const float cleared = LoudnessMeter::kSilence + 1.0f;
    s.have = maxShortTermLufs > cleared && integratedLufs > cleared;
    s.aboveAverageLu = s.have ? maxShortTermLufs - integratedLufs : 0.0f;
    return s;
}

// Programme loudness (ITU-R BS.1770 / EBU R128) of the signal flowing through, for judging a master against streaming
// normalisation and for the peak-to-loudness ratio: LoudnessMeter on the audio thread, the results published through
// atomics. It also keeps the block dynamics (blockdynamics::Histogram, fed from the same samples). Monitoring only: the
// samples are read, never written.
//
// Threading: process() runs on the audio thread (two biquads per channel per sample; once per 100 ms it walks a 900-bin
// histogram; once per 3 s block it walks the block histogram). requestReset() is safe from any thread and is applied by
// the next process(). prepare() must not overlap with process().
class LoudnessMonitor
{
public:
    // The integrated value needs at least this long to mean anything (the meter's own minimum is 400 ms).
    static constexpr double kMinSeconds = 3.0;

    void prepare (double sampleRate)
    {
        fs = sampleRate;
        meter.prepare (fs);
        blockLen = std::max (1, static_cast<int> (std::lround (blockdynamics::kBlockSeconds * fs)));
        clear();
    }

    void requestReset() { resetFlag.store (true, std::memory_order_release); }

    // Audio thread.
    void process (const float* l, const float* r, int n)
    {
        if (resetFlag.exchange (false, std::memory_order_acq_rel))
            clear();
        meter.process (l, r, n);
        accumulateBlocks (l, r, n);
        samples += static_cast<uint64_t> (n);
        published.integrated.store (meter.integrated(), std::memory_order_relaxed);
        published.shortTerm.store (meter.shortTerm(), std::memory_order_relaxed);
        published.momentary.store (meter.momentary(), std::memory_order_relaxed);
        published.range.store (meter.loudnessRange(), std::memory_order_relaxed);
        const double elapsed = static_cast<double> (samples) / fs;
        if (elapsed >= kMinSeconds) // before a full 3 s window the short-term value is a partial average
            published.maxShortTerm.store (loudestShortTerm = std::max (loudestShortTerm, meter.shortTerm()), std::memory_order_relaxed);
        published.seconds.store (elapsed, std::memory_order_relaxed);
    }

    // A pending reset counts as "no data" right away, even if no audio is flowing to apply it (a stopped host): every
    // reader below sees the cleared meter, so no readout keeps the old programme next to others that were reset.
    bool hasData() const { return seconds() >= kMinSeconds && integrated() > LoudnessMeter::kSilence + 1.0f; }

    float integrated() const { return read (published.integrated, LoudnessMeter::kSilence); }       // LUFS since the reset, gated
    float shortTerm() const { return read (published.shortTerm, LoudnessMeter::kSilence); }         // LUFS over the last 3 s
    float maxShortTerm() const { return read (published.maxShortTerm, LoudnessMeter::kSilence); }   // loudest 3 s since the reset
    double seconds() const { return read (published.seconds, 0.0); }                                // audio measured since the reset
    float momentary() const { return read (published.momentary, LoudnessMeter::kSilence); }         // LUFS over the last 400 ms
    float loudnessRange() const { return read (published.range, 0.0f); }                            // LU, EBU Tech 3342, since the reset
    // Block dynamics (blockdynamics::, BlockDynamics.h): Themis' own figure over the 3 s blocks since the reset. False until
    // there are enough blocks (and while a reset is pending).
    bool hasBlockDynamics() const { return read (published.haveDynamics, false); }
    float blockDynamicsDb() const { return read (published.dynamicsDb, 0.0f); }

    // The short-term value as a time graph takes it (any thread): the history floor until hasData() and while the value is
    // the meter's silence, so a partial window or a gated passage plots nothing.
    float shortTermForHistory() const { return hasData() && shortTerm() > LoudnessMeter::kSilence + 1.0f ? shortTerm() : kHistoryFloorDb; }

private:
    template <typename T>
    T read (const std::atomic<T>& value, T cleared) const
    {
        return resetFlag.load (std::memory_order_acquire) ? cleared : value.load (std::memory_order_relaxed);
    }

    // Samples of the block in progress go to the block histogram once they fill one (non-finite samples count as silence,
    // like the meter's). A block that is silent is dropped; the figure is taken over the complete, non-silent ones.
    void accumulateBlocks (const float* l, const float* r, int n)
    {
        for (int i = 0; i < n; ++i)
        {
            const float xl = std::isfinite (l[i]) ? l[i] : 0.0f, xr = std::isfinite (r[i]) ? r[i] : 0.0f;
            blockSquares += static_cast<double> (xl) * xl + static_cast<double> (xr) * xr;
            blockPeak = std::max (blockPeak, std::max (std::abs (xl), std::abs (xr)));
            if (++blockSamples >= blockLen)
                finishBlock();
        }
    }

    void finishBlock()
    {
        const double meanSquare = blockSquares / (2.0 * blockSamples); // both channels, as the mean over both
        if (meanSquare > blockdynamics::kSilentMeanSquare && blockPeak > 0.0f)
            dynamics.add (static_cast<float> (powerToDb (meanSquare, 0.0)), linToDb (blockPeak));
        blockSamples = 0;
        blockSquares = 0.0;
        blockPeak = 0.0f;
        float value = 0.0f;
        const bool have = dynamics.value (value);
        published.dynamicsDb.store (value, std::memory_order_relaxed);
        published.haveDynamics.store (have, std::memory_order_relaxed);
    }

    void clear()
    {
        meter.reset();
        samples = 0;
        dynamics.clear();
        blockSamples = 0;
        blockSquares = 0.0;
        blockPeak = 0.0f;
        published.haveDynamics.store (false, std::memory_order_relaxed);
        published.dynamicsDb.store (0.0f, std::memory_order_relaxed);
        loudestShortTerm = LoudnessMeter::kSilence;
        published.integrated.store (LoudnessMeter::kSilence, std::memory_order_relaxed);
        published.shortTerm.store (LoudnessMeter::kSilence, std::memory_order_relaxed);
        published.maxShortTerm.store (LoudnessMeter::kSilence, std::memory_order_relaxed);
        published.seconds.store (0.0, std::memory_order_relaxed);
        published.momentary.store (LoudnessMeter::kSilence, std::memory_order_relaxed);
        published.range.store (0.0f, std::memory_order_relaxed);
    }

    double fs = 48000.0;
    LoudnessMeter meter;
    uint64_t samples = 0;
    float loudestShortTerm = LoudnessMeter::kSilence;
    std::atomic<bool> resetFlag { false };

    // The block dynamics: the block in progress, and the histogram of the finished ones (audio thread only).
    int blockLen = 0;
    int blockSamples = 0;
    double blockSquares = 0.0;
    float blockPeak = 0.0f;
    blockdynamics::Histogram dynamics;

    struct Published
    {
        std::atomic<float> integrated { LoudnessMeter::kSilence }, shortTerm { LoudnessMeter::kSilence }, maxShortTerm { LoudnessMeter::kSilence };
        std::atomic<float> momentary { LoudnessMeter::kSilence }, range { 0.0f };
        std::atomic<double> seconds { 0.0 };
        std::atomic<float> dynamicsDb { 0.0f };
        std::atomic<bool> haveDynamics { false };
    } published;
};
} // namespace themis
