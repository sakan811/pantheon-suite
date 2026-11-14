// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 The Pantheon authors

// Headless tests of the stereo imager: no plugin, no GUI. Every number the README quotes is measured here.

#include "Check.h"
#include "DSP/Engine.h"
#include <random>

using namespace castor;
using test::check;
using test::fmt;

namespace
{
constexpr double kFs = 48000.0;

struct Stereo
{
    std::vector<float> l, r;
};

// A tone that is only in the middle (side 0) or only in the sides (mid 0).
Stereo tone (double hz, int n, bool sideOnly, double amp = 0.25)
{
    Stereo s { std::vector<float> ((size_t) n), std::vector<float> ((size_t) n) };
    for (int i = 0; i < n; ++i)
    {
        const float v = (float) (amp * std::sin (2.0 * M_PI * hz * i / kFs));
        s.l[(size_t) i] = v;
        s.r[(size_t) i] = sideOnly ? -v : v;
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

// The level of the middle (sum) and of the sides (difference) of the last half second, in dBFS.
double midDb (const Stereo& s, size_t from) { double sum = 0.0; for (size_t i = from; i < s.l.size(); ++i) { const double m = 0.5 * (s.l[i] + s.r[i]); sum += m * m; } return 10.0 * std::log10 (std::fmax (sum / (double) (s.l.size() - from), 1e-24)); }
double sideDb (const Stereo& s, size_t from) { double sum = 0.0; for (size_t i = from; i < s.l.size(); ++i) { const double d = 0.5 * (s.l[i] - s.r[i]); sum += d * d; } return 10.0 * std::log10 (std::fmax (sum / (double) (s.l.size() - from), 1e-24)); }

double sideChange (const EngineParams& p, double hz)
{
    Engine e;
    e.prepare (kFs);
    const int n = 48000 * 2;
    const auto in = tone (hz, n, true);
    return sideDb (run (e, in, p), (size_t) n - 24000) - sideDb (in, (size_t) n - 24000);
}
}

int main()
{
    std::printf ("Castor tests (48 kHz)\n");

    // [1] Neutral: the input, bit for bit.
    std::printf ("\n[1] Transparency\n");
    {
        std::mt19937 gen (5);
        std::uniform_real_distribution<float> white (-0.5f, 0.5f);
        Stereo in { std::vector<float> (48000), std::vector<float> (48000) };
        for (auto& v : in.l) v = white (gen);
        for (auto& v : in.r) v = white (gen);
        Engine e;
        e.prepare (kFs);
        const auto out = run (e, in, EngineParams {});
        check (out.l == in.l && out.r == in.r, "every width at 100%, MID and SIDE at 0 dB, nothing else: the input, bit for bit (no delay)");
    }

    // [2] Width.
    std::printf ("\n[2] Width\n");
    {
        EngineParams p;
        p.width.fill (2.0f);
        check (std::fabs (sideChange (p, 1000.0) - 6.02) < 0.01 && std::fabs (sideChange (p, 60.0) - 6.02) < 0.01 && std::fabs (sideChange (p, 8000.0) - 6.02) < 0.01,
               fmt ("all bands 200%%: the sides are +6.02 dB at 60 Hz, 1 kHz and 8 kHz (%.3f, %.3f, %.3f): the bands add up to the side exactly", sideChange (p, 60.0), sideChange (p, 1000.0), sideChange (p, 8000.0)));
        p.width.fill (0.0f);
        check (sideChange (p, 1000.0) < -100.0 && sideChange (p, 100.0) < -100.0, fmt ("all bands 0%%: the sides are gone (%.0f dB): the sound is mono", sideChange (p, 1000.0)));
        EngineParams q;
        q.width[3] = 0.0f; // the Midrange, 500 Hz to 2 kHz
        const double mid = sideChange (q, 1000.0), bass = sideChange (q, 50.0), treble = sideChange (q, 9000.0);
        check (mid < -15.0 && std::fabs (bass) < 0.05 && std::fabs (treble) < 0.05, fmt ("Midrange at 0%%: the sides at 1 kHz are %.1f dB, at 50 Hz %+.3f dB and at 9 kHz %+.3f dB: only its own band", mid, bass, treble));
        EngineParams w;
        w.width[0] = 0.0f;
        w.width[5] = 1.5f;
        check (sideChange (w, 40.0) < -12.0 && std::fabs (sideChange (w, 12000.0) - 3.52) < 0.15, fmt ("Sub at 0%% and High at 150%%: the sides at 40 Hz are %.1f dB and at 12 kHz %+.2f dB (3.52 for 150%%)", sideChange (w, 40.0), sideChange (w, 12000.0)));
    }

    // [3] Mid and side gain.
    std::printf ("\n[3] MID and SIDE\n");
    {
        EngineParams p;
        p.midDb = 6.0f;
        Engine e;
        e.prepare (kFs);
        const int n = 48000 * 2;
        const auto mid = tone (500.0, n, false), side = tone (500.0, n, true);
        const auto om = run (e, mid, p);
        e.reset();
        const auto os = run (e, side, p);
        check (std::fabs (midDb (om, 72000) - midDb (mid, 72000) - 6.0) < 0.05 && std::fabs (sideDb (os, 72000) - sideDb (side, 72000)) < 0.01,
               fmt ("MID +6 dB: a centred sound is +%.2f dB, a side-only sound %+.3f dB", midDb (om, 72000) - midDb (mid, 72000), sideDb (os, 72000) - sideDb (side, 72000)));
        EngineParams q;
        q.sideDb = -6.0f;
        check (std::fabs (sideChange (q, 500.0) + 6.0) < 0.05, fmt ("SIDE -6 dB: the sides are %.2f dB", sideChange (q, 500.0)));
    }

    // [4] Mono bass.
    std::printf ("\n[4] Mono below\n");
    {
        EngineParams p;
        p.monoBelowHz = 120.0f;
        const double at40 = sideChange (p, 40.0), at120 = sideChange (p, 120.0), at480 = sideChange (p, 480.0), at2k = sideChange (p, 2000.0);
        check (at40 < -30.0 && std::fabs (at120 + 3.0) < 0.3 && std::fabs (at480) < 0.1 && std::fabs (at2k) < 0.01,
               fmt ("MONO BELOW 120 Hz (a 24 dB/oct high-pass on the sides): the sides at 40 Hz are %.1f dB, at 120 Hz %.2f dB, at 480 Hz %+.2f dB, at 2 kHz %+.3f dB", at40, at120, at480, at2k));
        Engine e;
        e.prepare (kFs);
        const auto in = tone (60.0, 48000, false);
        const auto out = run (e, in, p);
        check (out.l == in.l && out.r == in.r, "a centred bass (no sides) is untouched: mono bass changes only the sides");
    }

    // [5] Balance, output.
    std::printf ("\n[5] Balance and output\n");
    {
        EngineParams p;
        p.balance = 0.5f;
        Engine e;
        e.prepare (kFs);
        const int n = 48000;
        const auto in = tone (1000.0, n, false);
        const auto out = run (e, in, p);
        double pl = 0, pr = 0, il = 0;
        for (int i = n - 4800; i < n; ++i) { pl = std::fmax (pl, std::fabs (out.l[(size_t) i])); pr = std::fmax (pr, std::fabs (out.r[(size_t) i])); il = std::fmax (il, std::fabs (in.l[(size_t) i])); }
        check (std::fabs (20.0 * std::log10 (pl / il) + 6.02) < 0.05 && std::fabs (pr / il - 1.0) < 1e-4, fmt ("BALANCE +50%%: the left is %.2f dB, the right is untouched", 20.0 * std::log10 (pl / il)));
        EngineParams o;
        o.outDb = 6.0f;
        Engine f;
        f.prepare (kFs);
        const auto out2 = run (f, in, o);
        double po = 0;
        for (int i = n - 4800; i < n; ++i) po = std::fmax (po, std::fabs (out2.l[(size_t) i]));
        check (std::fabs (20.0 * std::log10 (po / il) - 6.02) < 0.05, "OUTPUT +6 dB is +6.02 dB");
    }

    // [6] Correlation.
    std::printf ("\n[6] Correlation\n");
    {
        EngineParams p;
        const auto corr = [&] (const Stereo& in) { Engine e; e.prepare (kFs); run (e, in, p); return e.correlation.load(); };
        const int n = 48000 * 2;
        std::mt19937 gen (3);
        std::uniform_real_distribution<float> white (-0.5f, 0.5f);
        Stereo noise { std::vector<float> ((size_t) n), std::vector<float> ((size_t) n) };
        for (auto& v : noise.l) v = white (gen);
        for (auto& v : noise.r) v = white (gen);
        const double mono = corr (tone (300.0, n, false)), anti = corr (tone (300.0, n, true)), wide = corr (noise);
        check (mono > 0.999 && anti < -0.999 && std::fabs (wide) < 0.1, fmt ("the correlation meter reads %+.3f for a centred sound, %+.3f for an out-of-phase one and %+.3f for independent noise", mono, anti, wide));
        p.width.fill (0.0f);
        check (corr (noise) > 0.999, "with every width at 0% the output reads +1.000: mono");
    }

    // [7] Block size and bad samples.
    std::printf ("\n[7] Block size and bad samples\n");
    {
        std::mt19937 gen (9);
        std::uniform_real_distribution<float> white (-0.5f, 0.5f);
        Stereo in { std::vector<float> (60000), std::vector<float> (60000) };
        for (auto& v : in.l) v = white (gen);
        for (auto& v : in.r) v = white (gen);
        EngineParams p;
        p.width = { 0.0f, 0.5f, 1.5f, 2.0f, 1.0f, 1.2f };
        p.midDb = 1.5f; p.sideDb = -2.0f; p.monoBelowHz = 150.0f; p.balance = -0.2f; p.outDb = -1.0f;
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

    // [8] The listening checks and the correlation minimum.
    std::printf ("\n[8] Listen and correlation minimum\n");
    {
        Stereo s;
        s.l = { 0.5f, -0.25f, 0.1f };
        s.r = { 0.25f, 0.25f, -0.3f };
        auto m = s, d = s, st = s;
        applyListen (m.l.data(), m.r.data(), 3, Listen::mono);
        applyListen (d.l.data(), d.r.data(), 3, Listen::side);
        applyListen (st.l.data(), st.r.data(), 3, Listen::stereo);
        bool ok = st.l == s.l && st.r == s.r;
        for (size_t i = 0; i < 3; ++i)
            ok = ok && m.l[i] == 0.5f * (s.l[i] + s.r[i]) && m.r[i] == m.l[i] && d.l[i] == 0.5f * (s.l[i] - s.r[i]) && d.r[i] == d.l[i];
        check (ok, "LISTEN: Mono is (L + R) / 2 in both channels, Side is (L - R) / 2 in both, Stereo touches nothing");
        const auto side = tone (700.0, 48000, true);
        auto sm = side;
        applyListen (sm.l.data(), sm.r.data(), (int) sm.l.size(), Listen::mono);
        double peak = 0.0;
        for (const float v : sm.l)
            peak = std::fmax (peak, std::fabs (v));
        check (peak < 1e-6, "a side-only tone is silent in the Mono check (what is out of phase vanishes)");

        // The minimum: a mono passage, then an out-of-phase one, then mono again; the minimum keeps the dip the live reading has left.
        Engine e;
        e.prepare (kFs);
        EngineParams p;
        run (e, tone (500.0, 24000, false), p);
        run (e, tone (500.0, 24000, true), p);
        const float dip = e.correlationMin.load();
        run (e, tone (500.0, 48000, false), p);
        check (dip < -0.5f && e.correlation.load() > 0.9f && e.correlationMin.load() <= dip + 1e-6f, fmt ("the correlation minimum keeps the dip (%.2f) after the live reading is back at %.2f", dip, e.correlation.load()));
        e.correlationMin.store (1.0f);
        run (e, tone (500.0, 4800, false), p);
        check (e.correlationMin.load() > 0.9f, "setting it back to 1 starts the minimum again");
    }

    return test::finish ("CastorTests");
}
