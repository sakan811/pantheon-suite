// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 The Pantheon authors

#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

// How far back a stem sits, made from what distance does to a sound that is not a room: the air takes the highs, the level falls, the image narrows
// and the first reflections of the room, a few tens of milliseconds after the direct sound, come up. No tail and no feedback: every cue is a gain, a
// filter or a handful of taps, so a stem at 0% is the input bit for bit and the sound is never delayed. Plain C++ (Tests.cpp includes it).
namespace hestia
{
inline constexpr float kAirHz = 5000.0f;         // the corner of the high shelf the air takes
inline constexpr float kAirMaxCutDb = 9.0f;      // what the shelf takes at the back
inline constexpr float kLevelMaxCutDb = 4.0f;    // what the level falls by at the back
inline constexpr float kWidthMaxLoss = 0.3f;     // the share of the side signal that is lost at the back
inline constexpr float kReflectMax = 0.45f;      // the gain of the first reflections at the back
inline constexpr float kDistanceYield = 0.35f;   // a stem at the back has this much less claim on a band (Roles.h claims())

// What each cue is for a distance of 0 (at the front) to 1 (at the back).
struct DepthCues
{
    float airDb;       // the high shelf's gain (negative)
    float levelGain;   // a gain
    float widthLoss;   // 0 to kWidthMaxLoss: the side signal is reduced by this share
    float reflections; // the gain of the early reflections
};

inline DepthCues cuesFor (float distance) noexcept
{
    const float d = std::clamp (distance, 0.0f, 1.0f);
    return { -kAirMaxCutDb * d, std::pow (10.0f, -kLevelMaxCutDb * d / 20.0f), kWidthMaxLoss * d, kReflectMax * d };
}

// The first reflections of a room: four taps on each channel (different on the two, so they widen), up to 37 ms, summed and darkened by a one-pole, since
// a wall takes the highs. Each reflection is heard once: there is nothing that feeds back, and 40 ms after the last sample in there is silence.
class EarlyReflections
{
public:
    static constexpr int kTaps = 4;
    static constexpr double kDelayMs[2][kTaps] = { { 6.3, 13.1, 21.7, 31.9 }, { 8.7, 16.9, 25.3, 37.1 } };
    static constexpr float kTapGain[kTaps] = { 0.5f, 0.4f, 0.3f, 0.2f };
    static constexpr double kDampHz = 3500.0;

    void prepare (double sampleRate)
    {
        size_t size = 1;
        while ((double) size < 0.04 * sampleRate + 2.0)
            size <<= 1;
        line[0].assign (size, 0.0);
        line[1].assign (size, 0.0);
        mask = size - 1;
        for (int c = 0; c < 2; ++c)
            for (int t = 0; t < kTaps; ++t)
                tap[c][t] = (size_t) std::max (1.0, std::round (kDelayMs[c][t] * 0.001 * sampleRate));
        damp = 1.0 - std::exp (-2.0 * 3.14159265358979323846 * kDampHz / sampleRate);
        reset();
    }

    void reset() noexcept
    {
        std::fill (line[0].begin(), line[0].end(), 0.0);
        std::fill (line[1].begin(), line[1].end(), 0.0);
        state[0] = state[1] = 0.0;
        write = 0;
    }

    inline void process (double inL, double inR, double& outL, double& outR) noexcept
    {
        line[0][write] = inL;
        line[1][write] = inR;
        double sum[2] = { 0.0, 0.0 };
        for (int c = 0; c < 2; ++c)
            for (int t = 0; t < kTaps; ++t)
                sum[c] += (double) kTapGain[t] * line[(size_t) c][(write - tap[c][t]) & mask];
        write = (write + 1) & mask;
        state[0] += damp * (sum[0] - state[0]);
        state[1] += damp * (sum[1] - state[1]);
        outL = state[0];
        outR = state[1];
    }

    // The longest delay, in milliseconds: what the tests and the README quote.
    static constexpr double longestMs() noexcept { return kDelayMs[1][kTaps - 1]; }

private:
    std::array<std::vector<double>, 2> line;
    size_t mask = 0, write = 0;
    size_t tap[2][kTaps] {};
    double damp = 0.0, state[2] {};
};
} // namespace hestia
