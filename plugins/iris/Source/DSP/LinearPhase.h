// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 The Pantheon authors

#pragma once

#include "Band.h"

#include <juce_dsp/juce_dsp.h>

#include <array>
#include <cmath>
#include <memory>
#include <vector>

namespace iris
{
// The two linear-phase resolutions. A kernel of 2 * half + 1 taps is cut out of the equaliser's zero-phase impulse response with a
// Kaiser window; it is applied in blocks of `hop` samples by overlap-save in an FFT of 2^order points (which must hold the kernel
// and a block: half * 2 + hop <= 2^order). The sound is delayed by half + hop samples: half is the kernel's own delay, hop is
// the wait for a block to fill.
struct LinearSpec
{
    int order, half, hop;
    constexpr int size() const noexcept { return 1 << order; }
    constexpr int latency() const noexcept { return half + hop; }
};

inline constexpr LinearSpec linearSpec (PhaseMode m) noexcept
{
    return m == PhaseMode::linearHq ? LinearSpec { 15, 8192, 1024 } : LinearSpec { 13, 2048, 256 };
}

inline constexpr int latencyFor (PhaseMode m) noexcept { return m == PhaseMode::zeroLatency ? 0 : linearSpec (m).latency(); }

// A linear-phase equaliser, driven by the engine. Every band is given as its minimum-phase filter (the one the display draws); only
// the magnitude |H|^2 is used, evaluated on the FFT's frequency grid, and the result is a symmetric FIR: the same curve as the
// zero-latency mode with no phase shift anywhere.
//
// The four channels an equaliser can work on (left, right, mid, side) each get a magnitude A, so each band is multiplied into
// the channels its placement touches. The bands act as the engine's do in series: left and right through A_L and A_R, then the mid and
// the side of that through A_M and A_S. Written out, that is one 2x2 matrix of zero-phase responses,
//     L' = A_L (A_M + A_S) / 2 . L  +  A_R (A_M - A_S) / 2 . R        R' = A_L (A_M - A_S) / 2 . L  +  A_R (A_M + A_S) / 2 . R,
// and each of the four is designed as one linear-phase kernel (a cascade of two kernels would need twice the delay). With no band
// on Mid or Side the cross terms are zero and they are not run.
//
// Everything that allocates happens in prepare(). The FFT runs in single precision (JUCE's), so the floor of this mode is about
// -130 dB, not the -160 dB of the zero-latency mode.
class LinearFilter
{
public:
    void prepare()
    {
        int maxSize = 0;
        for (int i = 0; i < 2; ++i)
        {
            const auto spec = linearSpec (i == 0 ? PhaseMode::linear : PhaseMode::linearHq);
            auto made = std::make_unique<Plan>();
            made->spec = spec;
            made->fft = std::make_unique<juce::dsp::FFT> (spec.order);
            const int n = spec.size();
            made->window.resize ((size_t) (2 * spec.half + 1));
            const double beta = 8.0, i0Beta = bessel0 (beta);
            for (int m = 0; m <= 2 * spec.half; ++m)
            {
                const double d = (double) (m - spec.half) / spec.half;
                made->window[(size_t) m] = (float) (bessel0 (beta * std::sqrt (std::max (0.0, 1.0 - d * d))) / i0Beta);
            }
            made->cosW.resize ((size_t) (n / 2 + 1));
            made->cos2W.resize ((size_t) (n / 2 + 1));
            for (int k = 0; k <= n / 2; ++k)
            {
                made->cosW[(size_t) k] = std::cos (2.0 * M_PI * k / n);
                made->cos2W[(size_t) k] = std::cos (4.0 * M_PI * k / n);
            }
            made->delay.resize ((size_t) n);
            for (int k = 0; k < n; ++k)
            {
                const double a = -2.0 * M_PI * (double) (((long long) k * spec.half) % n) / n;
                made->delay[(size_t) k] = { (float) std::cos (a), (float) std::sin (a) };
            }
            maxSize = std::max (maxSize, n);
            plans[(size_t) i] = std::move (made);
        }

        const auto big = (size_t) maxSize;
        ringL.assign (2 * big, 0.0);
        ringR.assign (2 * big, 0.0);
        zBuf.assign (big, {});
        uBuf.assign (big, {});
        specBuf.assign (big, {});
        outBuf.assign (big, {});
        for (auto& k : kernel)
            k.assign (big, {});
        for (auto& p : power)
            p.assign (big / 2 + 1, 1.0);
        for (auto& a : amp)
            a.assign (big / 2 + 1, 0.0);
        const auto hopMax = (size_t) linearSpec (PhaseMode::linearHq).hop;
        for (auto* v : { &wetL, &wetR, &soloL, &soloR })
            v->assign (hopMax, 0.0);
        start (PhaseMode::linear);
    }

    // Selects a resolution and forgets everything in the delay.
    void start (PhaseMode m)
    {
        plan = plans[m == PhaseMode::linearHq ? 1 : 0].get();
        std::fill (ringL.begin(), ringL.end(), 0.0);
        std::fill (ringR.begin(), ringR.end(), 0.0);
        for (auto* v : { &wetL, &wetR, &soloL, &soloR })
            std::fill (v->begin(), v->end(), 0.0);
        count = 0;
        pos = 0;
        due = false;
    }

    int latency() const noexcept { return plan->spec.latency(); }

    struct Frame
    {
        double l, r;          // the equaliser's output
        double dryL, dryR;    // the input, delayed by the latency
        double soloL, soloR;  // what the soloed band adds (when asked for)
    };

    // One sample in, the sample from `latency()` ago out. After it, hopDue() says that a block has been collected.
    Frame step (double l, double r) noexcept
    {
        const size_t mask = ringL.size() - 1;
        ringL[(size_t) count & mask] = l;
        ringR[(size_t) count & mask] = r;
        const size_t back = (size_t) count + ringL.size() - (size_t) plan->spec.latency();
        const Frame f { wetL[(size_t) pos], wetR[(size_t) pos], ringL[back & mask], ringR[back & mask], soloL[(size_t) pos], soloR[(size_t) pos] };
        ++count;
        if (++pos == plan->spec.hop)
        {
            pos = 0;
            due = true;
        }
        return f;
    }

    bool hopDue() const noexcept { return due; }

    // ---- the equaliser's curve, one band at a time ----------------------------------------------------------------------
    void beginKernels() noexcept
    {
        const size_t bins = (size_t) plan->spec.size() / 2 + 1;
        for (int c = 0; c < 4; ++c)
            std::fill (power[(size_t) c].begin(), power[(size_t) c].begin() + (long) bins, 1.0);
        usesMS = false;
    }

    // Multiplies a band's |H|^2 into the channels it touches. `mix` is the band's fade (1 = fully there): the curve is blended in dB.
    void addBand (const BandFilter& f, bool left, bool right, bool mid, bool side, double mix) noexcept
    {
        usesMS = usesMS || mid || side;
        const int bins = plan->spec.size() / 2 + 1;
        const bool blend = mix < 0.9999;
        double* const pw[4] = { power[0].data(), power[1].data(), power[2].data(), power[3].data() };
        const bool on[4] = { left, right, mid, side };
        for (int k = 0; k < bins; ++k)
        {
            double p = std::max (f.powerAt (plan->cosW[(size_t) k], plan->cos2W[(size_t) k]), 0.0); // (rounding can leave -1e-17 at a high-pass's DC)
            if (blend)
                p = std::pow (std::max (p, 1.0e-30), mix);
            for (int c = 0; c < 4; ++c)
                if (on[c])
                    pw[c][k] *= p;
        }
    }

    void finishKernels() noexcept
    {
        const int bins = plan->spec.size() / 2 + 1;
        for (int k = 0; k < bins; ++k)
        {
            const size_t i = (size_t) k;
            const double al = std::sqrt (power[0][i]), ar = std::sqrt (power[1][i]);
            if (usesMS)
            {
                const double am = std::sqrt (power[2][i]), as = std::sqrt (power[3][i]);
                amp[0][i] = al * 0.5 * (am + as);   // left to left
                amp[1][i] = ar * 0.5 * (am + as);   // right to right
                amp[2][i] = ar * 0.5 * (am - as);   // right to left
                amp[3][i] = al * 0.5 * (am - as);   // left to right
            }
            else
            {
                amp[0][i] = al;
                amp[1][i] = ar;
            }
        }
        buildPair (amp[0], amp[1], kernel[0], kernel[1], false);
        if (usesMS)
            buildPair (amp[2], amp[3], kernel[2], kernel[3], false);
    }

    // The soloed band alone, as what it adds (its kernel minus the plain delay).
    void setSoloBand (const BandFilter& f, double mix) noexcept
    {
        const int bins = plan->spec.size() / 2 + 1;
        auto& a = amp[4];
        for (int k = 0; k < bins; ++k)
        {
            double p = std::max (f.powerAt (plan->cosW[(size_t) k], plan->cos2W[(size_t) k]), 0.0);
            if (mix < 0.9999)
                p = std::pow (std::max (p, 1.0e-30), mix);
            a[(size_t) k] = std::sqrt (p);
        }
        buildPair (amp[4], amp[4], kernel[4], kernel[4], true, true);
    }

    // The block that has just been collected, filtered. Without bands (`filtering` false) it is the input delayed, exactly.
    void processHop (bool filtering, bool soloing, Placement soloPlace) noexcept
    {
        due = false;
        const auto& spec = plan->spec;
        const int n = spec.size(), hop = spec.hop;
        const size_t mask = ringL.size() - 1;
        const long long first = count - hop; // the time of the block's first sample

        if (! filtering && ! soloing)
        {
            for (int j = 0; j < hop; ++j)
            {
                const size_t at = (size_t) (first + j + (long long) ringL.size() - spec.half) & mask;
                wetL[(size_t) j] = ringL[at];
                wetR[(size_t) j] = ringR[at];
                soloL[(size_t) j] = soloR[(size_t) j] = 0.0;
            }
            return;
        }

        // The last n input samples, left in the real part and right in the imaginary part of one transform.
        for (int i = 0; i < n; ++i)
        {
            const size_t at = (size_t) (count - n + i + (long long) ringL.size()) & mask;
            zBuf[(size_t) i] = { (float) ringL[at], (float) ringR[at] };
        }
        plan->fft->perform (zBuf.data(), specBuf.data(), false);

        for (int k = 0; k < n; ++k)
        {
            const Cx a = specBuf[(size_t) k], b = conjugate (specBuf[(size_t) ((n - k) & (n - 1))]);
            const Cx xl = scale (add (a, b), 0.5f);             // the left spectrum
            const Cx v = sub (a, b);
            const Cx xr { 0.5f * v.imag(), -0.5f * v.real() };  // the right one: v / 2j

            Cx yl = mul (kernel[0][(size_t) k], xl);
            Cx yr = mul (kernel[1][(size_t) k], xr);
            if (usesMS)
            {
                yl = add (yl, mul (kernel[2][(size_t) k], xr));
                yr = add (yr, mul (kernel[3][(size_t) k], xl));
            }
            outBuf[(size_t) k] = { yl.real() - yr.imag(), yl.imag() + yr.real() };
        }
        plan->fft->perform (outBuf.data(), zBuf.data(), true);
        for (int j = 0; j < hop; ++j)
        {
            wetL[(size_t) j] = (double) zBuf[(size_t) (n - hop + j)].real();
            wetR[(size_t) j] = (double) zBuf[(size_t) (n - hop + j)].imag();
        }

        if (! soloing)
        {
            for (int j = 0; j < hop; ++j)
                soloL[(size_t) j] = soloR[(size_t) j] = 0.0;
            return;
        }

        // What the soloed band adds, in its own placement, from the input (the earlier bands are not in it).
        for (int k = 0; k < n; ++k)
        {
            const Cx a = specBuf[(size_t) k], b = conjugate (specBuf[(size_t) ((n - k) & (n - 1))]);
            const Cx xl = scale (add (a, b), 0.5f);
            const Cx v = sub (a, b);
            const Cx xr { 0.5f * v.imag(), -0.5f * v.real() };
            const Cx& h = kernel[4][(size_t) k];
            Cx sl {}, sr {};
            switch (soloPlace)
            {
                case Placement::stereo: sl = mul (h, xl); sr = mul (h, xr); break;
                case Placement::left:   sl = mul (h, xl); break;
                case Placement::right:  sr = mul (h, xr); break;
                case Placement::mid:    sl = sr = mul (h, scale (add (xl, xr), 0.5f)); break;
                case Placement::side:   sl = mul (h, scale (sub (xl, xr), 0.5f)); sr = { -sl.real(), -sl.imag() }; break;
            }
            outBuf[(size_t) k] = { sl.real() - sr.imag(), sl.imag() + sr.real() };
        }
        plan->fft->perform (outBuf.data(), zBuf.data(), true);
        for (int j = 0; j < hop; ++j)
        {
            soloL[(size_t) j] = (double) zBuf[(size_t) (n - hop + j)].real();
            soloR[(size_t) j] = (double) zBuf[(size_t) (n - hop + j)].imag();
        }
    }

    // The kernel's magnitude and delay, for the tests: the impulse response of the left/right kernel (taps 0 ... 2 * half).
    std::vector<float> leftImpulseResponse() const
    {
        const int n = plan->spec.size();
        std::vector<Cx> h ((size_t) n);
        plan->fft->perform (kernel[0].data(), h.data(), true);
        std::vector<float> out ((size_t) (2 * plan->spec.half + 1));
        for (size_t i = 0; i < out.size(); ++i)
            out[i] = h[i].real();
        return out;
    }

private:
    using Cx = juce::dsp::Complex<float>;

    struct Plan
    {
        LinearSpec spec {};
        std::unique_ptr<juce::dsp::FFT> fft;
        std::vector<float> window;
        std::vector<double> cosW, cos2W;
        std::vector<Cx> delay; // the spectrum of the plain delay by `half` samples
    };

    static double bessel0 (double x) noexcept
    {
        double sum = 1.0, term = 1.0;
        for (int k = 1; k < 60; ++k)
        {
            term *= (x / (2.0 * k)) * (x / (2.0 * k));
            sum += term;
        }
        return sum;
    }

    static Cx add (Cx a, Cx b) noexcept { return { a.real() + b.real(), a.imag() + b.imag() }; }
    static Cx sub (Cx a, Cx b) noexcept { return { a.real() - b.real(), a.imag() - b.imag() }; }
    static Cx scale (Cx a, float k) noexcept { return { a.real() * k, a.imag() * k }; }
    static Cx conjugate (Cx a) noexcept { return { a.real(), -a.imag() }; }
    static Cx mul (Cx a, Cx b) noexcept { return { a.real() * b.real() - a.imag() * b.imag(), a.real() * b.imag() + a.imag() * b.real() }; }

    // Two zero-phase responses (real, on the half grid; they can be negative) become two linear-phase kernels with one pair of transforms: the first
    // goes in the real part, the second in the imaginary part. With `asDifference` the plain delay is taken off, so the kernel is
    // what the band adds. With `single`, only the first is wanted.
    void buildPair (const std::vector<double>& aa, const std::vector<double>& ab, std::vector<Cx>& outA, std::vector<Cx>& outB,
                    bool asDifference, bool single = false) noexcept
    {
        const int n = plan->spec.size(), half = plan->spec.half;
        for (int k = 0; k <= n / 2; ++k)
        {
            const Cx z { (float) aa[(size_t) k], single ? 0.0f : (float) ab[(size_t) k] };
            zBuf[(size_t) k] = z;
            if (k > 0 && k < n / 2)
                zBuf[(size_t) (n - k)] = z;
        }
        plan->fft->perform (zBuf.data(), uBuf.data(), true); // the zero-phase impulse responses, centred on sample 0

        std::fill (zBuf.begin(), zBuf.begin() + n, Cx {});
        for (int m = 0; m <= 2 * half; ++m)
        {
            const int at = (m - half + n) & (n - 1);
            zBuf[(size_t) m] = scale (uBuf[(size_t) at], plan->window[(size_t) m]);
        }
        plan->fft->perform (zBuf.data(), uBuf.data(), false);

        for (int k = 0; k < n; ++k)
        {
            const Cx a = uBuf[(size_t) k], b = conjugate (uBuf[(size_t) ((n - k) & (n - 1))]);
            const Cx v = sub (a, b);
            Cx ha = scale (add (a, b), 0.5f);
            Cx hb { 0.5f * v.imag(), -0.5f * v.real() };
            if (asDifference)
            {
                ha = sub (ha, plan->delay[(size_t) k]);
                hb = sub (hb, plan->delay[(size_t) k]);
            }
            outA[(size_t) k] = ha;
            if (! single)
                outB[(size_t) k] = hb;
        }
    }

    std::array<std::unique_ptr<Plan>, 2> plans;
    Plan* plan = nullptr;

    std::vector<double> ringL, ringR;            // the input history (2 * the largest FFT, a power of two)
    std::vector<double> wetL, wetR, soloL, soloR; // the block being handed out
    std::vector<Cx> zBuf, uBuf, specBuf, outBuf;
    std::array<std::vector<Cx>, 5> kernel;       // left to left, right to right, right to left, left to right, the soloed band (what it adds)
    std::array<std::vector<double>, 4> power;    // |H|^2 of the left, right, mid and side channels
    std::array<std::vector<double>, 5> amp;      // the four responses of the matrix above, then the soloed band's
    bool usesMS = false;
    long long count = 0;
    int pos = 0;
    bool due = false;
};
} // namespace iris
