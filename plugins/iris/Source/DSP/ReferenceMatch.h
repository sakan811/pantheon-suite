// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 The Pantheon authors

#pragma once

#include "Band.h"

#include <pantheon/dsp/BandLink.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <sstream>
#include <string>
#include <vector>

namespace iris
{
// The long-term shape of a spectrum on a 1/6-octave grid, and what it takes to make one track's shape follow another's.
// A SpectrumProfile averages what the display's analyzer sees (on the display's timer, never the audio thread); a Shape is
// that average with the level taken out (0 dB = the mean over 200 Hz to 5 kHz), which is what two tracks can be compared by.
// `matchBands` turns the difference between two shapes into a few equaliser bands. Every limit is a rule of thumb.
struct ProfileGrid
{
    static constexpr int kBands = 61;
    static constexpr int kPerOctave = 6;
    static constexpr double kF0 = 20.0;
    static constexpr double kAnchorLoHz = 200.0, kAnchorHiHz = 5000.0; // the level of a Shape is taken here

    static double centreHz (int i) { return kF0 * std::pow (2.0, static_cast<double> (i) / kPerOctave); }
    static double edgeHz (int i, int side) { return centreHz (i) * std::pow (2.0, 0.5 * side / kPerOctave); } // side: -1 low edge, +1 high edge
    static bool inAnchor (int i) { return centreHz (i) >= kAnchorLoHz && centreHz (i) <= kAnchorHiHz; }
};

// A band with nothing in it (silent, or above what the sample rate carries) is stored as kNoLevelDb in a Shape.
inline constexpr float kNoLevelDb = -99.0f;
using Shape = std::array<float, ProfileGrid::kBands>;
using ShapeMask = std::array<bool, ProfileGrid::kBands>;

class SpectrumProfile
{
public:
    static constexpr float kAverageSeconds = 30.0f; // a running mean until this much has been heard, then a slow average
    static constexpr float kMinSeconds = 3.0f;      // less than this is not a profile
    static constexpr float kSilenceDb = -110.0f;    // a spectrum whose loudest band is under this is not averaged

    SpectrumProfile() { reset(); }

    void reset()
    {
        seconds_ = 0.0;
        power.fill (0.0);
    }

    // `db` holds numBins levels of the input without the display's tilt, bin k at k * binHz.
    void update (const float* db, int numBins, double binHz, float dtSeconds)
    {
        std::array<float, ProfileGrid::kBands> now {};
        float loudest = -200.0f;
        for (int b = 0; b < ProfileGrid::kBands; ++b)
        {
            now[static_cast<size_t> (b)] = bandLevelDb (db, numBins, binHz, b);
            loudest = std::max (loudest, now[static_cast<size_t> (b)]);
        }
        if (loudest < kSilenceDb)
            return;
        const double w = std::max (dtSeconds / (seconds_ + dtSeconds), 1.0 - std::exp (-dtSeconds / kAverageSeconds));
        for (size_t b = 0; b < power.size(); ++b)
            power[b] += w * (std::pow (10.0, now[b] / 10.0) - power[b]);
        seconds_ += dtSeconds;
    }

    bool hasData() const { return seconds_ >= kMinSeconds; }
    double seconds() const { return seconds_; }

    float levelDb (int band) const { return static_cast<float> (10.0 * std::log10 (power[static_cast<size_t> (band)] + 1.0e-20)); }

    // The mean level over the anchor region, in dB: where a Shape sits in the absolute display.
    float anchorLevelDb() const
    {
        double sum = 0.0;
        int n = 0;
        for (int b = 0; b < ProfileGrid::kBands; ++b)
            if (ProfileGrid::inAnchor (b))
            {
                sum += levelDb (b);
                ++n;
            }
        return n > 0 ? static_cast<float> (sum / n) : -200.0f;
    }

    // The profile with its level taken out. `valid` marks the bands that carry something (below `usableMaxHz` and not silent);
    // the others read kNoLevelDb.
    bool shape (double usableMaxHz, Shape& out, ShapeMask& valid) const
    {
        if (!hasData())
            return false;
        const float anchor = anchorLevelDb();
        for (int b = 0; b < ProfileGrid::kBands; ++b)
        {
            const auto i = static_cast<size_t> (b);
            valid[i] = ProfileGrid::centreHz (b) <= usableMaxHz && levelDb (b) > kSilenceDb;
            out[i] = valid[i] ? levelDb (b) - anchor : kNoLevelDb;
        }
        return true;
    }

private:
    // The mean power of the bins inside a band; a band narrower than a bin (the lowest ones) reads the level between the nearest two.
    static float bandLevelDb (const float* db, int numBins, double binHz, int band)
    {
        const double lo = ProfileGrid::edgeHz (band, -1) / binHz, hi = ProfileGrid::edgeHz (band, +1) / binHz;
        const int a = std::clamp (static_cast<int> (std::ceil (lo)), 1, numBins - 1), b = static_cast<int> (std::floor (hi));
        if (b < a)
        {
            const double pos = std::clamp (ProfileGrid::centreHz (band) / binHz, 1.0, static_cast<double> (numBins - 2));
            const int k = static_cast<int> (pos);
            return db[k] + static_cast<float> (pos - k) * (db[k + 1] - db[k]);
        }
        double sum = 0.0;
        for (int k = a; k <= std::min (b, numBins - 1); ++k)
            sum += std::pow (10.0, db[k] / 10.0);
        return static_cast<float> (10.0 * std::log10 (sum / (std::min (b, numBins - 1) - a + 1) + 1.0e-30));
    }

    std::array<double, ProfileGrid::kBands> power {};
    double seconds_ = 0.0;
};

// ---- a Shape as text (the session keeps the reference this way) and from another plugin's curve ----------------------------
inline std::string shapeToText (const Shape& s)
{
    std::ostringstream out;
    out.setf (std::ios::fixed);
    out.precision (1);
    for (size_t i = 0; i < s.size(); ++i)
        out << (i == 0 ? "" : ",") << s[i];
    return out.str();
}

inline bool shapeFromText (const std::string& text, Shape& out)
{
    std::istringstream in (text);
    std::string item;
    size_t n = 0;
    Shape s {};
    while (std::getline (in, item, ','))
    {
        char* end = nullptr;
        const double v = std::strtod (item.c_str(), &end);
        if (end == item.c_str() || !std::isfinite (v) || n >= s.size())
            return false;
        s[n++] = static_cast<float> (v);
    }
    if (n != s.size())
        return false;
    out = s;
    return true;
}

// A curve from another plugin, resampled to the grid and levelled like a Shape (0 dB = the mean over the anchor region).
inline bool shapeFromCurve (const pdsp::LinkCurve& c, Shape& out)
{
    if (!c.valid())
        return false;
    double sum = 0.0;
    int n = 0;
    for (int b = 0; b < ProfileGrid::kBands; ++b)
    {
        out[static_cast<size_t> (b)] = static_cast<float> (c.at (ProfileGrid::centreHz (b)));
        if (ProfileGrid::inAnchor (b))
        {
            sum += out[static_cast<size_t> (b)];
            ++n;
        }
    }
    for (auto& v : out)
        v -= static_cast<float> (sum / std::max (n, 1));
    return true;
}

// ---- making one shape follow another -----------------------------------------------------------------------------------------
namespace match
{
inline constexpr double kLoHz = 30.0, kHiHz = 18000.0; // the part of the spectrum that is matched
inline constexpr float kMinDb = 1.0f;                   // a difference under this is left alone
inline constexpr float kMaxDb = 12.0f;                  // and no band asks for more than this
inline constexpr float kHalfExtent = 0.5f;              // a band is as wide as the difference stays above this share of its peak
inline constexpr int kMaxBands = 6;                     // leaves two of Iris' eight slots free
inline constexpr double kMinQ = 0.4, kMaxQ = 2.0;      // a match follows the broad shape of a track, not its individual notes
inline constexpr double kShelfMinOctaves = 1.0;         // a difference that reaches the end of the range and is this wide is a shelf

struct Band
{
    BandType type = BandType::bell;
    double freqHz = 1000.0, gainDb = 0.0, q = 1.0;
};

inline double bellQ (double octaves)
{
    const double span = std::pow (2.0, std::max (octaves, 0.3));
    return std::clamp (std::sqrt (span) / (span - 1.0), kMinQ, kMaxQ);
}

// The equaliser bands that bring `live` towards `reference`: the difference (smoothed over about two thirds of an octave, at
// most kMaxDb) is taken apart one band at a time, largest first, each band as wide as the difference stays above half its peak and
// each one's own response taken off before the next is looked for. A difference that reaches an end of the matched range and
// is wider than an octave there becomes a shelf. Bands the reference has nothing in (kNoLevelDb) are left out. Bands come back in
// frequency order.
inline bool hasLevel (float shapeDb) { return shapeDb > kNoLevelDb + 1.0f; }

inline std::vector<Band> matchBands (const Shape& live, const Shape& reference, const ShapeMask& valid, double sampleRate, int maxBands = kMaxBands)
{
    constexpr int n = ProfileGrid::kBands;
    std::array<bool, n> use {};
    std::array<float, n> diff {};
    int first = -1, last = -1;
    for (int i = 0; i < n; ++i)
    {
        const double f = ProfileGrid::centreHz (i);
        use[static_cast<size_t> (i)] = valid[static_cast<size_t> (i)] && hasLevel (reference[static_cast<size_t> (i)]) && f >= kLoHz && f <= std::min (kHiHz, sampleRate * 0.45);
        if (!use[static_cast<size_t> (i)])
            continue;
        diff[static_cast<size_t> (i)] = reference[static_cast<size_t> (i)] - live[static_cast<size_t> (i)];
        first = first < 0 ? i : first;
        last = i;
    }
    std::vector<Band> out;
    if (first < 0)
        return out;

    std::array<float, n> res {}; // the smoothed difference that is left to explain
    for (int i = first; i <= last; ++i)
    {
        const auto at = [&] (int k) { return diff[static_cast<size_t> (std::clamp (k, first, last))]; };
        res[static_cast<size_t> (i)] = std::clamp ((at (i - 2) + 2.0f * at (i - 1) + 3.0f * at (i) + 2.0f * at (i + 1) + at (i + 2)) / 9.0f, -kMaxDb, kMaxDb);
    }

    for (int k = 0; k < maxBands; ++k)
    {
        int peak = first;
        for (int i = first; i <= last; ++i)
            if (std::abs (res[static_cast<size_t> (i)]) > std::abs (res[static_cast<size_t> (peak)]))
                peak = i;
        const float top = res[static_cast<size_t> (peak)];
        if (std::abs (top) < kMinDb)
            break;
        const auto holds = [&] (int i) { return res[static_cast<size_t> (i)] * top > 0.0f && std::abs (res[static_cast<size_t> (i)]) >= kHalfExtent * std::abs (top); };
        int lo = peak, hi = peak;
        while (lo > first && holds (lo - 1))
            --lo;
        while (hi < last && holds (hi + 1))
            ++hi;
        const double octaves = static_cast<double> (hi - lo + 1) / ProfileGrid::kPerOctave;

        Band band;
        if (lo == first && octaves >= kShelfMinOctaves)
        {
            band.type = BandType::lowShelf;
            band.freqHz = ProfileGrid::centreHz (hi);
            band.q = 0.7071;
            float sum = 0.0f;
            for (int i = lo; i <= hi; ++i)
                sum += res[static_cast<size_t> (i)];
            band.gainDb = sum / static_cast<float> (hi - lo + 1);
        }
        else if (hi == last && octaves >= kShelfMinOctaves)
        {
            band.type = BandType::highShelf;
            band.freqHz = ProfileGrid::centreHz (lo);
            band.q = 0.7071;
            float sum = 0.0f;
            for (int i = lo; i <= hi; ++i)
                sum += res[static_cast<size_t> (i)];
            band.gainDb = sum / static_cast<float> (hi - lo + 1);
        }
        else
        {
            band.type = BandType::bell;
            band.freqHz = ProfileGrid::centreHz (peak);
            band.q = bellQ (octaves);
            band.gainDb = top;
        }
        band.gainDb = std::round (band.gainDb * 10.0) / 10.0;

        BandParams p;
        p.type = band.type;
        p.freq = band.freqHz;
        p.q = band.q;
        p.gainDb = band.gainDb;
        for (int i = first; i <= last; ++i)
            res[static_cast<size_t> (i)] -= static_cast<float> (bandResponseDb (p, 0.0, sampleRate, ProfileGrid::centreHz (i)));
        out.push_back (band);
    }
    std::sort (out.begin(), out.end(), [] (const Band& a, const Band& b) { return a.freqHz < b.freqHz; });
    return out;
}

// How far `live` is from `reference` after the bands: the RMS of what is left over the matched range, in dB (the same
// measure before the bands, with an empty list).
inline float remainingDb (const Shape& live, const Shape& reference, const ShapeMask& valid, double sampleRate, const std::vector<Band>& bands)
{
    double sum = 0.0;
    int count = 0;
    for (int i = 0; i < ProfileGrid::kBands; ++i)
    {
        const double f = ProfileGrid::centreHz (i);
        if (!valid[static_cast<size_t> (i)] || !hasLevel (reference[static_cast<size_t> (i)]) || f < kLoHz || f > std::min (kHiHz, sampleRate * 0.45))
            continue;
        double d = reference[static_cast<size_t> (i)] - live[static_cast<size_t> (i)];
        for (const auto& b : bands)
        {
            BandParams p;
            p.type = b.type;
            p.freq = b.freqHz;
            p.q = b.q;
            p.gainDb = b.gainDb;
            d -= bandResponseDb (p, 0.0, sampleRate, f);
        }
        sum += d * d;
        ++count;
    }
    return count > 0 ? static_cast<float> (std::sqrt (sum / count)) : 0.0f;
}
} // namespace match
} // namespace iris
