// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 The Pantheon authors

// Headless tests of the de-esser: no plugin, no GUI. Every number the README quotes is measured here.

#include "Check.h"
#include "DSP/Engine.h"
#include <random>

using namespace nemesis;
using test::check;
using test::fmt;

namespace
{
constexpr double kFs = 48000.0;

struct Stereo
{
    std::vector<float> l, r;
};

// A sum of tones: {frequency, level in dBFS (peak)} pairs, the same in both channels.
Stereo tones (std::initializer_list<std::pair<double, double>> parts, int n)
{
    Stereo s { std::vector<float> ((size_t) n, 0.0f), std::vector<float> ((size_t) n, 0.0f) };
    for (const auto& [hz, db] : parts)
        for (int i = 0; i < n; ++i)
        {
            const float v = (float) (std::pow (10.0, db / 20.0) * std::sin (2.0 * M_PI * hz * i / kFs));
            s.l[(size_t) i] += v;
            s.r[(size_t) i] += v;
        }
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

// The level of one frequency in a signal's last half second, in dB (a single-bin DFT: the other tones do not count).
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

// What a tone's level changes by, in dB, in a signal of several.
double changeAt (const Stereo& in, const Stereo& out, double hz) { return levelAt (out.l, hz) - levelAt (in.l, hz); }
}

int main()
{
    std::printf ("Nemesis tests (48 kHz)\n");
    const int n = 48000 * 2;

    // [1] The law.
    std::printf ("\n[1] The reduction\n");
    {
        EngineParams p;
        p.thresholdDb = -30.0f;
        p.rangeDb = 24.0f;
        Engine e;
        e.prepare (kFs);
        const auto in = tones ({ { 6500.0, -20.0 } }, n);
        const auto out = run (e, in, p);
        check (std::fabs (changeAt (in, out, 6500.0) + 10.0) < 0.3, fmt ("a sibilant at -20 dBFS over a -30 dB threshold: turned down by %.2f dB (the law: 10: it holds the band at the threshold)", -changeAt (in, out, 6500.0)));
        p.rangeDb = 6.0f;
        Engine f;
        f.prepare (kFs);
        const auto out2 = run (f, in, p);
        check (std::fabs (changeAt (in, out2, 6500.0) + 6.0) < 0.2, fmt ("RANGE 6 dB: the reduction stops at %.2f dB", -changeAt (in, out2, 6500.0)));
    }

    // [2] Only the band: SPLIT-BAND turns down the sibilance and leaves the rest.
    std::printf ("\n[2] Split-band and wideband\n");
    {
        EngineParams p;
        p.thresholdDb = -30.0f;
        p.rangeDb = 12.0f;
        const auto in = tones ({ { 300.0, -12.0 }, { 6500.0, -16.0 } }, n);
        Engine e;
        e.prepare (kFs);
        const auto split = run (e, in, p);
        p.mode = Mode::wideband;
        Engine f;
        f.prepare (kFs);
        const auto wide = run (f, in, p);
        check (std::fabs (changeAt (in, split, 300.0)) < 0.3 && changeAt (in, split, 6500.0) < -8.0,
               fmt ("SPLIT-BAND: the vowel at 300 Hz changes by %+.2f dB, the sibilant at 6.5 kHz by %+.1f dB", changeAt (in, split, 300.0), changeAt (in, split, 6500.0)));
        check (changeAt (in, wide, 300.0) < -8.0 && std::fabs (changeAt (in, wide, 300.0) - changeAt (in, wide, 6500.0)) < 0.3,
               fmt ("WIDEBAND: the whole sound is turned down together (300 Hz %+.1f dB, 6.5 kHz %+.1f dB)", changeAt (in, wide, 300.0), changeAt (in, wide, 6500.0)));
        // Selectivity: a tone far from the band, as loud as the sibilant, is not touched.
        p.mode = Mode::splitBand;
        const auto far = tones ({ { 1500.0, -16.0 } }, n);
        Engine g;
        g.prepare (kFs);
        const auto outFar = run (g, far, p);
        check (std::fabs (changeAt (far, outFar, 1500.0)) < 0.05, fmt ("a tone at 1.5 kHz as loud as the sibilant is not touched (%+.3f dB): the detector listens to the band", changeAt (far, outFar, 1500.0)));
    }

    // [3] Listen.
    std::printf ("\n[3] Listen\n");
    {
        EngineParams p;
        p.listen = true;
        const auto in = tones ({ { 300.0, -12.0 }, { 6500.0, -16.0 } }, n);
        Engine e;
        e.prepare (kFs);
        const auto out = run (e, in, p);
        check (levelAt (out.l, 300.0) < levelAt (in.l, 300.0) - 25.0 && std::fabs (changeAt (in, out, 6500.0)) < 0.1,
               fmt ("LISTEN: you hear the band the detector hears: the 6.5 kHz tone passes (%+.2f dB), the 300 Hz one is %.0f dB down", changeAt (in, out, 6500.0), -changeAt (in, out, 300.0)));
    }

    // [4] Nothing to do: the input, bit for bit.
    std::printf ("\n[4] Transparency\n");
    {
        EngineParams p;
        std::mt19937 gen (5);
        std::uniform_real_distribution<float> white (-1.0f, 1.0f);
        Stereo in { std::vector<float> (96000), std::vector<float> (96000) };
        for (auto& v : in.l) v = 0.01f * white (gen); // -40 dBFS: under the threshold
        for (auto& v : in.r) v = 0.01f * white (gen);
        for (const Mode m : { Mode::splitBand, Mode::wideband })
        {
            p.mode = m;
            Engine e;
            e.prepare (kFs);
            const auto out = run (e, in, p);
            check (out.l == in.l && out.r == in.r, fmt ("%s: a signal under the threshold comes out as it went in, bit for bit", m == Mode::splitBand ? "SPLIT-BAND" : "WIDEBAND"));
        }
        Engine e;
        e.prepare (kFs);
        run (e, tones ({ { 6500.0, -8.0 } }, 24000), p);
        const auto quiet = tones ({ { 6500.0, -50.0 }, { 200.0, -30.0 } }, 96000);
        const auto out = run (e, quiet, p);
        check (std::equal (out.l.begin() + 72000, out.l.end(), quiet.l.begin() + 72000), "half a second after a loud sibilant the gain is back at exactly 1: the quiet sound is the input, bit for bit");
    }

    // [5] Release.
    std::printf ("\n[5] Release\n");
    {
        EngineParams p;
        p.thresholdDb = -30.0f;
        p.rangeDb = 24.0f;
        p.releaseMs = 50.0f;
        // A sibilant for a second, then a quiet 7.2 kHz tone (under the threshold) that shows the gain coming back: read in windows of 5 ms.
        Stereo in = tones ({ { 6500.0, -14.0 } }, 48000 * 3);
        const auto quiet = tones ({ { 6500.0, -60.0 } }, 48000 * 3);
        for (int i = 48000; i < 48000 * 3; ++i)
            in.l[(size_t) i] = in.r[(size_t) i] = quiet.l[(size_t) i];
        Engine e;
        e.prepare (kFs);
        const auto out = run (e, in, p, 64);
        const auto windowDb = [&] (const std::vector<float>& x, double seconds)
        {
            const size_t c = (size_t) (seconds * kFs);
            double peak = 0.0;
            for (size_t i = c; i < c + 240; ++i)
                peak = std::fmax (peak, std::fabs ((double) x[i]));
            return db (peak);
        };
        const double reduction0 = windowDb (in.l, 1.001) - windowDb (out.l, 1.001), reduction50 = windowDb (in.l, 1.05) - windowDb (out.l, 1.05);
        const double full = windowDb (in.l, 0.9) - windowDb (out.l, 0.9);
        check (full > 15.0 && reduction50 / reduction0 > 0.25 && reduction50 / reduction0 < 0.5 && windowDb (in.l, 2.0) - windowDb (out.l, 2.0) < 0.05,
               fmt ("RELEASE 50 ms: the reduction is %.1f dB at the end of the sibilant, %.0f%% of it (%.1f dB) 50 ms later, and gone after a second", reduction0, 100.0 * reduction50 / reduction0, reduction50));
    }

    // [6] Relative detection: the threshold follows the level of the sound.
    std::printf ("\n[6] Absolute and relative\n");
    {
        EngineParams p;
        p.rangeDb = 24.0f;
        p.detection = Detection::relative;
        p.thresholdDb = -8.0f; // the sibilant is allowed to be 8 dB under the whole sound's level
        const auto loud = tones ({ { 300.0, -12.0 }, { 6500.0, -12.0 } }, n);
        const auto soft = tones ({ { 300.0, -42.0 }, { 6500.0, -42.0 } }, n);
        Engine e, f;
        e.prepare (kFs);
        f.prepare (kFs);
        const auto ol = run (e, loud, p), os = run (f, soft, p);
        const double a = changeAt (loud, ol, 6500.0), b = changeAt (soft, os, 6500.0);
        check (a < -1.0 && std::fabs (a - b) < 0.5, fmt ("RELATIVE: the same sound 30 dB quieter is turned down by the same amount (%.2f dB loud, %.2f dB soft)", -a, -b));
        p.detection = Detection::absolute;
        p.thresholdDb = -30.0f;
        Engine g, h;
        g.prepare (kFs);
        h.prepare (kFs);
        const double c = changeAt (loud, run (g, loud, p), 6500.0), d = changeAt (soft, run (h, soft, p), 6500.0);
        check (c < -5.0 && std::fabs (d) < 0.05, fmt ("ABSOLUTE: the quiet one is not touched at all (%.2f dB loud, %+.3f dB soft)", -c, d));
    }

    // [7] Stereo link, block size, bad samples.
    std::printf ("\n[7] Link, block size, bad samples\n");
    {
        EngineParams p;
        p.thresholdDb = -30.0f;
        p.rangeDb = 12.0f;
        Stereo in = tones ({ { 6500.0, -14.0 } }, n);
        for (auto& v : in.r) v *= 0.1f;
        Engine e;
        e.prepare (kFs);
        const auto out = run (e, in, p);
        check (std::fabs (changeAt (in, out, 6500.0) - (levelAt (out.r, 6500.0) - levelAt (in.r, 6500.0))) < 0.01, "a loud left and a quiet right get the same gain: the image does not move");
        std::mt19937 gen (9);
        std::uniform_real_distribution<float> white (-1.0f, 1.0f);
        Stereo noise { std::vector<float> (60000), std::vector<float> (60000) };
        for (size_t i = 0; i < noise.l.size(); ++i)
        {
            const float burst = (i / 6000) % 2 == 0 ? 0.6f : 0.05f;
            noise.l[i] = burst * white (gen);
            noise.r[i] = burst * white (gen);
        }
        EngineParams q;
        q.detection = Detection::relative;
        q.thresholdDb = -6.0f;
        Engine a;
        a.prepare (kFs);
        const auto ref = run (a, noise, q, 512);
        bool same = true;
        for (const int block : { 1, 7, 64, 333, 4096 })
        {
            Engine b;
            b.prepare (kFs);
            const auto o = run (b, noise, q, block);
            same = same && o.l == ref.l && o.r == ref.r;
        }
        check (same, "blocks of 1, 7, 64, 333 and 4096 samples give the output of 512, bit for bit");
        auto bad = noise;
        bad.l[2000] = std::nanf ("");
        bad.r[3000] = INFINITY;
        Engine c;
        c.prepare (kFs);
        const auto o = run (c, bad, q);
        bool fine = true;
        for (size_t i = 0; i < o.l.size(); ++i)
            fine = fine && std::isfinite (o.l[i]) && std::isfinite (o.r[i]);
        check (fine, "a NaN and an infinity in the input are silence: the output is finite");
    }

    // [8] Look-ahead.
    std::printf ("\n[8] Look-ahead\n");
    {
        const int index = 2; // 1 ms
        const int delay = (int) std::lround (0.001 * kLookaheadMs[(size_t) index] * kFs);
        EngineParams quiet;
        quiet.lookahead = index;
        const auto in = tones ({ { 6500.0, -50.0 }, { 300.0, -30.0 } }, 24000); // far under the threshold: nothing is turned down
        Engine e;
        e.prepare (kFs, index);
        const auto out = run (e, in, quiet);
        Stereo expected { std::vector<float> (in.l.size(), 0.0f), std::vector<float> (in.l.size(), 0.0f) };
        for (int i = delay; i < (int) in.l.size(); ++i)
        {
            expected.l[(size_t) i] = in.l[(size_t) (i - delay)];
            expected.r[(size_t) i] = in.r[(size_t) (i - delay)];
        }
        check (out.l == expected.l && out.r == expected.r && e.latencySamples.load() == delay, fmt ("nothing to turn down: the output is the input %d samples later (1 ms), bit for bit, and the engine says so", delay));

        // A burst of sibilance at -10 dBFS after near silence: the attack is 0.3 ms, so without look-ahead the first 0.3 ms gets through. (Wideband: in split-band the band-pass
        // itself takes about 70 us to answer, so the first cycle of a burst is not turned down whatever the look-ahead.)
        EngineParams p;
        p.thresholdDb = -30.0f;
        p.rangeDb = 12.0f;
        p.mode = Mode::wideband;
        const int step = 4800, window = 14;
        auto burst = tones ({ { 6500.0, -70.0 } }, step);
        const auto loud = tones ({ { 6500.0, -10.0 } }, 4800);
        burst.l.insert (burst.l.end(), loud.l.begin(), loud.l.end());
        burst.r = burst.l;
        EngineParams withLookahead = p;
        withLookahead.lookahead = index;
        Engine plain, ahead;
        plain.prepare (kFs);
        ahead.prepare (kFs, index);
        const auto a = run (plain, burst, p), b = run (ahead, burst, withLookahead);
        const auto peakOf = [] (const std::vector<float>& x, int from, int count)
        {
            double peak = 0.0;
            for (int i = from; i < from + count; ++i)
                peak = std::fmax (peak, std::fabs ((double) x[(size_t) i]));
            return peak;
        };
        const double without = db (peakOf (a.l, step, window)), with = db (peakOf (b.l, step + delay, window));
        check (with < without - 6.0, fmt ("the first 0.3 ms of a -10 dBFS sibilant: %.1f dBFS without look-ahead, %.1f dBFS with 1 ms (already turned down)", without, with));

        bool same = true;
        for (const int block : { 1, 7, 333, 4096 })
        {
            Engine c;
            c.prepare (kFs, index);
            const auto o = run (c, burst, withLookahead, block);
            same = same && o.l == b.l && o.r == b.r;
        }
        check (same, "with look-ahead, blocks of 1, 7, 333 and 4096 samples give the output of 512, bit for bit");
        Engine d;
        d.prepare (kFs);
        EngineParams two = p;
        two.lookahead = 4;
        auto tiny = tones ({ { 6500.0, -70.0 } }, 256);
        run (d, tiny, two);
        check (d.latencySamples.load() == (int) std::lround (0.005 * kFs), "changing the look-ahead while running changes the latency the engine reports (5 ms: 240 samples)");
    }

    // [9] DELTA and FIND.
    std::printf ("\n[9] Delta and find\n");
    {
        for (const Mode mode : { Mode::splitBand, Mode::wideband })
        {
            EngineParams p;
            p.mode = mode;
            p.rangeDb = 12.0f;
            const auto in = tones ({ { 6500.0, -12.0 }, { 300.0, -20.0 } }, 48000);
            Engine a, b;
            a.prepare (kFs);
            b.prepare (kFs);
            const auto heard = run (a, in, p);
            p.delta = true;
            const auto delta = run (b, in, p);
            double worst = 0.0, deltaPeak = 0.0;
            for (size_t i = 0; i < in.l.size(); ++i)
            {
                worst = std::fmax (worst, std::fabs ((double) in.l[i] - ((double) heard.l[i] + (double) delta.l[i])));
                deltaPeak = std::fmax (deltaPeak, std::fabs ((double) delta.l[i]));
            }
            check (worst < 1e-6 && deltaPeak > 0.05, fmt ("%s: the output plus DELTA is the input (largest difference %.1e; the delta reaches %.3f)", mode == Mode::splitBand ? "split-band" : "wideband", worst, deltaPeak));
        }
        {
            EngineParams p;
            p.delta = true;
            p.thresholdDb = 0.0f;
            const auto in = tones ({ { 6500.0, -30.0 } }, 24000);
            Engine e;
            e.prepare (kFs);
            const auto out = run (e, in, p);
            double peak = 0.0;
            for (const float v : out.l)
                peak = std::fmax (peak, std::fabs ((double) v));
            check (peak == 0.0, "DELTA with nothing turned down is exactly silent");
        }

        // FIND: the strongest place between 4 and 10 kHz, of a tone, of a tone over a louder one outside the range, of nothing.
        const auto found = [] (const Stereo& in, int blocks = 1)
        {
            Engine e;
            e.prepare (kFs);
            e.startFind();
            EngineParams p;
            auto s = in;
            for (int i = 0; i < blocks; ++i)
                s = run (e, s, p);
            return std::make_pair (e.foundHz.load(), e.findCount.load());
        };
        for (const double hz : { 4500.0, 6200.0, 7800.0, 9500.0 })
        {
            const auto r = found (tones ({ { hz, -20.0 } }, 96000));
            check (r.second == 1 && std::fabs (r.first - hz) < 0.04 * hz, fmt ("FIND: a %.0f Hz tone is found at %.0f Hz", hz, (double) r.first));
        }
        const auto over = found (tones ({ { 300.0, 0.0 }, { 7000.0, -26.0 } }, 96000));
        check (std::fabs (over.first - 7000.0) < 280.0, fmt ("FIND: a 7 kHz tone at -26 dBFS is found at %.0f Hz under a 300 Hz voice at 0 dBFS (26 dB louder, outside the range)", (double) over.first));
        std::mt19937 gen (11);
        std::uniform_real_distribution<float> white (-1.0f, 1.0f);
        Stereo noise { std::vector<float> (96000), std::vector<float> (96000) };
        pdsp::Biquad shape;
        shape.setBandpass (kFs, 5600.0, 4.0);
        for (size_t i = 0; i < noise.l.size(); ++i)
            noise.l[i] = noise.r[i] = 0.02f * white (gen) + 0.5f * (float) shape.process (white (gen));
        const auto n = found (noise);
        check (n.first > 5000.0f && n.first < 6200.0f, fmt ("FIND: noise with a hump at 5.6 kHz is found at %.0f Hz", (double) n.first));
        const auto none = found (tones ({ { 6500.0, -130.0 } }, 96000));
        check (none.second == 1 && none.first == 0.0f, "FIND: a sound at -130 dBFS gives no answer (0), not a frequency");
        Engine pending;
        pending.prepare (kFs);
        pending.startFind();
        auto shortSound = tones ({ { 6500.0, -20.0 } }, 4800);
        run (pending, shortSound, EngineParams {});
        check (pending.finding.load() && pending.findCount.load() == 0, "FIND takes the next 1.5 s of sound: 0.1 s in, it is still listening");
    }

    return test::finish ("NemesisTests");
}
