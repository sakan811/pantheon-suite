// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 The Pantheon authors

// Headless DSP verification for Hephaestus. Prints measured numbers (not only pass/fail) so the
// behaviour documented in the README can be checked against reality.

#include "Check.h"
#include "DSP/Engine.h"
#include <pantheon/dsp/SpectrumAnalyzer.h>
#include "TestSignals.h"
#include <chrono>
#include <limits>
#include <random>

using namespace hep;
using namespace test;

namespace
{
double db (double lin) { return 20.0 * std::log10 (std::max (lin, 1.0e-12)); }

// Hann-windowed single-frequency DFT; returns the amplitude of a stationary sinusoid.
double amplitudeAt (const std::vector<float>& x, int start, int len, double f)
{
    double re = 0, im = 0, wsum = 0;
    for (int i = 0; i < len; ++i)
    {
        const double w = 0.5 - 0.5 * std::cos (2.0 * M_PI * i / (len - 1));
        const double ph = 2.0 * M_PI * f * (start + i) / fs;
        re += w * x[(size_t) (start + i)] * std::cos (ph);
        im -= w * x[(size_t) (start + i)] * std::sin (ph);
        wsum += w;
    }
    return 2.0 * std::sqrt (re * re + im * im) / wsum;
}

// Amplitude over the second half of a one-second signal, where every filter has settled.
double settledAmplitude (const std::vector<float>& x, double f) { return amplitudeAt (x, 24000, 16000, f); }

BandParams saturateBand (double lo, double hi, double drive)
{
    BandParams b;
    b.on = true;
    b.mode = BandMode::saturate;
    b.lo = lo;
    b.hi = hi;
    b.drive = drive;
    return b;
}

BandParams fillBand (const FundamentalEngine::Params& fill)
{
    BandParams b;
    b.on = true;
    b.mode = BandMode::fill;
    b.lo = fill.lo;
    b.hi = fill.hi;
    b.fill = fill;
    return b;
}

// One full-range saturate band at 0 dB drive: the plugin's "nothing to hear" baseline.
EngineParams cleanParams()
{
    EngineParams e;
    e.bands[0] = saturateBand (20.0, 20000.0, 1.0);
    e.osMode = 2;
    return e;
}

struct Stereo { std::vector<float> l, r; };

// An engine plus its parameters, fed in `block`-sized pieces.
struct Rig
{
    Engine eng;
    EngineParams p = cleanParams();

    explicit Rig (int os = 2) { eng.prepare (fs, 512); p.osMode = os; }

    Stereo run (const std::vector<float>& left, const std::vector<float>* rightIn = nullptr, int block = 480)
    {
        const auto& right = rightIn ? *rightIn : left;
        Stereo out { std::vector<float> (left.size()), std::vector<float> (left.size()) };
        juce::AudioBuffer<float> buf (2, block);

        for (size_t pos = 0; pos < left.size(); pos += (size_t) block)
        {
            const int n = (int) std::min ((size_t) block, left.size() - pos);
            std::copy_n (left.begin() + (long) pos, n, buf.getWritePointer (0));
            std::copy_n (right.begin() + (long) pos, n, buf.getWritePointer (1));
            juce::AudioBuffer<float> view (buf.getArrayOfWritePointers(), 2, n);
            eng.process (view, p);
            std::copy_n (buf.getReadPointer (0), n, out.l.begin() + (long) pos);
            std::copy_n (buf.getReadPointer (1), n, out.r.begin() + (long) pos);
        }
        return out;
    }

    std::vector<float> runMono (const std::vector<float>& in, int block = 480) { return run (in, nullptr, block).l; }
};

std::vector<float> noise (int n, unsigned seed, float amp = 0.3f)
{
    std::mt19937 rng (seed);
    std::uniform_real_distribution<float> u (-amp, amp);
    std::vector<float> x ((size_t) n);
    for (auto& v : x) v = u (rng);
    return x;
}

// ------------------------------------------------------------------------------------------
void testTransparency()
{
    std::printf ("\n[1] Small-signal transparency (sat on, drive 0 dB, -60 dBFS sines; gain must be flat)\n");
    const char* names[] = { "off", "2x", "4x", "8x" };

    for (int os = 0; os < 4; ++os)
    {
        Rig rig (os);
        double worst = 0;
        std::string line;
        for (double f : { 40.0, 150.0, 1000.0, 3500.0, 8000.0, 15000.0, 20000.0 })
        {
            const auto in = sines ({ { f, 0.001 } }, 48000);
            const double g = db (settledAmplitude (rig.runMono (in), f) / settledAmplitude (in, f));
            worst = std::max (worst, std::fabs (g));
            line += fmt (" %.0fHz:%+.2f", f, g);
        }
        check (worst < 0.2, fmt ("os=%s (latency %d) worst %.3f dB |", names[os], rig.eng.getLatency(), worst) + line);
    }

    // Bands overlap and leave gaps freely; the clean signal never goes through them.
    Rig many;
    many.p.bands = { saturateBand (60, 300, 8.0), saturateBand (200, 5000, 8.0), saturateBand (3000, 12000, 8.0),
                     saturateBand (100, 400, 8.0), saturateBand (20, 20000, 8.0), saturateBand (500, 700, 8.0) };
    double worst = 0;
    for (double f : { 40.0, 150.0, 1000.0, 3500.0, 8000.0, 15000.0 })
    {
        const auto in = sines ({ { f, 0.001 } }, 48000);
        worst = std::max (worst, std::fabs (db (settledAmplitude (many.runMono (in), f) / settledAmplitude (in, f))));
    }
    check (worst < 0.2, fmt ("six overlapping bands at 18 dB drive: worst %.3f dB", worst));
}

void testLatencyAlignment()
{
    std::printf ("\n[2] Latency alignment: dry impulse must land exactly on the reported latency\n");
    const auto peakAt = [] (const std::vector<float>& v)
    {
        return (int) (std::max_element (v.begin(), v.end(), [] (float a, float b) { return std::fabs (a) < std::fabs (b); }) - v.begin());
    };

    for (int os = 0; os < 4; ++os)
    {
        std::vector<float> imp (4096, 0.0f);
        imp[100] = 0.001f;

        Rig dryRig (os), wetRig (os);
        dryRig.p.mix = 0.0f;
        wetRig.p.mix = 1.0f;
        const int dryAt = peakAt (dryRig.runMono (imp)) - 100, wetAt = peakAt (wetRig.runMono (imp)) - 100;
        const int latency = dryRig.eng.getLatency();
        check (dryAt == latency && std::abs (wetAt - latency) <= 4,
               fmt ("os=%d latency %d, dry peak at +%d, wet peak at +%d", os, latency, dryAt, wetAt));
    }
}

void testAliasing()
{
    std::printf ("\n[3] Aliasing: 5.3 kHz sine, tanh, +18 dB drive. Alias energy relative to the fundamental\n");
    const int N = 1 << 16;
    const double f = 7237.0 * fs / N; // exactly on a bin
    const auto in = sines ({ { f, 0.5 } }, N + 16384);

    const auto aliasLevel = [&] (const std::vector<float>& y)
    {
        std::vector<float> data ((size_t) N * 2, 0.0f);
        for (int i = 0; i < N; ++i)
            data[(size_t) i] = y[(size_t) (16384 + i)] * (float) (0.5 - 0.5 * std::cos (2.0 * M_PI * i / (N - 1)));
        juce::dsp::FFT fft (16);
        fft.performFrequencyOnlyForwardTransform (data.data());

        const double binHz = fs / N;
        double pFund = 0, pAlias = 0;
        for (int k = 20; k < N / 2; ++k)
        {
            const double hz = k * binHz;
            bool harmonic = false;
            for (int h = 1; h * f < fs * 0.5 + 100; h += 2) // odd harmonics expected from a symmetric curve
                if (std::fabs (hz - h * f) < 12 * binHz) harmonic = true;
            const double pw = (double) data[(size_t) k] * data[(size_t) k];
            if (std::fabs (hz - f) < 12 * binHz) pFund += pw;
            else if (! harmonic) pAlias += pw;
        }
        return 10.0 * std::log10 (pAlias / pFund + 1.0e-30);
    };

    // Naive reference: same drive, plain tanh at the base rate, no ADAA, no oversampling.
    std::vector<float> naive (in.size());
    for (size_t i = 0; i < in.size(); ++i)
        naive[i] = (float) (std::tanh (8.0 * in[i]) / 8.0);
    const double naiveDb = aliasLevel (naive);
    std::printf ("  naive tanh, no ADAA, no oversampling : %7.1f dB\n", naiveDb);

    double levels[4];
    for (int os = 0; os < 4; ++os)
    {
        Rig rig (os);
        rig.p.bands[0].drive = 8.0; // +18 dB
        levels[os] = aliasLevel (rig.runMono (in));
        std::printf ("  Hephaestus os=%d                       : %7.1f dB\n", os, levels[os]);
    }

    check (levels[0] < naiveDb - 6.0, "ADAA alone beats the naive shaper by > 6 dB");
    check (levels[2] < levels[0] - 10.0, "4x oversampling is > 10 dB cleaner than ADAA alone");
    check (levels[3] <= levels[2] + 1.0, "8x is no worse than 4x");
}

void testEvenOdd()
{
    std::printf ("\n[4] Even/odd control: 1 kHz sine, tanh, +12 dB drive on a 300 Hz - 4 kHz band\n");
    const auto in = sines ({ { 1000.0, 0.3 } }, 48000);

    for (double bias : { 0.0, 0.5 })
    {
        Rig rig;
        rig.p.bands[0] = saturateBand (300.0, 4000.0, 4.0);
        rig.p.bias = bias;
        const auto out = rig.runMono (in);
        const double a1 = settledAmplitude (out, 1000);
        const double h2 = db (settledAmplitude (out, 2000) / a1), h3 = db (settledAmplitude (out, 3000) / a1);
        const auto detail = fmt ("bias %.1f: H2 %+.1f dB, H3 %+.1f dB (re fundamental)", bias, h2, h3);
        check (bias == 0.0 ? (h2 < -90.0 && h3 > -40.0) : h2 > -40.0, detail);
    }
}

FundamentalEngine::Params extendParams()
{
    FundamentalEngine::Params p;
    p.extGain = 1.0;
    const double weightsDb[4] = { -6.0, -9.0, -15.0, -22.0 };
    for (int i = 0; i < 4; ++i)
        p.harmonic[i] = std::pow (10.0, weightsDb[i] / 20.0);
    return p;
}

std::vector<float> runFund (const std::vector<float>& in, const FundamentalEngine::Params& p)
{
    FundamentalEngine fe;
    fe.prepare (fs);
    std::vector<float> out (in.size());
    for (size_t pos = 0; pos < in.size(); pos += 480)
    {
        const int n = (int) std::min<size_t> (480, in.size() - pos);
        const float* inp[1] = { in.data() + pos };
        float* outp[1] = { out.data() + pos };
        fe.process (inp, outp, 1, n, p);
    }
    return out;
}

// Magnitude of the engine's 4th-order Butterworth bass-range filter at f.
double rangeGain (double f, const FundamentalEngine::Params& p)
{
    const double hi = effectiveRange (BandMode::fill, p.lo, p.hi).hi;
    return 1.0 / std::sqrt (1.0 + std::pow (p.lo / f, 8.0)) / std::sqrt (1.0 + std::pow (f / hi, 8.0));
}

void testExtend()
{
    std::printf ("\n[5] Harmonic Bass (EXTEND): weights relative to the band-passed fundamental, across the range\n");
    const double expected[4] = { -6.0, -9.0, -15.0, -22.0 };
    const auto params = extendParams();

    for (double f0 : { 45.0, 60.0, 90.0 })
        for (double amp : { 0.5, 0.05 })
        {
            const auto in = sines ({ { f0, amp } }, 96000);
            const auto add = runFund (in, params);
            // Harmonics are defined relative to the fundamental *as the range filter passes it*; a
            // fundamental at the very edge of the range is attenuated by that filter by design.
            const double ref = amplitudeAt (in, 48000, 32000, f0) * rangeGain (f0, params);

            double worst = 0;
            std::string line;
            for (int h = 0; h < 4; ++h)
            {
                const double g = db (amplitudeAt (add, 48000, 32000, f0 * (h + 2)) / ref);
                worst = std::max (worst, std::fabs (g - expected[h]));
                line += fmt (" H%d:%+.1f", h + 2, g);
            }
            const double sub = db (amplitudeAt (add, 48000, 32000, f0 / 2) / ref);
            const double fund = db (amplitudeAt (add, 48000, 32000, f0) / ref);
            check (worst < 1.5 && sub < -45.0 && fund < -25.0,
                   fmt ("%3.0f Hz @ %.2f: max deviation %.2f dB |", f0, amp, worst) + line
                       + fmt (" | f/2 %.0f dB, f %.0f dB", sub, fund));
        }
}

void testExtendNoteOff()
{
    std::printf ("\n[5b] Harmonic Bass must not hang after a note ends (envelope release vs. tails)\n");
    auto burst = sines ({ { 60.0, 0.5 } }, 72000);
    std::fill (burst.begin() + 24000, burst.end(), 0.0f); // note ends at 0.5 s
    const auto add = runFund (burst, extendParams());

    const auto rms = [&] (int from, int to)
    {
        double s = 0;
        for (int i = from; i < to; ++i) s += (double) add[(size_t) i] * add[(size_t) i];
        return std::sqrt (s / (to - from));
    };
    const double sustain = rms (12000, 22000);
    for (double afterMs : { 50.0, 100.0 })
    {
        const int from = 24000 + (int) (afterMs * 48.0);
        const double tail = db (rms (from, from + 2400) / sustain);
        check (tail < -30.0, fmt ("harmonic output %3.0f ms after note-off: %.0f dB re sustain", afterMs, tail));
    }
}

// Energy of x over [start, start + len) that is not on a multiple of f0 (up to the 5th), in dB re everything in the window. len must hold
// whole periods of f0, or the fitted sines are not orthogonal and the window leaks into its own residual.
double offHarmonicDb (const std::vector<float>& x, int start, int len, double f0)
{
    std::vector<double> rest (x.begin() + start, x.begin() + start + len);
    double total = 0.0;
    for (double v : rest) total += v * v;
    for (int k = 1; k <= 5; ++k)
    {
        double c = 0, s = 0;
        for (int i = 0; i < len; ++i)
        {
            const double ph = 2.0 * M_PI * k * f0 * (start + i) / fs;
            c += rest[(size_t) i] * std::cos (ph);
            s += rest[(size_t) i] * std::sin (ph);
        }
        c *= 2.0 / len;
        s *= 2.0 / len;
        for (int i = 0; i < len; ++i)
            rest[(size_t) i] -= c * std::cos (2.0 * M_PI * k * f0 * (start + i) / fs) + s * std::sin (2.0 * M_PI * k * f0 * (start + i) / fs);
    }
    double left = 0.0;
    for (double v : rest) left += v * v;
    return 10.0 * std::log10 (std::max (left, 1.0e-30) / std::max (total, 1.0e-30));
}

// A window of whole periods of f0 that starts at `startSec`.
int wholePeriods (double f0, double seconds) { return (int) std::floor (f0 * seconds) / f0 * fs; }

// Energy at the frequencies that are not multiples of either tone, in dB re the energy at those that are, over a window of whole 10 Hz periods.
double intermodDb (const std::vector<float>& x, int start, double f1, double f2)
{
    constexpr int len = 48000;
    double wanted = 0.0, other = 0.0;
    for (int k = 1; k <= 60; ++k)
    {
        const double f = 10.0 * k;
        double c = 0, s = 0;
        for (int i = 0; i < len; ++i)
        {
            c += x[(size_t) (start + i)] * std::cos (2.0 * M_PI * f * (start + i) / fs);
            s += x[(size_t) (start + i)] * std::sin (2.0 * M_PI * f * (start + i) / fs);
        }
        const double e = (c * c + s * s) * 4.0 / ((double) len * len);
        const bool onTone = std::fabs (std::remainder (f, f1)) < 1.0e-6 || std::fabs (std::remainder (f, f2)) < 1.0e-6;
        (onTone ? wanted : other) += e;
    }
    return 10.0 * std::log10 (std::max (other, 1.0e-30) / std::max (wanted, 1.0e-30));
}

double rmsOf (const std::vector<float>& x, int from, int to)
{
    double s = 0;
    for (int i = from; i < to; ++i) s += (double) x[(size_t) i] * x[(size_t) i];
    return std::sqrt (s / (to - from));
}

std::vector<float> runFundAt (const std::vector<float>& in, const FundamentalEngine::Params& p, double rate)
{
    FundamentalEngine fe;
    fe.prepare (rate);
    std::vector<float> out (in.size());
    for (size_t pos = 0; pos < in.size(); pos += 480)
    {
        const int n = (int) std::min<size_t> (480, in.size() - pos);
        const float* inp[1] = { in.data() + pos };
        float* outp[1] = { out.data() + pos };
        fe.process (inp, outp, 1, n, p);
    }
    return out;
}

void testCleanFill()
{
    std::printf ("\n[5g] Clean fill: harmonics built as sines locked to the band's strongest note, not shaped from the signal (band 40-120 Hz)\n");
    auto clean = extendParams();
    clean.character = Character::clean;
    const auto warm = extendParams();
    const double expected[4] = { -6.0, -9.0, -15.0, -22.0 };

    // The same weights as Warm, and nothing besides them in the output.
    for (double f0 : { 45.0, 60.0, 90.0 })
        for (double amp : { 0.5, 0.005 })
        {
            const auto in = sines ({ { f0, amp } }, 144000);
            const auto add = runFund (in, clean);
            const double ref = amplitudeAt (in, 72000, 32000, f0) * rangeGain (f0, clean);
            double worst = 0;
            std::string line;
            for (int h = 0; h < 4; ++h)
            {
                const double g = db (amplitudeAt (add, 72000, 32000, f0 * (h + 2)) / ref);
                worst = std::max (worst, std::fabs (g - expected[h]));
                line += fmt (" H%d:%+.1f", h + 2, g);
            }
            const int len = wholePeriods (f0, 1.0);
            const double leftover = offHarmonicDb (add, 72000, len, f0), warmLeftover = offHarmonicDb (runFund (in, warm), 72000, len, f0);
            check (worst < 1.5 && leftover < -45.0 && leftover < warmLeftover - 12.0,
                   fmt ("%3.0f Hz @ %.3f: max deviation %.2f dB |", f0, amp, worst) + line
                       + fmt (" | not a harmonic: %.0f dB (Warm %.0f dB)", leftover, warmLeftover));
        }

    // A note that decays (an 808): the balance of the harmonics does not move with the level.
    {
        std::vector<float> in (144000);
        for (size_t i = 0; i < in.size(); ++i)
            in[i] = (float) (0.6 * std::exp (-(double) i / fs / 0.5) * std::sin (2.0 * M_PI * 60.0 * (double) i / fs));
        const auto add = runFund (in, clean);
        const auto ratio = [&] (int at)
        {
            return db (amplitudeAt (add, at, 4800, 120.0) / amplitudeAt (in, at, 4800, 60.0));
        };
        const double early = ratio (14400), late = ratio (96000);
        check (std::fabs (early - late) < 1.0,
               fmt ("decaying note: 2nd harmonic re fundamental %+.2f dB at 0.3 s and %+.2f dB at 2 s (%.0f dB lower)", early, late,
                    db (amplitudeAt (in, 14400, 4800, 60.0) / amplitudeAt (in, 96000, 4800, 60.0))));
    }

    // A level that moves faster than Warm's envelope follows (a 3 Hz tremolo, 30 - 100 %): its harmonics fall away when the level falls,
    // because the k-th Chebyshev harmonic scales as a^k when the envelope is above the peak. Clean's follow the level.
    {
        std::vector<float> in (192000);
        for (size_t i = 0; i < in.size(); ++i)
        {
            const double t = (double) i / fs;
            in[i] = (float) (0.5 * (0.65 + 0.35 * std::sin (2.0 * M_PI * 3.0 * t)) * std::sin (2.0 * M_PI * 60.0 * t));
        }
        const auto spread = [&] (const FundamentalEngine::Params& p)
        {
            const auto add = runFund (in, p);
            double lowest = 1.0e9, highest = -1.0e9;
            for (int at = 48000; at + 2400 < (int) in.size(); at += 1200)
            {
                const double r = db (amplitudeAt (add, at, 2400, 120.0) / amplitudeAt (in, at, 2400, 60.0));
                lowest = std::min (lowest, r);
                highest = std::max (highest, r);
            }
            return highest - lowest;
        };
        const double cleanSpread = spread (clean), warmSpread = spread (warm);
        check (cleanSpread < 12.0 && cleanSpread < warmSpread - 30.0,
               fmt ("a 3 Hz tremolo (30 - 100 %%): the 2nd harmonic re the fundamental moves %.1f dB over the cycle (Warm %.1f dB)", cleanSpread, warmSpread));
    }

    // A band edge that keeps moving (the user drags it): the filters are redesigned every block, but the tracker must not be re-centred
    // each time, or the note it locked to is lost again and again (the harmonics come out at the wrong pitch or not at all).
    {
        const auto in = sines ({ { 60.0, 0.5 } }, 144000);
        std::vector<float> out (in.size());
        FundamentalEngine fe;
        fe.prepare (fs);
        auto p = clean;
        int block = 0;
        for (size_t pos = 0; pos < in.size(); pos += 480, ++block)
        {
            p.hi = clean.hi + 8.0 * (0.5 + 0.5 * std::sin (2.0 * M_PI * (double) block / 40.0));
            const int n = (int) std::min<size_t> (480, in.size() - pos);
            const float* inp[1] = { in.data() + pos };
            float* outp[1] = { out.data() + pos };
            fe.process (inp, outp, 1, n, p);
        }
        const double ref = amplitudeAt (in, 72000, 32000, 60.0) * rangeGain (60.0, clean);
        const double h2 = db (amplitudeAt (out, 72000, 32000, 120.0) / ref);
        check (std::fabs (h2 - expected[0]) < 1.5,
               fmt ("hi edge moving every block: 2nd harmonic %+.2f dB (expected %+.1f) - the tracker keeps its lock", h2, expected[0]));
    }

    // A note that arrives at full level: no overshoot while the harmonics settle.
    {
        std::vector<float> in (144000, 0.0f);
        for (size_t i = 24000; i < in.size(); ++i)
            in[i] = (float) (0.6 * std::sin (2.0 * M_PI * 60.0 * (double) i / fs));
        const auto add = runFund (in, clean);
        double steadyPeak = 0, earlyPeak = 0, finalRms = rmsOf (add, 120000, 144000);
        for (int i = 120000; i < 144000; ++i) steadyPeak = std::max (steadyPeak, (double) std::fabs (add[(size_t) i]));
        for (int i = 24000; i < 24000 + 9600; ++i) earlyPeak = std::max (earlyPeak, (double) std::fabs (add[(size_t) i]));
        int reached = -1;
        for (int i = 24000; i + 240 < 144000 && reached < 0; i += 240)
            if (rmsOf (add, i, i + 240) >= 0.9 * finalRms)
                reached = (i - 24000) * 1000 / (int) fs;
        check (earlyPeak < 1.1 * steadyPeak && reached >= 0 && reached <= 150,
               fmt ("note on at full level: peak of the first 200 ms %.3f vs %.3f settled; reaches 90%% of its level after %d ms", earlyPeak, steadyPeak, reached));
    }

    // Two notes in the band: with one far louder it is the only one filled and nothing else appears; with two alike it backs off
    // instead of intermodulating (the Warm path adds as much between the notes as it does on them).
    {
        const auto lone = runFund (sines ({ { 50.0, 0.3 } }, 144000), clean);
        const double loneRms = rmsOf (lone, 72000, 144000);
        const auto dominant = sines ({ { 50.0, 0.3 }, { 70.0, 0.03 } }, 144000);
        const auto alike = sines ({ { 50.0, 0.3 }, { 70.0, 0.3 } }, 144000);

        const double cleanMix = intermodDb (runFund (dominant, clean), 72000, 50.0, 70.0), warmMix = intermodDb (runFund (dominant, warm), 72000, 50.0, 70.0);
        check (cleanMix < -20.0 && cleanMix < warmMix - 8.0,
               fmt ("a note and a 20 dB quieter one a minor third up: energy between the notes %.0f dB re the harmonics (Warm %.0f dB)", cleanMix, warmMix));

        const double cleanLevel = db (rmsOf (runFund (alike, clean), 72000, 144000) / loneRms), warmLevel = db (rmsOf (runFund (alike, warm), 72000, 144000) / loneRms);
        check (cleanLevel < -6.0 && cleanLevel < warmLevel - 8.0,
               fmt ("two notes of equal level: output %+.1f dB re a lone note (Warm %+.1f dB)", cleanLevel, warmLevel));
    }

    // The note can move: a glide across the band, then holding, is followed.
    {
        std::vector<float> in (144000);
        double phase = 0;
        for (size_t i = 0; i < in.size(); ++i)
        {
            const double t = (double) i / fs;
            phase += 2.0 * M_PI * (t < 1.0 ? 100.0 - 50.0 * t : 50.0) / fs;
            in[i] = (float) (0.5 * std::sin (phase));
        }
        const auto add = runFund (in, clean);
        const double ref = amplitudeAt (in, 72000, 32000, 50.0) * rangeGain (50.0, clean);
        const double h2 = db (amplitudeAt (add, 72000, 32000, 100.0) / ref);
        const double glideLevel = db (rmsOf (add, 24000, 48000) / rmsOf (add, 100000, 144000));
        check (std::fabs (h2 - expected[0]) < 1.5 && offHarmonicDb (add, 72000, wholePeriods (50.0, 1.0), 50.0) < -45.0 && std::fabs (glideLevel) < 3.0,
               fmt ("100 -> 50 Hz glide, then held: H2 %+.1f dB (expected %.1f), not a harmonic %.0f dB, level during the glide %+.1f dB re held",
                    h2, expected[0], offHarmonicDb (add, 72000, wholePeriods (50.0, 1.0), 50.0), glideLevel));
    }

    // Stereo: a partial in one channel only is filled in that channel only.
    {
        const auto left = sines ({ { 60.0, 0.4 } }, 96000);
        const std::vector<float> silent (left.size(), 0.0f);
        Rig rig;
        rig.p.bands[0] = fillBand (clean);
        const auto out = rig.run (left, &silent);
        const double l = rmsOf (out.l, 48000, 96000), r = rmsOf (out.r, 48000, 96000);
        check (l > 0.01 && db (r / l) < -90.0, fmt ("a left-only note: left %.3f rms, right %.1e (%.0f dB)", l, r, db (r / l)));
    }

    // The engine around it: what the band adds is what its solo plays, and silence adds nothing.
    {
        const auto in = sines ({ { 60.0, 0.4 }, { 1000.0, 0.1 } }, 96000);
        Rig deltaRig, soloRig;
        for (auto* r : { &deltaRig, &soloRig })
            r->p.bands[0] = fillBand (clean);
        deltaRig.p.delta = true;
        soloRig.eng.monitor.soloKind = (int) SoloKind::adds;
        soloRig.eng.monitor.soloBand = 0;
        const auto whole = deltaRig.runMono (in), solo = soloRig.runMono (in);
        float worst = 0, peak = 0;
        for (size_t i = 48000; i < in.size(); ++i)
        {
            worst = std::max (worst, std::fabs (whole[i] - solo[i]));
            peak = std::max (peak, std::fabs (whole[i]));
        }
        check (peak > 0.05f && worst < 1.0e-4f, fmt ("delta equals the band's ADDS solo (peak %.3f, max difference %.1e)", peak, worst));

        const auto quiet = runFund (std::vector<float> (48000, 0.0f), clean);
        check (rmsOf (quiet, 0, 48000) < 1.0e-12, "silence in, silence out");
    }

    // Other host rates (the harmonics must still fit below Nyquist; the pitch bank is tuned in Hz).
    for (double rate : { 44100.0, 96000.0, 192000.0 })
    {
        std::vector<float> in ((size_t) (3.0 * rate));
        for (size_t i = 0; i < in.size(); ++i)
            in[i] = (float) (0.4 * std::sin (2.0 * M_PI * 60.0 * (double) i / rate));
        const auto add = runFundAt (in, clean, rate);
        double re = 0, im = 0;
        const size_t from = (size_t) (1.5 * rate), count = (size_t) (rate); // 60 whole periods: 60 Hz * 1 s
        for (size_t i = from; i < from + count; ++i)
        {
            re += add[i] * std::cos (2.0 * M_PI * 120.0 * (double) i / rate);
            im += add[i] * std::sin (2.0 * M_PI * 120.0 * (double) i / rate);
        }
        const double ref = 0.4 * rangeGain (60.0, clean);
        const double h2 = db (2.0 * std::sqrt (re * re + im * im) / (double) count / ref);
        check (std::fabs (h2 - expected[0]) < 1.5, fmt ("%6.0f Hz host: H2 of a 60 Hz note %+.1f dB (expected %.1f)", rate, h2, expected[0]));
    }

    // Switching between the three while playing stays finite and within bounds.
    {
        const auto in = sines ({ { 60.0, 0.5 }, { 95.0, 0.2 } }, 96000);
        FundamentalEngine fe;
        fe.prepare (fs);
        std::vector<float> out (in.size());
        bool finite = true;
        float peak = 0;
        for (size_t pos = 0, block = 0; pos < in.size(); pos += 480, ++block)
        {
            auto p = extendParams();
            p.character = (Character) ((block / 7) % 3);
            const float* inp[1] = { in.data() + pos };
            float* outp[1] = { out.data() + pos };
            fe.process (inp, outp, 1, 480, p);
            for (size_t i = pos; i < pos + 480; ++i)
            {
                finite = finite && std::isfinite (out[i]);
                peak = std::max (peak, std::fabs (out[i]));
            }
        }
        check (finite && peak < 2.0f, fmt ("switching Warm / Clean / Octave every 7 blocks: finite, peak %.2f", peak));
    }
}

void testOctaveFill()
{
    std::printf ("\n[5h] Octave fill: every resolved partial's harmonics made in the spectrum, phase-locked and late by N - 1 samples (band 500-2000 Hz)\n");
    auto octave = extendParams();
    octave.character = Character::octave;
    octave.lo = 500.0;
    octave.hi = 2000.0;
    octave.ceiling = 20000.0;
    auto warm = octave;
    warm.character = Character::warm;
    const double weightsDb[4] = { -6.0, -9.0, -15.0, -22.0 };
    const int latency = OctaveGenerator::latencyFor (fs);
    constexpr double pi = M_PI;

    // The weights are exact, for partials that fall between bins, at any level.
    for (double amp : { 0.3, 0.003 })
    {
        double worst = 0.0, worstAt = 0.0;
        for (double f0 : { 520.0, 733.3, 1000.0, 1234.5, 1777.7 })
        {
            const auto in = sines ({ { f0, amp } }, 144000);
            const auto add = runFund (in, octave);
            for (int h = 0; h < 4; ++h)
            {
                const double g = db (amplitudeAt (add, 72000, 32000, f0 * (h + 2)) / amp) - weightsDb[h];
                if (std::fabs (g) > std::fabs (worst)) { worst = g; worstAt = f0; }
            }
        }
        check (std::fabs (worst) < 0.5, fmt ("partials at 520 / 733.3 / 1000 / 1234.5 / 1777.7 Hz, amplitude %.3f: H2..H5 within %.2f dB of -6 / -9 / -15 / -22 (worst at %.1f Hz)", amp, worst, worstAt));
    }

    // Phase-locked, with the fundamental's own peaks lined up (the convention Warm and Clean have): sample for sample, what comes out is
    // the harmonics of the input as it was `latency` samples ago.
    {
        const double f0 = 1000.0, theta = 0.7, amp = 0.3;
        const auto in = sines ({ { f0, amp } }, 144000);
        std::vector<float> shifted (in.size());
        for (size_t i = 0; i < in.size(); ++i)
            shifted[i] = (float) (amp * std::sin (2.0 * pi * f0 * (double) i / fs + theta));
        const auto add = runFund (shifted, octave);
        double worst = 0.0, peak = 0.0;
        for (int i = 96000; i < 100000; ++i)
        {
            double ideal = 0.0;
            for (int h = 0; h < 4; ++h)
                ideal += std::pow (10.0, weightsDb[h] / 20.0) * amp * std::cos ((h + 2) * (2.0 * pi * f0 * (double) (i - latency) / fs + theta - pi / 2.0));
            worst = std::max (worst, std::fabs (ideal - add[(size_t) i]));
            peak = std::max (peak, std::fabs (ideal));
        }
        check (db (worst / peak) < -25.0, fmt ("a 1 kHz partial: the harmonics are the ideal phase-locked ones delayed by %d samples (max error %.1f dB re their peak)", latency, db (worst / peak)));
    }

    // No intermodulation: with several partials in the band, what is not on a harmonic of one of them.
    {
        struct Case { const char* name; std::vector<double> f; double lo; };
        for (const auto& c : { Case { "three partials 520 / 640 / 780", { 520.0, 640.0, 780.0 }, 500.0 },
                               Case { "six partials 520 ... 1555", { 520.0, 640.0, 780.0, 910.0, 1230.0, 1555.0 }, 500.0 } })
        {
            std::vector<float> in (144000, 0.0f);
            for (double f : c.f)
                for (int i = 0; i < 144000; ++i)
                    in[(size_t) i] += (float) (0.3 / std::sqrt ((double) c.f.size()) * std::sin (2.0 * pi * f * i / fs + f));
            const auto cleanOut = runFund (in, octave), warmOut = runFund (in, warm);
            const auto offLattice = [&] (const std::vector<float>& x)
            {
                double on = 0.0, off = 0.0;
                for (int bin = 20; bin <= 12000; ++bin)
                {
                    double re = 0, im = 0;
                    for (int i = 96000; i < 144000; ++i) { re += x[(size_t) i] * std::cos (2.0 * pi * bin * i / fs); im += x[(size_t) i] * std::sin (2.0 * pi * bin * i / fs); }
                    bool onLattice = false;
                    for (double f : c.f)
                        for (int k = 2; k <= 5; ++k)
                            onLattice = onLattice || std::fabs ((double) bin - k * f) <= 1.0;
                    (onLattice ? on : off) += re * re + im * im;
                }
                return 10.0 * std::log10 (std::max (off, 1.0e-30) / std::max (on, 1.0e-30));
            };
            const double cleanDb = offLattice (cleanOut), warmDb = offLattice (warmOut);
            check (cleanDb < -35.0 && cleanDb < warmDb - 30.0, fmt ("%s: energy that is not on a harmonic %.1f dB re the harmonics (Warm %+.1f dB)", c.name, cleanDb, warmDb));
        }
    }

    // Dynamics: the harmonics follow the level, whatever the envelope would have done.
    {
        std::vector<float> in (192000);
        for (size_t i = 0; i < in.size(); ++i)
        {
            const double t = (double) i / fs;
            in[i] = (float) (0.3 * (0.65 + 0.35 * std::sin (2.0 * pi * 3.0 * t)) * std::sin (2.0 * pi * 1000.0 * t));
        }
        const auto spread = [&] (const FundamentalEngine::Params& p)
        {
            const auto add = runFund (in, p);
            double lowest = 1.0e9, highest = -1.0e9;
            for (int at = 72000; at + 2400 < (int) in.size(); at += 1200)
            {
                const double r = db (amplitudeAt (add, at, 2400, 2000.0) / amplitudeAt (in, at - latency, 2400, 1000.0));
                lowest = std::min (lowest, r);
                highest = std::max (highest, r);
            }
            return highest - lowest;
        };
        const double octaveSpread = spread (octave), warmSpread = spread (warm);
        check (octaveSpread < 1.0, fmt ("a 1 kHz tone under a 3 Hz tremolo (30 - 100 %%): the 2nd harmonic re the fundamental moves %.2f dB over the cycle (Warm %.1f dB)", octaveSpread, warmSpread));
    }

    // A note's start and end: no echo of it before it begins, and a short one after it ends.
    {
        std::vector<float> in (192000, 0.0f);
        for (size_t i = 48000; i < 120000; ++i)
            in[i] = (float) (0.5 * std::sin (2.0 * pi * 1000.0 * (double) i / fs));
        const auto add = runFund (in, octave);
        const double steady = rmsOf (add, 96000 + latency, 120000);
        // Output samples [48000, 48000 + latency) belong to the time before the note started.
        const double before = db (rmsOf (add, 48000, 48000 + latency - 1) / steady);
        int reached = -1;
        for (int i = 48000 + latency; i + 240 < 192000 && reached < 0; i += 48)
            if (rmsOf (add, i, i + 480) >= 0.9 * steady)
                reached = (i - 48000 - latency) * 1000 / (int) fs;
        const double after = db (rmsOf (add, 120000 + latency + 1920, 120000 + latency + 2160) / steady); // 40 ms after it ended
        check (before < -40.0 && reached >= 0 && reached <= 25 && after < -40.0,
               fmt ("a note: %.0f dB re its level before it starts, 90 %% of it %d ms after it does, %.0f dB 40 ms after it ends", before, reached, after));
    }

    // Only the partials in the band are filled.
    {
        const auto inBand = runFund (sines ({ { 1000.0, 0.3 } }, 144000), octave);
        const double reference = amplitudeAt (inBand, 72000, 32000, 2000.0);
        double loudest = -1.0e9;
        for (double f : { 300.0, 420.0, 2300.0, 3500.0 })
        {
            const auto add = runFund (sines ({ { f, 0.3 } }, 144000), octave);
            loudest = std::max (loudest, db (rmsOf (add, 72000, 144000) / (reference / std::sqrt (2.0))));
        }
        check (loudest < -50.0, fmt ("tones at 300 / 420 Hz (under the band) and 2.3 / 3.5 kHz (over it): the loudest thing added is %.0f dB re one partial's 2nd harmonic", loudest));
    }

    // Partials too close to tell apart (60 Hz apart, under four bins) are one peak: its harmonics follow it; what comes out stays far under Warm's.
    {
        std::vector<float> in (144000);
        for (int i = 0; i < 144000; ++i)
            in[(size_t) i] = (float) (0.21 * (std::sin (2.0 * pi * 1000.0 * i / fs) + std::sin (2.0 * pi * 1060.0 * i / fs)));
        const auto bandEnergy = [&] (const std::vector<float>& x, double lo, double hi)
        {
            double e = 0.0;
            for (double f = lo; f <= hi; f += 5.0)
            {
                const double a = amplitudeAt (x, 72000, 48000, f);
                e += a * a;
            }
            return e;
        };
        const auto cleanOut = runFund (in, octave), warmOut = runFund (in, warm);
        // The energy between the two notes' own 2nd harmonics' neighbours (2.0-2.12 kHz holds them) versus 3rd-order products' neighbours (2.5-2.9 kHz).
        const double intermod = 10.0 * std::log10 (bandEnergy (cleanOut, 2200.0, 2900.0) / bandEnergy (cleanOut, 1990.0, 2130.0));
        const double warmIntermod = 10.0 * std::log10 (bandEnergy (warmOut, 2200.0, 2900.0) / bandEnergy (warmOut, 1990.0, 2130.0));
        check (intermod < -20.0 && intermod < warmIntermod - 10.0, fmt ("two partials 60 Hz apart: %.0f dB between their harmonics' neighbourhood and the rest (Warm %.0f dB)", intermod, warmIntermod));
    }

    // Silence adds nothing, noise adds less than it holds, and the block size changes nothing.
    {
        check (rmsOf (runFund (std::vector<float> (48000, 0.0f), octave), 0, 48000) < 1.0e-12, "silence in, silence out");

        std::mt19937 rng (5);
        std::normal_distribution<float> gauss (0.0f, 0.1f);
        std::vector<float> noiseIn (96000);
        for (auto& v : noiseIn) v = gauss (rng);
        const auto noiseOut = runFund (noiseIn, octave);
        float peak = 0.0f;
        for (size_t i = 48000; i < noiseOut.size(); ++i) peak = std::max (peak, std::fabs (noiseOut[i]));
        check (rmsOf (noiseOut, 48000, 96000) < rmsOf (noiseIn, 48000, 96000) && peak < 0.5f, fmt ("white noise 0.1 rms: adds %.3f rms, peak %.2f", rmsOf (noiseOut, 48000, 96000), peak));

        const auto in = sines ({ { 700.0, 0.3 }, { 1230.0, 0.2 } }, 48000);
        const auto whole = runFund (in, octave);
        FundamentalEngine fe;
        fe.prepare (fs);
        std::vector<float> pieces (in.size());
        for (size_t pos = 0; pos < in.size();)
        {
            const size_t n = std::min<size_t> (1 + pos % 97, in.size() - pos); // blocks of 1 .. 97 samples
            const float* inp[1] = { in.data() + pos };
            float* outp[1] = { pieces.data() + pos };
            fe.process (inp, outp, 1, (int) n, octave);
            pos += n;
        }
        float worst = 0.0f;
        for (size_t i = 0; i < in.size(); ++i) worst = std::max (worst, std::fabs (pieces[i] - whole[i]));
        check (worst == 0.0f || worst < 1.0e-7f, fmt ("blocks of 1 to 97 samples give the output of one block of 480 (max difference %.1e)", worst));
    }

    // Stereo: a partial in one channel only is filled in that channel only.
    {
        const auto left = sines ({ { 1000.0, 0.4 } }, 96000);
        const std::vector<float> silent (left.size(), 0.0f);
        Rig rig;
        rig.p.bands[0] = fillBand (octave);
        const auto out = rig.run (left, &silent);
        const double l = rmsOf (out.l, 60000, 96000), r = rmsOf (out.r, 60000, 96000);
        check (l > 0.1 && db (r / l) < -60.0, fmt ("a left-only partial: left %.3f rms, right %.1e (%.0f dB)", l, r, db (r / l)));
    }

    // Other host rates: the frame follows the rate (about 43 ms) and the weights stay exact.
    for (double rate : { 44100.0, 96000.0, 192000.0 })
    {
        const int size = OctaveGenerator::sizeFor (rate);
        std::vector<float> in ((size_t) (3.0 * rate));
        for (size_t i = 0; i < in.size(); ++i)
            in[i] = (float) (0.3 * std::sin (2.0 * pi * 1000.0 * (double) i / rate));
        const auto add = runFundAt (in, octave, rate);
        double re = 0, im = 0;
        const size_t from = (size_t) (1.5 * rate), count = (size_t) rate;
        for (size_t i = from; i < from + count; ++i)
        {
            re += add[i] * std::cos (2.0 * pi * 2000.0 * (double) i / rate);
            im += add[i] * std::sin (2.0 * pi * 2000.0 * (double) i / rate);
        }
        const double h2 = db (2.0 * std::sqrt (re * re + im * im) / (double) count / 0.3);
        check (std::fabs (h2 - weightsDb[0]) < 0.5 && std::fabs (1000.0 * (double) size / rate - 1000.0 * 2048.0 / 48000.0) < 5.0,
               fmt ("%6.0f Hz host: frame %d samples (%.1f ms), H2 of a 1 kHz partial %+.2f dB (expected %.1f)", rate, size, 1000.0 * size / rate, h2, weightsDb[0]));
    }

    // Rebuild, summed with it, is held back the same way.
    {
        auto rebuild = octave;
        rebuild.extGain = 0.0;
        rebuild.resGain = 1.0;
        rebuild.fill = 0.0;
        rebuild.lo = 40.0;
        rebuild.hi = 120.0;
        auto plain = rebuild;
        plain.character = Character::warm;
        const auto in = sines ({ { 120.0, 0.3 }, { 180.0, 0.3 } }, 96000);
        const auto late = runFund (in, rebuild), now = runFund (in, plain);
        float worst = 0.0f;
        for (size_t i = (size_t) latency; i < in.size(); ++i) worst = std::max (worst, std::fabs (late[i] - now[i - (size_t) latency]));
        check (worst < 1.0e-6f, fmt ("an octave band's Rebuild is the plain one, %d samples later (max difference %.1e)", latency, worst));
    }

    // The engine around it: every path is brought to the one latency, bit for bit.
    {
        const auto in = noise (96000, 11, 0.3f);
        const auto settings = [] (Rig& rig, bool withOctave, bool withWarmFill)
        {
            rig.p.bands[0] = saturateBand (60.0, 300.0, 8.0);
            rig.p.bands[1] = saturateBand (1000.0, 3000.0, 6.0);
            if (withWarmFill)
            {
                auto f = extendParams();
                f.lo = 40.0;
                f.hi = 120.0;
                rig.p.bands[2] = fillBand (f);
            }
            if (withOctave)
            {
                auto f = extendParams();
                f.character = Character::octave;
                f.extGain = 0.0; // on, in Octave, with nothing to add: it asks for the latency and for nothing else
                f.lo = 600.0;
                f.hi = 2000.0;
                rig.p.bands[3] = fillBand (f);
            }
        };
        Rig plain, withOctave, plainWarm, warmWithOctave;
        settings (plain, false, false);
        settings (withOctave, true, false);
        settings (plainWarm, false, true);
        settings (warmWithOctave, true, true);
        const auto a = plain.runMono (in), b = withOctave.runMono (in), c = plainWarm.runMono (in), d = warmWithOctave.runMono (in);
        float diffSat = 0.0f, diffWarm = 0.0f;
        for (size_t i = (size_t) latency + 2000; i < in.size(); ++i)
        {
            diffSat = std::max (diffSat, std::fabs (b[i] - a[i - (size_t) latency]));
            diffWarm = std::max (diffWarm, std::fabs (d[i] - c[i - (size_t) latency]));
        }
        check (withOctave.eng.getLatency() == plain.eng.getLatency() + latency,
               fmt ("an octave band adds %d samples to the reported latency (%d -> %d)", latency, plain.eng.getLatency(), withOctave.eng.getLatency()));
        check (diffSat == 0.0f && diffWarm == 0.0f, fmt ("with an octave band on, the saturate bands (max difference %.1e) and a warm fill band (%.1e) are exactly what they were, %d samples later", diffSat, diffWarm, latency));

        // The dry impulse comes out exactly at the reported latency.
        std::vector<float> impulse (8192, 0.0f);
        impulse[100] = 0.001f;
        Rig rig;
        settings (rig, true, false);
        rig.p.mix = 0.0f;
        const auto out = rig.runMono (impulse);
        const int at = (int) (std::max_element (out.begin(), out.end(), [] (float x, float y) { return std::fabs (x) < std::fabs (y); }) - out.begin()) - 100;
        check (at == rig.eng.getLatency(), fmt ("the dry impulse comes out at +%d, the reported latency is %d", at, rig.eng.getLatency()));

        // And what the octave band adds lands on the harmonics of the delayed dry signal.
        {
            const double f0 = 1000.0, theta = 0.7, amp = 0.3;
            std::vector<float> tone (144000);
            for (size_t i = 0; i < tone.size(); ++i)
                tone[i] = (float) (amp * std::sin (2.0 * pi * f0 * (double) i / fs + theta));
            auto f = extendParams();
            f.character = Character::octave;
            f.lo = 500.0;
            f.hi = 2000.0;
            f.ceiling = 20000.0;
            Rig tonal;
            tonal.p.bands[0] = fillBand (f);
            tonal.p.bands[0].fill = f;
            tonal.p.delta = true;
            const auto added = tonal.runMono (tone);
            const int total = tonal.eng.getLatency();
            double worst = 0.0, peak = 0.0;
            for (int i = 96000; i < 100000; ++i)
            {
                double ideal = 0.0;
                for (int h = 0; h < 4; ++h)
                    ideal += std::pow (10.0, weightsDb[h] / 20.0) * amp * std::cos ((h + 2) * (2.0 * pi * f0 * (double) (i - total) / fs + theta - pi / 2.0));
                worst = std::max (worst, std::fabs (ideal - added[(size_t) i]));
                peak = std::max (peak, std::fabs (ideal));
            }
            check (db (worst / peak) < -25.0, fmt ("through the whole engine (delta, %d samples of latency): the added harmonics are the ideal ones (max error %.1f dB re their peak)", total, db (worst / peak)));
        }

        // The band's solo plays what it adds, and the solos of all the bands sum to the delta.
        {
            const auto setup = [&] (Rig& r)
            {
                r.p.bands[0] = saturateBand (60.0, 300.0, 8.0);
                auto f = extendParams();
                f.character = Character::octave;
                f.lo = 600.0;
                f.hi = 2000.0;
                f.ceiling = 20000.0;
                r.p.bands[1] = fillBand (f);
                auto w = extendParams();
                w.lo = 40.0;
                w.hi = 120.0;
                r.p.bands[2] = fillBand (w);
            };
            Rig deltaRig;
            setup (deltaRig);
            deltaRig.p.delta = true;
            const auto whole = deltaRig.runMono (in);
            std::vector<float> sum (in.size(), 0.0f);
            for (int band = 0; band < 3; ++band)
            {
                Rig r;
                setup (r);
                r.eng.monitor.soloKind = (int) SoloKind::adds;
                r.eng.monitor.soloBand = band;
                const auto part = r.runMono (in);
                for (size_t i = 0; i < in.size(); ++i) sum[i] += part[i];
            }
            float worstSum = 0.0f, loudest = 0.0f;
            for (size_t i = 48000; i < in.size(); ++i)
            {
                worstSum = std::max (worstSum, std::fabs (sum[i] - whole[i]));
                loudest = std::max (loudest, std::fabs (whole[i]));
            }
            check (loudest > 0.01f && worstSum < 1.0e-4f, fmt ("saturate, octave and warm bands' ADDS solos sum to the delta (peak %.3f, max error %.1e)", loudest, worstSum));
        }
    }

    // Switching the character while playing, with every band running, stays finite.
    {
        Engine eng;
        eng.prepare (fs, 512);
        std::mt19937 rng (21);
        std::uniform_real_distribution<float> u (-1.0f, 1.0f);
        bool finite = true;
        float peak = 0.0f;
        for (int block = 0; block < 400; ++block)
        {
            EngineParams p;
            p.osMode = block / 100 % 4;
            for (int b = 0; b < 3; ++b)
            {
                auto f = extendParams();
                f.character = (Character) ((block / 7 + b) % 3);
                f.extGain = block % 5 == 0 ? 0.0 : 1.0;
                f.lo = 300.0 * (1 + b);
                f.hi = 3.0 * f.lo;
                f.resGain = b == 1 ? 0.5 : 0.0;
                p.bands[(size_t) b] = fillBand (f);
            }
            p.bands[3] = saturateBand (100.0, 4000.0, 3.0);
            juce::AudioBuffer<float> buf (2, 480);
            for (int c = 0; c < 2; ++c)
                for (int i = 0; i < 480; ++i)
                    buf.setSample (c, i, 0.4f * u (rng));
            eng.process (buf, p);
            for (int c = 0; c < 2; ++c)
                for (int i = 0; i < 480; ++i)
                {
                    finite = finite && std::isfinite (buf.getSample (c, i));
                    peak = std::max (peak, std::fabs (buf.getSample (c, i)));
                }
        }
        check (finite && peak < 5.0f, fmt ("400 blocks that change every band's character and the oversampling as they go: finite, peak %.2f", peak));
    }

    // CPU: six octave bands, stereo.
    {
        const auto in = noise (480000, 17, 0.3f);
        Rig rig;
        for (int b = 0; b < 6; ++b)
        {
            auto f = extendParams();
            f.character = Character::octave;
            f.lo = 500.0;
            f.hi = 2000.0;
            f.ceiling = 20000.0;
            rig.p.bands[(size_t) b] = fillBand (f);
        }
        const auto t0 = std::chrono::steady_clock::now();
        rig.run (in, nullptr, 480);
        const double seconds = std::chrono::duration<double> (std::chrono::steady_clock::now() - t0).count();
        std::printf ("  six octave bands, 10 s of stereo noise at 48 kHz: %.2f s -> %.1f%% of one core\n", seconds, seconds * 10.0);
    }
}

void testBandsActOnTheirRange()
{
    std::printf("\n[4b] Saturate bands act on their own range: 100 Hz sine, +24 dB drive\n");
    const auto in = sines ({ { 100.0, 0.3 } }, 48000);

    double h3[2];
    int i = 0;
    for (auto [lo, hi] : { std::pair<double, double> { 50.0, 200.0 }, { 2000.0, 4000.0 } })
    {
        Rig rig;
        rig.p.bands[0] = saturateBand (lo, hi, 16.0);
        const auto out = rig.runMono (in);
        h3[i++] = db (settledAmplitude (out, 300) / settledAmplitude (out, 100));
    }
    check (h3[0] > -30.0 && h3[1] < -60.0,
           fmt ("3rd harmonic: %+.1f dB with the band on the tone, %+.1f dB with the band at 2-4 kHz", h3[0], h3[1]));
}

void testBandSwitching()
{
    std::printf ("\n[8b] A band that is switched off fades out and leaves the clean signal exactly\n");
    const auto in = noise (96000, 5);
    Rig rig;
    rig.p.bands[0] = saturateBand (20.0, 20000.0, 8.0);
    rig.p.bands[1] = fillBand (extendParams());

    const std::vector<float> first (in.begin(), in.begin() + 48000), second (in.begin() + 48000, in.end());
    const auto outOn = rig.runMono (first);
    float sat = 0;
    for (size_t i = 24000; i < 48000; ++i) sat = std::max (sat, std::fabs (outOn[i] - first[i - (size_t) rig.eng.getLatency()]));

    rig.p.bands[0].on = rig.p.bands[1].on = false;
    const auto outOff = rig.runMono (second);
    const int latency = rig.eng.getLatency();
    float residue = 0;
    for (size_t i = 24000; i < 48000; ++i) residue = std::max (residue, std::fabs (outOff[i] - second[i - (size_t) latency]));

    check (sat > 0.01f && residue < 1.0e-6f,
           fmt ("bands on change the signal by up to %.3f; once off the output is the delayed input (max error %.1e)", sat, residue));
}

void testToneWeights()
{
    std::printf ("\n[5d] Tone: H2..H5 weights always carry unit energy, dark -> bright\n");
    bool unit = true;
    std::string line;
    for (double tone : { 0.0, 0.25, 0.5, 0.75, 1.0 })
    {
        double w[4];
        harmonicsForTone (tone, w);
        unit = unit && std::fabs (w[0] * w[0] + w[1] * w[1] + w[2] * w[2] + w[3] * w[3] - 1.0) < 1.0e-9;
        line += fmt (" %.2f:[%+.1f %+.1f %+.1f %+.1f]", tone, db (w[0]), db (w[1]), db (w[2]), db (w[3]));
    }
    double dark[4], bright[4];
    harmonicsForTone (0.0, dark);
    harmonicsForTone (1.0, bright);
    check (unit && dark[0] > 0.95 && bright[1] > bright[0], "sum of squares = 1; dark is H2, bright is led by H3 |" + line + " dB");
}

void testNyquistGuard()
{
    std::printf ("\n[5e] Harmonics that would land too close to Nyquist are dropped (16 kHz host, 1 - 2 kHz band)\n");
    constexpr double rate = 16000.0; // far below real hosts; the guard exists for ranges a low rate would alias
    std::vector<float> in (32000);
    for (size_t i = 0; i < in.size(); ++i)
        in[i] = (float) (0.5 * std::sin (2.0 * M_PI * 1500.0 * (double) i / rate));

    FundamentalEngine::Params p = extendParams();
    p.lo = 1000.0;
    p.hi = 2000.0;
    p.ceiling = 7000.0;
    FundamentalEngine fe;
    fe.prepare (rate);
    std::vector<float> out (in.size());
    for (size_t pos = 0; pos < in.size(); pos += 480)
    {
        const int n = (int) std::min<size_t> (480, in.size() - pos);
        const float* inp[1] = { in.data() + pos };
        float* outp[1] = { out.data() + pos };
        fe.process (inp, outp, 1, n, p);
    }

    const auto level = [&] (double f)
    {
        double re = 0, im = 0;
        for (size_t i = 16000; i < 32000; ++i)
        {
            re += out[i] * std::cos (2.0 * M_PI * f * (double) i / rate);
            im += out[i] * std::sin (2.0 * M_PI * f * (double) i / rate);
        }
        return db (2.0 * std::sqrt (re * re + im * im) / 16000.0);
    };
    const double h2 = level (3000.0), h3 = level (4500.0), h4 = level (6000.0);
    check (h2 > -20.0 && h3 > -25.0 && h4 < h2 - 40.0,
           fmt ("H2 %.0f dB and H3 %.0f dB are generated; H4 (6 kHz, from a band reaching 2 kHz) %.0f dB is dropped", h2, h3, h4));
}

// Energy of the engine's added signal over the settled second half of a two-second run.
double addedEnergy (const std::vector<float>& in, const FundamentalEngine::Params& p)
{
    const auto add = runFund (in, p);
    double sum = 0;
    for (size_t i = 48000; i < add.size(); ++i) sum += (double) add[i] * add[i];
    return sum / (double) (add.size() - 48000);
}

void testAdaptiveFill()
{
    std::printf ("\n[5c] Adaptive fill: 60 Hz band (40-120 Hz), Amount -6 dB; energy already above the band decides how much is added\n");
    FundamentalEngine::Params p;
    harmonicsForTone (0.5, p.harmonic);
    p.extGain = std::pow (10.0, -6.0 / 20.0);
    p.target = p.extGain * p.extGain;

    // The 60 Hz tone as the band-pass passes it, and the 2nd-order pair that looks for energy above it.
    const double bandAmp = 0.4 * rangeGain (60.0, p), eBand = bandAmp * bandAmp / 2.0;
    const auto reach = harmonicReach (effectiveRange (BandMode::fill, p.lo, p.hi).hi);
    const auto above = [&] (double f, double amp)
    {
        const double g = amp / std::sqrt (1.0 + std::pow (reach.lo / f, 4.0)) / std::sqrt (1.0 + std::pow (f / reach.hi, 4.0));
        return g * g / 2.0;
    };

    // 200 / 280 Hz are not harmonics of 60 Hz, so they can't be mistaken for generated content.
    const auto hollow = sines ({ { 60.0, 0.4 } }, 96000);
    const auto rich = sines ({ { 60.0, 0.4 }, { 200.0, 0.3 }, { 280.0, 0.3 } }, 96000);
    const auto partial = sines ({ { 60.0, 0.4 }, { 200.0, 0.1 } }, 96000);

    p.adapt = 0.0;
    const double openHollow = addedEnergy (hollow, p), openRich = addedEnergy (rich, p);
    p.adapt = 1.0;
    const double gHollow = addedEnergy (hollow, p), gRich = addedEnergy (rich, p), gPartial = addedEnergy (partial, p);

    check (std::fabs (db (std::sqrt (openHollow / openRich))) < 1.0,
           fmt ("adapt 0%%: open loop adds the same to a hollow and to a full passage (%+.2f dB apart)",
                db (std::sqrt (openHollow / openRich))));
    check (std::fabs (db (std::sqrt (gHollow / eBand)) + 6.0) < 1.5,
           fmt ("adapt 100%%, nothing above the band: added energy %+.1f dB re band (Amount -6.0)", db (std::sqrt (gHollow / eBand))));
    check (db (std::sqrt (gRich / gHollow)) < -20.0,
           fmt ("adapt 100%%, plenty above the band: adds %+.1f dB re the hollow case", db (std::sqrt (gRich / gHollow))));

    // The ABOVE read-out on the band's tab: the energy above the band re the band's own, as the fill measures it.
    const auto reachDb = [&] (const std::vector<float>& x)
    {
        FundamentalEngine e;
        e.prepare (fs);
        std::vector<float> out (x.size());
        for (size_t i = 0; i + 512 <= x.size(); i += 512)
        {
            const float* in[1] = { x.data() + i };
            float* o[1] = { out.data() + i };
            e.process (in, o, 1, 512, p);
        }
        return e.currentReachDb();
    };
    const double wantRich = 10.0 * std::log10 ((above (200.0, 0.3) + above (280.0, 0.3)) / eBand);
    check (std::fabs (reachDb (rich) - wantRich) < 1.0,
           fmt ("ABOVE read-out on the full passage: %+.1f dB (expected %+.1f)", reachDb (rich), wantRich));
    // With nothing above the band, what remains is the band's own tone through the skirt of the region's 2nd-order filters:
    // the read-out shows what the fill compares, leak included.
    const double wantHollow = 10.0 * std::log10 (above (60.0, 0.4) / eBand);
    check (std::fabs (reachDb (hollow) - wantHollow) < 1.0,
           fmt ("ABOVE read-out with nothing above the band: %+.1f dB (the band's own tone through the region filter: %+.1f)", reachDb (hollow), wantHollow));

    const double expected = 1.0 - above (200.0, 0.1) / (eBand * p.target);
    const double measured = gPartial / gHollow;
    check (std::fabs (db (std::sqrt (measured / expected))) < 1.5,
           fmt ("adapt 100%%, a quarter of the target above the band: adds %.2f of the full amount (law says %.2f)", measured, expected));
}

void testRestore()
{
    std::printf ("\n[6] Fundamental Restore: 120 + 180 Hz present, 60 Hz missing\n");
    FundamentalEngine::Params p;
    p.resGain = 1.0;
    p.fill = 0.0;

    const auto harmonics = sines ({ { 120.0, 0.3 }, { 180.0, 0.3 } }, 96000);
    const double restored = amplitudeAt (runFund (harmonics, p), 48000, 32000, 60.0);
    check (restored > 0.15 && restored < 0.6,
           fmt ("restored 60 Hz amplitude %.3f (harmonics are 0.300 each; target ~0.3)", restored));

    // Half the input level -> must scale linearly (not quadratically).
    const auto quiet = sines ({ { 120.0, 0.15 }, { 180.0, 0.15 } }, 96000);
    const double restoredQuiet = amplitudeAt (runFund (quiet, p), 48000, 32000, 60.0);
    check (std::fabs (db (restoredQuiet / restored) + 6.0) < 1.5,
           fmt ("-6 dB input -> %+.1f dB restored (linear tracking = -6.0)", db (restoredQuiet / restored)));

    // With a real fundamental present and fill = 100%, the restored component must back off.
    p.fill = 1.0;
    const auto full = sines ({ { 60.0, 0.4 }, { 120.0, 0.3 }, { 180.0, 0.3 } }, 96000);
    const double withFund = amplitudeAt (runFund (full, p), 48000, 32000, 60.0);
    const double without = amplitudeAt (runFund (harmonics, p), 48000, 32000, 60.0);
    check (withFund < 0.6 * without,
           fmt ("fill=100%%: restored %.3f with fundamental vs %.3f without (ratio %.2f)", withFund, without, withFund / without));
}

void testBassStaysCentred()
{
    std::printf ("\n[7] Mid/Side keeps bass centred: a pure-side 70 Hz tone (L = -R)\n");
    const auto left = sines ({ { 70.0, 0.4 } }, 48000);
    std::vector<float> right (left.size());
    for (size_t i = 0; i < left.size(); ++i) right[i] = -left[i];

    double h2[2];
    for (int ms = 0; ms < 2; ++ms)
    {
        Rig rig;
        rig.p.midSide = ms == 1;
        rig.p.bands[0] = fillBand (extendParams());
        const auto out = rig.run (left, &right).l;
        h2[ms] = db (settledAmplitude (out, 140) / settledAmplitude (out, 70));
    }
    check (h2[0] > -12.0 && h2[1] < -80.0,
           fmt ("2nd harmonic of a side-only tone: L/R mode %+.1f dB, M/S mode %+.1f dB (re fundamental)", h2[0], h2[1]));
}

void testMixDeltaMatch()
{
    std::printf ("\n[8] Mix, Delta and Gain Match behave as documented\n");
    const auto in = noise (96000, 11);
    const auto configure = [] (Rig& rig)
    {
        rig.p.bands[0].drive = 4.0;
        rig.p.bias = 0.3;
        rig.p.bands[1] = fillBand (extendParams());
    };

    Rig wetRig, dryRig, deltaRig, halfRig;
    for (auto* r : { &wetRig, &dryRig, &deltaRig, &halfRig }) configure (*r);
    wetRig.p.mix = 1.0f;
    dryRig.p.mix = 0.0f;
    deltaRig.p.delta = true;
    halfRig.p.mix = 0.37f;

    const auto wet = wetRig.runMono (in), dry = dryRig.runMono (in), delta = deltaRig.runMono (in), half = halfRig.runMono (in);
    const int latency = dryRig.eng.getLatency();

    float dryErr = 0, deltaErr = 0, mixErr = 0;
    for (size_t i = 0; i < in.size(); ++i)
    {
        if ((int) i >= latency) dryErr = std::max (dryErr, std::fabs (dry[i] - in[i - (size_t) latency]));
        deltaErr = std::max (deltaErr, std::fabs (delta[i] - (wet[i] - dry[i])));
        mixErr = std::max (mixErr, std::fabs (half[i] - (dry[i] + 0.37f * (wet[i] - dry[i]))));
    }
    check (dryErr < 1.0e-6f, fmt ("mix = 0 is the input delayed by exactly the reported latency (max error %.1e)", dryErr));
    check (deltaErr < 1.0e-5f, fmt ("delta = wet - dry sample for sample (max error %.1e)", deltaErr));
    check (mixErr < 1.0e-5f, fmt ("mix = 0.37 is the linear blend (max error %.1e)", mixErr));

    // Gain match: heavy saturation lowers the level; matching must restore it.
    const auto tone = sines ({ { 1000.0, 0.5 } }, 48000 * 8);
    const auto rms = [] (const std::vector<float>& x)
    {
        double s = 0; size_t n = 0;
        for (size_t i = x.size() - 48000; i < x.size(); ++i) { s += (double) x[i] * x[i]; ++n; }
        return std::sqrt (s / (double) n);
    };
    double offDb = 0, onDb = 0;
    for (int match = 0; match < 2; ++match)
    {
        Rig rig;
        rig.p.bands[0].drive = 4.0;
        rig.p.match = match == 1;
        (match ? onDb : offDb) = db (rms (rig.runMono (tone)) / rms (tone));
    }
    check (offDb < -3.0 && std::fabs (onDb) < 1.0,
           fmt ("gain match: level change %+.1f dB without, %+.1f dB with", offDb, onDb));
}

void testRobustness()
{
    std::printf ("\n[9] Robustness: random parameters, block sizes, mode switches, mono, mid/side\n");
    std::mt19937 rng (1234);
    std::uniform_real_distribution<float> u (0.0f, 1.0f);

    Engine eng;
    eng.prepare (fs, 512);
    bool finite = true;
    float maxAbs = 0;

    for (int iter = 0; iter < 600; ++iter)
    {
        EngineParams p;
        p.inGainDb = -12.0f + 24.0f * u (rng);
        p.outGainDb = -12.0f + 24.0f * u (rng);
        p.mix = u (rng);
        p.midSide = u (rng) > 0.5f;
        p.delta = u (rng) > 0.8f;
        p.match = u (rng) > 0.5f;
        p.osMode = (int) (u (rng) * 3.99f);
        p.curve = (int) (u (rng) * 2.99f);
        p.bias = 0.8 * u (rng);
        for (auto& band : p.bands)
        {
            band.on = u (rng) > 0.3f;
            band.mode = u (rng) > 0.5f ? BandMode::fill : BandMode::saturate;
            band.lo = 20.0 * std::pow (1000.0, (double) u (rng));
            band.hi = band.lo * (1.0 + 4.0 * u (rng)); // sometimes closer than the minimum ratio
            band.drive = std::pow (10.0, 24.0 * u (rng) / 20.0);
            band.fill = extendParams();
            band.fill.resGain = u (rng) > 0.5f ? 1.0 : 0.0;
            band.fill.extGain = u (rng) > 0.3f ? 2.0 : 0.0;
            band.fill.target = band.fill.extGain * band.fill.extGain;
            band.fill.adapt = u (rng);
            band.fill.character = (Character) (int) (u (rng) * 2.99f);
            band.fill.ceiling = 10.0 * band.hi;
        }

        const int block = 1 + (int) (u (rng) * 1500.0f); // includes > maxBlock and tiny sizes
        juce::AudioBuffer<float> buf (u (rng) > 0.15f ? 2 : 1, block);
        for (int c = 0; c < buf.getNumChannels(); ++c)
            for (int i = 0; i < block; ++i)
                buf.setSample (c, i, 0.9f * (2.0f * u (rng) - 1.0f));

        eng.process (buf, p);
        for (int c = 0; c < buf.getNumChannels(); ++c)
            for (int i = 0; i < block; ++i)
            {
                const float s = buf.getSample (c, i);
                if (! std::isfinite (s)) finite = false;
                maxAbs = std::max (maxAbs, std::fabs (s));
            }
    }
    check (finite && maxAbs < 200.0f, fmt ("600 randomised blocks: all finite, peak %.1f", maxAbs));

    // Mid/side on a mono signal: both channels must remain identical.
    Rig ms;
    ms.p.midSide = true;
    ms.p.bands[0].drive = 4.0;
    ms.p.bias = 0.4;
    ms.p.bands[1] = fillBand (extendParams());
    const auto out = ms.run (sines ({ { 70.0, 0.4 }, { 900.0, 0.2 } }, 24000));
    float maxDiff = 0;
    for (size_t i = 0; i < out.l.size(); ++i) maxDiff = std::max (maxDiff, std::fabs (out.l[i] - out.r[i]));
    check (maxDiff < 1.0e-6f, fmt ("mid/side keeps a mono input mono (max L-R %.1e)", maxDiff));
}

// ------------------------------------------------------------------------------------------
void testMetersRecover()
{
    std::printf ("\n[9b] Level meters: a bad sample must not pin them, and the Clear button zeroes them\n");
    Rig rig;
    auto burst = noise (480, 7);
    burst[100] = std::numeric_limits<float>::infinity();
    burst[200] = std::numeric_limits<float>::quiet_NaN();
    rig.run (burst);
    rig.run (std::vector<float> (48000 * 2, 0.0f)); // two seconds of silence: the ~300 ms fall-back has long finished
    const float in = rig.eng.inPeak.load(), out = rig.eng.outPeak.load();
    check (std::isfinite (in) && in < 0.01f, fmt ("IN meter falls back after an inf / NaN sample (%.4f)", (double) in));
    check (std::isfinite (out) && out < 0.01f, fmt ("OUT meter falls back after an inf / NaN sample (%.4f)", (double) out));

    rig.run (noise (4800, 8, 0.5f));
    check (rig.eng.inPeak.load() > 0.1f, "IN meter reads a normal signal");
    rig.eng.clearMeters();
    check (rig.eng.inPeak.load() <= 0.0f && rig.eng.outPeak.load() <= 0.0f, "clearMeters() zeroes both meters");

    // The sound itself has to come back too: a NaN that reaches a filter, an envelope or the gain match stays there for good.
    // Every kind of band, Gain Match and mid/side, so each piece of state that could keep one is in the path.
    for (const bool midSide : { false, true })
    {
        Rig bad, good;
        for (auto* r : { &bad, &good })
        {
            r->p.midSide = midSide;
            r->p.match = true;
            r->p.bands[0].drive = 6.0;
            r->p.bands[1] = fillBand (extendParams());
            r->p.bands[2] = fillBand (extendParams());
            r->p.bands[2].fill.character = Character::clean;
            r->p.bands[3] = fillBand (extendParams());
            r->p.bands[3].fill.character = Character::octave;
        }
        auto first = sines ({ { 70.0, 0.4 }, { 900.0, 0.2 } }, 4800);
        const auto clean = first;
        first[1000] = std::numeric_limits<float>::quiet_NaN();
        first[2000] = std::numeric_limits<float>::infinity();
        first[3000] = -std::numeric_limits<float>::infinity();
        bad.run (first);
        good.run (clean);
        const auto after = sines ({ { 70.0, 0.4 }, { 900.0, 0.2 } }, 48000 * 3);
        const auto a = bad.run (after), b = good.run (after);
        bool finite = true;
        float worst = 0.0f;
        for (size_t i = 0; i < a.l.size(); ++i)
        {
            finite = finite && std::isfinite (a.l[i]) && std::isfinite (a.r[i]);
            if (i >= a.l.size() - 48000 && finite)
                worst = std::max ({ worst, std::fabs (a.l[i] - b.l[i]), std::fabs (a.r[i] - b.r[i]) });
        }
        check (finite && worst < 1.0e-3f, fmt ("%s: after a NaN and two infs the output is finite and back to what it would have been (max difference %.1e in the third second)",
                                               midSide ? "mid/side" : "stereo", (double) worst));
    }
}

// ------------------------------------------------------------------------------------------
void testSpectrumAnalyzer()
{
    std::printf ("\n[10] Spectrum analyzer calibration (8192-point Hann, dBFS, tilt around 1 kHz)\n");
    const int N = pdsp::SpectrumAnalyzer::fftSize;
    const double binHz = fs / N;

    // Pushes a sine on the given channels, lets the analyzer consume it, and returns the analyzer.
    const auto measure = [&] (double f, float tilt, bool bothChannels, pdsp::SpectrumAnalyzer& an)
    {
        static pdsp::StereoTap tap;
        const auto x = sines ({ { f, 0.5 } }, 3 * N); // 0.5 amplitude = -6.02 dBFS
        const std::vector<float> silence (x.size(), 0.0f);
        const float* ch[2] = { x.data(), bothChannels ? x.data() : silence.data() };
        tap.push (ch, 2, (int) x.size());
        an.setSampleRate (fs);
        an.setTilt (tilt);
        an.update (tap);
    };

    const double f1k = 171 * binHz, f4k = 683 * binHz; // bin-centred
    pdsp::SpectrumAnalyzer flat, tilted, offBin, oneSided;
    measure (f1k, 0.0f, true, flat);
    measure (f4k, 4.5f, true, tilted);
    measure (f1k + 0.5 * binHz, 0.0f, true, offBin);
    measure (f1k, 0.0f, false, oneSided);

    const double sixDb = -6.02;
    check (std::fabs (flat.peakInRange (900, 1100) - sixDb) < 0.15,
           fmt ("bin-centred 1 kHz sine at -6.02 dBFS reads %.2f dB", flat.peakInRange (900, 1100)));
    check (std::fabs (tilted.peakInRange (3900, 4100) - (sixDb + 4.5 * std::log2 (f4k / 1000.0))) < 0.15,
           fmt ("4 kHz with 4.5 dB/oct tilt reads %.2f dB (expected %.2f)", tilted.peakInRange (3900, 4100),
                sixDb + 4.5 * std::log2 (f4k / 1000.0)));
    check (std::fabs (offBin.peakInRange (900, 1100) - sixDb) < 1.5,
           fmt ("worst-case off-bin sine (Hann scalloping) reads %.2f dB, within 1.5 dB", offBin.peakInRange (900, 1100)));
    check (std::fabs (oneSided.peakInRange (900, 1100) - (sixDb - 3.01)) < 0.2,
           fmt ("left-only signal reads %.2f dB: L/R power is averaged, not summed", oneSided.peakInRange (900, 1100)));
    check (std::fabs (tilted.getTilt() - pdsp::SpectrumAnalyzer::defaultTilt) < 1.0e-6f && pdsp::SpectrumAnalyzer::defaultTilt == 4.5f,
           "default tilt is 4.5 dB/oct, as in common EQ analyzers");
}


// Power of the settled tail of `x` (Hann-windowed FFT) inside [lo, hi) Hz, and in total.
struct Power { double inside, total; };
Power powerBetween (const std::vector<float>& x, double lo, double hi)
{
    constexpr int order = 16, size = 1 << order;
    juce::dsp::FFT fft (order);
    std::vector<float> buf ((size_t) size * 2, 0.0f);
    for (int i = 0; i < size; ++i)
        buf[(size_t) i] = x[x.size() - (size_t) size + (size_t) i] * (float) (0.5 - 0.5 * std::cos (2.0 * M_PI * i / (size - 1)));
    fft.performRealOnlyForwardTransform (buf.data());

    Power sum { 0.0, 0.0 };
    for (int k = 1; k < size / 2; ++k)
    {
        const double f = k * fs / size, re = buf[(size_t) (2 * k)], im = buf[(size_t) (2 * k + 1)];
        sum.total += re * re + im * im;
        if (f >= lo && f < hi)
            sum.inside += re * re + im * im;
    }
    return sum;
}

// Noise confined to [lo, hi] by the same kind of filter the bands use.
std::vector<float> bandNoise (int n, double lo, double hi, unsigned seed)
{
    pdsp::Bandpass bp;
    bp.setup (fs, 4, lo, 4, hi);
    auto x = noise (n, seed, 1.0f);
    double peak = 1.0e-9;
    for (auto& v : x)
    {
        v = (float) bp.process (v);
        peak = std::max (peak, (double) std::fabs (v));
    }
    for (auto& v : x)
        v = (float) (v * 0.5 / peak);
    return x;
}

// What the display shades as "harmonics added": it must match where the energy really goes.
void testHarmonicSpan()
{
    std::printf ("\n[5f] Harmonic span: 2nd harmonic of the low edge to the 5th of the high edge, short of Nyquist\n");

    const auto sat = harmonicSpan (BandMode::saturate, 100.0, 300.0, fs);
    const auto fill = harmonicSpan (BandMode::fill, 40.0, 120.0, fs);
    check (std::fabs (sat.lo - 200.0) < 1.0e-9 && std::fabs (sat.hi - 1500.0) < 1.0e-9
               && std::fabs (fill.lo - 80.0) < 1.0e-9 && std::fabs (fill.hi - 600.0) < 1.0e-9,
           fmt ("saturate 100-300 Hz -> %.0f-%.0f Hz, fill 40-120 Hz -> %.0f-%.0f Hz", sat.lo, sat.hi, fill.lo, fill.hi));

    const auto wide = harmonicSpan (BandMode::saturate, 20.0, 20000.0, fs);
    const auto high = harmonicSpan (BandMode::saturate, 12000.0, 20000.0, fs);
    check (std::fabs (wide.hi - 0.48 * fs) < 1.0e-9 && high.hi <= high.lo,
           fmt ("a full-range saturate band stops at 0.48 fs (%.0f Hz); a band above Nyquist/2 has no span", wide.hi));

    const auto capped = harmonicSpan (BandMode::fill, 500.0, 9000.0, fs);
    const auto slow = harmonicSpan (BandMode::fill, 1000.0, 2000.0, 16000.0);
    check (std::fabs (capped.hi - 10000.0) < 1.0e-9 && std::fabs (slow.hi - 3.0 * 2000.0) < 1.0e-9,
           fmt ("fill stops at 2 kHz (span top %.0f Hz) and drops H4/H5 on a 16 kHz host (top %.0f Hz)", capped.hi, slow.hi));

    // Band-limited noise through each mode: what is added must not reach past the span.
    const auto outside = [] (const std::vector<float>& added, Range span)
    {
        const auto p = powerBetween (added, span.lo, span.hi);
        return std::pair<double, double> { 100.0 * (1.0 - p.inside / p.total), 100.0 * powerBetween (added, span.hi, 24000.0).inside / p.total };
    };

    for (double driveDb : { 6.0, 12.0, 24.0 })
    {
        Rig rig;
        rig.p.delta = true;
        rig.p.bias = 0.16;
        rig.p.bands[0] = saturateBand (300.0, 900.0, std::pow (10.0, driveDb / 20.0));
        const auto added = rig.runMono (bandNoise (200000, 300.0, 900.0, 7));
        const auto [notInSpan, above] = outside (added, harmonicSpan (BandMode::saturate, 300.0, 900.0, fs));
        check (above < 1.0, fmt ("saturate 300-900 Hz, drive %2.0f dB: %.2f%% of the added energy lies above the span (%.0f%% below it or in the band)",
                                 driveDb, above, notInSpan));
    }

    FundamentalEngine::Params p;
    p.lo = 100.0; p.hi = 250.0; p.extGain = 1.0;
    harmonicsForTone (0.5, p.harmonic);
    p.ceiling = 10.0 * p.hi;
    const auto span = harmonicSpan (BandMode::fill, p.lo, p.hi, fs);
    const auto added = runFund (bandNoise (200000, p.lo, p.hi, 9), p);
    const auto [notInSpan, above] = outside (added, span);
    check (above < 3.0 && notInSpan < 30.0,
           fmt ("fill 100-250 Hz: %.2f%% above the span, %.0f%% of the added energy inside it", above, 100.0 - notInSpan));
}

// Every sample rate a host might run at: unity at small signal, exact latency compensation, no blow-ups.
void testSampleRates()
{
    std::printf ("\n[1b] Sample rates: small-signal gain, exact latency alignment, stability with all band types\n");

    for (double rate : { 22050.0, 44100.0, 96000.0, 192000.0 })
    {
        const auto sine = [&] (double f, double amp, int n)
        {
            std::vector<float> x ((size_t) n);
            for (int i = 0; i < n; ++i)
                x[(size_t) i] = (float) (amp * std::sin (2.0 * M_PI * f * i / rate));
            return x;
        };
        const auto render = [&] (Engine& eng, const EngineParams& p, const std::vector<float>& in)
        {
            std::vector<float> out (in.size());
            juce::AudioBuffer<float> buf (2, 480);
            for (size_t pos = 0; pos < in.size(); pos += 480)
            {
                const int n = (int) std::min<size_t> (480, in.size() - pos);
                for (int c = 0; c < 2; ++c)
                    std::copy_n (in.begin() + (long) pos, n, buf.getWritePointer (c));
                juce::AudioBuffer<float> view (buf.getArrayOfWritePointers(), 2, n);
                eng.process (view, p);
                std::copy_n (buf.getReadPointer (0), n, out.begin() + (long) pos);
            }
            return out;
        };

        // Small signal through a full-range and a narrow saturate band: the input, delayed.
        Engine eng;
        eng.prepare (rate, 512, 2);
        EngineParams p;
        p.osMode = 2;
        p.bands[0] = saturateBand (20.0, 20000.0, std::pow (10.0, 18.0 / 20.0));
        p.bands[1] = saturateBand (200.0, 2000.0, std::pow (10.0, 18.0 / 20.0));
        const int n = (int) rate;
        auto quiet = sine (1000.0, 0.001, n);
        const auto out = render (eng, p, quiet);
        const int latency = eng.getLatency();
        double worst = 0.0;
        for (int i = n / 2; i < n; ++i)
            worst = std::max (worst, std::fabs ((double) out[(size_t) i] - (double) quiet[(size_t) (i - latency)]));

        // All band types at once, loud, with the bands moving: finite and bounded.
        EngineParams loud = p;
        loud.bands[2] = fillBand ({ 50.0, 150.0, 1.0, { 0.5, 0.35, 0.2, 0.1 }, 1500.0, 0.5, 0.8, 0.5, 1.0 });
        loud.bands[3] = saturateBand (5000.0, 15000.0, std::pow (10.0, 12.0 / 20.0));
        bool finite = true;
        float peak = 0.0f;
        const auto loudIn = sine (80.0, 0.7, n);
        for (int rep = 0; rep < 4; ++rep)
        {
            loud.bands[3].lo = 4000.0 + 500.0 * rep;
            for (float v : render (eng, loud, loudIn))
            {
                finite = finite && std::isfinite (v);
                peak = std::max (peak, std::fabs (v));
            }
        }

        check (worst < 2.0e-6 && finite && peak < 20.0,
               fmt ("%6.0f Hz: latency %d, small-signal error %.1e (-60 dBFS), loud peak %.2f, finite %s",
                    rate, latency, worst, peak, finite ? "yes" : "NO"));
    }

    // The latency is known as soon as the plugin is prepared, not after the first block.
    Engine eng;
    eng.prepare (fs, 512, 3);
    Rig reference (3);
    reference.run (std::vector<float> (960, 0.0f));
    check (eng.getLatency() > 0 && eng.getLatency() == reference.eng.getLatency(),
           fmt ("latency after prepare (8x): %d, same as after the first block: %d", eng.getLatency(), reference.eng.getLatency()));
}

void testBandSolo()
{
    std::printf ("\n[12] Band solo: ADDS plays only what a band adds, BAND the band's own region; releasing returns the output bit for bit\n");
    const auto in = noise (48000, 7, 0.3f);
    const auto setup = [] (Rig& rig)
    {
        rig.p.bands[0] = saturateBand (60.0, 300.0, 8.0);
        rig.p.bands[1] = saturateBand (1000.0, 3000.0, 8.0);
        rig.p.bands[2] = fillBand (extendParams());
    };

    // What the bands add together is the delta; the bands' solos are its parts, so they must sum to it.
    Rig delta;
    setup (delta);
    delta.p.delta = true;
    const auto whole = delta.runMono (in);

    std::vector<float> sum (in.size(), 0.0f);
    for (int b = 0; b < 3; ++b)
    {
        Rig rig;
        setup (rig);
        rig.eng.monitor.soloKind = (int) SoloKind::adds;
        rig.eng.monitor.soloBand = b;
        const auto part = rig.runMono (in);
        for (size_t i = 0; i < in.size(); ++i)
            sum[i] += part[i];
    }
    float worst = 0.0f, peak = 0.0f;
    for (size_t i = 24000; i < in.size(); ++i)
    {
        worst = std::max (worst, std::fabs (sum[i] - whole[i]));
        peak = std::max (peak, std::fabs (whole[i]));
    }
    check (peak > 0.01f && worst < 1.0e-4f, fmt ("the three bands' ADDS solos sum to the delta (peak %.3f, max error %.1e)", peak, worst));

    // BAND: a tone inside the band passes, one well outside is gone.
    {
        const auto tones = sines ({ { 1400.0, 0.2 }, { 200.0, 0.2 } }, 48000);
        Rig rig;
        setup (rig);
        rig.eng.monitor.soloKind = (int) SoloKind::band;
        rig.eng.monitor.soloBand = 1;
        const auto out = rig.runMono (tones);
        const double inside = db (settledAmplitude (out, 1400.0) / 0.2), outside = db (settledAmplitude (out, 200.0) / 0.2);
        check (inside > -1.5 && outside < -30.0, fmt ("BAND solo of a 1-3 kHz band: 1.4 kHz %+.1f dB, 200 Hz %+.1f dB", inside, outside));
    }

    // Releasing it: the output is the plain output again, exactly.
    {
        Rig soloed, plain;
        setup (soloed);
        setup (plain);
        soloed.eng.monitor.soloKind = (int) SoloKind::adds;
        soloed.eng.monitor.soloBand = 0;
        const std::vector<float> first (in.begin(), in.begin() + 24000), second (in.begin() + 24000, in.end());
        soloed.runMono (first);
        plain.runMono (first);
        soloed.eng.monitor.soloBand = -1;
        const auto a = soloed.runMono (second), b = plain.runMono (second);
        float mid = 0.0f;
        for (size_t i = 0; i < 400; ++i)
            mid = std::max (mid, std::fabs (a[i] - b[i]));
        float after = 0.0f;
        for (size_t i = 1000; i < a.size(); ++i)
            after = std::max (after, std::fabs (a[i] - b[i]));
        check (mid > 0.001f && after == 0.0f, fmt ("while it fades out the output differs (%.3f), once it has, it equals the plain output (max difference %.1e)", mid, after));
    }
}

void testBandSoloStrip()
{
    std::printf ("\n[12b] Band solo strip: the six main bands of music (pdsp::bands); each passes its own range, all six are flat, releasing is bit-exact\n");
    namespace mb = pdsp::bands;
    // Half a second of a settled tone, the frequencies on even hertz so the window holds whole cycles (no leakage).
    const auto wholeCycles = [] (const std::vector<float>& x, double f) { return amplitudeAt (x, 24000, 24000, f); };
    double worstOwn = 0.0, worstNarrow = 0.0, loudestNeighbour = -200.0, loudestOther = -200.0;
    bool ok = true;
    for (int band = 0; band < mb::kCount; ++band)
        for (int tone = 0; tone < mb::kCount; ++tone)
        {
            const double f = 2.0 * std::round (std::sqrt (mb::kBands[(size_t) tone].loHz * mb::kBands[(size_t) tone].hiHz) / 2.0); // the middle, in octaves (whole cycles in the window)
            Rig rig, plain;
            rig.eng.monitor.regions = 1u << band;
            const auto x = sines ({ { f, 0.2 } }, 48000);
            const double g = db (wholeCycles (rig.runMono (x), f) / wholeCycles (plain.runMono (x), f)); // against the plain output
            if (band == tone)
            {
                const bool narrow = mb::kBands[(size_t) band].hiHz / mb::kBands[(size_t) band].loHz <= 2.01; // Low-Mid: one octave
                (narrow ? worstNarrow : worstOwn) = std::max (narrow ? worstNarrow : worstOwn, std::fabs (g));
                ok = ok && std::fabs (g) < (narrow ? 4.3 : 2.0);
            }
            else if (std::abs (band - tone) == 1)
                loudestNeighbour = std::max (loudestNeighbour, g), ok = ok && g < -13.0;
            else
                loudestOther = std::max (loudestOther, g), ok = ok && g < -35.0;
        }
    check (ok, fmt ("a band passes a tone in its middle within %.2f dB (%.2f dB for the octave-wide low-mid); the next band's tone is %.1f dB down at most, the others %.1f dB",
                    worstOwn, worstNarrow, loudestNeighbour, loudestOther));

    double worst = 0.0;
    for (double f : { 30.0, 60.0, 150.0, 250.0, 500.0, 1000.0, 2000.0, 6000.0, 12000.0 })
    {
        Rig soloed, plain;
        soloed.eng.monitor.regions = mb::kAllBands;
        const auto x = sines ({ { f, 0.2 } }, 48000);
        worst = std::max (worst, std::fabs (db (wholeCycles (soloed.runMono (x), f) / wholeCycles (plain.runMono (x), f))));
    }
    check (worst < 0.05, fmt ("all six together are flat against the plain output (largest deviation %.3f dB over nine tones, the crossovers included)", worst));

    // It works on what is heard, so it combines with a band solo; releasing it returns the plain output exactly.
    {
        const auto in = noise (48000, 11, 0.3f);
        Rig soloed, plain;
        soloed.p.bands[0] = plain.p.bands[0] = saturateBand (60.0, 300.0, 8.0);
        soloed.eng.monitor.regions = 0b000110u;
        const std::vector<float> first (in.begin(), in.begin() + 24000), second (in.begin() + 24000, in.end());
        soloed.runMono (first);
        plain.runMono (first);
        soloed.eng.monitor.regions = 0u;
        const auto a = soloed.runMono (second), b = plain.runMono (second);
        float fading = 0.0f, after = 0.0f;
        for (size_t i = 0; i < 400; ++i)
            fading = std::max (fading, std::fabs (a[i] - b[i]));
        for (size_t i = 1000; i < a.size(); ++i)
            after = std::max (after, std::fabs (a[i] - b[i]));
        check (fading > 0.001f && after == 0.0f, fmt ("while it fades out the output differs (%.3f), once it has, it equals the plain output (max difference %.1e)", fading, after));
    }

    // A mono buffer (one channel) is soloed too, and stays finite.
    {
        Engine e;
        e.prepare (fs, 512);
        e.monitor.regions = 1u << 5;
        juce::AudioBuffer<float> mono (1, 24000);
        const auto x = sines ({ { 100.0, 0.3 }, { 10000.0, 0.3 } }, 24000);
        std::copy (x.begin(), x.end(), mono.getWritePointer (0));
        e.process (mono, cleanParams());
        std::vector<float> out (mono.getReadPointer (0), mono.getReadPointer (0) + 24000);
        const double hi = db (amplitudeAt (out, 12000, 12000, 10000.0) / 0.3), lo = db (amplitudeAt (out, 12000, 12000, 100.0) / 0.3);
        check (hi > -2.0 && lo < -60.0, fmt ("a mono buffer: High solo keeps 10 kHz at %+.1f dB (the crossover's skirt, 0.7 octave above 6 kHz) and takes 100 Hz to %.0f dB", hi, lo));
    }
}

void benchmark()
{
    std::printf ("\n[11] CPU: 10 s of stereo @ 48 kHz, all six bands on (4 saturate, 2 adaptive fill with rebuild)\n");
    const auto in = sines ({ { 55.0, 0.3 }, { 440.0, 0.2 }, { 5000.0, 0.1 } }, 480000);

    for (int os = 0; os < 4; ++os)
    {
        Rig rig (os);
        rig.p.bias = 0.3;
        rig.p.bands = { saturateBand (20, 150, 3.0), saturateBand (150, 3500, 3.0), saturateBand (3500, 20000, 3.0),
                        saturateBand (60, 8000, 2.0), fillBand (extendParams()), fillBand (extendParams()) };
        for (size_t b = 4; b < 6; ++b)
        {
            rig.p.bands[b].fill.resGain = 1.0;
            rig.p.bands[b].fill.adapt = 1.0;
            rig.p.bands[b].fill.target = 0.25;
        }

        const auto t0 = std::chrono::steady_clock::now();
        rig.run (in, nullptr, 512);
        const double sec = std::chrono::duration<double> (std::chrono::steady_clock::now() - t0).count();
        std::printf ("  os=%d: %.2f s for 10 s of audio  ->  %.1f%% of one core\n", os, sec, sec * 10.0);
    }
}
}

int main()
{
    testTransparency();
    testSampleRates();
    testLatencyAlignment();
    testAliasing();
    testEvenOdd();
    testBandsActOnTheirRange();
    testExtend();
    testExtendNoteOff();
    testCleanFill();
    testOctaveFill();
    testToneWeights();
    testNyquistGuard();
    testHarmonicSpan();
    testAdaptiveFill();
    testRestore();
    testBassStaysCentred();
    testMixDeltaMatch();
    testBandSwitching();
    testRobustness();
    testMetersRecover();
    testBandSolo();
    testBandSoloStrip();
    testSpectrumAnalyzer();
    benchmark();
    return finish ("ALL TESTS");
}
