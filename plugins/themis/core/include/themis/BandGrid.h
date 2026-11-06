// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 The Pantheon authors

#pragma once

#include <algorithm>
#include <array>
#include <cmath>

namespace themis
{
// Fixed analysis grid shared by the analyzer, target curves and EQ fitting:
// 1/6-octave constant-Q bands from 20 Hz upward (61 bands, 20 Hz .. ~20.5 kHz).
struct BandGrid
{
    static constexpr int kPerOctave = 6;
    static constexpr int kNum = 61;
    static constexpr double kF0 = 20.0;
    static constexpr double kMaxHz = 20000.0; // top of the audible range: displays and device ranges span [kF0, kMaxHz]

    // Bands above this share of the sampling rate sit at the analysis' roll-off: displays and checks leave them out.
    static double usableMaxHz (double sampleRate) { return sampleRate * 0.47; }

    static double centreHz (int i) { return kF0 * std::pow (2.0, static_cast<double> (i) / kPerOctave); }
    static double lowEdgeHz (int i) { return centreHz (i) * std::pow (2.0, -0.5 / kPerOctave); }
    static double highEdgeHz (int i) { return centreHz (i) * std::pow (2.0, 0.5 / kPerOctave); }

    // Fractional band index for a frequency (clamped to the grid).
    static double indexForHz (double f)
    {
        return std::clamp (std::log2 (std::max (f, 1.0) / kF0) * kPerOctave, 0.0, static_cast<double> (kNum - 1));
    }

    // Normalisation region: curves are shifted so their mean over this range is 0 dB.
    static constexpr double kAnchorLoHz = 200.0;
    static constexpr double kAnchorHiHz = 5000.0;
};

using BandArray = std::array<float, BandGrid::kNum>;
} // namespace themis
