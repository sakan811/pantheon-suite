// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 The Pantheon authors

#pragma once

#include "AnalyzerTap.h"
#include <juce_dsp/juce_dsp.h>
#include <algorithm>
#include <array>
#include <cmath>

namespace pdsp
{
// Real-time spectrum analysis for the display, modelled on the analyzer of a typical parametric EQ:
//  - an 8192-point FFT (the highest resolution such analyzers offer) for good low-frequency detail,
//  - fast attack and a selectable release ("Speed"),
//  - a tilt in dB/octave around 1 kHz (4.5 dB/oct is a common default), so typical programme
//    material looks roughly flat and tonal imbalances stand out.
//
// Levels are calibrated in dBFS: a full-scale sine centred on a bin reads 0 dB (before tilt).
// Left and right are analysed separately and their power averaged. Free of GUI code so that the
// calibration can be unit-tested.
class SpectrumAnalyzer
{
public:
    static constexpr int fftOrder = 13;
    static constexpr int fftSize = 1 << fftOrder;
    static constexpr int numBins = fftSize / 2;
    static constexpr double tiltPivotHz = 1000.0;
    static constexpr float floorDb = -140.0f;
    static constexpr float defaultTilt = 4.5f;

    SpectrumAnalyzer()
    {
        juce::dsp::WindowingFunction<float>::fillWindowingTables (window.data(), (size_t) fftSize,
                                                                  juce::dsp::WindowingFunction<float>::hann, false);
        smoothedDb.fill (floorDb);
        setSampleRate (48000.0);
    }

    void setSampleRate (double sr)
    {
        sampleRate = sr;
        rebuildTilt();
    }

    void setTilt (float dbPerOctave)
    {
        tilt = dbPerOctave;
        rebuildTilt();
    }

    void setReleaseSeconds (float seconds) noexcept { releaseSeconds = seconds; }
    float getTilt() const noexcept { return tilt; }

    // Pulls whatever the audio thread has produced, transforms it, and updates the smoothed trace.
    void update (StereoTap& tap)
    {
        int consumed = 0;
        for (size_t c = 0; c < 2; ++c)
        {
            int got, total = 0;
            while ((got = tap.channel[c].pull (chunk.data(), (int) chunk.size())) > 0)
            {
                for (int i = 0; i < got; ++i)
                {
                    ring[c][(size_t) writePos[c]] = chunk[(size_t) i];
                    writePos[c] = (writePos[c] + 1) % fftSize;
                }
                total += got;
            }
            if (c == 0) consumed = total;
        }

        power.fill (0.0f);
        for (size_t c = 0; c < 2; ++c)
        {
            for (int i = 0; i < fftSize; ++i)
                work[(size_t) i] = ring[c][(size_t) ((writePos[c] + i) % fftSize)] * window[(size_t) i];
            std::fill (work.begin() + fftSize, work.end(), 0.0f);

            fft.performFrequencyOnlyForwardTransform (work.data());
            for (int i = 0; i < numBins; ++i)
                power[(size_t) i] += 0.5f * work[(size_t) i] * work[(size_t) i];
        }

        // Smoothing is derived from the audio time consumed, so it stays correct if the UI stalls.
        const double dt = std::max (consumed, 1) / sampleRate;
        const float attack = 1.0f - (float) std::exp (-dt / attackSeconds);
        const float release = 1.0f - (float) std::exp (-dt / std::max (releaseSeconds, 1.0e-3f));
        constexpr float norm = 4.0f / (float) fftSize; // Hann window: coherent gain 0.5, two-sided spectrum

        for (int i = 0; i < numBins; ++i)
        {
            const float db = juce::Decibels::gainToDecibels (std::sqrt (power[(size_t) i]) * norm, floorDb);
            auto& s = smoothedDb[(size_t) i];
            s += (db - s) * (db > s ? attack : release);
        }
    }

    double binFrequency (int bin) const noexcept { return bin * sampleRate / fftSize; }

    // Level of one bin, tilt included.
    float levelDb (int bin) const noexcept { return smoothedDb[(size_t) bin] + tiltDb[(size_t) bin]; }

    // Level at an arbitrary frequency, linearly interpolated between bins.
    float levelAtFrequency (double hz) const noexcept
    {
        const double pos = std::clamp (hz * fftSize / sampleRate, 0.0, (double) numBins - 2.0);
        const int i = (int) pos;
        const float frac = (float) (pos - i);
        return levelDb (i) + frac * (levelDb (i + 1) - levelDb (i));
    }

    // Strongest level between two frequencies: used when one display pixel spans several bins.
    float peakInRange (double loHz, double hiHz) const noexcept
    {
        const int a = std::clamp ((int) (loHz * fftSize / sampleRate), 0, numBins - 1);
        const int b = std::clamp ((int) (hiHz * fftSize / sampleRate), a, numBins - 1);
        float peak = floorDb;
        for (int i = a; i <= b; ++i)
            peak = std::max (peak, levelDb (i));
        return peak;
    }

private:
    void rebuildTilt()
    {
        for (int i = 0; i < numBins; ++i)
        {
            const double f = std::max (binFrequency (i), 1.0);
            tiltDb[(size_t) i] = (float) (tilt * std::log2 (f / tiltPivotHz));
        }
    }

    static constexpr float attackSeconds = 0.02f;

    double sampleRate = 48000.0;
    float tilt = defaultTilt, releaseSeconds = 0.4f;
    juce::dsp::FFT fft { fftOrder };
    std::array<float, fftSize> window {};
    std::array<std::array<float, fftSize>, 2> ring {};
    std::array<int, 2> writePos { 0, 0 };
    std::array<float, 2048> chunk {};
    std::array<float, fftSize * 2> work {};
    std::array<float, numBins> power {}, smoothedDb {}, tiltDb {};
};
}
