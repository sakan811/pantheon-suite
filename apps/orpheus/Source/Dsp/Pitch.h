// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 The Pantheon authors

#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

// Pitch in Orpheus is a MIDI note number as a double (69 = A4 = 440 Hz, a cent is 0.01), measured against 440 Hz. The reference
// tuning of a song (A4 = 432, 442 ...) only shifts where the scale grid sits (Scales.h); the pitch itself is always absolute.
namespace orpheus
{
inline double hzToMidi (double hz) { return 69.0 + 12.0 * std::log2 (hz / 440.0); }
inline double midiToHz (double midi) { return 440.0 * std::exp2 ((midi - 69.0) / 12.0); }

// The pitch of a recording, one value per frame. Frame i is centred on sample i * hop. Nothing here is edited: edits are
// notes (NoteModel.h) laid over this and turned into a plan (Plan.h).
struct Contour
{
    double sampleRate = 48000.0;
    int hop = 240;                     // samples between frames (about 5 ms)
    std::vector<float> midi;           // detected pitch; meaningful where voiced
    std::vector<float> confidence;     // 0..1, how periodic the signal is (the correlation at the chosen period)
    std::vector<float> levelDb;        // loudness of the frame, dB below full scale (negative)
    std::vector<uint8_t> voiced;

    int numFrames() const noexcept { return (int) midi.size(); }
    bool isVoiced (int frame) const noexcept { return frame >= 0 && frame < numFrames() && voiced[(size_t) frame] != 0; }
    double frameSeconds (int frame) const noexcept { return (double) frame * hop / sampleRate; }
    int frameAtSample (double sample) const noexcept
    {
        return std::clamp ((int) std::lround (sample / hop), 0, std::max (0, numFrames() - 1));
    }

    // The pitch at any sample: linear between frame centres, using the voiced neighbour where only one side is voiced.
    double midiAt (double sample) const noexcept
    {
        const int n = numFrames();
        if (n == 0)
            return 69.0;
        const double pos = std::clamp (sample / hop, 0.0, (double) (n - 1));
        const int i = std::min ((int) pos, n - 1), j = std::min (i + 1, n - 1);
        const double f = pos - i;
        const bool vi = voiced[(size_t) i] != 0, vj = voiced[(size_t) j] != 0;
        if (vi && vj)
            return midi[(size_t) i] + f * (midi[(size_t) j] - midi[(size_t) i]);
        if (vi)
            return midi[(size_t) i];
        if (vj)
            return midi[(size_t) j];
        return midi[(size_t) (f < 0.5 ? i : j)];
    }
};
} // namespace orpheus
