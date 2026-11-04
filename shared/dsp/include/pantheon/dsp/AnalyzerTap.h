// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 The Pantheon authors

#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <algorithm>
#include <array>

namespace pdsp
{
// Single-producer/single-consumer sample FIFO: the audio thread pushes, the editor pulls.
class AnalyzerFifo
{
public:
    static constexpr int capacity = 1 << 15;

    void push (const float* data, int n) noexcept
    {
        int s1, z1, s2, z2;
        fifo.prepareToWrite (n, s1, z1, s2, z2);
        std::copy_n (data, z1, storage.begin() + s1);
        if (z2 > 0) std::copy_n (data + z1, z2, storage.begin() + s2);
        fifo.finishedWrite (z1 + z2);
    }

    int pull (float* dest, int maxCount) noexcept
    {
        int s1, z1, s2, z2;
        fifo.prepareToRead (maxCount, s1, z1, s2, z2);
        std::copy_n (storage.begin() + s1, z1, dest);
        if (z2 > 0) std::copy_n (storage.begin() + s2, z2, dest + z1);
        fifo.finishedRead (z1 + z2);
        return z1 + z2;
    }

private:
    juce::AbstractFifo fifo { capacity };
    std::array<float, capacity> storage {};
};

// One FIFO per channel, so the display can analyse left and right separately instead of a mono
// sum (which would hide out-of-phase content).
struct StereoTap
{
    std::array<AnalyzerFifo, 2> channel;

    // `channels` holds nCh pointers; a mono signal feeds both sides.
    void push (const float* const* channels, int nCh, int n) noexcept
    {
        channel[0].push (channels[0], n);
        channel[1].push (channels[nCh > 1 ? 1 : 0], n);
    }
};
}
