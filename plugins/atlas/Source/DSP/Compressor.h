// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 The Pantheon authors

#pragma once

#include <algorithm>
#include <cmath>
#include <pantheon/dsp/MusicBands.h>

namespace atlas
{
inline constexpr int kBands = pdsp::bands::kCount;
inline constexpr int kCrossovers = pdsp::bands::kNumCrossovers;

// What a band's compressor works on. Stereo: left and right with one gain (the louder decides), so the image does not move.
// L/R and M/S: the two signals each with their own gain. Left, Right, Mid, Side: that one signal only; the other is untouched.
enum class Channels { stereo, dual, midSide, left, right, mid, side };
inline constexpr int kNumChannelModes = 7;

inline constexpr bool usesMidSide (Channels c) noexcept { return c == Channels::midSide || c == Channels::mid || c == Channels::side; }
inline constexpr bool runsFirst (Channels c) noexcept { return c != Channels::right && c != Channels::side; }   // left, or mid
inline constexpr bool runsSecond (Channels c) noexcept { return c != Channels::left && c != Channels::mid; }    // right, or side
inline constexpr bool isLinked (Channels c) noexcept { return c == Channels::stereo; }

struct BandParams
{
    bool on = true;
    Channels channels = Channels::stereo;
    double thresholdDb = 0.0, ratio = 2.0;
    double attackMs = 10.0, releaseMs = 120.0;
    double makeupDb = 0.0;
    bool match = false; // the band's output is brought back to the loudness of its input
    bool upward = false;    // it lifts what is under the threshold instead of turning down what is over it (up to kUpwardMaxBoostDb, and not what is far under it)
    bool sidechain = false; // its compressor listens to the same band of the sidechain input instead of its own (itself again when none is connected)
};

inline constexpr double kKneeDb = 6.0;

// Upward compression lifts a level that is under the threshold towards it, by (1 - 1 / ratio) dB for every dB it is under, through the
// same 6 dB knee, never by more than kUpwardMaxBoostDb, and not at all for a level more than kUpwardGateDb + kUpwardFadeDb under
// the threshold: that is noise and silence, and lifting them is what makes an upward compressor hiss. Between the two it fades out.
inline constexpr double kUpwardMaxBoostDb = 12.0, kUpwardGateDb = 30.0, kUpwardFadeDb = 10.0;

// The gain computer: how many dB are taken off at a level (peak, dB). Past the threshold the level rises by 1 / ratio dB per dB,
// through a 6 dB soft knee. `slope` is 1 - 1 / ratio.
inline double reductionDb (double levelDb, double thresholdDb, double slope) noexcept
{
    const double over = levelDb - thresholdDb;
    if (over >= kKneeDb * 0.5)
        return slope * over;
    if (over > -kKneeDb * 0.5)
        return slope * (over + kKneeDb * 0.5) * (over + kKneeDb * 0.5) / (2.0 * kKneeDb);
    return 0.0;
}

// How many dB an upward compressor adds at a level (peak, dB): the mirror of reductionDb.
inline double boostDb (double levelDb, double thresholdDb, double slope) noexcept
{
    const double under = thresholdDb - levelDb;
    double boost = 0.0;
    if (under >= kKneeDb * 0.5)
        boost = slope * under;
    else if (under > -kKneeDb * 0.5)
        boost = slope * (under + kKneeDb * 0.5) * (under + kKneeDb * 0.5) / (2.0 * kKneeDb);
    boost = std::min (boost, kUpwardMaxBoostDb);
    return boost * std::clamp ((kUpwardGateDb + kUpwardFadeDb - under) / kUpwardFadeDb, 0.0, 1.0);
}

// How loud one signal of a band is: its mean square, smoothed by two one-pole stages of half a cycle of the band's lowest
// frequency each (which takes the ripple at twice the signal's frequency out), read as the peak of a sine of the same power.
// So the threshold means the same for any attack, and the gain does not move inside a cycle of the lowest note in the band.
struct Level
{
    double stage1 = 0.0, stage2 = 0.0;
    void reset() noexcept { stage1 = stage2 = 0.0; }
    // Returns the square of the level (peak of the equivalent sine).
    inline double push (double x, double k) noexcept
    {
        stage1 += k * (x * x - stage1);
        stage2 += k * (stage1 - stage2);
        return 2.0 * stage2;
    }
};

// One compressor gain: the reduction in dB follows what the gain computer asks for, with the attack while it grows and the
// release while it comes back. The times act on the gain in dB, after the level is measured, so they are the times it takes.
struct GainFollower
{
    double thresholdDb = 0.0, slope = 0.0, attack = 1.0, release = 1.0;
    double floorSq = 1.0; // the squared level under which the gain computer asks for nothing (the bottom of the knee): no logarithm below it
    double ceilSq = 1.0, gateSq = 0.0; // upward: the squared level over which it asks for nothing (the top of the knee), and under which (the gate)
    bool upward = false;
    double detector = 1.0; // the level's smoothing coefficient
    bool on = false;

    void set (const BandParams& p, double sampleRate, double lowestHz) noexcept
    {
        detector = 1.0 - std::exp (-2.0 * std::max (lowestHz, 10.0) / sampleRate);
        on = p.on;
        thresholdDb = p.thresholdDb;
        slope = 1.0 - 1.0 / std::max (p.ratio, 1.0);
        attack = 1.0 - std::exp (-1.0 / (std::max (p.attackMs, 0.01) * 0.001 * sampleRate));
        release = 1.0 - std::exp (-1.0 / (std::max (p.releaseMs, 0.01) * 0.001 * sampleRate));
        floorSq = std::pow (10.0, (p.thresholdDb - kKneeDb * 0.5) / 10.0);
        upward = p.upward;
        ceilSq = std::pow (10.0, (p.thresholdDb + kKneeDb * 0.5) / 10.0);
        gateSq = std::pow (10.0, (p.thresholdDb - kUpwardGateDb - kUpwardFadeDb) / 10.0);
    }

    // Steps the reduction `gr` (dB: positive takes off, negative lifts, which only an upward band does) for a sample at the squared level
    // `levelSq`, and returns the gain to apply. The attack is the time the gain takes to give way to a louder level (a downward band turns
    // down, an upward one stops lifting), the release the time it takes to come back.
    inline double step (double& gr, double levelSq) const noexcept
    {
        double wanted = 0.0;
        if (on)
        {
            if (! upward)
                wanted = levelSq > floorSq ? reductionDb (10.0 * std::log10 (levelSq), thresholdDb, slope) : 0.0;
            else if (levelSq < ceilSq && levelSq > gateSq)
                wanted = -boostDb (10.0 * std::log10 (levelSq), thresholdDb, slope);
        }
        gr += (wanted > gr ? attack : release) * (wanted - gr);
        if (std::fabs (gr) < 1.0e-7)
        {
            if (upward ? wanted >= 0.0 : wanted <= 0.0)
                gr = 0.0; // lets a band that is switched off come to rest, and keeps denormals out
            return 1.0;
        }
        return std::exp (gr * -0.11512925464970229); // 10 ^ (-gr / 20)
    }
};
} // namespace atlas
