// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 The Pantheon authors

#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

namespace iris
{
// Where this track and another one (fed to the sidechain input) fight for the same frequencies. A third-octave band clashes
// when both are loud there (within kLoudDb of their own loudest band) and within kCloseDb of each other, so neither stands
// clear of the other. Neighbouring clashing bands are merged into one zone, and each zone says how far to cut this track so the
// other one sits kClearDb above it there. The display feeds it both input spectra on its timer (never the audio thread).
// The limits are rules of thumb, not measured data.
class MaskingMeter
{
public:
    static constexpr int kBands = 28;                    // third octaves, 31.5 Hz to 16 kHz
    static constexpr double kFirstHz = 31.5;
    static constexpr float kLoudDb = 24.0f, kCloseDb = 6.0f, kClearDb = 6.0f, kMaxCutDb = 12.0f;
    static constexpr float kAverageSeconds = 2.0f, kMinSeconds = 2.0f;

    struct Clash
    {
        double loHz = 0.0, hiHz = 0.0, centreHz = 0.0;
        float ownDb = 0.0f, otherDb = 0.0f; // the averaged levels at the zone's strongest band
        float cutDb = 0.0f;                 // how far to cut this track there (negative)
        double q = 4.3;
    };

    // What the meter can say about itself, so the display can explain an empty result instead of showing nothing.
    enum class Status { silent, otherSilent, measuring, clear, found };

    // Where both tracks are loud, and how far apart they are there (this track minus the other, mean of those bands).
    struct Overlap
    {
        int bands = 0;
        float gapDb = 0.0f;
    };

    MaskingMeter() { reset(); }

    static double centreHz (int band) { return kFirstHz * std::pow (2.0, band / 3.0); }

    void reset()
    {
        seconds = 0.0f;
        ownSilent = otherSilent = true;
        own.fill (-200.0f);
        other.fill (-200.0f);
    }

    // Both spectra hold numBins levels, bin i at i * binHz. Nothing is averaged while either side is silent.
    void update (const float* ownDb, const float* otherDb, int numBins, double binHz, float dtSeconds)
    {
        std::array<float, kBands> a {}, b {};
        ownSilent = ! bandLevels (ownDb, numBins, binHz, a);
        otherSilent = ! bandLevels (otherDb, numBins, binHz, b);
        if (ownSilent || otherSilent)
            return;
        const float w = seconds <= 0.0f ? 1.0f : 1.0f - std::exp (-dtSeconds / kAverageSeconds);
        for (size_t i = 0; i < (size_t) kBands; ++i)
        {
            own[i] += w * (a[i] - own[i]);
            other[i] += w * (b[i] - other[i]);
        }
        seconds += dtSeconds;
    }

    bool hasData() const { return seconds >= kMinSeconds; }
    float measuredSeconds() const { return seconds; }

    // Whether the last update heard something on each side (a stopped transport, or a sidechain nobody routed, is silence).
    bool hearsOwn() const { return ! ownSilent; }
    bool hearsOther() const { return ! otherSilent; }

    Status status() const
    {
        if (ownSilent)
            return Status::silent;
        if (otherSilent)
            return Status::otherSilent;
        if (! hasData())
            return Status::measuring;
        return clashes().empty() ? Status::clear : Status::found;
    }

    // Why there is no clash: the bands where both are loud, and whether one of them is simply louder than the other there.
    Overlap overlap() const
    {
        Overlap o;
        if (! hasData())
            return o;
        const float ownTop = *std::max_element (own.begin(), own.end()), otherTop = *std::max_element (other.begin(), other.end());
        double sum = 0.0;
        for (size_t i = 0; i < (size_t) kBands; ++i)
            if (own[i] >= ownTop - kLoudDb && other[i] >= otherTop - kLoudDb)
            {
                ++o.bands;
                sum += own[i] - other[i];
            }
        o.gapDb = o.bands > 0 ? (float) (sum / o.bands) : 0.0f;
        return o;
    }

    std::vector<Clash> clashes() const
    {
        std::vector<Clash> out;
        if (! hasData())
            return out;
        const float ownTop = *std::max_element (own.begin(), own.end()), otherTop = *std::max_element (other.begin(), other.end());
        const auto clashing = [&] (size_t i)
        {
            return own[i] >= ownTop - kLoudDb && other[i] >= otherTop - kLoudDb && std::fabs (own[i] - other[i]) <= kCloseDb;
        };
        for (size_t i = 0; i < (size_t) kBands;)
        {
            if (! clashing (i))
            {
                ++i;
                continue;
            }
            size_t j = i, strongest = i;
            while (j < (size_t) kBands && clashing (j))
            {
                if (own[j] + other[j] > own[strongest] + other[strongest])
                    strongest = j;
                ++j;
            }
            Clash c;
            c.loHz = centreHz ((int) i) * std::pow (2.0, -1.0 / 6.0);
            c.hiHz = centreHz ((int) j - 1) * std::pow (2.0, 1.0 / 6.0);
            c.centreHz = std::sqrt (c.loHz * c.hiHz);
            c.ownDb = own[strongest];
            c.otherDb = other[strongest];
            c.cutDb = -std::clamp (own[strongest] - (other[strongest] - kClearDb), 1.0f, kMaxCutDb);
            const double ratio = c.hiHz / c.loHz; // a bell as wide as the zone: Q = sqrt (r) / (r - 1)
            c.q = std::clamp (std::sqrt (ratio) / (ratio - 1.0), 0.5, 8.0);
            out.push_back (c);
            i = j;
        }
        return out;
    }

private:
    // The mean power of the bins in each third octave, in dB. False when everything is at the floor (silence).
    static bool bandLevels (const float* db, int numBins, double binHz, std::array<float, kBands>& out)
    {
        float loudest = -200.0f;
        for (int band = 0; band < kBands; ++band)
        {
            const double lo = centreHz (band) * std::pow (2.0, -1.0 / 6.0), hi = centreHz (band) * std::pow (2.0, 1.0 / 6.0);
            const int a = std::clamp ((int) std::ceil (lo / binHz), 1, numBins - 1), b = std::clamp ((int) std::floor (hi / binHz), a, numBins - 1);
            double power = 0.0;
            for (int k = a; k <= b; ++k)
                power += std::pow (10.0, db[k] / 10.0);
            out[(size_t) band] = (float) (10.0 * std::log10 (power / (b - a + 1) + 1.0e-30));
            loudest = std::max (loudest, out[(size_t) band]);
        }
        return loudest > -120.0f;
    }

    std::array<float, kBands> own {}, other {};
    float seconds = 0.0f;
    bool ownSilent = true, otherSilent = true;
};
} // namespace iris
