// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 The Pantheon authors

// Headless tests of the compressor: no plugin, no GUI. Every number the README quotes is measured here.

#include "Check.h"
#include "DSP/Engine.h"
#include <random>

using namespace hera;
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

Stereo run (Engine& e, Stereo in, const EngineParams& p, int block = 512, const Stereo* key = nullptr)
{
    for (int start = 0; start < (int) in.l.size(); start += block)
    {
        const int m = std::min (block, (int) in.l.size() - start);
        e.process (in.l.data() + start, in.r.data() + start, m, p, key ? key->l.data() + start : nullptr, key ? key->r.data() + start : nullptr);
    }
    return in;
}

double peakOf (const std::vector<float>& x, size_t from, size_t to)
{
    double peak = 0.0;
    for (size_t i = from; i < to && i < x.size(); ++i)
        peak = std::fmax (peak, std::fabs ((double) x[i]));
    return peak;
}

double db (double lin) { return 20.0 * std::log10 (std::fmax (lin, 1e-12)); }

// The steady gain (dB) a sine of `levelDb` gets: the peak of the last 100 ms against the input's.
double gainOf (const EngineParams& p, double levelDb, double hz = 1000.0)
{
    Engine e;
    e.prepare (kFs);
    const int n = 48000 * 2;
    const auto out = run (e, sine (hz, levelDb, n), p);
    return db (peakOf (out.l, (size_t) n - 4800, (size_t) n)) - levelDb;
}
}

int main()
{
    std::printf ("Hera tests (48 kHz)\n");

    // [1] The law.
    std::printf ("\n[1] The gain computer\n");
    {
        EngineParams p;
        p.thresholdDb = -24.0f;
        p.ratio = 4.0f;
        p.kneeDb = 0.0f;
        p.attackMs = 1.0f;
        p.releaseMs = 50.0f;
        for (const Detector d : { Detector::peak, Detector::rms })
        {
            p.detector = d;
            const double g = gainOf (p, -6.0);
            check (std::fabs (g + 13.5) < 0.2, fmt ("%s: a sine at -6 dBFS over a -24 dB threshold at 4:1 (no knee) is turned down by %.2f dB (the law: 13.5)", d == Detector::peak ? "peak" : "RMS", -g));
        }
        p.ratio = 20.0f;
        check (std::fabs (gainOf (p, -6.0) + 17.1) < 0.2, fmt ("20:1: %.2f dB (the law: 17.1)", -gainOf (p, -6.0)));
        p.ratio = 4.0f;
        p.kneeDb = 12.0f;
        p.thresholdDb = -12.0f;
        check (std::fabs (gainOf (p, -12.0) + 1.125) < 0.12, fmt ("a 12 dB knee: exactly at the threshold the reduction is %.2f dB (the law: 1.125)", -gainOf (p, -12.0)));
        check (std::fabs (gainOf (p, -20.0)) < 0.01, fmt ("... and half a knee under it (8 dB under), %.3f dB: none", -gainOf (p, -20.0)));
        p.thresholdDb = -24.0f;
        p.kneeDb = 6.0f;
        p.ratio = 1.0f;
        check (gainOf (p, -3.0) == 0.0 || std::fabs (gainOf (p, -3.0)) < 1e-6, "RATIO 1:1 does nothing");
    }

    // [2] Nothing to do: the input, bit for bit.
    std::printf ("\n[2] Transparency\n");
    {
        EngineParams p;
        p.thresholdDb = -6.0f;
        std::mt19937 gen (5);
        std::uniform_real_distribution<float> white (-0.4f, 0.4f);
        Stereo in { std::vector<float> (48000), std::vector<float> (48000) };
        for (auto& v : in.l) v = white (gen) * 0.2f; // peaks at -22 dBFS, the threshold is -6
        for (auto& v : in.r) v = white (gen) * 0.2f;
        const auto out = run ([&] { static Engine e; e.prepare (kFs); return std::ref (e); }().get(), in, p);
        check (out.l == in.l && out.r == in.r, "a signal under the threshold (and MIX 100, MAKEUP 0) comes out as it went in, bit for bit");
        // After a loud passage the gain comes back to exactly 1.
        Engine e;
        e.prepare (kFs);
        auto loud = sine (200.0, -1.0, 24000);
        run (e, loud, p);
        auto quiet = sine (200.0, -30.0, 48000 * 4);
        const auto in2 = quiet;
        const auto out2 = run (e, quiet, p);
        check (std::equal (out2.l.begin() + 48000 * 3, out2.l.end(), in2.l.begin() + 48000 * 3), "three seconds after the loud passage the gain is back at exactly 1: the quiet tone is the input, bit for bit");
    }

    // [3] Attack and release.
    std::printf ("\n[3] Attack and release\n");
    {
        EngineParams p;
        p.thresholdDb = -30.0f;
        p.ratio = 8.0f;
        p.kneeDb = 0.0f;
        p.detector = Detector::peak;
        p.attackMs = 20.0f;
        p.releaseMs = 200.0f;
        // A tone at -6 dBFS from 0.5 s to 1.5 s, then a quiet one (-45 dBFS: under the threshold) that shows the gain coming back.
        const int n = 48000 * 3;
        Stereo in = sine (1000.0, -6.0, n);
        const auto quiet = sine (1000.0, -45.0, n);
        for (int i = 0; i < 24000; ++i) in.l[(size_t) i] = in.r[(size_t) i] = 0.0f;
        for (int i = 72000; i < n; ++i) in.l[(size_t) i] = in.r[(size_t) i] = quiet.l[(size_t) i];
        Engine e;
        e.prepare (kFs);
        const auto out = run (e, in, p, 64);
        const auto reductionAt = [&] (double seconds) // a millisecond is one period of the tone
        {
            const size_t c = (size_t) (seconds * kFs);
            return db (peakOf (in.l, c, c + 48)) - db (peakOf (out.l, c, c + 48));
        };
        const double full = reductionAt (1.45), start = reductionAt (0.5 + 0.0005), atAttack = reductionAt (0.5 + 0.020);
        check (std::fabs (full - 21.0) < 1.0 && start < 0.2 * full, fmt ("the reduction settles at %.1f dB (the law: 24 x 7/8 = 21) and starts from %.1f", full, start));
        check (atAttack / full > 0.5 && atAttack / full < 0.75, fmt ("ATTACK 20 ms: after 20 ms the reduction has come %.0f%% of the way (a one-pole: 63%%)", 100.0 * atAttack / full));
        const double atRelease = reductionAt (1.5 + 0.2), gone = reductionAt (2.9);
        check (atRelease / full > 0.28 && atRelease / full < 0.48 && gone < 0.05, fmt ("RELEASE 200 ms: 200 ms after the loud tone stops %.0f%% is left (a one-pole: 37%%); after 1.4 s %.2f dB", 100.0 * atRelease / full, gone));
    }

    // [4] Auto release.
    std::printf ("\n[4] Auto release\n");
    {
        EngineParams p;
        p.thresholdDb = -30.0f;
        p.ratio = 8.0f;
        p.kneeDb = 0.0f;
        p.detector = Detector::peak;
        p.attackMs = 5.0f;
        p.autoRelease = true;
        const auto halfTime = [&] (double burstSeconds)
        {
            const int n = 48000 * 4, start = 24000, end = start + (int) (burstSeconds * kFs);
            Stereo in = sine (100.0, -6.0, n);
            for (int i = 0; i < start; ++i) in.l[(size_t) i] = in.r[(size_t) i] = 0.0f;
            for (int i = end; i < n; ++i) in.l[(size_t) i] = in.r[(size_t) i] = 0.0f;
            // After the burst a quiet tone shows the gain.
            const auto quiet = sine (100.0, -20.0, n);
            for (int i = end; i < n; ++i) in.l[(size_t) i] = in.r[(size_t) i] = quiet.l[(size_t) i];
            Engine e;
            e.prepare (kFs);
            const auto out = run (e, in, p, 64);
            const auto reduction = [&] (int at) { return db (peakOf (in.l, (size_t) at, (size_t) at + 480)) - db (peakOf (out.l, (size_t) at, (size_t) at + 480)); };
            const double first = reduction (end + 10);
            for (int t = 10; t < n - end - 500; t += 48)
                if (reduction (end + t) < 0.5 * first)
                    return std::pair<double, double> { 1000.0 * t / kFs, first };
            return std::pair<double, double> { 1e9, first };
        };
        const auto shortBurst = halfTime (0.03), longBurst = halfTime (1.5);
        check (shortBurst.first * 2.0 < longBurst.first, fmt ("AUTO: the reduction halves %.0f ms after a 30 ms burst but %.0f ms after a 1.5 s one: the release follows the programme", shortBurst.first, longBurst.first));
    }

    // [5] Stereo link and sidechain.
    std::printf ("\n[5] Link and sidechain\n");
    {
        EngineParams p;
        p.thresholdDb = -24.0f;
        p.ratio = 4.0f;
        p.kneeDb = 0.0f;
        p.attackMs = 1.0f;
        p.detector = Detector::peak;
        const int n = 48000;
        Stereo in = sine (1000.0, -6.0, n);
        for (auto& v : in.r) v *= 0.05f; // the right is 26 dB under the left: under the threshold
        for (const float link : { 1.0f, 0.0f })
        {
            p.link = link;
            Engine e;
            e.prepare (kFs);
            const auto out = run (e, in, p);
            const double gl = db (peakOf (out.l, n - 4800, n)) - db (peakOf (in.l, n - 4800, n)), gr = db (peakOf (out.r, n - 4800, n)) - db (peakOf (in.r, n - 4800, n));
            if (link == 1.0f)
                check (std::fabs (gl - gr) < 0.01 && gl < -10.0, fmt ("LINK 100%%: the loud left (%.1f dB) and the quiet right (%.1f dB) get the same gain: the image does not move", gl, gr));
            else
                check (gl < -10.0 && std::fabs (gr) < 0.01, fmt ("LINK 0%%: the loud left is turned down by %.1f dB and the quiet right is not (%.2f dB)", -gl, gr));
        }
        // The key.
        p.link = 1.0f;
        p.external = true;
        const auto quiet = sine (300.0, -40.0, n), key = sine (80.0, -6.0, n);
        Engine e;
        e.prepare (kFs);
        const auto out = run (e, quiet, p, 512, &key);
        const double ducked = db (peakOf (out.l, n - 4800, n)) - db (peakOf (quiet.l, n - 4800, n));
        check (ducked < -10.0, fmt ("SIDECHAIN: a quiet tone is turned down by %.1f dB by a loud key it never contained", -ducked));
        Engine f;
        f.prepare (kFs);
        p.external = false;
        const auto own = run (f, quiet, p, 512, &key);
        check (own.l == quiet.l, "with the key not selected, the sidechain input is ignored: the quiet tone is untouched");
        // The detector's high-pass.
        p.external = false;
        const auto bass = sine (50.0, -6.0, n);
        p.hpfHz = 20.0f;
        const double plain = gainOf (p, -6.0, 50.0);
        p.hpfHz = 250.0f;
        const double filtered = gainOf (p, -6.0, 50.0);
        check (plain < -10.0 && filtered > plain + 8.0, fmt ("detector high-pass at 250 Hz: a 50 Hz tone that is turned down %.1f dB without it is turned down %.1f dB with it", -plain, -filtered));
        (void) bass;
    }

    // [6] Makeup and mix.
    std::printf ("\n[6] Makeup and mix\n");
    {
        EngineParams p;
        p.thresholdDb = -24.0f;
        p.ratio = 4.0f;
        p.kneeDb = 0.0f;
        p.attackMs = 1.0f;
        p.detector = Detector::peak;
        p.makeupDb = 6.0f;
        check (std::fabs (gainOf (p, -6.0) + 13.5 - 6.0) < 0.2, fmt ("MAKEUP +6 dB: %.2f dB net at -6 dBFS (law 13.5 down, 6 up)", gainOf (p, -6.0)));
        p.makeupDb = 0.0f;
        p.autoMakeup = true;
        check (std::fabs (gainOf (p, 0.0)) < 0.2, fmt ("AUTO makeup: a full-scale tone comes out at %+.2f dBFS: full scale in, full scale out", gainOf (p, 0.0)));
        p.autoMakeup = false;
        p.mix = 0.5f;
        const double expected = db (0.5 + 0.5 * std::pow (10.0, -13.5 / 20.0));
        check (std::fabs (gainOf (p, -6.0) - expected) < 0.2, fmt ("MIX 50%%: the compressed tone is added to the dry one, %.2f dB (expected %.2f): parallel compression", gainOf (p, -6.0), expected));
    }

    // [7] Detector kinds.
    std::printf ("\n[7] Peak and RMS\n");
    {
        // A click train: very short, very high peaks.
        const int n = 48000;
        Stereo in { std::vector<float> ((size_t) n, 0.0f), std::vector<float> ((size_t) n, 0.0f) };
        for (int i = 0; i < n; i += 4800)
            in.l[(size_t) i] = in.r[(size_t) i] = 0.9f;
        EngineParams p;
        p.thresholdDb = -20.0f;
        p.ratio = 6.0f;
        p.kneeDb = 0.0f;
        p.attackMs = 0.1f;
        p.releaseMs = 100.0f;
        double reduction[2];
        for (const Detector d : { Detector::peak, Detector::rms })
        {
            p.detector = d;
            Engine e;
            e.prepare (kFs);
            auto copy = in;
            run (e, copy, p, 64);
            reduction[(int) d] = e.reductionMeter.load();
        }
        check (reduction[0] > reduction[1] + 4.0, fmt ("a train of clicks: a peak detector turns it down by %.1f dB, an RMS one (10 ms) by %.1f dB", reduction[0], reduction[1]));
    }

    // [8] Block size, bad samples.
    std::printf ("\n[8] Block size and bad samples\n");
    {
        std::mt19937 gen (9);
        std::uniform_real_distribution<float> white (-1.0f, 1.0f);
        Stereo in { std::vector<float> (60000), std::vector<float> (60000) };
        for (size_t i = 0; i < in.l.size(); ++i)
        {
            const float beat = (i / 6000) % 2 == 0 ? 0.8f : 0.1f;
            in.l[i] = beat * white (gen);
            in.r[i] = beat * white (gen);
        }
        EngineParams p;
        p.autoRelease = true;
        p.mix = 0.7f;
        p.link = 0.6f;
        p.hpfHz = 90.0f;
        p.makeupDb = 3.0f;
        Engine a;
        a.prepare (kFs);
        const auto ref = run (a, in, p, 512);
        bool same = true;
        for (const int block : { 1, 7, 64, 333, 4096 })
        {
            Engine b;
            b.prepare (kFs);
            const auto out = run (b, in, p, block);
            same = same && out.l == ref.l && out.r == ref.r;
        }
        check (same, "blocks of 1, 7, 64, 333 and 4096 samples give the output of 512, bit for bit");
        auto bad = in;
        bad.l[2000] = std::nanf ("");
        bad.r[3000] = INFINITY;
        Engine c;
        c.prepare (kFs);
        const auto out = run (c, bad, p);
        bool fine = true;
        for (size_t i = 0; i < out.l.size(); ++i)
            fine = fine && std::isfinite (out.l[i]) && std::isfinite (out.r[i]);
        check (fine, "a NaN and an infinity in the input are silence: the output is finite");
    }

    // [9] Look-ahead.
    std::printf ("\n[9] Look-ahead\n");
    {
        const int index = 3; // 5 ms
        const int delay = (int) std::lround (0.001 * kLookaheadMs[(size_t) index] * kFs);
        EngineParams quiet;
        quiet.thresholdDb = -6.0f;
        quiet.lookahead = index;
        std::mt19937 gen (9);
        std::uniform_real_distribution<float> white (-0.08f, 0.08f);
        Stereo in { std::vector<float> (24000), std::vector<float> (24000) };
        for (auto& v : in.l) v = white (gen);
        for (auto& v : in.r) v = white (gen);
        Engine e;
        e.prepare (kFs, index);
        const auto out = run (e, in, quiet);
        Stereo expected { std::vector<float> (in.l.size(), 0.0f), std::vector<float> (in.l.size(), 0.0f) };
        for (int i = delay; i < (int) in.l.size(); ++i)
        {
            expected.l[(size_t) i] = in.l[(size_t) (i - delay)];
            expected.r[(size_t) i] = in.r[(size_t) (i - delay)];
        }
        const bool exact = out.l == expected.l && out.r == expected.r;
        check (exact && e.latencySamples.load() == delay, fmt ("nothing to compress: the output is the input %d samples later (5 ms), bit for bit, and the engine says so", delay));

        // A step from -40 to 0 dBFS at 20:1 with a 5 ms attack: without look-ahead the first millisecond gets through almost whole; with it the gain has been going down for 5 ms.
        EngineParams p;
        p.thresholdDb = -18.0f;
        p.ratio = 20.0f;
        p.kneeDb = 0.0f;
        p.attackMs = 5.0f;
        p.detector = Detector::peak;
        const int step = 9600;
        auto stepIn = sine (1000.0, -40.0, step);
        const auto loud = sine (1000.0, 0.0, 4800);
        stepIn.l.insert (stepIn.l.end(), loud.l.begin(), loud.l.end());
        stepIn.r = stepIn.l;
        Engine plain, ahead;
        plain.prepare (kFs);
        EngineParams withLookahead = p;
        withLookahead.lookahead = index;
        ahead.prepare (kFs, index);
        const auto a = run (plain, stepIn, p), b = run (ahead, stepIn, withLookahead);
        const double without = db (peakOf (a.l, (size_t) step, (size_t) step + 48)), with = db (peakOf (b.l, (size_t) (step + delay), (size_t) (step + delay + 48)));
        check (with < without - 3.0, fmt ("the first millisecond of a 0 dBFS step: %.1f dBFS without look-ahead, %.1f dBFS with 5 ms (the gain is already down)", without, with));

        // The block size does not change the output, and a new look-ahead moves the latency the engine reports.
        bool same = true;
        for (const int block : { 1, 7, 333, 4096 })
        {
            Engine c;
            c.prepare (kFs, index);
            const auto o = run (c, stepIn, withLookahead, block);
            same = same && o.l == b.l && o.r == b.r;
        }
        check (same, "with look-ahead, blocks of 1, 7, 333 and 4096 samples give the output of 512, bit for bit");
        Engine d;
        d.prepare (kFs);
        EngineParams two = p;
        two.lookahead = 2;
        auto tiny = sine (1000.0, -40.0, 256);
        run (d, tiny, two);
        check (d.latencySamples.load() == (int) std::lround (0.002 * kFs), "changing the look-ahead while running changes the latency the engine reports (2 ms: 96 samples)");
    }

    // [10] LEVEL MATCH, DELTA and SC LISTEN.
    std::printf("\n[10] Level match, delta, sidechain listen\n");
    {
        std::mt19937 gen (31);
        std::uniform_real_distribution<float> white (-0.2f, 0.2f);
        const int n = 48000 * 6;
        Stereo noise { std::vector<float> ((size_t) n), std::vector<float> ((size_t) n) };
        for (auto& v : noise.l) v = white (gen);
        for (auto& v : noise.r) v = white (gen);
        const auto rmsOf = [] (const std::vector<float>& x, size_t from)
        {
            double sum = 0.0;
            for (size_t i = from; i < x.size(); ++i)
                sum += (double) x[i] * x[i];
            return std::sqrt (sum / (double) (x.size() - from));
        };
        const size_t last = (size_t) n - 48000;
        EngineParams p;
        p.thresholdDb = -30.0f;
        p.ratio = 4.0f;
        p.kneeDb = 0.0f;
        Engine plain;
        plain.prepare (kFs);
        const double louder = db (rmsOf (run (plain, noise, p).l, last) / rmsOf (noise.l, last));
        p.match = true;
        Engine matched;
        matched.prepare (kFs);
        const auto m = run (matched, noise, p);
        const double after = db (rmsOf (m.l, last) / rmsOf (noise.l, last));
        check (louder < -3.0 && std::fabs (after) < 0.5 && std::fabs (matched.matchDb.load() + louder) < 0.5,
               fmt ("noise at 4:1 over -30 dB comes out %.2f dB; with LEVEL MATCH it is %+.2f dB after 5 s (applying %+.2f dB)", louder, after, matched.matchDb.load()));
        EngineParams off = p;
        off.match = false;
        Engine offE;
        offE.prepare (kFs);
        run (offE, noise, off);
        check (offE.matchDb.load() == 0.0f, "with LEVEL MATCH off it applies 0.00 dB");
        bool same = true;
        Stereo head { std::vector<float> (noise.l.begin(), noise.l.begin() + 60000), std::vector<float> (noise.r.begin(), noise.r.begin() + 60000) };
        Engine a;
        a.prepare (kFs);
        const auto ref = run (a, head, p, 512);
        for (const int block : { 1, 7, 333, 4096 })
        {
            Engine b;
            b.prepare (kFs);
            const auto o = run (b, head, p, block);
            same = same && o.l == ref.l && o.r == ref.r;
        }
        check (same, "with LEVEL MATCH on, blocks of 1, 7, 333 and 4096 samples give the output of 512, bit for bit");

        // DELTA: output minus the sound that came in (delayed like the output). A +6 dB makeup alone leaves (10^(6/20) - 1) of the sound.
        EngineParams gainOnly;
        gainOnly.thresholdDb = 0.0f;
        gainOnly.makeupDb = 6.0f;
        Engine d;
        d.prepare (kFs);
        d.delta = true;
        const auto t = sine (1000.0, -20.0, n);
        const double got = db (rmsOf (run (d, t, gainOnly).l, last) / rmsOf (t.l, last)), want = db (std::pow (10.0, 6.0 / 20.0) - 1.0);
        check (std::fabs (got - want) < 0.1, fmt ("DELTA of +6 dB makeup is %+.2f dB of the input (the wanted %+.2f)", got, want));
        Engine silent;
        silent.prepare (kFs, 3);
        silent.delta = true;
        EngineParams idle;
        idle.thresholdDb = 0.0f;
        idle.lookahead = 3;
        const auto none = run (silent, noise, idle);
        check (peakOf (none.l, 0, none.l.size()) == 0.0 && peakOf (none.r, 0, none.r.size()) == 0.0, "DELTA with nothing to compress is silence, exactly (with look-ahead too: the input is delayed like the output)");

        // SC LISTEN: the key after the detector's high-pass, not the sound; the key itself when KEY is on.
        const auto levelOf = [&] (double hz, double hpf, bool useKey)
        {
            EngineParams q;
            q.hpfHz = (float) hpf;
            q.external = useKey;
            Engine e;
            e.prepare (kFs);
            e.scListen = true;
            const auto tone = sine (hz, -20.0, 48000), other = sine (useKey ? hz : 200.0, -6.0, 48000);
            const auto out = run (e, useKey ? other : tone, q, 512, useKey ? &tone : nullptr);
            return db (rmsOf (out.l, 24000) / rmsOf (tone.l, 24000));
        };
        const double passes = levelOf (5000.0, 1000.0, false), cut = levelOf (100.0, 1000.0, false), key = levelOf (5000.0, 1000.0, true);
        check (std::fabs (passes) < 0.5 && cut < -30.0 && std::fabs (key) < 0.5, fmt ("SC LISTEN with the high-pass at 1 kHz: 5 kHz passes (%+.2f dB), 100 Hz is %+.1f dB; with KEY on it is the key (%+.2f dB), not the sound", passes, cut, key));
        Engine quietListen;
        quietListen.prepare (kFs);
        const auto o1 = run (quietListen, noise, EngineParams());
        Engine un;
        un.prepare (kFs);
        un.scListen = false;
        check (run (un, noise, EngineParams()).l == o1.l, "with SC LISTEN off the sound is as it was");
    }

    // [11] AUTO THRESH: four seconds of sound, then the THRESH that turns it down by 3.5 dB on average at the RATIO and KNEE set.
    std::printf ("\n[11] AUTO THRESH\n");
    {
        // A listening as the editor starts it: the request, the audio thread, the answer. Returns the answer and whether it is done.
        const auto listenTo = [] (const Stereo& in, const EngineParams& p, int block = 512)
        {
            Engine e;
            e.prepare (kFs);
            e.autoThreshStart();
            run (e, in, p, block);
            return std::make_pair (e.autoThreshAnswer (p.ratio, p.kneeDb), e.autoThreshState() == Engine::AutoState::done);
        };
        // The reduction the compressor really gives: the RMS ratio of input to output over each 10 ms of audible sound (left channel).
        const auto averageReduction = [] (const Stereo& in, const EngineParams& p)
        {
            Engine e;
            e.prepare (kFs);
            const auto out = run (e, in, p);
            const auto rmsOf10ms = [] (const std::vector<float>& x, size_t from)
            {
                double acc = 0.0;
                for (size_t i = from; i < from + 480; ++i)
                    acc += (double) x[i] * (double) x[i];
                return std::sqrt (acc / 480.0);
            };
            double sum = 0.0;
            int count = 0;
            for (size_t s = 0; s + 480 <= in.l.size(); s += 480)
            {
                const double inDb = db (rmsOf10ms (in.l, s));
                if (inDb < -70.0)
                    continue;
                sum += inDb - db (rmsOf10ms (out.l, s));
                ++count;
            }
            return count > 0 ? sum / count : 0.0;
        };

        EngineParams p;
        p.ratio = 4.0f;
        p.kneeDb = 6.0f;
        p.thresholdDb = -18.0f;

        // A steady tone at -20 dB (peak): the reduction is 0.75 x (level - THRESH) past the knee, so 3.5 dB is 4.67 dB over: THRESH -24.7.
        const Stereo steady = sine (1000.0, -20.0, 48000 * 5);
        const auto [steadyAnswer, steadyDone] = listenTo (steady, p);
        check (steadyDone, "a listening of four seconds ends by itself: the state is done after a five-second tone");
        check (steadyAnswer.result == AutoThresh::Result::set, "a steady tone at -20 dB: THRESH is set");
        check (std::fabs (steadyAnswer.thresholdDb - (-24.67)) < 0.3, fmt ("... to %.1f dB (the law: -24.7 for 3.5 dB at 4:1, 6 dB knee)", steadyAnswer.thresholdDb));
        EngineParams set = p;
        set.thresholdDb = steadyAnswer.thresholdDb;
        const double steadyRed = averageReduction (steady, set);
        check (std::fabs (steadyRed - 3.5) < 0.3, fmt ("... and the sound is then turned down %.2f dB on average (aimed at 3.5)", steadyRed));

        // Loud bursts: 0 dB for 100 ms of every second, -30 dB for the other 900 ms. A peak rule would put THRESH near -4.7 dB (3.5 dB under 0 dB);
        // the average over time, as the sound is heard, is turned down 3.5 dB at a THRESH well under that. The answer is corrected for the
        // release tail the listening measured (the follower is not the law), so it is not the law's own -31 dB either.
        Stereo bursty;
        bursty.l.resize (48000 * 5);
        bursty.r = bursty.l;
        for (size_t i = 0; i < bursty.l.size(); ++i)
        {
            const double level = (i % 48000) < 4800 ? 0.0 : -30.0;
            bursty.l[i] = bursty.r[i] = (float) (std::pow (10.0, level / 20.0) * std::sin (2.0 * M_PI * 1000.0 * (double) i / kFs));
        }
        const auto [burstAnswer, burstDone] = listenTo (bursty, p);
        check (burstDone && burstAnswer.result == AutoThresh::Result::set, "loud bursts over a quiet tone: THRESH is set");
        check (burstAnswer.thresholdDb < -10.0f, fmt ("... to %.1f dB: well under the peak rule's -4.7 dB, because the bursts are 10%% of the time", burstAnswer.thresholdDb));
        set.thresholdDb = burstAnswer.thresholdDb;
        const double burstRed = averageReduction (bursty, set);
        check (std::fabs (burstRed - 3.5) < 0.5, fmt ("... and the sound is turned down %.2f dB on average, bursts and release tail included (aimed at 3.5)", burstRed));

        // Its answer does not depend on how the host cuts the sound into blocks.
        const auto [burstTiny, tinyDone] = listenTo (bursty, p, 1);
        check (tinyDone && burstTiny.thresholdDb == burstAnswer.thresholdDb, "blocks of 1 sample give the same THRESH as blocks of 512, bit for bit");

        // Silence, and a tone under the silence gate: nothing audible, THRESH stays where it is.
        const auto [silentAnswer, silentDone] = listenTo (sine (1000.0, -120.0, 48000 * 5), p);
        check (silentDone && silentAnswer.result == AutoThresh::Result::silent, "silence for four seconds: nothing audible, so THRESH is left alone");
        const auto [quietAnswer, quietDone] = listenTo (sine (1000.0, -80.0, 48000 * 5), p);
        check (quietDone && quietAnswer.result == AutoThresh::Result::silent, "a tone at -80 dB (under the -70 dB gate) is silence too");

        // RATIO 1:1 reduces nothing, so there is nothing to set THRESH from.
        EngineParams flat = p;
        flat.ratio = 1.0f;
        const auto [flatAnswer, flatDone] = listenTo (steady, flat);
        check (flatDone && flatAnswer.result == AutoThresh::Result::flat, "RATIO 1:1: nothing to set, THRESH is left alone");

        // A tone that is audible but cannot be reduced by 3.5 dB even at -60 dB THRESH (2:1, no knee: 1 dB at most there).
        EngineParams gentle = p;
        gentle.ratio = 2.0f;
        gentle.kneeDb = 0.0f;
        const auto [quietTone, quietToneDone] = listenTo (sine (1000.0, -58.0, 48000 * 5), gentle);
        check (quietToneDone && quietTone.result == AutoThresh::Result::quiet, "a tone at -58 dB at 2:1 cannot reach 3.5 dB even at -60 dB THRESH: THRESH is left alone");

        // The listening ends after its four seconds whatever it hears: a cancel puts it back to idle.
        Engine idle;
        idle.prepare (kFs);
        idle.autoThreshStart();
        run (idle, sine (1000.0, -20.0, 48000 * 2), p);
        check (idle.autoThreshState() == Engine::AutoState::listening, "two seconds in, the listening is still going");
        idle.autoThreshCancel();
        check (idle.autoThreshState() == Engine::AutoState::idle, "a cancel puts it back to idle");
    }

    return test::finish ("HeraTests");
}
