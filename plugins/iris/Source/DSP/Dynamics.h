// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 The Pantheon authors

#pragma once

#include <array>
#include "Band.h"

namespace iris
{
// Measures how loud a band's own frequency region is: a filter that matches the band (band-pass for a bell or notch,
// low-pass for a low shelf, high-pass for a high shelf), then the mean square of what comes out, smoothed over about two
// cycles of the band's own frequency by two one-pole stages (which also takes the ripple at twice the signal frequency out
// of it). It reads as the peak level of a sine of the same power, so "-20 dB" means a sine whose peak is at -20 dBFS.
// Attack and release are not here: they act on the gain (see Engine), where the time they ask for is the time they take.
// It listens to what goes *into* the band, so the band's own boost cannot feed back into its detector.
class Detector
{
public:
    void prepare (double sampleRate) noexcept { fs = sampleRate; reset(); }
    void reset() noexcept { filter.reset(); filter2.reset(); stage1 = stage2 = 0.0; }

    void setFilter (BandType type, double f, double q) noexcept
    {
        f = clampFreq (f, fs);
        k = 1.0 - std::exp (-f / fs); // each stage: a time constant of one cycle
        if (type == BandType::lowShelf)
        {
            filter.setLowpass (fs, f, 0.7071);
            filter2.setLowpass (fs, f, 0.7071);
        }
        else if (type == BandType::highShelf)
        {
            filter.setHighpass (fs, f, 0.7071);
            filter2.setHighpass (fs, f, 0.7071);
        }
        else
        {
            const double dq = std::clamp (q, 0.7, 6.0);
            filter.setBandpass (fs, f, dq);
            filter2.setBandpass (fs, f, dq);
        }
    }

    inline void push (double x) noexcept
    {
        const double y = filter2.process (filter.process (x));
        stage1 += k * (y * y - stage1);
        stage2 += k * (stage1 - stage2);
    }

    double level() const noexcept { return std::sqrt (2.0 * std::max (stage2, 0.0)); }

private:
    double fs = 48000.0, k = 0.02, stage1 = 0.0, stage2 = 0.0;
    pdsp::Biquad filter, filter2; // two in series: a steeper skirt, so a neighbouring band does not trigger this one
};

// The gain computer: how far the dynamics move the band's gain at a given level (dB, signed like `rangeDb`).
// Past the threshold the gain moves by (1 - 1/ratio) dB per dB, through a 6 dB soft knee, and stops at the range.
inline double dynamicOffsetDb (const DynamicsParams& p, double levelDb) noexcept
{
    constexpr double knee = 6.0;
    const double over = p.mode == DynMode::above ? levelDb - p.thresholdDb : p.thresholdDb - levelDb;
    double x = 0.0;
    if (over >= knee * 0.5)        x = over;
    else if (over > -knee * 0.5)   x = (over + knee * 0.5) * (over + knee * 0.5) / (2.0 * knee);

    const double moved = x * (1.0 - 1.0 / std::max (p.ratio, 1.0));
    const double limit = std::fabs (p.rangeDb);
    return std::copysign (std::min (moved, limit), p.rangeDb);
}

// AUTO: the times a dynamic band uses when it is left to choose them. They come from two things.
// The band's frequency: the times are counted in cycles, so a band at 60 Hz is not asked to move within one cycle of what it
// hears (which is distortion) and a band at 8 kHz is not as slow as one at 60 Hz. And the programme: two gains are run, a
// fast one and a slow one that only builds up while the signal stays past the threshold, and the larger is used. A single
// hit is let go quickly; a held note or a dense passage is let go slowly, so the gain does not pump between its peaks.
struct AutoTimes
{
    static constexpr double kAttackCycles = 4.0, kReleaseCycles = 40.0;
    static constexpr double kMinAttackMs = 3.0, kMaxAttackMs = 80.0;
    static constexpr double kMinReleaseMs = 60.0, kMaxReleaseMs = 600.0;
    static constexpr double kSlowRatio = 6.0;  // the slow gain's release against the fast one's
    static constexpr double kSlowBuild = 4.0;  // and how long it takes to build up, in fast releases: a single hit leaves little of it

    double attackMs = 10.0, releaseMs = 120.0, slowAttackMs = 480.0, slowReleaseMs = 720.0;

    static AutoTimes forFrequency (double hz) noexcept
    {
        const double cycleMs = 1000.0 / std::max (hz, 1.0);
        AutoTimes t;
        t.attackMs = std::clamp (kAttackCycles * cycleMs, kMinAttackMs, kMaxAttackMs);
        t.releaseMs = std::clamp (kReleaseCycles * cycleMs, kMinReleaseMs, kMaxReleaseMs);
        t.slowAttackMs = kSlowBuild * t.releaseMs;
        t.slowReleaseMs = kSlowRatio * t.releaseMs;
        return t;
    }
};

// Whether a dynamic band is doing anything, over the last few seconds of what it heard: a band whose level never reaches its
// threshold is a static band that costs a detector, and one that sits at its full range all the time is a static band at
// GAIN + RANGE. Fed from the meters (the editor's timer), so it lives outside the audio thread and allocates nothing.
struct DynamicsUsage
{
    enum class Verdict { measuring, idle, pinned, working };

    static constexpr int kBuckets = 8;              // one second each: the window the verdict covers
    static constexpr double kBucketSeconds = 1.0;
    static constexpr double kSilentDb = -90.0;      // the detector's level below which the band hears nothing
    static constexpr double kIdleDb = 0.1;          // moved less than this: never engaged
    static constexpr double kPinnedShare = 0.9;     // never further from its full range than this share of it: always engaged

    void reset() noexcept { *this = {}; }

    void push (double levelDb, double movedDb, double rangeDb, double dt) noexcept
    {
        if (levelDb <= kSilentDb)
            return; // a stopped transport says nothing about the settings
        auto& b = buckets[(size_t) current];
        if (b.seconds <= 0.0)
            b = { 0.0, levelDb, levelDb, std::fabs (movedDb), std::fabs (movedDb) };
        b.seconds += dt;
        b.maxLevel = std::max (b.maxLevel, levelDb);
        b.minLevel = std::min (b.minLevel, levelDb);
        b.maxMoved = std::max (b.maxMoved, std::fabs (movedDb));
        b.minMoved = std::min (b.minMoved, std::fabs (movedDb));
        range = std::fabs (rangeDb);
        if (b.seconds >= kBucketSeconds)
        {
            current = (current + 1) % kBuckets;
            buckets[(size_t) current] = {};
        }
    }

    struct Summary { Verdict verdict = Verdict::measuring; double maxLevelDb = 0.0, minLevelDb = 0.0; };

    Summary summary() const noexcept
    {
        Summary s;
        double seconds = 0.0, maxMoved = 0.0, minMoved = 1.0e9;
        s.maxLevelDb = -1.0e9;
        s.minLevelDb = 1.0e9;
        for (const auto& b : buckets)
        {
            if (b.seconds <= 0.0)
                continue;
            seconds += b.seconds;
            s.maxLevelDb = std::max (s.maxLevelDb, b.maxLevel);
            s.minLevelDb = std::min (s.minLevelDb, b.minLevel);
            maxMoved = std::max (maxMoved, b.maxMoved);
            minMoved = std::min (minMoved, b.minMoved);
        }
        if (seconds < kBucketSeconds * (kBuckets - 2))
            return s;
        s.verdict = maxMoved < kIdleDb                            ? Verdict::idle
                  : range > kIdleDb && minMoved >= kPinnedShare * range ? Verdict::pinned
                                                                    : Verdict::working;
        return s;
    }

private:
    struct Bucket { double seconds = 0.0, maxLevel = 0.0, minLevel = 0.0, maxMoved = 0.0, minMoved = 0.0; };
    std::array<Bucket, kBuckets> buckets {};
    int current = 0;
    double range = 0.0;
};
} // namespace iris
