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

    return test::finish ("HeraTests");
}
