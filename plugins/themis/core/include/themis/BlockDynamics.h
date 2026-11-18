// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 The Pantheon authors

#pragma once

#include <algorithm>
#include <array>
#include <cmath>

namespace themis
{
// Block dynamics: Themis' own measure of how far a master's loud blocks stand above its average, from the 3 s blocks of the
// whole signal since the reset. It is NOT a certified or standard meter, and its figure is not comparable with the dynamic
// range that other meters report.
//
// Definition: each complete block (kBlockSeconds) has an RMS level (both channels' mean square, in dB) and a sample peak (the
// larger channel's, in dB). Sort the blocks by RMS, keep the loudest kLoudestFraction of them (at least one), and
//   block dynamics = (mean of their peak dB) - (mean of their RMS dB).
// A block of digital silence is not a block. Fewer than kMinBlocks blocks give no value.
//
// Memory is fixed: the blocks are kept as a histogram of their RMS in kBinDb steps, each bin carrying the count and the sums
// of its RMS and peak dB. The loudest blocks are then taken from the top bin down. The only difference from sorting every
// block is at the cut: a bin that the loudest fraction ends inside contributes its blocks at the bin's mean peak and RMS.
// A pure header: no JUCE, no allocation after construction.
namespace blockdynamics
{
inline constexpr double kBlockSeconds = 3.0;       // the block length (complete blocks only, counted from the reset)
inline constexpr double kLoudestFraction = 0.2;    // the share of blocks, by RMS, that the figure is taken over
inline constexpr int kMinBlocks = 5;               // complete, non-silent blocks before there is a figure (15 s)
inline constexpr double kSilentMeanSquare = 1.0e-12; // a block at or under this mean square is silence, not a block

// Rules of thumb, not measured data: a figure at or under kHeavilyLimitedDb reads as heavily limited, one at or over
// kDynamicDb as dynamic. Between them nothing is said.
inline constexpr float kHeavilyLimitedDb = 8.0f;
inline constexpr float kDynamicDb = 12.0f;

inline constexpr float kBinDb = 0.1f;        // the RMS resolution of the histogram
inline constexpr float kLowestRmsDb = -120.0f; // the bottom of the histogram (quieter blocks share the lowest bin)
inline constexpr float kHighestRmsDb = 6.0f;   // the top of the histogram (louder blocks share the highest bin)
inline constexpr int kBins = static_cast<int> ((kHighestRmsDb - kLowestRmsDb) / kBinDb + 0.5f);

class Histogram
{
public:
    void clear()
    {
        bins.fill (Bin {});
        blocks = 0;
    }

    // One complete, non-silent block: its RMS and its sample peak, both in dB.
    void add (float rmsDb, float peakDb)
    {
        const int idx = std::clamp (static_cast<int> (std::floor ((rmsDb - kLowestRmsDb) / kBinDb)), 0, kBins - 1);
        Bin& b = bins[static_cast<size_t> (idx)];
        ++b.count;
        b.sumRms += rmsDb;
        b.sumPeak += peakDb;
        ++blocks;
    }

    int count() const { return blocks; }

    // The block dynamics in dB, or false while there are too few blocks.
    bool value (float& outDb) const
    {
        if (blocks < kMinBlocks)
            return false;
        const int wanted = std::max (1, static_cast<int> (std::ceil (kLoudestFraction * blocks)));
        int left = wanted;
        double peakSum = 0.0, rmsSum = 0.0;
        for (int i = kBins - 1; i >= 0 && left > 0; --i)
        {
            const Bin& b = bins[static_cast<size_t> (i)];
            if (b.count == 0)
                continue;
            const int take = std::min (b.count, left);
            const double share = static_cast<double> (take) / b.count;
            peakSum += b.sumPeak * share;
            rmsSum += b.sumRms * share;
            left -= take;
        }
        outDb = static_cast<float> ((peakSum - rmsSum) / wanted);
        return true;
    }

private:
    struct Bin
    {
        int count = 0;
        double sumRms = 0.0, sumPeak = 0.0;
    };
    std::array<Bin, static_cast<size_t> (kBins)> bins {};
    int blocks = 0;
};
} // namespace blockdynamics
} // namespace themis
