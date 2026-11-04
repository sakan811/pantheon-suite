// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 The Pantheon authors

#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <cmath>
#include <random>
#include <vector>

// Signal sources and measurements shared by the test executables.
namespace test
{
constexpr double fs = 48000.0;

inline std::vector<float> sine (double freq, double amp, int n, double phase = 0.0, double rate = fs)
{
    std::vector<float> x ((size_t) n);
    for (int i = 0; i < n; ++i)
        x[(size_t) i] = (float) (amp * std::sin (2.0 * M_PI * freq * i / rate + phase));
    return x;
}

inline std::vector<float> noise (unsigned seed, double amp, int n)
{
    std::mt19937 rng (seed);
    std::uniform_real_distribution<float> dist (-1.0f, 1.0f);
    std::vector<float> x ((size_t) n);
    for (auto& v : x)
        v = (float) amp * dist (rng);
    return x;
}

inline std::vector<float> scaled (std::vector<float> x, double k)
{
    for (auto& v : x)
        v = (float) (v * k);
    return x;
}

// Amplitude of the component at `freq` over [from, from + n): a single-bin DFT. Use a whole number of cycles for an exact reading.
inline double amplitudeAt (const std::vector<float>& x, double freq, int from, int n, double rate = fs)
{
    double re = 0.0, im = 0.0;
    for (int i = 0; i < n; ++i)
    {
        const double w = 2.0 * M_PI * freq * (from + i) / rate;
        re += x[(size_t) (from + i)] * std::cos (w);
        im += x[(size_t) (from + i)] * std::sin (w);
    }
    return 2.0 * std::sqrt (re * re + im * im) / n;
}

inline double toDb (double ratio) { return 20.0 * std::log10 (std::max (ratio, 1.0e-12)); }

inline double rms (const std::vector<float>& x, int from, int n)
{
    double sum = 0.0;
    for (int i = 0; i < n; ++i)
        sum += (double) x[(size_t) (from + i)] * x[(size_t) (from + i)];
    return std::sqrt (sum / n);
}

inline double maxAbs (const std::vector<float>& x, int from, int n)
{
    double m = 0.0;
    for (int i = 0; i < n; ++i)
        m = std::max (m, (double) std::fabs (x[(size_t) (from + i)]));
    return m;
}
}

namespace test
{
// Pink-ish noise (Paul Kellet's economy filter).
class PinkNoise
{
public:
    explicit PinkNoise (unsigned seed) : rng (seed) {}

    float next()
    {
        const double w = dist (rng);
        b1 = 0.99765 * b1 + w * 0.0990460;
        b2 = 0.96300 * b2 + w * 0.2965164;
        b3 = 0.57000 * b3 + w * 1.0526913;
        return (float) ((b1 + b2 + b3 + w * 0.1848) * 0.035);
    }

private:
    std::mt19937 rng;
    std::uniform_real_distribution<double> dist { -1.0, 1.0 };
    double b1 = 0, b2 = 0, b3 = 0;
};

// A stereo programme for the screenshots: pink noise that is wider the higher it goes (the sides carry the top), a bass note in
// the centre, and a sibilant burst on and off at 7 kHz.
class Programme
{
public:
    Programme() : pinkMid (31), pinkSide (32) {}

    void fill (juce::AudioBuffer<float>& buffer)
    {
        for (int i = 0; i < buffer.getNumSamples(); ++i)
        {
            const double s = t / fs;
            const float mid = 0.7f * pinkMid.next() + (float) (0.10 * std::sin (2 * M_PI * 55 * s) + 0.05 * std::sin (2 * M_PI * 110 * s));
            const bool burst = std::fmod (s, 1.2) < 0.25;
            const float side = 0.5f * pinkSide.next() + (burst ? (float) (0.07 * std::sin (2 * M_PI * 7000 * s)) : 0.0f);
            buffer.setSample (0, i, mid + side);
            buffer.setSample (1, i, mid - side);
            t += 1.0;
        }
    }

private:
    PinkNoise pinkMid, pinkSide;
    double t = 0;
};
}
