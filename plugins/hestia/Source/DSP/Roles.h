// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 The Pantheon authors

#pragma once

#include "Depth.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

// The frequency space of a mix, as Hestia divides it: thirty third-octave bands, and for each role the claim it has on each of them.
// Plain C++, no JUCE (Tests.cpp includes it).
namespace hestia
{
inline constexpr int kBands = 30;
inline constexpr double kFirstBandHz = 25.0; // centres are 25 Hz * 10^(k/10): 25 Hz ... 19.9 kHz, a ratio of 1.2589 (a third of an octave)

inline double bandCentreHz (int k) noexcept { return kFirstBandHz * std::pow (10.0, k / 10.0); }

// The band whose centre is nearest to `hz` on a log axis.
inline int bandForHz (double hz) noexcept
{
    return std::clamp ((int) std::lround (10.0 * std::log10 (std::max (hz, 1.0) / kFirstBandHz)), 0, kBands - 1);
}

enum class Role
{
    kick, bass, drums, cymbals, leadVocal, backVocal, guitar, keys, pad, leadSynth, strings, fx, fullRange
};
inline constexpr int kNumRoles = 13;

inline const char* roleName (Role r) noexcept
{
    static constexpr const char* names[kNumRoles] = { "Kick", "Bass", "Drums", "Hats / Cymbals", "Lead Vocal", "Backing Vocal", "Guitar",
                                                      "Keys", "Pad", "Lead Synth", "Strings", "FX", "Full Range" };
    return names[std::clamp ((int) r, 0, kNumRoles - 1)];
}

struct Anchor { double hz, claim; };

// How much of each part of the spectrum a role is *for*: 1 is where the sound lives (a kick's thump, a voice's presence), 0 where it
// has nothing to say. Straight lines on a log-frequency axis between the anchors. These are rules of thumb from common mixing practice,
// not measurements of any sound: Importance moves a whole role up or down, and the display shows what each stem holds.
inline const std::vector<Anchor>& anchorsFor (Role r)
{
    static const std::array<std::vector<Anchor>, kNumRoles> table = {{
        /* kick      */ { { 20, 0.2 }, { 40, 0.7 }, { 60, 1.0 }, { 90, 0.8 }, { 150, 0.4 }, { 300, 0.1 }, { 2000, 0.1 }, { 3500, 0.45 }, { 6000, 0.15 }, { 20000, 0.0 } },
        /* bass      */ { { 20, 0.1 }, { 40, 0.5 }, { 60, 0.6 }, { 90, 0.8 }, { 130, 1.0 }, { 250, 0.9 }, { 500, 0.4 }, { 1000, 0.2 }, { 2500, 0.1 }, { 20000, 0.0 } },
        /* drums     */ { { 20, 0.0 }, { 80, 0.3 }, { 150, 0.6 }, { 250, 0.7 }, { 600, 0.3 }, { 2500, 0.3 }, { 4500, 0.7 }, { 8000, 0.3 }, { 20000, 0.1 } },
        /* cymbals   */ { { 20, 0.0 }, { 500, 0.0 }, { 2000, 0.2 }, { 6000, 0.7 }, { 10000, 1.0 }, { 16000, 0.7 }, { 20000, 0.4 } },
        /* leadVocal */ { { 20, 0.0 }, { 100, 0.1 }, { 200, 0.4 }, { 400, 0.6 }, { 1000, 0.9 }, { 2500, 1.0 }, { 4000, 0.9 }, { 6000, 0.6 }, { 10000, 0.4 }, { 20000, 0.1 } },
        /* backVocal */ { { 20, 0.0 }, { 150, 0.1 }, { 300, 0.4 }, { 800, 0.6 }, { 2000, 0.6 }, { 4000, 0.5 }, { 8000, 0.3 }, { 20000, 0.05 } },
        /* guitar    */ { { 20, 0.0 }, { 80, 0.2 }, { 200, 0.6 }, { 500, 0.7 }, { 1200, 0.8 }, { 3000, 0.8 }, { 5000, 0.5 }, { 10000, 0.2 }, { 20000, 0.0 } },
        /* keys      */ { { 20, 0.0 }, { 80, 0.3 }, { 250, 0.7 }, { 800, 0.8 }, { 2000, 0.6 }, { 5000, 0.3 }, { 10000, 0.1 }, { 20000, 0.0 } },
        /* pad       */ { { 20, 0.0 }, { 150, 0.3 }, { 400, 0.5 }, { 1000, 0.4 }, { 3000, 0.3 }, { 8000, 0.2 }, { 20000, 0.1 } },
        /* leadSynth */ { { 20, 0.0 }, { 150, 0.2 }, { 500, 0.7 }, { 1500, 0.9 }, { 3500, 0.9 }, { 7000, 0.5 }, { 14000, 0.2 }, { 20000, 0.05 } },
        /* strings   */ { { 20, 0.0 }, { 100, 0.2 }, { 300, 0.6 }, { 1000, 0.7 }, { 3000, 0.5 }, { 8000, 0.3 }, { 20000, 0.05 } },
        /* fx        */ { { 20, 0.1 }, { 20000, 0.1 } },
        /* fullRange */ { { 20, 0.4 }, { 20000, 0.4 } },
    }};
    return table[(size_t) std::clamp ((int) r, 0, kNumRoles - 1)];
}

// The claim of a role at a frequency.
inline double claimAt (Role r, double hz) noexcept
{
    const auto& a = anchorsFor (r);
    if (hz <= a.front().hz)
        return a.front().claim;
    if (hz >= a.back().hz)
        return a.back().claim;
    size_t i = 1;
    while (a[i].hz < hz)
        ++i;
    const double t = std::log (hz / a[i - 1].hz) / std::log (a[i].hz / a[i - 1].hz);
    return a[i - 1].claim + t * (a[i].claim - a[i - 1].claim);
}

// A role's claim on each band, scaled by how important the stem is: importance 0.5 leaves the claim as it is, 0 halves it, 1 makes it one and a
// half times (so an important stem can out-claim a role that usually owns the band). A stem placed at the back (`distance` 1) has about a third
// less claim (Depth.h, kDistanceYield): what is far gives way to what is near.
inline std::array<float, kBands> claims (Role r, float importance, float distance = 0.0f) noexcept
{
    std::array<float, kBands> c {};
    const float scale = (0.5f + std::clamp (importance, 0.0f, 1.0f)) * (1.0f - kDistanceYield * std::clamp (distance, 0.0f, 1.0f));
    for (int k = 0; k < kBands; ++k)
        c[(size_t) k] = (float) claimAt (r, bandCentreHz (k)) * scale;
    return c;
}
} // namespace hestia
