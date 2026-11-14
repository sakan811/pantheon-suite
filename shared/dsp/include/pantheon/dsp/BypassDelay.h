// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 The Pantheon authors

#pragma once

#include <algorithm>
#include <vector>

// A bypassed plugin that reports latency must still delay its audio by that latency, or the track jumps early against the rest of the mix the moment the host bypasses it
// (the host compensates for the reported latency whether the plugin is bypassed or not). `pdsp::BypassDelay` is that delay: a ring per channel, read `delay` samples back.
// Plain C++, no JUCE. A plugin calls `prepare` from prepareToPlay and `process` from its processBlockBypassed override, with the latency it reports.
namespace pdsp
{
class BypassDelay
{
public:
    // The longest delay that will be asked for, and the channels that will be delayed. Allocates: not for the audio thread.
    void prepare (int numChannels, int maxDelaySamples)
    {
        size = std::max (maxDelaySamples, 0) + 1;
        ring.assign ((size_t) std::max (numChannels, 0), std::vector<float> ((size_t) size, 0.0f));
        pos = 0;
    }

    void reset() noexcept
    {
        for (auto& r : ring)
            std::fill (r.begin(), r.end(), 0.0f);
        pos = 0;
    }

    // Delays the first `numChannels` channels in place by `delay` samples (clamped to what prepare allowed). No allocation.
    void process (float* const* channels, int numChannels, int numSamples, int delay) noexcept
    {
        const int d = std::clamp (delay, 0, size - 1);
        const int n = std::min (numChannels, (int) ring.size());
        if (d == 0)
        {
            // Nothing to delay, but keep the ring current, so a later change of the delay does not read old audio.
            for (int c = 0; c < n; ++c)
                for (int i = 0; i < numSamples; ++i)
                    ring[(size_t) c][(size_t) ((pos + i) % size)] = channels[c][i];
            pos = (pos + numSamples) % size;
            return;
        }
        for (int c = 0; c < n; ++c)
        {
            auto& r = ring[(size_t) c];
            int w = pos;
            for (int i = 0; i < numSamples; ++i)
            {
                r[(size_t) w] = channels[c][i];
                int rd = w - d;
                if (rd < 0)
                    rd += size;
                channels[c][i] = r[(size_t) rd];
                if (++w == size)
                    w = 0;
            }
        }
        pos = (pos + numSamples) % size;
    }

private:
    std::vector<std::vector<float>> ring;
    int size = 1, pos = 0;
};
} // namespace pdsp
