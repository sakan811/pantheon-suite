// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 The Pantheon authors

#pragma once

#include <algorithm>
#include <array>
#include <cmath>

namespace atlas
{
// What a band's compressor is set to after it has listened to the band (LEARN): the four numbers that depend on the material.
struct Learned
{
    float thresholdDb = 0.0f, ratio = 2.0f, attackMs = 10.0f, releaseMs = 120.0f;
    float typicalDb = -120.0f; // the band's median level while it was not silent (not a setting: TARGET compares bands by it)
};

// Listens to one band's level, once per engine tick, and says how to set its compressor. Plain C++, no allocation: it runs on the
// audio thread (the engine feeds it the level the band's own detector read, the one THRESH is compared with).
//
//   LEVEL   A histogram of the level (1 dB bins) of the ticks that were not silence. `loud` is its 95th percentile, `typical` its
//           median. THRESH goes under `loud` by 3 dB or by 60 % of the spread between the two, whichever is more; RATIO takes off
//           a larger part of what is over it the wider the spread (1.3:1 for a steady band, 4:1 from 30 dB of spread). An upward
//           band puts THRESH at `typical`, so what is under the typical level is what it lifts, and RATIO from the spread under it.
//   TIMES   The level is followed as a zigzag with 6 dB of hysteresis: every peak that stood 6 dB over what came before it and then
//           fell 6 dB is an event. The time its last 6 dB took to rise is the *rise*, the time the first 6 dB took to fall the *fall*.
//           ATTACK is the median rise (the front of a hit gets through, then the gain closes), RELEASE three times the median fall
//           (the gain comes back more slowly than the band dies away, so the tail does not swell). A band with fewer than
//           kMinEvents events is steady: it gets the slow, safe kSteady* times.
class BandProfile
{
public:
    static constexpr double kListenSeconds = 4.0;   // how long a band is listened to
    static constexpr double kSkipSeconds = 0.3;     // the band has to start first (its detector starts from zero)
    static constexpr double kMinActiveSeconds = 0.4; // less than this of sound in the window and there is nothing to learn from
    static constexpr double kActiveDb = -75.0;      // under this a tick is silence
    static constexpr double kEventFloorDb = -60.0;  // a peak under this is not an event
    static constexpr double kHysteresisDb = 6.0;
    static constexpr double kMinOverDb = 3.0, kOverShare = 0.6;
    static constexpr double kSteadyAttackMs = 20.0, kSteadyReleaseMs = 250.0, kReleasePerFall = 3.0;
    static constexpr int kMinEvents = 3;

    // Starts a new listening. `tickMs` is the time between two pushes.
    void start (double tickMs) noexcept
    {
        dtMs = tickMs;
        skipTicks = (int) std::ceil (kSkipSeconds * 1000.0 / tickMs);
        totalTicks = (int) std::ceil (kListenSeconds * 1000.0 / tickMs);
        ticks = fed = active = numEvents = 0;
        hist.fill (0);
        rising = true;
        peak = trough = 0.0;
        peakIdx = 0;
    }

    // One tick's level (dB, peak of the equivalent sine). True when the listening is over.
    bool push (double levelDb) noexcept
    {
        if (ticks >= totalTicks)
            return true;
        if (++ticks > skipTicks)
            feed (std::clamp (levelDb, kLowestDb, kHighestDb));
        return ticks >= totalTicks;
    }

    // The settings, or false when the band was silent. Call once the listening is over.
    bool result (bool upward, Learned& out) noexcept
    {
        if ((double) active * dtMs < kMinActiveSeconds * 1000.0)
            return false;
        const double loud = percentile (0.95), typical = percentile (0.5), low = percentile (0.1);
        double threshold, share;
        if (! upward)
        {
            const double spread = loud - typical;
            threshold = loud - std::max (kMinOverDb, kOverShare * spread);
            share = std::clamp (0.25 + spread / 40.0, 0.25, 0.75);
        }
        else
        {
            threshold = typical;
            share = std::clamp (0.2 + (typical - low) / 40.0, 0.2, 0.6);
        }
        out.typicalDb = (float) typical;
        out.thresholdDb = (float) std::clamp (threshold, -60.0, 0.0);
        out.ratio = (float) std::clamp (1.0 / (1.0 - share), 1.0, 20.0);
        if (numEvents >= kMinEvents)
        {
            out.attackMs = (float) std::clamp (median (rise), 0.5, 100.0);
            out.releaseMs = (float) std::clamp (kReleasePerFall * median (fall), 30.0, 1500.0);
        }
        else
        {
            out.attackMs = (float) kSteadyAttackMs;
            out.releaseMs = (float) kSteadyReleaseMs;
        }
        return true;
    }

    int events() const noexcept { return numEvents; }

private:
    static constexpr double kLowestDb = -90.0, kHighestDb = 10.0;
    static constexpr int kBins = 100;       // 1 dB each, from kLowestDb up
    static constexpr int kRing = 1024;      // the last ticks of level, to look back over a rise
    static constexpr int kMaxEvents = 256;

    void feed (double e) noexcept
    {
        if (e >= kActiveDb)
        {
            ++hist[(size_t) std::min ((int) (e - kLowestDb), kBins - 1)];
            ++active;
        }
        ring[(size_t) (fed % kRing)] = (float) e;
        const int idx = fed++;
        if (idx == 0)
        {
            peak = e;
            peakIdx = 0;
            return;
        }
        if (rising)
        {
            if (e > peak)
            {
                peak = e;
                peakIdx = idx;
            }
            else if (e <= peak - kHysteresisDb)
            {
                event (idx);
                rising = false;
                trough = e;
            }
        }
        else if (e < trough)
        {
            trough = e;
        }
        else if (e >= trough + kHysteresisDb)
        {
            rising = true;
            peak = e;
            peakIdx = idx;
        }
    }

    // The peak at `peakIdx` has just been left: `idx` is the first tick 6 dB under it.
    void event (int idx) noexcept
    {
        if (peak < kEventFloorDb || numEvents >= kMaxEvents)
            return;
        const int oldest = std::max (0, idx - kRing + 1);
        int from = -1;
        for (int j = peakIdx - 1; j >= oldest; --j)
            if (ring[(size_t) (j % kRing)] <= peak - kHysteresisDb)
            {
                from = j;
                break;
            }
        if (from < 0 && oldest == 0)
            return; // the peak is the start of what was heard: no rise was seen
        const int riseTicks = from >= 0 ? peakIdx - from : peakIdx - oldest; // a rise longer than the ring is counted as the ring
        rise[(size_t) numEvents] = (float) (riseTicks * dtMs);
        fall[(size_t) numEvents] = (float) ((idx - peakIdx) * dtMs);
        ++numEvents;
    }

    double percentile (double q) const noexcept
    {
        const double target = q * (double) active;
        double seen = 0.0;
        for (int b = 0; b < kBins; ++b)
        {
            const double n = (double) hist[(size_t) b];
            if (seen + n >= target && n > 0.0)
                return kLowestDb + (double) b + (target - seen) / n; // inside the bin
            seen += n;
        }
        return kHighestDb;
    }

    double median (std::array<float, kMaxEvents>& v) const noexcept
    {
        const auto mid = v.begin() + numEvents / 2;
        std::nth_element (v.begin(), mid, v.begin() + numEvents);
        return (double) *mid;
    }

    double dtMs = 1.0;
    int skipTicks = 0, totalTicks = 0, ticks = 0, fed = 0, active = 0, numEvents = 0;
    std::array<int, kBins> hist {};
    std::array<float, kRing> ring {};
    std::array<float, kMaxEvents> rise {}, fall {};
    bool rising = true;
    double peak = 0.0, trough = 0.0;
    int peakIdx = 0;
};

// OUTPUT for LEARN ALL: the gain that makes the output as loud as the input over the same window. Plain C++.
//
// Why a measurement and not a formula: a listener prefers the louder of two versions, so a compressed sound with its makeup left
// at 0 dB sounds worse than the input for no other reason (A/B comparisons of dynamics are only fair at the same loudness; practice
// is within about 0.5 dB). The bands' gains, MATCH, MAKEUP, the detectors and the crossovers add up in ways no formula from the
// settings gets exactly right, so the engine measures the mean square of the input and of the sum of the bands before OUTPUT (the
// monitoring of solo and delta is left out) over the window, with the learned settings in effect. OUTPUT is linear after that sum, so
// the gain that equalises the two powers is exactly 10 log10 (in / out), whatever OUTPUT is set to.
struct LoudnessMatch
{
    static constexpr double kSilentPower = 1.0e-10; // -100 dBFS mean square: below it a window heard nothing

    // The gain in dB that makes `outPower` (before OUTPUT) equal `inPower`; false when either is silence (nothing to match).
    static bool gainDb (double inPower, double outPower, double& db) noexcept
    {
        if (! (inPower > kSilentPower) || ! (outPower > kSilentPower))
            return false;
        db = 10.0 * std::log10 (inPower / outPower);
        return true;
    }
};
} // namespace atlas
