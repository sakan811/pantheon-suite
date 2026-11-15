// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 The Pantheon authors

#pragma once

#include "Scales.h"

#include <array>
#include <algorithm>
#include <cmath>

namespace harmonia
{
// Follows the key of the song while the singer sings: a pitch-class histogram that forgets slowly
// (counted in seconds of singing, so silence does not erase it) is matched against the major and
// minor key profiles. The reported key only changes once a different key has been the clear
// winner for a while, so a few chromatic notes never move it but a modulation or a new section does.
class KeyTracker
{
public:
    struct Result
    {
        bool valid = false;
        int root = 0;
        bool minor = false;
        float score = 0.0f;
    };

    void prepare (double hopRateHz)
    {
        hopRate = (float) hopRateHz;
        decay = std::exp (-1.0f / (kMemorySeconds * hopRate));
        evalEvery = std::max (1, (int) std::lround (0.25 * hopRateHz));
        reset();
    }

    void reset()
    {
        hist.fill (0.0f);
        weight = 0.0f;
        hops = 0;
        pendingTime = 0.0f;
        current = {};
        pending = {};
    }

    // One voiced analysis hop: the (vibrato-free) sung pitch in MIDI notes, how sure the detector is (0..1)
    // and how fast that pitch is moving (cents per second). A pitch that is gliding or sits halfway between
    // two semitones says little about the key, so it counts for less.
    void push (float midi, float confidence, float speedCentsPerSec)
    {
        for (auto& h : hist)
            h *= decay;
        weight *= decay;

        const float nearest = std::round (midi);
        const float onGrid = 1.0f - 0.8f * std::min (1.0f, std::fabs (midi - nearest) * 2.0f);
        const float steady = 1.0f - std::clamp ((speedCentsPerSec - 40.0f) / 160.0f, 0.0f, 1.0f);
        const float w = std::max (0.0f, confidence - 0.5f) * 2.0f * onGrid * steady;
        hist[(size_t) pitchClass ((int) nearest)] += w;
        weight += w;

        if (++hops % evalEvery == 0)
            evaluate();
    }

    Result key() const { return current; }

    static constexpr float kMemorySeconds = 12.0f;  // of singing
    // Before the first guess: a total weight of kMinSeconds x hopRate x 0.25. A hop weighs 0..1 (less when unsure, off the grid or gliding), so ordinary
    // singing, about a quarter of full weight per hop, reaches it in about 6 s (what the README says); perfectly steady, confident notes would in about 1.5 s.
    static constexpr float kMinSeconds = 6.0f;
    static constexpr float kSwitchSeconds = 4.0f;   // a new key has to win this long
    static constexpr float kSwitchMargin = 0.10f;   // ... and beat the old one by this much

private:
    void evaluate()
    {
        if (weight < kMinSeconds * hopRate * 0.25f)
            return;

        const KeyGuess g = guessKey (hist.data());
        if (! current.valid)
        {
            current = { true, g.root, g.minor, g.score };
            return;
        }

        const float held = keyCorrelation (hist.data(), current.root, current.minor);
        current.score = held;
        // A relative major/minor pair has the same notes: moving between them changes nothing.
        const bool sameNotes = scaleMask (g.root, g.minor ? kMinorScale : kMajorScale)
                               == scaleMask (current.root, current.minor ? kMinorScale : kMajorScale);
        if (sameNotes || g.score < held + kSwitchMargin)
        {
            pendingTime = 0.0f;
            return;
        }

        if (pendingTime > 0.0f && pending.root == g.root && pending.minor == g.minor)
            pendingTime += (float) evalEvery / hopRate;
        else
            pendingTime = (float) evalEvery / hopRate;
        pending = { true, g.root, g.minor, g.score };

        if (pendingTime >= kSwitchSeconds)
        {
            current = pending;
            pendingTime = 0.0f;
        }
    }

    float hopRate = 250.0f, decay = 0.999f;
    int evalEvery = 62, hops = 0;
    std::array<float, 12> hist {};
    float weight = 0.0f, pendingTime = 0.0f;
    Result current, pending;
};
} // namespace harmonia
