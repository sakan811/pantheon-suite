// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 The Pantheon authors

#pragma once

#include <pantheon/dsp/Biquad.h>
#include <pantheon/dsp/MusicBands.h>

#include <algorithm>
#include <array>
#include <cmath>

namespace iris
{
constexpr int maxBands = 8;
constexpr int numRegions = pdsp::bands::kCount; // the six main frequency bands of music: sub, bass, low-mid, midrange, upper-mid, high
constexpr int numCrossovers = numRegions - 1;
// Where the regions are cut by default: the edges of the six main bands (shared/dsp MusicBands.h), the same as Themis' AUDITION ranges
// and Hephaestus' band solo strip, so a region soloed here and a range soloed there are the same part of the spectrum.
constexpr std::array<double, numCrossovers> defaultCrossoverHz = pdsp::bands::defaultCrossovers();
constexpr double minHz = 20.0, maxHz = 20000.0;

// How the equaliser treats phase. Zero Latency runs the bands as minimum-phase filters (no delay, phase shift around each band);
// the linear modes give every band the same magnitude with no phase shift at all, at the price of a fixed delay (LinearPhase.h).
enum class PhaseMode { zeroLatency, linear, linearHq };

enum class BandType { bell, lowShelf, highShelf, lowCut, highCut, notch };

// Which signal a band works on. Left/Right/Mid/Side leave the rest of the stereo image exactly as it was.
enum class Placement { stereo, left, right, mid, side };

// When the dynamic part of a band engages: while the band's own level is above or below its threshold.
enum class DynMode { above, below };

enum class SatStyle { tape, tube, console };

inline constexpr bool hasGain (BandType t) noexcept { return t == BandType::bell || t == BandType::lowShelf || t == BandType::highShelf; }
inline constexpr bool isCut (BandType t) noexcept { return t == BandType::lowCut || t == BandType::highCut; }

// Cut slopes are Butterworth: 12, 24, 36 or 48 dB per octave are orders 2, 4, 6 and 8.
inline constexpr int orderForSlope (int slopeIndex) noexcept { return 2 * (std::clamp (slopeIndex, 0, 3) + 1); }

struct DynamicsParams
{
    DynMode mode = DynMode::above;
    double thresholdDb = -24.0;
    double ratio = 4.0;
    double rangeDb = -6.0;     // the most the gain moves by while the band is fully engaged (sign = direction)
    double attackMs = 10.0, releaseMs = 120.0;
};

// Everything one band is told. `freq`, `gainDb` and `q` are targets: the engine glides to them.
struct BandParams
{
    bool on = false;
    BandType type = BandType::bell;
    Placement place = Placement::stereo;
    double freq = 1000.0, gainDb = 0.0, q = 1.0;
    int slope = 1;             // cuts only: index into 12 / 24 / 36 / 48 dB per octave
    bool dynamic = false;
    bool external = false;     // the dynamics listen to the sidechain input instead of the band's own signal (when one is connected)
    DynamicsParams dyn;

    // Whether the dynamic part does anything: cuts and notches have no gain to move.
    bool isDynamic() const noexcept { return dynamic && hasGain (type); }
};

inline double clampFreq (double f, double fs) noexcept { return std::clamp (f, minHz, std::min (maxHz, fs * 0.45)); }

// One band's filter for one channel: a biquad for bells, shelves and notches, a Butterworth cascade for cuts.
class BandFilter
{
public:
    void configure (BandType t, double fs, double f, double q, double gainDb, int order) noexcept
    {
        type = t;
        f = clampFreq (f, fs);
        switch (t)
        {
            case BandType::bell:      bell.setPeak (fs, f, q, gainDb); break;
            case BandType::lowShelf:  bell.setLowShelf (fs, f, q, gainDb); break;
            case BandType::highShelf: bell.setHighShelf (fs, f, q, gainDb); break;
            case BandType::notch:     bell.setNotch (fs, f, q); break;
            case BandType::lowCut:    cut.setup (pdsp::Butterworth::Type::highpass, order, fs, f); break;
            case BandType::highCut:   cut.setup (pdsp::Butterworth::Type::lowpass, order, fs, f); break;
        }
    }

    void reset() noexcept { bell.reset(); cut.reset(); }
    inline double process (double x) noexcept { return isCut (type) ? cut.process (x) : bell.process (x); }

    double magnitudeAt (double fs, double f) const noexcept
    {
        return isCut (type) ? cut.magnitudeAt (fs, f) : bell.magnitudeAt (fs, f);
    }

    // |H|^2 from cos(w) and cos(2w): the same curve as magnitudeAt, for a whole grid at once.
    double powerAt (double cosW, double cos2W) const noexcept
    {
        return isCut (type) ? cut.powerAt (cosW, cos2W) : bell.powerAt (cosW, cos2W);
    }

private:
    BandType type = BandType::bell;
    pdsp::Biquad bell;
    pdsp::Butterworth cut;
};

// The curve a band draws, in dB, at one frequency (what the display sums). `dynamicDb` is where the dynamics have moved the gain to.
inline double bandResponseDb (const BandParams& b, double dynamicDb, double fs, double f)
{
    BandFilter filter;
    filter.configure (b.type, fs, b.freq, b.q, std::clamp (b.gainDb + dynamicDb, -36.0, 36.0), orderForSlope (b.slope));
    return 20.0 * std::log10 (std::max (filter.magnitudeAt (fs, f), 1.0e-9));
}

// Which of the four channels (L, R, M, S) a placement touches. Stereo touches L and R; Mid and Side are their own channels.
inline constexpr bool touchesLeft (Placement p) noexcept { return p == Placement::stereo || p == Placement::left; }
inline constexpr bool touchesRight (Placement p) noexcept { return p == Placement::stereo || p == Placement::right; }
inline constexpr bool touchesMid (Placement p) noexcept { return p == Placement::mid; }
inline constexpr bool touchesSide (Placement p) noexcept { return p == Placement::side; }
} // namespace iris
