// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 The Pantheon authors

#pragma once

#include "Oversampler.h"
#include "Shapes.h"
#include <atomic>
#include <pantheon/dsp/Biquad.h>

// The whole saturator: no GUI, no JUCE. Everything is sized in prepare() / configure(); process() allocates and locks nothing.
namespace dionysus
{
inline constexpr double kToneHz = 1000.0;      // the tone's two shelves turn around here
inline constexpr double kToneMaxDb = 6.0;      // each shelf: the tilt is twice this at 100%
inline constexpr double kDcBlockHz = 10.0;
inline constexpr int kMaxLatency = oversamplingLatency (kNumOversampling - 1); // the dry sound's ring is this long at most

struct EngineParams
{
    float driveDb = 12.0f;
    Shape shape = Shape::tape;
    float bias = 0.0f;           // -1 .. 1: the working point of the curve (even harmonics)
    float tone = 0.0f;           // -1 .. 1: a tilt after the curve: bright or dark
    float mix = 1.0f;            // 0 dry .. 1 saturated
    float outDb = 0.0f;
    bool autoGain = true;
    int oversampling = 1;        // index into kOversamplingFactor
};

class Engine
{
public:
    void prepare (double sampleRate, int oversamplingIndex)
    {
        fs = sampleRate;
        smooth = 1.0 - std::exp (-1.0 / (0.005 * fs));
        configure (oversamplingIndex);
    }

    void reset() noexcept
    {
        for (auto& c : ch)
        {
            c.os.reset();
            c.dc.reset();
            c.lowShelf.reset();
            c.highShelf.reset();
            c.dry.fill (0.0);
        }
        head = 0;
        compFor = {};
        started = false;
        toneFor = -2.0;
    }

    // A change of oversampling restarts the saturator (the latency moves with it).
    void configure (int oversamplingIndex) noexcept
    {
        index = std::clamp (oversamplingIndex, 0, kNumOversampling - 1);
        latency = oversamplingLatency (index);
        latencySamples.store (latency, std::memory_order_relaxed);
        for (auto& c : ch)
        {
            c.os.prepare (index);
            c.dry.fill (0.0); // a fixed array: a change of oversampling on the audio thread allocates nothing
        }
        dcBlock();
        reset();
    }

    // In place. Everything that is not a number is silence.
    void process (float* left, float* right, int n, const EngineParams& p) noexcept
    {
        if (p.oversampling != index)
            configure (p.oversampling);
        const auto shapeNow = p.shape;
        const double bias = std::clamp ((double) p.bias, -1.0, 1.0);
        const double driveT = std::pow (10.0, std::clamp (p.driveDb, 0.0f, kMaxDriveDb) / 20.0);
        const double compT = p.autoGain ? compensationFor (shapeNow, std::clamp ((double) p.driveDb, 0.0, (double) kMaxDriveDb), bias) : 1.0;
        const double outT = std::pow (10.0, std::clamp (p.outDb, -24.0f, 12.0f) / 20.0) * compT;
        const double mixT = std::clamp ((double) p.mix, 0.0, 1.0);
        const double tone = std::clamp ((double) p.tone, -1.0, 1.0);
        if (tone != toneFor)
        {
            toneFor = tone;
            for (auto& c : ch)
            {
                c.lowShelf.setLowShelf (fs, kToneHz, 0.7071, -tone * kToneMaxDb);
                c.highShelf.setHighShelf (fs, kToneHz, 0.7071, tone * kToneMaxDb);
            }
        }
        const bool toneOn = tone != 0.0, dcOn = bias != 0.0 || shapeNow == Shape::diode;
        if (! started)
        {
            drive = driveT;
            out = outT;
            mix = mixT;
            started = true;
        }
        float* io[2] = { left, right };
        float peak = 0.0f;
        const size_t ring = (size_t) latency + 1;
        size_t w = head; // (always < ring: a change of ring restarts it) where the next sample goes; the oldest one, `latency` samples back, is right after it

        for (int i = 0; i < n; ++i)
        {
            drive += (driveT - drive) * smooth;
            out += (outT - out) * smooth;
            mix += (mixT - mix) * smooth;
            for (int c = 0; c < 2; ++c)
            {
                auto& s = ch[(size_t) c];
                const double x = std::isfinite (io[c][i]) ? (double) io[c][i] : 0.0;
                const size_t r = w + 1 == ring ? 0 : w + 1;
                s.dry[w] = x;
                const double dry = s.dry[r]; // the sound as it was `latency` samples ago: what the oversampler delays by
                double y = s.os.process (x * drive, [&] (double v) { return saturate (shapeNow, v, bias); });
                if (dcOn)
                    y = s.dc.process (y);
                else
                    s.dc.reset();
                if (toneOn)
                    y = s.highShelf.process (s.lowShelf.process (y));
                else
                {
                    s.lowShelf.reset();
                    s.highShelf.reset();
                }
                const double wet = y * out;
                const double result = mix >= 1.0 - 1e-12 ? wet : (mix <= 1e-12 ? dry : dry + mix * (wet - dry));
                io[c][i] = (float) result;
                peak = std::max (peak, (float) std::fabs (result));
            }
            w = w + 1 == ring ? 0 : w + 1;
        }
        head = w;
        if (peak > outPeak.load (std::memory_order_relaxed))
            outPeak.store (peak, std::memory_order_relaxed);
    }

    std::atomic<int> latencySamples { 0 };
    std::atomic<float> outPeak { 0.0f };

private:
    struct Channel
    {
        Oversampler os;
        pdsp::Biquad dc, lowShelf, highShelf;
        std::array<double, kMaxLatency + 1> dry {}; // the first latency + 1 are used
    };

    // AUTO GAIN is a loop over a cycle of the curve (256 tanh / sin): measure it when the shape, the drive or the bias moves, not on every block.
    double compensationFor (Shape s, double driveDb, double bias) noexcept
    {
        if (! compFor.valid || compFor.shape != s || compFor.driveDb != driveDb || compFor.bias != bias)
            compFor = { true, s, driveDb, bias, compensation (s, driveDb, bias) };
        return compFor.value;
    }

    void dcBlock() noexcept
    {
        for (auto& c : ch)
            c.dc.setHighpass (fs, kDcBlockHz, 0.7071);
    }

    struct Compensation
    {
        bool valid = false;
        Shape shape = Shape::tape;
        double driveDb = 0.0, bias = 0.0, value = 1.0;
    } compFor;

    double fs = 48000.0, smooth = 0.004, drive = 1.0, out = 1.0, mix = 1.0, toneFor = -2.0;
    int index = 0, latency = 0;
    size_t head = 0;
    bool started = false;
    std::array<Channel, 2> ch;
};
} // namespace dionysus
