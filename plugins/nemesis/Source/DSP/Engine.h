// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 The Pantheon authors

#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <pantheon/dsp/Biquad.h>

// The whole de-esser: no GUI, no JUCE. Nothing is allocated in process().
namespace nemesis
{
enum class Mode { splitBand, wideband };
inline constexpr int kNumModes = 2;
enum class Detection { absolute, relative };
inline constexpr int kNumDetections = 2;

inline constexpr double kAttackMs = 0.3;       // fixed: a sibilant is over in tens of milliseconds
inline constexpr double kEnvelopeFallMs = 2.0; // the detector's peak hold between the cycles of the band
inline constexpr double kBodyMs = 30.0;        // the broadband level RELATIVE detection compares with
inline constexpr double kKneeDb = 6.0;
inline constexpr double kRestDb = 1e-3;        // a reduction under this, with nothing to reduce, is let go to exactly 0
inline constexpr double kFloorDb = -120.0;

struct EngineParams
{
    float freqHz = 6500.0f;     // the centre of the sibilance
    float q = 1.5f;             // the width of what is listened to (and, in SPLIT-BAND, of what is turned down)
    float thresholdDb = -30.0f; // absolute: dBFS of the band; relative: dB of the band under the whole sound
    float rangeDb = 8.0f;       // the most it takes off
    float releaseMs = 40.0f;
    Mode mode = Mode::splitBand;
    Detection detection = Detection::absolute;
    bool listen = false;        // hear the band the detector hears
};

// The reduction for a level `over` the threshold: it holds the band at the threshold (infinite ratio), softly over a knee, up to the range.
inline double reductionDb (double levelDb, double thresholdDb, double rangeDb) noexcept
{
    const double over = levelDb - thresholdDb;
    double r;
    if (2.0 * over < -kKneeDb)
        r = 0.0;
    else if (2.0 * std::fabs (over) <= kKneeDb)
        r = (over + 0.5 * kKneeDb) * (over + 0.5 * kKneeDb) / (2.0 * kKneeDb);
    else
        r = over;
    return std::clamp (r, 0.0, std::fmax (rangeDb, 0.0));
}

class Engine
{
public:
    void prepare (double sampleRate)
    {
        fs = sampleRate;
        attackCoef = coefOf (kAttackMs);
        envFall = std::exp (-1.0 / (0.001 * kEnvelopeFallMs * fs));
        bodyCoef = coefOf (kBodyMs);
        reset();
    }

    void reset() noexcept
    {
        for (auto& c : ch)
            c = Channel();
        gr = 0.0;
        designedFor = { -1.0, -1.0 };
        started = false;
    }

    // In place. Everything that is not a number is silence.
    void process (float* left, float* right, int n, const EngineParams& p) noexcept
    {
        const double f = std::clamp ((double) p.freqHz, 1000.0, std::fmin (16000.0, 0.45 * fs)), q = std::clamp ((double) p.q, 0.3, 8.0);
        if (designedFor[0] != f || designedFor[1] != q)
        {
            designedFor = { f, q };
            for (auto& c : ch)
                c.band.setBandpass (fs, f, q); // the coefficients move, the states stay
        }
        const double releaseCoef = coefOf (p.releaseMs);
        float deepest = 0.0f;

        for (int i = 0; i < n; ++i)
        {
            const double x[2] = { finite (left[i]), finite (right[i]) };
            double band[2], level = 0.0, body = 0.0;
            for (int c = 0; c < 2; ++c)
            {
                auto& s = ch[(size_t) c];
                band[c] = s.band.process (x[c]);
                s.env = std::fmax (std::fabs (band[c]), s.env * envFall);
                s.body += bodyCoef * (x[c] * x[c] - s.body);
                level = std::fmax (level, s.env);
                body = std::fmax (body, s.body);
            }
            // One gain for both channels, so the image does not move.
            double levelDb = level > 1e-6 ? 20.0 * std::log10 (level) : kFloorDb;
            if (p.detection == Detection::relative)
                levelDb -= body > 1e-12 ? 10.0 * std::log10 (body * 2.0) : kFloorDb; // the whole sound, read as the peak of a sine
            const double wanted = reductionDb (levelDb, p.thresholdDb, p.rangeDb);
            gr += (wanted - gr) * (wanted > gr ? attackCoef : releaseCoef);
            if (gr < kRestDb && wanted == 0.0)
                gr = 0.0; // at rest exactly: a gain of 1 is exactly 1
            const double gain = gr == 0.0 ? 1.0 : std::exp (-gr * 0.11512925464970229);
            deepest = std::max (deepest, (float) gr);

            for (int c = 0; c < 2; ++c)
            {
                double y;
                if (p.listen)
                    y = band[c];
                else if (p.mode == Mode::wideband)
                    y = x[c] * gain;
                else
                    y = x[c] - (1.0 - gain) * band[c]; // the band turned down, the rest left alone (and exactly the input when the gain is 1)
                (c == 0 ? left : right)[i] = (float) y;
            }
        }
        if (deepest > reductionMeter.load (std::memory_order_relaxed))
            reductionMeter.store (deepest, std::memory_order_relaxed);
    }

    std::atomic<float> reductionMeter { 0.0f }; // the deepest reduction since the editor last asked, in dB

private:
    struct Channel
    {
        pdsp::Biquad band;
        double env = 0.0, body = 0.0;
    };

    static inline double finite (float v) noexcept { return std::isfinite (v) ? (double) v : 0.0; }
    double coefOf (double ms) const noexcept { return 1.0 - std::exp (-1.0 / std::fmax (0.001 * ms * fs, 1.0)); }

    double fs = 48000.0, attackCoef = 0.05, envFall = 0.97, bodyCoef = 0.001, gr = 0.0;
    std::array<double, 2> designedFor { -1.0, -1.0 };
    bool started = false;
    std::array<Channel, 2> ch;
};
} // namespace nemesis
