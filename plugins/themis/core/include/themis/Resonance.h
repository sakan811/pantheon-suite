// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 The Pantheon authors

#pragma once

#include "BandGrid.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace themis
{
// Narrow rings in the long-term tonal curve: a band that stands above both of its sides, the way a resonance (a ringing mode,
// a harsh formant) does. A broad tonal balance does not: its bands are high together, so each one has a neighbour as high as
// itself, and a slope or a step has a side that is as high. Judged against the curve's own neighbours only, never against the
// target: a difference from the reference is taste (see MasterAdvisor.h).
//
// The curve is on the 1/6-octave BandGrid, so it cannot show a ring narrower than about a sixth of an octave: such a ring
// averages into its band and reads smaller than it is, and its frequency is only good to about a twelfth of an octave. The
// figures are a place to start listening, not a measurement of the ring.
namespace resonance
{
inline constexpr int kSkirt = 1;             // the band next to a band may be part of the same ring: not compared
inline constexpr int kSide = 2;              // the sides compared are the two bands after the skirt, on each side (+-1/3 to +-1/2 octave)
inline constexpr int kReach = kSkirt + kSide; // the bands looked at on each side of a judged band
inline constexpr float kMinStandOutDb = 4.0f; // a band this far above the louder of its two sides rings (rule of thumb)
inline constexpr size_t kMaxRings = 3;       // the worst that are reported
// Below this a sustained bass note makes the same narrow peak as a ring (a bass line in one key shows its notes), and the low
// end has its own checks (bass fundamental, low-end mono, low-end crest), so rings are judged from here up only.
inline constexpr double kLowestHz = 250.0;
inline constexpr double kCutQ = 8.0;         // the suggested cut is a narrow bell: Q 8 is about an eighth of an octave wide

struct Ring
{
    int band = 0;
    double freqHz = 0.0;     // the band's centre
    float levelDb = 0.0f;    // the curve's level there (anchor-normalised)
    float standOutDb = 0.0f; // how far it stands above the louder of its two sides
};

// The rings in the first `bands` bands of the curve (the ones the sample rate carries), the worst first, at most kMaxRings.
// Only bands with a full neighbourhood, from kLowestHz up, are judged.
inline std::vector<Ring> find (const BandArray& db, int bands)
{
    std::vector<Ring> all;
    const int last = std::clamp (bands, 0, BandGrid::kNum) - kReach; // exclusive
    for (int i = kReach; i < last; ++i)
    {
        if (BandGrid::centreHz (i) < kLowestHz)
            continue;
        const auto at = [&] (int k) { return db[static_cast<size_t> (k)]; };
        const float x = at (i);
        if (x <= at (i - 1) || x < at (i + 1)) // a top: the first of a flat pair is the one that counts
            continue;
        const float leftMean = 0.5f * (at (i - kSkirt - 1) + at (i - kSkirt - 2)), rightMean = 0.5f * (at (i + kSkirt + 1) + at (i + kSkirt + 2));
        const float standOut = x - std::max (leftMean, rightMean);
        if (standOut >= kMinStandOutDb)
            all.push_back ({ i, BandGrid::centreHz (i), x, standOut });
    }
    std::stable_sort (all.begin(), all.end(), [] (const Ring& a, const Ring& b) { return a.standOutDb > b.standOutDb; });
    if (all.size() > kMaxRings)
        all.resize (kMaxRings);
    return all;
}
} // namespace resonance
} // namespace themis
