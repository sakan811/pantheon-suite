// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 The Pantheon authors

#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

namespace iris
{
// Finds resonances in a spectrum: narrow peaks that stand out of their own neighbourhood and stay there, which is what a
// ringing room mode, a boxy drum or a harsh vocal formant looks like, as opposed to a note that comes and goes or a broad
// tilt. The display feeds it the analyzer's input spectrum on its timer (never the audio thread) and draws what it finds.
//
// The spectrum is read on a 1/24-octave grid (the loudest bin in each step), averaged over a few seconds in dB, and a step is
// a resonance when it is the highest point within +-1/12 octave and stands at least kMinProminenceDb above the mean of the
// grid from 1/12 to 1/3 octave either side. The limits are rules of thumb, not measured data.
class ResonanceFinder
{
public:
    static constexpr double kLowHz = 40.0, kHighHz = 16000.0;
    static constexpr int kStepsPerOctave = 24;
    static constexpr int kNear = 2, kFar = 8;            // 1/12 and 1/3 octave, in steps
    static constexpr float kMinProminenceDb = 6.0f;
    static constexpr float kRangeDb = 60.0f;             // ignore anything this far under the loudest step
    static constexpr float kAverageSeconds = 3.0f;
    static constexpr float kMinSeconds = 2.0f;           // nothing is reported before this much has been averaged
    static constexpr int kMaxResonances = 6;

    struct Resonance
    {
        double freqHz = 0.0;
        float levelDb = 0.0f;       // the averaged level at the peak
        float prominenceDb = 0.0f;  // how far it stands above its neighbourhood
        double q = 4.0;             // centre frequency over the width where it is 3 dB down
    };

    static constexpr int kSteps = 208; // 1 + 24 x log2 (16000 / 40) steps
    static double stepHz (int i) { return kLowHz * std::pow (2.0, (double) i / kStepsPerOctave); }

    void reset()
    {
        seconds = 0.0f;
        avg.fill (-200.0f);
    }

    // `binDb` holds numBins levels, bin i at i * binHz. Silence (everything at the floor) is not averaged in.
    void update (const float* binDb, int numBins, double binHz, float dtSeconds)
    {
        std::array<float, (size_t) kSteps> now {};
        float loudest = -200.0f;
        for (int i = 0; i < kSteps; ++i)
        {
            const double lo = stepHz (i) * std::pow (2.0, -0.5 / kStepsPerOctave), hi = stepHz (i) * std::pow (2.0, 0.5 / kStepsPerOctave);
            const int a = std::clamp ((int) std::ceil (lo / binHz), 1, numBins - 1), b = std::clamp ((int) std::floor (hi / binHz), a, numBins - 1);
            float peak = -200.0f;
            for (int k = a; k <= b; ++k)
                peak = std::max (peak, binDb[k]);
            now[(size_t) i] = peak;
            loudest = std::max (loudest, peak);
        }
        if (loudest < -120.0f)
            return; // a stopped transport says nothing about resonances
        const float w = seconds <= 0.0f ? 1.0f : 1.0f - std::exp (-dtSeconds / kAverageSeconds);
        for (int i = 0; i < kSteps; ++i)
            avg[(size_t) i] += w * (now[(size_t) i] - avg[(size_t) i]);
        seconds += dtSeconds;
    }

    std::vector<Resonance> find() const
    {
        std::vector<Resonance> out;
        if (seconds < kMinSeconds)
            return out;
        const float top = *std::max_element (avg.begin(), avg.end());
        for (int i = kFar; i < kSteps - kFar; ++i)
        {
            const float v = avg[(size_t) i];
            if (v < top - kRangeDb)
                continue;
            bool highest = true;
            for (int k = -kNear; k <= kNear && highest; ++k)
                highest = k == 0 || avg[(size_t) (i + k)] < v || (avg[(size_t) (i + k)] <= v && k > 0);
            if (! highest)
                continue;
            double sum = 0.0;
            int n = 0;
            for (int k = kNear + 1; k <= kFar; ++k)
            {
                sum += avg[(size_t) (i - k)] + avg[(size_t) (i + k)];
                n += 2;
            }
            const float prominence = v - (float) (sum / n);
            if (prominence < kMinProminenceDb)
                continue;

            // Width where the peak is 3 dB down, interpolated between grid steps.
            const auto edge = [&] (int dir)
            {
                for (int k = 1; k <= kFar; ++k)
                {
                    const float x = avg[(size_t) (i + dir * k)];
                    if (x <= v - 3.0f)
                    {
                        const float prev = avg[(size_t) (i + dir * (k - 1))];
                        const double frac = prev > x ? (double) (prev - (v - 3.0f)) / (double) (prev - x) : 1.0;
                        return (k - 1 + frac) / kStepsPerOctave;
                    }
                }
                return (double) kFar / kStepsPerOctave;
            };
            const double octaves = edge (-1) + edge (1);
            const double q = std::sqrt (std::pow (2.0, octaves)) / (std::pow (2.0, octaves) - 1.0);
            out.push_back ({ stepHz (i), v, prominence, std::clamp (q, 1.0, 20.0) });
        }
        std::sort (out.begin(), out.end(), [] (const Resonance& a, const Resonance& b) { return a.prominenceDb > b.prominenceDb; });
        if ((int) out.size() > kMaxResonances)
            out.resize (kMaxResonances);
        return out;
    }

private:
    std::array<float, (size_t) kSteps> avg = filled();
    float seconds = 0.0f;

    static std::array<float, (size_t) kSteps> filled()
    {
        std::array<float, (size_t) kSteps> a {};
        a.fill (-200.0f);
        return a;
    }
};
} // namespace iris
