// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 The Pantheon authors

// Headless tests of the saturator: no plugin, no GUI. Every number the README quotes is measured here.

#include "Check.h"
#include "DSP/Engine.h"
#include <random>

using namespace dionysus;
using test::check;
using test::fmt;

namespace
{
constexpr double kFs = 48000.0;

struct Stereo
{
    std::vector<float> l, r;
};

Stereo sine (double hz, double peakDb, int n)
{
    Stereo s { std::vector<float> ((size_t) n), std::vector<float> ((size_t) n) };
    const double a = std::pow (10.0, peakDb / 20.0);
    for (int i = 0; i < n; ++i)
        s.l[(size_t) i] = s.r[(size_t) i] = (float) (a * std::sin (2.0 * M_PI * hz * i / kFs));
    return s;
}

Stereo run (Engine& e, Stereo in, const EngineParams& p, int block = 512)
{
    for (int start = 0; start < (int) in.l.size(); start += block)
    {
        const int m = std::min (block, (int) in.l.size() - start);
        e.process (in.l.data() + start, in.r.data() + start, m, p);
    }
    return in;
}

double db (double lin) { return 20.0 * std::log10 (std::fmax (lin, 1e-12)); }

// The level at one frequency of the last 0.5 s, dB (a single-bin DFT; the frequency has to be a whole number of cycles in the window to be exact: 48000 / hz divides 24000).
double levelAt (const std::vector<float>& x, double hz)
{
    const size_t from = x.size() - 24000;
    double re = 0.0, im = 0.0;
    for (size_t i = from; i < x.size(); ++i)
    {
        re += x[i] * std::cos (2.0 * M_PI * hz * (double) i / kFs);
        im += x[i] * std::sin (2.0 * M_PI * hz * (double) i / kFs);
    }
    return db (2.0 * std::sqrt (re * re + im * im) / 24000.0);
}

double rms (const std::vector<float>& x, size_t from)
{
    double sum = 0.0;
    for (size_t i = from; i < x.size(); ++i)
        sum += (double) x[i] * x[i];
    return std::sqrt (sum / (double) (x.size() - from));
}

const char* shapeName (Shape s) { static const char* names[] = { "tape", "tube", "transformer", "diode", "fold", "clip" }; return names[(int) s]; }
}

int main()
{
    std::printf ("Dionysus tests (48 kHz)\n");
    const int n = 48000 * 2;

    // [1] The curves.
    std::printf ("\n[1] The curves\n");
    {
        bool slopeOk = true, bounded = true;
        for (int s = 0; s < kNumShapes; ++s)
        {
            const double h = 1e-6;
            slopeOk = slopeOk && std::fabs ((shape ((Shape) s, h) - shape ((Shape) s, -h)) / (2 * h) - 1.0) < 1e-3;
            for (double x = -20.0; x <= 20.0; x += 0.01)
                bounded = bounded && std::fabs (shape ((Shape) s, x)) <= 1.0 + 1e-9;
        }
        check (slopeOk, "every curve has a slope of 1 at 0: a small signal passes unchanged");
        check (bounded, "every curve stays within +-1 for any input");
        check (std::fabs (shape (Shape::tape, 0.7) + shape (Shape::tape, -0.7)) < 1e-12 && std::fabs (shape (Shape::diode, 0.7) + shape (Shape::diode, -0.7)) > 0.01,
               "tape is symmetric; the diode is not (it makes even harmonics on its own)");
        check (saturate (Shape::tape, 0.0, 0.8) == 0.0, "BIAS never moves silence: the curve's value at the working point is taken off");
    }

    // [2] Harmonics: what the engine does is what the curve says.
    std::printf ("\n[2] Harmonics\n");
    {
        EngineParams p;
        p.driveDb = 14.0f;
        p.oversampling = 3;
        p.autoGain = false;
        p.bias = 0.0f;
        Engine e;
        e.prepare (kFs, 3);
        const auto in = sine (1000.0, -12.0, n);
        const auto out = run (e, in, p);
        const auto h = harmonicsDb (Shape::tape, 14.0, 0.0);
        const double h1 = levelAt (out.l, 1000.0) - levelAt (in.l, 1000.0), h2 = levelAt (out.l, 2000.0) - levelAt (in.l, 1000.0), h3 = levelAt (out.l, 3000.0) - levelAt (in.l, 1000.0);
        check (std::fabs (h1 - h[0]) < 0.1 && std::fabs (h3 - h[2]) < 0.2, fmt ("tape, 14 dB drive, 8x: the fundamental %+.2f dB and the 3rd harmonic %.2f dB are what the curve gives (%+.2f, %.2f)", h1, h3, h[0], h[2]));
        check (h2 < -80.0, fmt ("... and a symmetric curve has no 2nd harmonic (%.0f dB)", h2));
        p.bias = 0.6f;
        Engine f;
        f.prepare (kFs, 3);
        const auto out2 = run (f, in, p);
        const double b2 = levelAt (out2.l, 2000.0) - levelAt (in.l, 1000.0), b2want = harmonicsDb (Shape::tape, 14.0, 0.6)[1];
        check (b2 > -40.0 && std::fabs (b2 - b2want) < 0.3, fmt ("BIAS 60%%: a 2nd harmonic appears, %.1f dB (the curve: %.1f)", b2, b2want));
        double dc = 0.0;
        for (size_t i = out2.l.size() - 24000; i < out2.l.size(); ++i)
            dc += out2.l[i];
        check (std::fabs (dc / 24000.0) < 2e-4, fmt ("... with no DC (the mean of the output is %.1e): a 10 Hz high-pass takes it away", dc / 24000.0));
        // Each curve's character at 14 dB: harmonic levels 2 and 3.
        for (int s = 0; s < kNumShapes; ++s)
        {
            const auto hh = harmonicsDb ((Shape) s, 14.0, 0.0);
            std::printf ("    %-12s 2nd %6.1f dB   3rd %6.1f dB   5th %6.1f dB\n", shapeName ((Shape) s), hh[1], hh[2], hh[4]);
        }
    }

    // [3] Aliasing and oversampling.
    std::printf ("\n[3] Oversampling\n");
    {
        EngineParams p;
        p.driveDb = 24.0f;
        p.shape = Shape::clip;
        p.autoGain = false;
        const auto in = sine (7000.0, -6.0, n); // the 5th harmonic, 35 kHz, folds back to 13 kHz
        double alias[kNumOversampling];
        for (int k = 0; k < kNumOversampling; ++k)
        {
            p.oversampling = k;
            Engine e;
            e.prepare (kFs, k);
            const auto out = run (e, in, p);
            alias[k] = levelAt (out.l, 13000.0);
        }
        check (alias[0] > -30.0 && alias[3] < alias[0] - 40.0 && alias[1] < alias[0] - 10.0 && alias[2] < alias[1],
               fmt ("a 7 kHz sine hard-clipped: the 5th harmonic folds back to 13 kHz at %.1f dB (1x), %.1f (2x), %.1f (4x) and %.1f dB (8x)", alias[0], alias[1], alias[2], alias[3]));
        // The same fundamental, whatever the oversampling.
        p.shape = Shape::tape;
        p.driveDb = 6.0f;
        double fundamental[kNumOversampling];
        for (int k = 0; k < kNumOversampling; ++k)
        {
            p.oversampling = k;
            Engine e;
            e.prepare (kFs, k);
            fundamental[k] = levelAt (run (e, sine (1000.0, -12.0, n), p).l, 1000.0);
        }
        check (std::fabs (fundamental[1] - fundamental[0]) < 0.05 && std::fabs (fundamental[3] - fundamental[0]) < 0.05, fmt ("the level of the fundamental is the same at 1x, 2x, 4x and 8x (%.3f, %.3f, %.3f, %.3f dB)", fundamental[0], fundamental[1], fundamental[2], fundamental[3]));
    }

    // [4] Latency, and the dry sound.
    std::printf ("\n[4] Latency and mix\n");
    {
        for (int k = 0; k < kNumOversampling; ++k)
        {
            EngineParams p;
            p.mix = 0.0f;
            p.oversampling = k;
            Engine e;
            e.prepare (kFs, k);
            std::vector<float> x (6000, 0.0f);
            std::mt19937 gen (3 + k);
            std::uniform_real_distribution<float> white (-0.5f, 0.5f);
            for (auto& v : x) v = white (gen);
            const auto out = run (e, { x, x }, p);
            const size_t latency = (size_t) e.latencySamples.load();
            bool same = true;
            for (size_t i = latency; i < x.size(); ++i)
                same = same && out.l[i] == x[i - latency];
            check (same && e.latencySamples.load() == oversamplingLatency (k), fmt ("%dx: MIX 0%% is the input delayed by %d samples, bit for bit: it lines up with the saturated path", kOversamplingFactor[k], (int) latency));
        }
        // An impulse through the wet path arrives at the same sample as the dry one.
        EngineParams p;
        p.mix = 1.0f;
        p.driveDb = 0.0f;
        p.autoGain = false;
        p.oversampling = 3;
        Engine e;
        e.prepare (kFs, 3);
        std::vector<float> x (4000, 0.0f);
        x[1000] = 0.05f;
        const auto out = run (e, { x, x }, p);
        size_t where = 0;
        float best = 0.0f;
        for (size_t i = 0; i < out.l.size(); ++i)
            if (std::fabs (out.l[i]) > best) { best = std::fabs (out.l[i]); where = i; }
        check ((int) where - 1000 == e.latencySamples.load(), fmt ("8x: an impulse through the saturated path peaks %d samples later, the latency reported (%d)", (int) where - 1000, e.latencySamples.load()));
    }

    // [5] Auto gain.
    std::printf ("\n[5] Auto gain\n");
    {
        double worst = 0.0;
        for (int s = 0; s < kNumShapes; ++s)
            for (const float drive : { 6.0f, 18.0f, 30.0f })
            {
                EngineParams p;
                p.shape = (Shape) s;
                p.driveDb = drive;
                p.autoGain = true;
                Engine e;
                e.prepare (kFs, 0);
                const auto in = sine (1000.0, kReferenceDb, n);
                const auto out = run (e, in, p);
                worst = std::fmax (worst, std::fabs (db (rms (out.l, (size_t) n - 24000) / rms (in.l, (size_t) n - 24000))));
            }
        check (worst < 0.05, fmt ("AUTO GAIN: a sine at -12 dBFS comes out as loud as it went in, for every curve at 6, 18 and 30 dB drive, within %.3f dB (RMS)", worst));
    }

    // [6] Tone.
    std::printf ("\n[6] Tone\n");
    {
        EngineParams p;
        p.driveDb = 0.0f;
        p.autoGain = false;
        p.tone = 1.0f;
        const auto at = [&] (double hz)
        {
            Engine e;
            e.prepare (kFs, 0);
            const auto in = sine (hz, -50.0, n); // quiet: the curve is linear
            return levelAt (run (e, in, p).l, hz) - levelAt (in.l, hz);
        };
        const double lo = at (100.0), hi = at (8000.0);
        check (lo < -5.0 && hi > 5.0 && std::fabs ((hi - lo) - 12.0) < 1.5, fmt ("TONE +100%%: 100 Hz %+.1f dB, 8 kHz %+.1f dB: a tilt of %.1f dB (6 up, 6 down)", lo, hi, hi - lo));
        p.tone = 0.0f;
        check (std::fabs (at (100.0)) < 0.02 && std::fabs (at (8000.0)) < 0.02, "TONE 0: flat");
    }

    // [7] Block size, bad samples, mix.
    std::printf ("\n[7] Block size and bad samples\n");
    {
        std::mt19937 gen (9);
        std::uniform_real_distribution<float> white (-0.5f, 0.5f);
        Stereo in { std::vector<float> (50000), std::vector<float> (50000) };
        for (auto& v : in.l) v = white (gen);
        for (auto& v : in.r) v = white (gen);
        EngineParams p;
        p.shape = Shape::diode;
        p.driveDb = 20.0f;
        p.bias = 0.3f;
        p.tone = 0.4f;
        p.mix = 0.6f;
        p.outDb = -3.0f;
        p.oversampling = 2;
        Engine a;
        a.prepare (kFs, 2);
        const auto ref = run (a, in, p, 512);
        bool same = true;
        for (const int block : { 1, 7, 64, 333, 4096 })
        {
            Engine b;
            b.prepare (kFs, 2);
            const auto out = run (b, in, p, block);
            same = same && out.l == ref.l && out.r == ref.r;
        }
        check (same, "blocks of 1, 7, 64, 333 and 4096 samples give the output of 512, bit for bit");
        auto bad = in;
        bad.l[2000] = std::nanf ("");
        bad.r[3000] = INFINITY;
        Engine c;
        c.prepare (kFs, 2);
        const auto out = run (c, bad, p);
        bool fine = true;
        for (size_t i = 0; i < out.l.size(); ++i)
            fine = fine && std::isfinite (out.l[i]) && std::isfinite (out.r[i]);
        check (fine, "a NaN and an infinity in the input are silence: the output is finite");
    }

    return test::finish ("DionysusTests");
}
