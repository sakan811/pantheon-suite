// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 The Pantheon authors

#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <pantheon/dsp/Biquad.h>

// One band of the equaliser: what it is (BandParams), the filter it makes (Section), and its response. No GUI, no JUCE; the engine, the editor's curve and the
// tests all read the response from here, so the curve on the screen is the filter that runs.
namespace apollo
{
inline constexpr int kBands = 8;
inline constexpr double kMinHz = 20.0, kMaxHz = 20000.0;
inline constexpr double kMaxGainDb = 18.0;
inline constexpr double kIdentityDb = 0.005; // a bell or a shelf of less than this is not run

enum class Type { bell, lowShelf, highShelf, highPass, lowPass, notch };
inline constexpr int kNumTypes = 6;
enum class Target { stereo, mid, side };
inline constexpr int kNumTargets = 3;
inline constexpr int kNumSlopes = 4;
inline constexpr int kSlopeOrder[kNumSlopes] = { 2, 4, 6, 8 }; // 12, 24, 36 and 48 dB per octave

inline constexpr bool hasGain (Type t) noexcept { return t == Type::bell || t == Type::lowShelf || t == Type::highShelf; }
inline constexpr bool hasSlope (Type t) noexcept { return t == Type::highPass || t == Type::lowPass; }

struct BandParams
{
    bool on = false;
    Type type = Type::bell;
    double freqHz = 1000.0, gainDb = 0.0, q = 1.0;
    int slope = 0;                 // for a high-pass or low-pass: index into kSlopeOrder
    Target target = Target::stereo; // which part of the sound the band works on
};

// A band that changes nothing: off, or a bell or shelf of 0 dB.
inline bool isIdentity (const BandParams& b) noexcept { return ! b.on || (hasGain (b.type) && std::fabs (b.gainDb) < kIdentityDb); }

// The filter of a band: one biquad, or a Butterworth cascade for a high-pass or low-pass.
struct Section
{
    pdsp::Biquad bq;
    pdsp::Butterworth bw;
    bool cascade = false;

    void design (const BandParams& b, double fs) noexcept
    {
        const double f = std::clamp (b.freqHz, kMinHz, std::min (kMaxHz, 0.45 * fs)), q = std::clamp (b.q, 0.1, 10.0);
        const double g = std::clamp (b.gainDb, -kMaxGainDb, kMaxGainDb);
        cascade = hasSlope (b.type);
        switch (b.type)
        {
            case Type::bell: bq.setPeak (fs, f, q, g); break;
            case Type::lowShelf: bq.setLowShelf (fs, f, q, g); break;
            case Type::highShelf: bq.setHighShelf (fs, f, q, g); break;
            case Type::notch: bq.setNotch (fs, f, q); break;
            case Type::highPass: bw.setup (pdsp::Butterworth::Type::highpass, kSlopeOrder[std::clamp (b.slope, 0, kNumSlopes - 1)], fs, f); break;
            case Type::lowPass: bw.setup (pdsp::Butterworth::Type::lowpass, kSlopeOrder[std::clamp (b.slope, 0, kNumSlopes - 1)], fs, f); break;
        }
    }

    void reset() noexcept { bq.reset(); bw.reset(); }
    inline double process (double x) noexcept { return cascade ? bw.process (x) : bq.process (x); }
    double powerAt (double cosW, double cos2W) const noexcept { return cascade ? bw.powerAt (cosW, cos2W) : bq.powerAt (cosW, cos2W); }
};

// Whether a band is heard by the part of the sound `path` (mid or side): stereo bands by both, a mid band by the mid, a side band by the side.
inline bool reaches (Target target, Target path) noexcept { return target == Target::stereo || target == path; }

// The response of the whole equaliser, in dB, at `hz`, for the mid path (bands on Stereo and Mid) or the side path (Stereo and Side). Left and right are the mid path
// when no band is on Side or Mid. Designed from the bands as given (no smoothing): this is the curve of the settings, the engine's is the same once it has caught up.
inline double responseDb (const std::array<BandParams, kBands>& bands, double fs, double hz, Target path = Target::mid) noexcept
{
    const double w = 2.0 * M_PI * std::clamp (hz, 1.0, 0.499 * fs) / fs, c1 = std::cos (w), c2 = std::cos (2.0 * w);
    double power = 1.0;
    for (const auto& b : bands)
        if (! isIdentity (b) && reaches (b.target, path))
        {
            Section s;
            s.design (b, fs);
            power *= s.powerAt (c1, c2);
        }
    return 10.0 * std::log10 (std::fmax (power, 1e-30));
}
} // namespace apollo
