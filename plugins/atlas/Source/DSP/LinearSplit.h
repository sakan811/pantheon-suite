// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 The Pantheon authors

#pragma once

#include "Compressor.h"
#include <array>
#include <juce_dsp/juce_dsp.h>
#include <vector>

namespace atlas
{
// The linear-phase band split: the same six bands as the zero-latency splitter (the magnitudes of its Linkwitz-Riley
// crossovers, low-pass k times the high-passes below it), with no phase shift at all, so the bands add back to the input
// delayed and nothing else. FFT overlap-save, one frame per hop.
//
//   - Five kernels (bands 0 to 4) are symmetric around `half`, so in the frequency domain each is a real curve times a
//     delay of `half` samples; with the frame four times `half` that delay is a quarter turn per bin, (-j)^bin: no table.
//   - The top band is the delayed input minus the other five, so the six always add to the delayed input whatever the
//     kernels' truncation did.
//   - Sizes follow the sample rate (`scale`), so the resolution in Hz, and the latency in ms, stay the same.
class LinearSplit
{
public:
    static constexpr int kBaseHalf = 2048, kBaseHop = 1024; // at 44.1 / 48 kHz

    static int scaleFor (double sampleRate) noexcept { return sampleRate > 100000.0 ? 4 : (sampleRate > 50000.0 ? 2 : 1); }
    static int latencyFor (double sampleRate) noexcept { return (kBaseHalf + kBaseHop) * scaleFor (sampleRate); }

    void prepare (double sampleRate)
    {
        fs = sampleRate;
        const int scale = scaleFor (fs);
        half = kBaseHalf * scale;
        hop = kBaseHop * scale;
        size = 4 * half;
        fft = std::make_unique<juce::dsp::FFT> (juce::roundToInt (std::log2 ((double) size)));
        for (auto& h : history) h.assign ((size_t) size, 0.0f);
        for (auto& band : out)
            for (auto& ch : band) ch.assign ((size_t) hop, 0.0f);
        for (auto& c : curve) c.assign ((size_t) (size / 2 + 1), 0.0f);
        spectrum.assign ((size_t) (2 * size), 0.0f);
        work.assign ((size_t) (2 * size), 0.0f);
        window.resize ((size_t) (half + 1));
        for (int n = 0; n <= half; ++n)
            window[(size_t) n] = (float) (0.5 + 0.5 * std::cos (juce::MathConstants<double>::pi * n / (half + 1.0)));
        reset();
    }

    void reset() noexcept
    {
        for (auto& h : history) std::fill (h.begin(), h.end(), 0.0f);
        for (auto& band : out)
            for (auto& ch : band) std::fill (ch.begin(), ch.end(), 0.0f);
        pos = 0;
    }

    void setCrossovers (const std::array<double, kCrossovers>& hz) noexcept { crossover = hz; dirty = true; }
    int latency() const noexcept { return half + hop; }

    // One stereo sample in, the six bands of the sample `latency()` ago out.
    inline void process (float l, float r, std::array<std::array<float, 2>, kBands>& bands) noexcept
    {
        history[0][(size_t) (size - hop + pos)] = l;
        history[1][(size_t) (size - hop + pos)] = r;
        for (int b = 0; b < kBands; ++b)
            bands[(size_t) b] = { out[(size_t) b][0][(size_t) pos], out[(size_t) b][1][(size_t) pos] };
        if (++pos == hop)
        {
            frame();
            pos = 0;
        }
    }

private:
    void frame() noexcept
    {
        if (dirty)
            design();
        for (int c = 0; c < 2; ++c)
        {
            auto& h = history[(size_t) c];
            std::copy (h.begin(), h.end(), spectrum.begin());
            fft->performRealOnlyForwardTransform (spectrum.data(), true);

            auto& top = out[(size_t) (kBands - 1)][(size_t) c];
            for (int i = 0; i < hop; ++i)
                top[(size_t) i] = h[(size_t) (size - hop + i - half)];

            for (int b = 0; b < kBands - 1; ++b)
            {
                const auto& k = curve[(size_t) b];
                for (int bin = 0; bin <= size / 2; ++bin)
                {
                    const float re = spectrum[(size_t) (2 * bin)] * k[(size_t) bin], im = spectrum[(size_t) (2 * bin + 1)] * k[(size_t) bin];
                    float* o = &work[(size_t) (2 * bin)];
                    switch (bin & 3) // times (-j)^bin
                    {
                        case 0: o[0] = re;  o[1] = im;  break;
                        case 1: o[0] = im;  o[1] = -re; break;
                        case 2: o[0] = -re; o[1] = -im; break;
                        default: o[0] = -im; o[1] = re; break;
                    }
                }
                fft->performRealOnlyInverseTransform (work.data());
                auto& o = out[(size_t) b][(size_t) c];
                for (int i = 0; i < hop; ++i)
                {
                    o[(size_t) i] = work[(size_t) (size - hop + i)];
                    top[(size_t) i] -= o[(size_t) i];
                }
            }
            std::copy (h.begin() + hop, h.end(), h.begin());
        }
    }

    // The kernels of bands 0 to 4 from the crossovers: the wanted curve, to the time domain, cut to +-half under a window, and back.
    void design() noexcept
    {
        dirty = false;
        const double pi = juce::MathConstants<double>::pi;
        std::array<double, kCrossovers> warped {};
        for (int k = 0; k < kCrossovers; ++k)
            warped[(size_t) k] = std::tan (pi * std::clamp (crossover[(size_t) k], 10.0, fs * 0.45) / fs);

        for (int b = 0; b < kBands - 1; ++b)
        {
            std::fill (work.begin(), work.end(), 0.0f);
            for (int bin = 0; bin <= size / 2; ++bin)
            {
                // The digital Linkwitz-Riley low-pass is 1 / (1 + r^4) with r the ratio of the warped frequencies; its high-pass is the rest.
                const double w = bin == size / 2 ? 1.0e12 : std::tan (pi * bin / size);
                const auto low = [&] (int k) { const double r = w / warped[(size_t) k], r2 = r * r; return 1.0 / (1.0 + r2 * r2); };
                double m = low (b);
                for (int k = 0; k < b; ++k)
                    m *= 1.0 - low (k);
                work[(size_t) (2 * bin)] = (float) m;
            }
            fft->performRealOnlyInverseTransform (work.data());
            for (int n = half + 1; n < size - half; ++n)
                work[(size_t) n] = 0.0f;
            for (int n = 1; n <= half; ++n)
            {
                work[(size_t) n] *= window[(size_t) n];
                work[(size_t) (size - n)] *= window[(size_t) n];
            }
            std::fill (work.begin() + size, work.end(), 0.0f);
            fft->performRealOnlyForwardTransform (work.data(), true);
            for (int bin = 0; bin <= size / 2; ++bin)
                curve[(size_t) b][(size_t) bin] = work[(size_t) (2 * bin)];
        }
    }

    double fs = 48000.0;
    int half = kBaseHalf, hop = kBaseHop, size = 4 * kBaseHalf, pos = 0;
    bool dirty = true;
    std::array<double, kCrossovers> crossover = pdsp::bands::defaultCrossovers();
    std::unique_ptr<juce::dsp::FFT> fft;
    std::array<std::vector<float>, 2> history;
    std::array<std::array<std::vector<float>, 2>, kBands> out;
    std::array<std::vector<float>, kBands - 1> curve;
    std::vector<float> spectrum, work, window;
};
} // namespace atlas
