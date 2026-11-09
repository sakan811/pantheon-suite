// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 The Pantheon authors

#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

namespace hep
{
// The Octave fill: harmonics of every partial in a band, made one partial at a time, so that nothing the band holds is ever
// multiplied by anything else (no waveshaper, so no intermodulation, however many notes there are).
//
// Every quarter of a frame a Hann-windowed FFT of the last N samples is taken. Each resolved spectral peak in the band is a
// partial: its frequency (to a fraction of a bin), amplitude and phase are read, and for each harmonic k (2..5) a sine at k times
// that frequency, with k times that phase and the weight of that harmonic times that amplitude, is written into a synthesis
// spectrum *as the window's own transform at that exact frequency*. The inverse FFT therefore returns a windowed sine of exactly
// the wanted frequency (not one rounded to a bin), and overlap-adding the frames at 75 % gives a continuous sine that is phase-locked
// to the partial it came from. The phase needs no memory: with the window centred on bin zero, a partial's bin phase is its phase at
// the middle of the frame, and k times it is the harmonic's.
//
// The price is latency: a frame's output can only be finished when its last input sample has arrived, so the output is
// latencyFor (fs) = N - 1 samples behind the input (N is 2048 up to 64 kHz, 4096 up to 128 kHz and 8192 above: about 43 ms).
// Partials closer than about four bins (94 Hz at 48 kHz) are not resolved and are not filled; it is the tool for the mids and
// highs, where the partials are far enough apart, and not for the lowest octaves.
class OctaveGenerator
{
public:
    static constexpr int maxChannels = 2;
    static constexpr int kHarmonics = 4; // H2..H5

    static int sizeFor (double sampleRate) noexcept { return sampleRate <= 64000.0 ? 2048 : sampleRate <= 128000.0 ? 4096 : 8192; }
    static int latencyFor (double sampleRate) noexcept { return sizeFor (sampleRate) - 1; }

    // Allocates everything it will use (call from prepare, never from the audio thread).
    void prepare (double sampleRate)
    {
        fs = sampleRate;
        n = sizeFor (fs);
        mask = n - 1;
        hop = n / 4;

        window.resize ((size_t) n);
        for (int i = 0; i < n; ++i)
            window[(size_t) i] = 0.5 - 0.5 * std::cos (2.0 * kPi * (double) i / (double) n);

        // Bit reversal and twiddles of the radix-2 transform.
        bitReverse.resize ((size_t) n);
        int bits = 0;
        while ((1 << bits) < n) ++bits;
        for (int i = 0; i < n; ++i)
        {
            int r = 0;
            for (int b = 0; b < bits; ++b)
                if (i & (1 << b))
                    r |= 1 << (bits - 1 - b);
            bitReverse[(size_t) i] = r;
        }
        cosTable.resize ((size_t) n / 2);
        sinTable.resize ((size_t) n / 2);
        for (int i = 0; i < n / 2; ++i)
        {
            cosTable[(size_t) i] = std::cos (2.0 * kPi * (double) i / (double) n);
            sinTable[(size_t) i] = -std::sin (2.0 * kPi * (double) i / (double) n);
        }

        // The Hann window's transform at a fractional distance from a bin, 1 at the centre: what a sine looks like in the spectrum.
        kernel.resize ((size_t) (2 * kKernelReach * kKernelSteps + 2));
        for (size_t i = 0; i < kernel.size(); ++i)
            kernel[i] = hannTransform ((double) i / (double) kKernelSteps - (double) kKernelReach);

        re.assign ((size_t) n, 0.0);
        im.assign ((size_t) n, 0.0);
        mag.assign ((size_t) n / 2 + 1, 0.0);
        synthRe.assign ((size_t) n, 0.0);
        synthIm.assign ((size_t) n, 0.0);
        for (auto& c : channel)
        {
            c.input.assign ((size_t) n, 0.0);
            c.overlap.assign ((size_t) n, 0.0);
        }
        reset();
    }

    void reset() noexcept
    {
        for (auto& c : channel)
        {
            std::fill (c.input.begin(), c.input.end(), 0.0);
            std::fill (c.overlap.begin(), c.overlap.end(), 0.0);
            c.position = 0;
        }
        seen = 0;
    }

    int latency() const noexcept { return n - 1; }
    int frameSize() const noexcept { return n; }

    // One frame of input in, one frame of output out (n - 1 samples late). `lo` / `hi` are the range whose partials are filled and
    // `weights` the level of H2..H5 relative to the partial (a harmonic that must not be made has weight 0). nCh <= maxChannels.
    void process (const std::array<double, maxChannels>& in, std::array<double, maxChannels>& out, int nCh, double lo, double hi,
                  const double (&weights)[kHarmonics]) noexcept
    {
        for (int c = 0; c < nCh; ++c)
        {
            auto& ch = channel[(size_t) c];
            ch.input[(size_t) ch.position] = in[(size_t) c];
            ch.position = (ch.position + 1) & mask;
        }
        ++seen;
        if (seen >= n && seen % hop == 0)
            for (int c = 0; c < nCh; ++c)
                makeFrame (channel[(size_t) c], lo, hi, weights);

        for (int c = 0; c < nCh; ++c)
        {
            auto& ch = channel[(size_t) c];
            out[(size_t) c] = ch.overlap[(size_t) ch.position];
            ch.overlap[(size_t) ch.position] = 0.0;
        }
    }

    // What the synthesis uses for a sine `distance` bins away from a bin (for the tests).
    double kernelAt (double distance) const noexcept
    {
        const double x = (distance + (double) kKernelReach) * (double) kKernelSteps;
        if (x < 0.0 || x >= (double) (kernel.size() - 1))
            return 0.0;
        const int i = (int) x;
        const double f = x - (double) i;
        return kernel[(size_t) i] * (1.0 - f) + kernel[(size_t) i + 1] * f;
    }

private:
    static constexpr double kPi = 3.14159265358979323846;
    static constexpr int kKernelReach = 4, kKernelSteps = 128;
    static constexpr int kSidelobeReach = 6;
    static constexpr double kSidelobeFloor = 0.04; // a peak under this share of the strongest bin within 6 is a window sidelobe, not a partial
    static constexpr double kSilence = 1.0e-5;     // amplitude under which a peak is not worth a harmonic (-100 dBFS)

    struct Channel
    {
        std::vector<double> input, overlap;
        int position = 0;
    };

    // Dirichlet kernel of the symmetric (2n-1 sample) form the zero-phase window has: sum of exp (-j 2 pi x t / N) over t = -(N/2 - 1) .. N/2 - 1.
    double dirichlet (double x) const noexcept
    {
        if (std::fabs (x) < 1.0e-9)
            return (double) (n - 1);
        return std::sin (kPi * x * (double) (n - 1) / (double) n) / std::sin (kPi * x / (double) n);
    }

    // The periodic Hann window is 1/2 + 1/2 cos (2 pi t / N) about its centre: three Dirichlet kernels, scaled so the centre is 1.
    double hannTransform (double x) const noexcept
    {
        return (0.5 * dirichlet (x) + 0.25 * (dirichlet (x - 1.0) + dirichlet (x + 1.0))) / (0.5 * (double) n);
    }

    // In place, forward (e^-j) transform of the n points in re / im.
    void fft (std::vector<double>& xr, std::vector<double>& xi) noexcept
    {
        for (int i = 0; i < n; ++i)
        {
            const int j = bitReverse[(size_t) i];
            if (j > i)
            {
                std::swap (xr[(size_t) i], xr[(size_t) j]);
                std::swap (xi[(size_t) i], xi[(size_t) j]);
            }
        }
        for (int len = 2; len <= n; len <<= 1)
        {
            const int half = len / 2, stride = n / len;
            for (int start = 0; start < n; start += len)
                for (int k = 0; k < half; ++k)
                {
                    const double wr = cosTable[(size_t) (k * stride)], wi = sinTable[(size_t) (k * stride)];
                    const size_t a = (size_t) (start + k), b = a + (size_t) half;
                    const double tr = xr[b] * wr - xi[b] * wi, ti = xr[b] * wi + xi[b] * wr;
                    xr[b] = xr[a] - tr;
                    xi[b] = xi[a] - ti;
                    xr[a] += tr;
                    xi[a] += ti;
                }
        }
    }

    void makeFrame (Channel& ch, double lo, double hi, const double (&weights)[kHarmonics]) noexcept
    {
        // The oldest sample of the last n sits where the next one will be written. The frame is rotated by half its length so
        // that the middle of the window is time zero: then a partial's phase in its bin is its phase in the middle of the frame.
        const int oldest = ch.position;
        for (int i = 0; i < n; ++i)
        {
            const size_t at = (size_t) ((i + n / 2) & mask);
            re[at] = ch.input[(size_t) ((oldest + i) & mask)] * window[(size_t) i];
            im[at] = 0.0;
        }
        fft (re, im);

        const int half = n / 2;
        for (int m = 0; m <= half; ++m)
            mag[(size_t) m] = std::hypot (re[(size_t) m], im[(size_t) m]);

        const double binHz = fs / (double) n;
        const int first = std::max (kSidelobeReach, (int) std::ceil (lo / binHz));
        const int last = std::min (half - kKernelReach - 2, (int) std::floor (hi / binHz));
        const double scale = 4.0 / (double) n; // |X| of a sine of amplitude 1 under the Hann window is n / 4

        // The synthesis spectrum, positive half only (it is mirrored below).
        bool any = false;
        std::vector<double>& yr = synthRe;
        std::vector<double>& yi = synthIm;
        std::fill (yr.begin(), yr.end(), 0.0);
        std::fill (yi.begin(), yi.end(), 0.0);

        for (int m = first; m <= last; ++m)
        {
            const double here = mag[(size_t) m];
            if (! (here > mag[(size_t) m - 1] && here >= mag[(size_t) m + 1]) || here * scale < kSilence)
                continue;

            double strongest = here;
            for (int j = m - kSidelobeReach; j <= m + kSidelobeReach; ++j)
                strongest = std::max (strongest, mag[(size_t) j]);
            if (here < kSidelobeFloor * strongest)
                continue;

            // Where the peak really is: a parabola through the logs of the three bins around it.
            const double a = std::log (mag[(size_t) m - 1] + 1.0e-30), b = std::log (here + 1.0e-30), c = std::log (mag[(size_t) m + 1] + 1.0e-30);
            const double curve = a - 2.0 * b + c;
            const double offset = curve < -1.0e-12 ? std::clamp (0.5 * (a - c) / curve, -0.5, 0.5) : 0.0;
            const double peak = std::exp (b - 0.25 * (a - c) * offset);
            const double position = (double) m + offset;

            // Unit phase vector of the partial (the same in every bin of its main lobe).
            double ur = re[(size_t) m] / here, ui = im[(size_t) m] / here;
            double pr = ur, pi = ui; // u^k, starting at k = 1
            for (int k = 2; k < 2 + kHarmonics; ++k)
            {
                const double nr = pr * ur - pi * ui;
                pi = pr * ui + pi * ur;
                pr = nr;

                const double w = weights[k - 2];
                const double target = (double) k * position;
                if (w <= 0.0 || target + (double) kKernelReach + 1.0 >= (double) half)
                    continue;

                const double level = w * peak;
                const int centre = (int) std::floor (target);
                for (int q = centre - kKernelReach + 1; q <= centre + kKernelReach; ++q)
                {
                    if (q < 1 || q >= half)
                        continue;
                    const double shape = kernelAt ((double) q - target) * level;
                    yr[(size_t) q] += shape * pr;
                    yi[(size_t) q] += shape * pi;
                }
                any = true;
            }
        }
        if (! any)
            return;

        // Conjugate-symmetric, then the inverse transform (conjugate in, forward, take the real part).
        for (int q = 1; q < half; ++q)
        {
            re[(size_t) q] = yr[(size_t) q];
            im[(size_t) q] = -yi[(size_t) q];
            re[(size_t) (n - q)] = yr[(size_t) q];
            im[(size_t) (n - q)] = yi[(size_t) q];
        }
        re[0] = im[0] = re[(size_t) half] = im[(size_t) half] = 0.0;
        fft (re, im);

        // Overlap-add: Hann frames at a quarter of their length sum to 2, so each carries half.
        const double gain = 0.5 / (double) n;
        for (int i = 0; i < n; ++i)
            ch.overlap[(size_t) ((oldest + i) & mask)] += re[(size_t) ((i + n / 2) & mask)] * gain;
    }

    double fs = 48000.0;
    int n = 2048, mask = 2047, hop = 512;
    long long seen = 0;

    std::vector<double> window, cosTable, sinTable, kernel, re, im, mag, synthRe, synthIm;
    std::vector<int> bitReverse;
    std::array<Channel, maxChannels> channel;
};
}
