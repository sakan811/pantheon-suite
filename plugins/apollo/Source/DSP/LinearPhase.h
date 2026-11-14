// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 The Pantheon authors

#pragma once

#include "Bands.h"
#include <complex>
#include <vector>

// The linear-phase equaliser: the magnitude of the bands made into a symmetric FIR, and the FIR run by FFT overlap-save, one frame per hop. Plain C++ (its own FFT),
// so the engine has no JUCE in it. Latency = (frame + hop) / 2: the kernel's own delay, (frame - hop) / 2, plus the hop the output waits for.
namespace apollo
{
class Fft
{
public:
    void prepare (int n)
    {
        size = n;
        twiddle.resize ((size_t) n / 2);
        for (int k = 0; k < n / 2; ++k)
            twiddle[(size_t) k] = std::polar (1.0, -2.0 * M_PI * k / n);
        rev.resize ((size_t) n);
        int bits = 0;
        while ((1 << bits) < n)
            ++bits;
        for (int i = 0; i < n; ++i)
        {
            int r = 0;
            for (int b = 0; b < bits; ++b)
                r |= ((i >> b) & 1) << (bits - 1 - b);
            rev[(size_t) i] = r;
        }
    }

    // In place. The inverse is scaled by 1 / n.
    void transform (std::vector<std::complex<double>>& x, bool inverse) const noexcept
    {
        for (int i = 0; i < size; ++i)
            if (i < rev[(size_t) i])
                std::swap (x[(size_t) i], x[(size_t) rev[(size_t) i]]);
        for (int len = 2; len <= size; len <<= 1)
        {
            const int half = len / 2, step = size / len;
            for (int start = 0; start < size; start += len)
                for (int k = 0; k < half; ++k)
                {
                    auto w = twiddle[(size_t) (k * step)];
                    if (inverse)
                        w = std::conj (w);
                    const auto a = x[(size_t) (start + k)], b = x[(size_t) (start + k + half)] * w;
                    x[(size_t) (start + k)] = a + b;
                    x[(size_t) (start + k + half)] = a - b;
                }
        }
        if (inverse)
            for (auto& v : x)
                v /= (double) size;
    }

private:
    int size = 0;
    std::vector<std::complex<double>> twiddle;
    std::vector<int> rev;
};

// The sizes for a sample rate: the same resolution in hertz (about 12 Hz) at any rate.
struct LinearSizes
{
    int frame = 4096, hop = 512;
    int latency() const noexcept { return (frame + hop) / 2; }
    int half() const noexcept { return (frame - hop) / 2; } // the kernel is 2 * half + 1 taps
};
inline LinearSizes linearSizesFor (double fs) noexcept
{
    LinearSizes s;
    while (s.frame < fs / 12.0 && s.frame < 16384)
        s.frame *= 2;
    s.hop = s.frame / 8;
    return s;
}

// One signal (the mid or the side) through a linear-phase kernel.
class Convolver
{
public:
    void prepare (const LinearSizes& s)
    {
        sizes = s;
        fft.prepare (s.frame);
        ring.assign ((size_t) s.frame, 0.0);
        work.assign ((size_t) s.frame, {});
        kernel.assign ((size_t) s.frame, {});
        previous.assign ((size_t) s.hop, 0.0);
        current.assign ((size_t) s.hop, 0.0);
        exact.assign ((size_t) s.latency() + 1, 0.0);
        cosW.resize ((size_t) s.frame / 2 + 1);
        cos2W.resize (cosW.size());
        for (size_t k = 0; k < cosW.size(); ++k)
        {
            cosW[k] = std::cos (2.0 * M_PI * (double) k / s.frame);
            cos2W[k] = std::cos (4.0 * M_PI * (double) k / s.frame);
        }
        setIdentity();
        reset();
    }

    void reset() noexcept
    {
        std::fill (ring.begin(), ring.end(), 0.0);
        std::fill (previous.begin(), previous.end(), 0.0);
        std::fill (current.begin(), current.end(), 0.0);
        std::fill (exact.begin(), exact.end(), 0.0);
        head = 0;
        exactHead = 0;
        count = 0;
    }

    // The kernel for the bands that reach `path` (the filters as they are now, `sections[i]` for the band i, `running[i]` whether it is in, `mix[i]` how far: below 1 a band that is fading out
    // has its magnitude taken to that power, so its dB shrink to nothing).
    void design (const std::array<Section, kBands>& sections, const std::array<bool, kBands>& running, const std::array<double, kBands>& mix, bool& identity) noexcept
    {
        const int F = sizes.frame, half = sizes.half();
        identity = true;
        for (bool r : running)
            identity = identity && ! r;
        for (int k = 0; k <= F / 2; ++k)
        {
            double power = 1.0;
            for (int b = 0; b < kBands; ++b)
                if (running[(size_t) b])
                {
                    const double p = sections[(size_t) b].powerAt (cosW[(size_t) k], cos2W[(size_t) k]);
                    power *= mix[(size_t) b] == 1.0 ? p : std::pow (p, mix[(size_t) b]);
                }
            const double m = std::sqrt (std::max (power, 0.0)); // a notch on a bin's exact frequency sums to a hair below 0: its root was NaN, and the NaN went into every sample
            work[(size_t) k] = m;
            if (k > 0 && k < F / 2)
                work[(size_t) (F - k)] = m;
        }
        fft.transform (work, true); // zero phase: a real, symmetric impulse response around 0
        std::fill (kernel.begin(), kernel.end(), std::complex<double> {});
        for (int n = -half; n <= half; ++n)
        {
            const double window = 0.5 + 0.5 * std::cos (M_PI * n / (half + 1.0));
            kernel[(size_t) (n + half)] = work[(size_t) ((n + F) % F)].real() * window;
        }
        fft.transform (kernel, false);
        kernelIsIdentity = identity;
    }

    void setIdentity() noexcept
    {
        std::fill (kernel.begin(), kernel.end(), std::complex<double> { 1.0, 0.0 });
        kernelIsIdentity = true;
    }

    // One sample in, one out (late by the latency). When the kernel is the identity the sound is only delayed, exactly.
    inline double process (double x) noexcept
    {
        const size_t F = (size_t) sizes.frame, H = (size_t) sizes.hop;
        ring[head] = x;
        head = (head + 1) % F;
        exact[exactHead] = x;
        const size_t lat = (size_t) sizes.latency(), N = exact.size();
        const double delayed = exact[(exactHead + N - lat) % N];
        exactHead = (exactHead + 1) % N;
        const double y = previous[count];
        if (++count == H)
        {
            count = 0;
            for (size_t i = 0; i < F; ++i)
                work[i] = ring[(head + i) % F]; // the oldest first
            fft.transform (work, false);
            for (size_t i = 0; i < F; ++i)
                work[i] *= kernel[i];
            fft.transform (work, true);
            for (size_t i = 0; i < H; ++i)
                current[i] = work[F - H + i].real();
            previous.swap (current);
        }
        return kernelIsIdentity ? delayed : y;
    }

    LinearSizes sizes;

private:
    Fft fft;
    std::vector<double> ring, previous, current, exact;
    std::vector<std::complex<double>> work, kernel;
    std::vector<double> cosW, cos2W;
    size_t head = 0, exactHead = 0, count = 0;
    bool kernelIsIdentity = true;
};
} // namespace apollo
