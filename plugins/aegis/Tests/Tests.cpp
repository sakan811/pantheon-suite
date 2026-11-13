// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 The Pantheon authors

// Headless tests of the limiter: no plugin, no GUI. Every number the README quotes is measured here.

#include "Check.h"
#include "DSP/Engine.h"
#include <random>

using namespace aegis;
using test::check;
using test::fmt;

namespace
{
constexpr double kFs = 48000.0;

struct Stereo
{
    std::vector<float> l, r;
};

float db (double lin) { return (float) (20.0 * std::log10 (std::fmax (lin, 1e-12))); }

// The loud programme of the tests: noise that is band-limited the way a mix is (two one-pole low-passes at 8 kHz), in bursts, with a bass note and a few lone clicks.
Stereo programme (int n, double peakDb, unsigned seed = 3, bool fullBand = false)
{
    std::mt19937 gen (seed);
    std::uniform_real_distribution<float> white (-1.0f, 1.0f);
    Stereo s { std::vector<float> ((size_t) n), std::vector<float> ((size_t) n) };
    const float a = (float) std::pow (10.0, peakDb / 20.0);
    float lp[2][2] = {};
    for (int i = 0; i < n; ++i)
    {
        const double t = i / kFs;
        const float burst = (i / 6000) % 3 == 0 ? 1.0f : 0.25f;
        const float bass = 0.6f * (float) std::sin (2.0 * M_PI * 55.0 * t);
        for (int c = 0; c < 2; ++c)
        {
            float noise = white (gen);
            if (! fullBand)
            {
                lp[c][0] += 0.6f * (noise - lp[c][0]);
                lp[c][1] += 0.6f * (lp[c][0] - lp[c][1]);
                noise = 1.8f * lp[c][1];
            }
            (c == 0 ? s.l : s.r)[(size_t) i] = a * (0.5f * bass + 0.45f * burst * noise);
        }
        if (i % 17000 == 9000)
            s.l[(size_t) i] = s.r[(size_t) i] = a;
    }
    return s;
}

// The block size the host chooses: the same signal, in pieces.
Stereo run (Engine& e, Stereo in, const EngineParams& p, int block = 512)
{
    for (int start = 0; start < (int) in.l.size(); start += block)
    {
        const int m = std::min (block, (int) in.l.size() - start);
        e.process (in.l.data() + start, in.r.data() + start, m, p);
    }
    return in;
}

double samplePeak (const std::vector<float>& x, size_t from = 0)
{
    double peak = 0.0;
    for (size_t i = from; i < x.size(); ++i)
        peak = std::fmax (peak, std::fabs ((double) x[i]));
    return peak;
}

// The true peak, measured without the limiter's own reader: 8x, by a long windowed sinc (a 64-point Hann), over the whole signal.
double truePeakOf (const std::vector<float>& x, size_t from = 0)
{
    constexpr int half = 32, ratio = 8;
    std::vector<double> kernel ((size_t) (2 * half * ratio));
    for (size_t k = 0; k < kernel.size(); ++k)
    {
        const double t = ((double) k - half * ratio) / ratio;
        kernel[k] = (std::fabs (t) < 1e-12 ? 1.0 : std::sin (M_PI * t) / (M_PI * t)) * (0.5 + 0.5 * std::cos (M_PI * t / half));
    }
    double peak = 0.0;
    for (size_t i = std::max<size_t> (from, half); i + half < x.size(); ++i)
        for (int phase = 0; phase < ratio; ++phase)
        {
            double sum = 0.0;
            for (int j = -half + 1; j <= half; ++j)
                sum += (double) x[(size_t) ((long) i + j)] * kernel[(size_t) (half * ratio + phase - j * ratio)];
            peak = std::fmax (peak, std::fabs (sum));
        }
    return peak;
}
}

int main()
{
    std::printf ("Aegis tests (48 kHz)\n");

    // [1] The ceiling holds, for any programme and any settings.
    std::printf ("\n[1] The ceiling\n");
    {
        for (const float ceilingDb : { -0.3f, -1.0f, -6.0f })
            for (const int look : { 0, 2, 3 })
              for (const bool autoRelease : { false, true })
            {
                Engine e;
                e.prepare (kFs, look);
                EngineParams p;
                p.autoRelease = autoRelease;
                p.gainDb = 12.0f;
                p.ceilingDb = ceilingDb;
                p.lookahead = look;
                p.releaseMs = 50.0f;
                const auto out = run (e, programme (48000 * 2, 0.0), p);
                const size_t from = (size_t) e.latencySamples.load() * 2;
                const double sp = samplePeak (out.l, from), tp = std::fmax (truePeakOf (out.l, from), truePeakOf (out.r, from));
                check (db (std::fmax (sp, samplePeak (out.r, from))) <= ceilingDb + 0.001f && e.clamped == 0,
                       fmt ("ceiling %.1f dB, look-ahead %.1f ms, auto release %s: sample peak %.3f dBFS, and the final clamp never had to act (%llu)", ceilingDb, kLookaheadMs[look], autoRelease ? "on" : "off",
                            db (std::fmax (sp, samplePeak (out.r, from))), (unsigned long long) e.clamped));
                check (db (tp) <= ceilingDb + 0.1f, fmt ("    true peak %.3f dBTP (limit %.1f + 0.1)", db (tp), ceilingDb));
            }
    }

    // [2] The peaks between the samples.
    std::printf ("\n[2] True peak\n");
    {
        // A tone at a quarter of the sample rate, 45 degrees off the samples: every sample is 0.707 of the peak between them.
        std::vector<float> tone (48000);
        for (size_t i = 0; i < tone.size(); ++i)
            tone[i] = (float) (1.6 * std::sin (2.0 * M_PI * 0.25 * (double) i + M_PI / 4.0));
        const auto measure = [&] (bool tp)
        {
            Engine e;
            e.prepare (kFs, 2);
            EngineParams p;
            p.truePeak = tp;
            p.ceilingDb = -1.0f;
            auto out = run (e, { tone, tone }, p);
            return std::pair<double, double> { truePeakOf (out.l, 24000), samplePeak (out.l, 24000) };
        };
        const auto on = measure (true), off = measure (false);
        check (db (on.first) <= -1.0f + 0.1f, fmt ("TRUE PEAK on: a 1.6 (+4.1 dBFS) tone whose samples read 1.13 comes out at %.3f dBTP, under the -1 dBTP ceiling", db (on.first)));
        check (db (off.first) > -1.0f + 0.5f && db (off.second) <= -1.0f + 0.001f,
               fmt ("TRUE PEAK off: the samples are held to -1 dBFS (%.3f) but the tone's true peak is %.2f dBTP, over: the switch matters", db (off.second), db (off.first)));
    }

    // [3] Nothing to limit: the input, later, bit for bit.
    std::printf ("\n[3] Transparency\n");
    {
        Engine e;
        e.prepare (kFs, 2);
        EngineParams p;
        const auto in = programme (48000, -12.0);
        const auto out = run (e, in, p);
        const int latency = e.latencySamples.load();
        bool same = true;
        for (size_t i = (size_t) latency; i < in.l.size(); ++i)
            same = same && out.l[i] == in.l[i - (size_t) latency] && out.r[i] == in.r[i - (size_t) latency];
        check (same, fmt ("a programme peaking at -12 dBFS with the ceiling at -1 dBTP comes out as the input delayed by %d samples, bit for bit", latency));
        check (e.reductionMeter.load() == 0.0f, "... and the reduction meter read 0.0 dB");
    }

    // [4] The same output for any block size.
    std::printf ("\n[4] Block size\n");
    {
        const auto in = programme (48000, 0.0);
        EngineParams p;
        p.gainDb = 9.0f;
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
    }

    // [5] The limiter starts before the peak, not after it.
    std::printf ("\n[5] Look-ahead\n");
    {
        Engine e;
        e.prepare (kFs, 2);
        EngineParams p;
        p.ceilingDb = -6.0f;
        p.truePeak = false; // a step's own true peak is 0.8 dB over its samples: here it is the timing that is measured
        std::vector<float> x (24000, 0.0f);
        for (size_t i = 12000; i < 12000 + 4800; ++i)
            x[i] = 1.0f; // a step to full scale, held for 100 ms
        auto out = run (e, { x, x }, p, 64);
        const int latency = e.latencySamples.load(), look = lookaheadSamples (kFs, 2);
        const size_t at = 12000 + (size_t) latency; // where the step arrives in the output
        const float before = out.l[at - 1], arrives = out.l[at];
        // The gain has been coming down for `look` samples: the step is at the ceiling when it arrives, never over.
        check (std::fabs (db (arrives) - (-6.0f)) < 0.05f && before == 0.0f, fmt ("a step to 0 dBFS arrives at %.2f dB (the ceiling is -6) the sample it is heard, with %d samples of look-ahead", db (arrives), look));
        // And after it ends the gain recovers with the release: 63% of the way in the release time.
        EngineParams slow = p;
        slow.releaseMs = 200.0f;
        Engine f;
        f.prepare (kFs, 2);
        std::vector<float> y (96000, 0.0f);
        for (size_t i = 8000; i < 12000; ++i)
            y[i] = 1.0f;
        for (size_t i = 12000; i < y.size(); ++i)
            y[i] = 0.25f; // quiet material after the burst: its level follows the gain
        auto o2 = run (f, { y, y }, slow, 64);
        const size_t end = 12000 + (size_t) f.latencySamples.load();
        const double g0 = o2.l[end + 200] / 0.25, g1 = o2.l[end + 200 + 9600] / 0.25; // 200 ms later
        const double deficit0 = 1.0 - g0, deficit1 = 1.0 - g1;
        check (g0 < 0.6 && std::fabs (deficit1 / deficit0 - std::exp (-1.0)) < 0.08,
               fmt ("200 ms after a burst the gain has come back to %.3f from %.3f: the deficit is %.2f of what it was (1/e = 0.37) with RELEASE at 200 ms", g1, g0, deficit1 / deficit0));
    }

    // [6] Latency, and the clock.
    std::printf ("\n[6] Latency\n");
    {
        for (int look = 0; look < kNumLookahead; ++look)
        {
            Engine e;
            e.prepare (kFs, look);
            EngineParams p;
            p.lookahead = look;
            std::vector<float> x (4000, 0.0f);
            x[1000] = 0.25f;
            auto out = run (e, { x, x }, p);
            size_t where = 0;
            for (size_t i = 0; i < out.l.size(); ++i)
                if (out.l[i] != 0.0f)
                    where = i;
            check ((int) where - 1000 == e.latencySamples.load() && e.latencySamples.load() == lookaheadSamples (kFs, look) + kDetectorDelay,
                   fmt ("look-ahead %.1f ms: an impulse is heard %d samples later = the look-ahead (%d) + %d for the true-peak reader, as reported", kLookaheadMs[look],
                        (int) where - 1000, lookaheadSamples (kFs, look), kDetectorDelay));
        }
    }

    // [7] Both channels share one gain.
    std::printf ("\n[7] Stereo link\n");
    {
        Engine e;
        e.prepare (kFs, 2);
        EngineParams p;
        p.ceilingDb = -6.0f;
        std::vector<float> l (24000, 0.0f), r (24000, 0.0f);
        for (size_t i = 4000; i < 20000; ++i)
        {
            l[i] = 1.0f * (float) std::sin (2.0 * M_PI * 220.0 * (double) i / kFs);
            r[i] = 0.1f * (float) std::sin (2.0 * M_PI * 220.0 * (double) i / kFs);
        }
        auto out = run (e, { l, r }, p);
        const double left = samplePeak (std::vector<float> (out.l.begin() + 12000, out.l.begin() + 14000)), right = samplePeak (std::vector<float> (out.r.begin() + 12000, out.r.begin() + 14000));
        check (std::fabs (right / left - 0.1) < 0.002, fmt ("a loud left and a quiet right keep their balance: right/left = %.4f (0.1 in), so the image does not move", right / left));
    }

    // [8] The limit of a reader: full-band noise.
    std::printf("\n[8] Full-band noise\n");
    {
        Engine e;
        e.prepare (kFs, 2);
        EngineParams p;
        p.gainDb = 12.0f;
        p.ceilingDb = -1.0f;
        p.releaseMs = 50.0f;
        const auto out = run (e, programme (48000 * 2, 0.0, 3, true), p);
        const size_t from = (size_t) e.latencySamples.load() * 2;
        const double tp = std::fmax (truePeakOf (out.l, from), truePeakOf (out.r, from));
        check (db (tp) <= -1.0f + 0.1f, fmt ("white noise up to Nyquist, driven 12 dB (the worst a 32-tap reader missed by 0.7 dB): true peak %.2f dBTP with the ceiling at -1", db (tp)));
    }

    // [9] Bad input.
    std::printf ("\n[9] Bad samples\n");
    {
        Engine e;
        e.prepare (kFs, 2);
        EngineParams p;
        auto in = programme (24000, -6.0);
        in.l[5000] = std::nanf ("");
        in.r[6000] = INFINITY;
        const auto out = run (e, in, p);
        bool fine = true;
        for (size_t i = 0; i < out.l.size(); ++i)
            fine = fine && std::isfinite (out.l[i]) && std::isfinite (out.r[i]) && std::fabs (out.l[i]) <= 1.0f;
        check (fine, "a NaN and an infinity in the input are silence: the output is finite and under the ceiling");
    }

    // [10] Gain ahead of the ceiling.
    std::printf ("\n[10] Input gain\n");
    {
        Engine e;
        e.prepare (kFs, 2);
        EngineParams p;
        p.gainDb = 6.0f;
        std::vector<float> x (48000);
        for (size_t i = 0; i < x.size(); ++i)
            x[i] = 0.1f * (float) std::sin (2.0 * M_PI * 440.0 * (double) i / kFs);
        auto out = run (e, { x, x }, p);
        const double gain = samplePeak (std::vector<float> (out.l.begin() + 24000, out.l.end())) / 0.1;
        check (std::fabs (db (gain) - 6.0f) < 0.05f, fmt ("GAIN +6 dB lifts a quiet tone by %.2f dB (nothing to limit)", db (gain)));
    }

    // [11] Auto release: a short peak is let go sooner than RELEASE, a sustained reduction at RELEASE.
    std::printf ("\n[11] Auto release\n");
    {
        // 25 ms burst (or 1 s of it) at +12 dB over the ceiling, then quiet material at 0.25 whose level follows the gain; 250 ms after the burst ends, how much of the deficit is left.
        const auto deficitAfter = [&] (bool autoRelease, int burstSamples)
        {
            Engine e;
            e.prepare (kFs, 2);
            EngineParams p;
            p.ceilingDb = -6.0f;
            p.releaseMs = 400.0f;
            p.autoRelease = autoRelease;
            p.truePeak = false;
            const size_t start = 4000, end = start + (size_t) burstSamples;
            std::vector<float> y (end + 48000, 0.25f);
            for (size_t i = start; i < end; ++i)
                y[i] = 1.0f;
            auto out = run (e, { y, y }, p, 64);
            const size_t at = end + (size_t) e.latencySamples.load();
            return std::pair<double, double> { 1.0 - out.l[at + 40] / 0.25, 1.0 - out.l[at + 12000] / 0.25 }; // just after the burst, and 250 ms later
        };
        const auto shortManual = deficitAfter (false, 1200), shortAuto = deficitAfter (true, 1200);
        check (shortAuto.second < 0.5 * shortManual.second, fmt ("after a 25 ms burst the deficit 250 ms later is %.3f with AUTO against %.3f with RELEASE at 400 ms: the peak is let go sooner", shortAuto.second, shortManual.second));
        const auto longManual = deficitAfter (false, 48000), longAuto = deficitAfter (true, 48000);
        check (std::fabs (longAuto.second - longManual.second) < 0.08, fmt ("after 1 s of reduction AUTO releases at RELEASE: the deficit 250 ms later is %.3f against %.3f", longAuto.second, longManual.second));
    }

    // [12] OUT: the output's own peak, read the way the detector reads, agrees with an independent measure and is never over the ceiling.
    std::printf ("\n[12] Output meter\n");
    {
        for (const bool tp : { true, false })
        {
            Engine e;
            e.prepare (kFs, 2);
            EngineParams p;
            p.gainDb = 12.0f;
            p.ceilingDb = -1.0f;
            p.truePeak = tp;
            const auto out = run (e, programme (48000 * 2, 0.0), p);
            const double shown = e.outPeak.load(), measured = tp ? std::fmax (truePeakOf (out.l), truePeakOf (out.r)) : std::fmax (samplePeak (out.l), samplePeak (out.r));
            check (std::fabs (db (shown) - db (measured)) < 0.1f && db (shown) <= -1.0f + 0.1f,
                   fmt ("TRUE PEAK %s: OUT reads %.3f dB, an independent reading of the output says %.3f dB, the ceiling is -1", tp ? "on" : "off", db (shown), db (measured)));
        }
    }

    return test::finish ("AegisTests");
}
