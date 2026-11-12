// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 The Pantheon authors

// Headless tests of the equaliser: no plugin, no GUI. Every number the README quotes is measured here.

#include "Check.h"
#include "DSP/Engine.h"
#include <random>

using namespace apollo;
using test::check;
using test::fmt;

namespace
{
constexpr double kFs = 48000.0;

struct Stereo
{
    std::vector<float> l, r;
};

Stereo tone (double hz, int n, double amp = 0.25, double sideShare = 0.0, bool onlyLeft = false)
{
    Stereo s { std::vector<float> ((size_t) n), std::vector<float> ((size_t) n) };
    for (int i = 0; i < n; ++i)
    {
        const float v = (float) (amp * std::sin (2.0 * M_PI * hz * i / kFs));
        s.l[(size_t) i] = v * (float) (1.0 + sideShare);
        s.r[(size_t) i] = onlyLeft ? 0.0f : v * (float) (1.0 - sideShare);
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

double rms (const std::vector<float>& x, size_t from)
{
    double sum = 0.0;
    for (size_t i = from; i < x.size(); ++i)
        sum += (double) x[i] * x[i];
    return std::sqrt (sum / (double) (x.size() - from));
}

double db (double lin) { return 20.0 * std::log10 (std::fmax (lin, 1e-12)); }

// The gain a tone gets, in dB, on the left channel, measured on the last half second.
double gainAt (const EngineParams& p, double hz, int look = 0)
{
    Engine e;
    e.prepare (kFs, p.phase);
    const int n = 48000 * 2;
    const auto in = tone (hz, n);
    const auto out = run (e, in, p);
    (void) look;
    return db (rms (out.l, (size_t) n - 24000) / rms (in.l, (size_t) n - 24000));
}

EngineParams one (Type type, double hz, double gainDb, double q = 1.0, Phase phase = Phase::minimum, Target target = Target::stereo, int slope = 0)
{
    EngineParams p;
    p.phase = phase;
    p.bands[0] = { true, type, hz, gainDb, q, slope, target };
    return p;
}
}

int main()
{
    std::printf ("Apollo tests (48 kHz)\n");

    // [1] Every type does what its response says, in the filter that runs.
    std::printf("\n[1] The bands\n");
    {
        double worst = 0.0;
        struct Case { Type type; double hz, gain, q; int slope; const char* name; };
        const Case cases[] = { { Type::bell, 1000.0, 9.0, 1.0, 0, "bell +9 dB" }, { Type::bell, 250.0, -12.0, 3.0, 0, "bell -12 dB, Q 3" },
                               { Type::lowShelf, 120.0, 6.0, 0.707, 0, "low shelf +6" }, { Type::highShelf, 8000.0, -6.0, 0.707, 0, "high shelf -6" },
                               { Type::highPass, 80.0, 0.0, 0.707, 1, "high-pass 24 dB/oct" }, { Type::lowPass, 12000.0, 0.0, 0.707, 3, "low-pass 48 dB/oct" },
                               { Type::notch, 3000.0, 0.0, 8.0, 0, "notch" } };
        for (const auto& c : cases)
        {
            const auto p = one (c.type, c.hz, c.gain, c.q, Phase::minimum, Target::stereo, c.slope);
            double caseWorst = 0.0;
            for (const double hz : { 60.0, 150.0, 400.0, 1000.0, 2500.0, 7000.0, 11000.0 })
                caseWorst = std::fmax (caseWorst, std::fabs (gainAt (p, hz) - responseDb (p.bands, kFs, hz)));
            worst = std::fmax (worst, caseWorst);
            check (caseWorst < 0.05, fmt ("%s: the tone's gain is the response's, within %.3f dB at seven frequencies (at its own: %+.2f dB)", c.name, caseWorst, responseDb (p.bands, kFs, c.hz)));
        }
        const auto bell = one (Type::bell, 1000.0, 9.0);
        check (std::fabs (gainAt (bell, 1000.0) - 9.0) < 0.05 && std::fabs (gainAt (bell, 100.0)) < 0.5, "a +9 dB bell at 1 kHz: +9.00 dB there, under 0.5 dB at 100 Hz");
        const auto hp = one (Type::highPass, 100.0, 0.0, 0.707, Phase::minimum, Target::stereo, 1);
        check (std::fabs (gainAt (hp, 100.0) + 3.0) < 0.1 && std::fabs (gainAt (hp, 50.0) + 24.1) < 1.5, "a 24 dB/oct high-pass at 100 Hz: -3 dB at its corner, about -24 dB an octave below");
    }

    // [2] Nothing to do: the input, bit for bit.
    std::printf("\n[2] Transparency\n");
    {
        std::mt19937 gen (5);
        std::uniform_real_distribution<float> white (-0.5f, 0.5f);
        Stereo in { std::vector<float> (48000), std::vector<float> (48000) };
        for (auto& v : in.l) v = white (gen);
        for (auto& v : in.r) v = white (gen);
        EngineParams off;
        for (auto& b : off.bands) { b.on = true; b.gainDb = 0.0; }  // every band on, at 0 dB
        for (const Phase phase : { Phase::minimum, Phase::linear })
        {
            off.phase = phase;
            Engine e;
            e.prepare (kFs, phase);
            const auto out = run (e, in, off);
            const size_t latency = (size_t) e.latencySamples.load();
            bool same = true;
            for (size_t i = latency; i < in.l.size(); ++i)
                same = same && out.l[i] == in.l[i - latency] && out.r[i] == in.r[i - latency];
            check (same, fmt ("%s: eight bells at 0 dB are the input%s, bit for bit", phase == Phase::minimum ? "minimum phase" : "linear phase",
                              latency == 0 ? "" : fmt (" delayed by %d samples", (int) latency).c_str()));
        }
    }

    // [3] Linear phase: the same curve, no phase shift.
    std::printf("\n[3] Linear phase\n");
    {
        EngineParams p;
        p.bands[0] = { true, Type::bell, 1000.0, 9.0, 1.0, 0, Target::stereo };
        p.bands[1] = { true, Type::lowShelf, 100.0, -6.0, 0.707, 0, Target::stereo };
        p.bands[2] = { true, Type::highPass, 40.0, 0.0, 0.707, 1, Target::stereo };
        p.bands[3] = { true, Type::bell, 6000.0, -8.0, 2.0, 0, Target::stereo };
        auto lin = p;
        lin.phase = Phase::linear;
        double worst = 0.0;
        for (const double hz : { 100.0, 300.0, 800.0, 1000.0, 1300.0, 3000.0, 6000.0, 12000.0 })
            worst = std::fmax (worst, std::fabs (gainAt (lin, hz) - gainAt (p, hz)));
        check (worst < 0.1, fmt ("the linear-phase curve is the minimum-phase curve: the same on eight frequencies from 100 Hz to 12 kHz within %.3f dB", worst));
        const double low = std::fabs (gainAt (lin, 50.0) - gainAt (p, 50.0));
        check (low < 1.0, fmt ("... but at 50 Hz, with a 24 dB/oct high-pass at 40 Hz in it, it is %.2f dB off: the kernel resolves about 12 Hz (a limit, see the README)", low));

        // An impulse comes out symmetric around the latency: no phase shift at all.
        Engine e;
        e.prepare (kFs, Phase::linear);
        std::vector<float> x (16384, 0.0f), y = x;
        x[2000] = 1.0f;
        Stereo s { x, x };
        const auto out = run (e, s, lin);
        const int latency = e.latencySamples.load();
        const int centre = 2000 + latency; // the impulse, heard `latency` later
        double asym = 0.0, peak = 0.0;
        for (int k = 1; k < 1500; ++k)
        {
            asym = std::fmax (asym, std::fabs ((double) out.l[(size_t) (centre + k)] - (double) out.l[(size_t) (centre - k)]));
            peak = std::fmax (peak, std::fabs ((double) out.l[(size_t) (centre + k)]));
        }
        check (asym < 5e-5, fmt ("an impulse comes out symmetric around its centre (largest difference %.1e): linear phase, no phase shift", asym));
        check (latency == linearSizesFor (kFs).latency() && latency == 2304, fmt ("latency, as reported to the host: %d samples (%.1f ms): the kernel's %d plus the hop of %d", latency, 1000.0 * latency / kFs, linearSizesFor (kFs).half(), linearSizesFor (kFs).hop));
        (void) peak;
    }

    // [4] Mid and side.
    std::printf("\n[4] Mid / Side\n");
    {
        const int n = 48000 * 2;
        for (const Phase phase : { Phase::minimum, Phase::linear })
        {
            const char* name = phase == Phase::minimum ? "minimum phase" : "linear phase";
            Stereo mid = tone (1000.0, n), side;
            side.l = mid.l;
            side.r.resize (mid.l.size());
            for (size_t i = 0; i < mid.l.size(); ++i)
                side.r[i] = -mid.l[i]; // left and right opposite: only side
            const auto asMid = one (Type::bell, 1000.0, 9.0, 1.0, phase, Target::mid), asSide = one (Type::bell, 1000.0, 9.0, 1.0, phase, Target::side);
            const auto gainOf = [&] (const Stereo& in, const EngineParams& p)
            {
                Engine e;
                e.prepare (kFs, p.phase);
                const auto out = run (e, in, p);
                return db (rms (out.l, (size_t) n - 24000) / rms (in.l, (size_t) n - 24000));
            };
            check (std::fabs (gainOf (mid, asMid) - 9.0) < 0.1 && std::fabs (gainOf (side, asMid)) < 0.1, fmt ("%s: a MID bell lifts a centred sound by 9 dB and leaves a side-only sound alone", name));
            check (std::fabs (gainOf (side, asSide) - 9.0) < 0.1 && std::fabs (gainOf (mid, asSide)) < 0.1, fmt ("%s: a SIDE bell lifts a side-only sound by 9 dB and leaves a centred one alone", name));
            const auto stereo = one (Type::bell, 1000.0, 9.0, 1.0, phase, Target::stereo);
            Stereo leftOnly = tone (1000.0, n, 0.25, 0.0, true);
            check (std::fabs (gainOf (leftOnly, stereo) - 9.0) < 0.1, fmt ("%s: a STEREO bell lifts a left-only sound by 9 dB too (left and right are treated alike)", name));
        }
    }

    // [5] Block size.
    std::printf("\n[5] Block size\n");
    {
        std::mt19937 gen (9);
        std::uniform_real_distribution<float> white (-0.5f, 0.5f);
        Stereo in { std::vector<float> (60000), std::vector<float> (60000) };
        for (auto& v : in.l) v = white (gen);
        for (auto& v : in.r) v = white (gen);
        for (const Phase phase : { Phase::minimum, Phase::linear })
        {
            auto p = one (Type::bell, 700.0, 7.0, 1.4, phase, Target::stereo);
            p.bands[1] = { true, Type::highShelf, 5000.0, -4.0, 0.707, 0, Target::mid };
            p.outDb = -2.0;
            Engine a;
            a.prepare (kFs, phase);
            const auto ref = run (a, in, p, 512);
            bool same = true;
            for (const int block : { 1, 7, 64, 333, 4096 })
            {
                Engine b;
                b.prepare (kFs, phase);
                const auto out = run (b, in, p, block);
                same = same && out.l == ref.l && out.r == ref.r;
            }
            check (same, fmt ("%s: blocks of 1, 7, 64, 333 and 4096 samples give the output of 512, bit for bit", phase == Phase::minimum ? "minimum phase" : "linear phase"));
        }
    }

    // [6] Changes do not click.
    std::printf("\n[6] Smoothing\n");
    {
        const int n = 48000 * 2;
        auto in = tone (1000.0, n, 0.25);
        const auto jump = [&] (const Stereo& s, size_t from)
        {
            double worst = 0.0;
            for (size_t i = from + 1; i < s.l.size(); ++i)
                worst = std::fmax (worst, std::fabs ((double) s.l[i] - (double) s.l[i - 1]));
            return worst;
        };
        EngineParams on = one (Type::bell, 1000.0, 12.0), off = on;
        off.bands[0].on = false;
        Engine e;
        e.prepare (kFs, Phase::minimum);
        auto head = Stereo { std::vector<float> (in.l.begin(), in.l.begin() + 48000), std::vector<float> (in.r.begin(), in.r.begin() + 48000) };
        auto tail = Stereo { std::vector<float> (in.l.begin() + 48000, in.l.end()), std::vector<float> (in.r.begin() + 48000, in.r.end()) };
        auto a = run (e, head, on);
        auto b = run (e, tail, off);
        const double steady = jump (a, 40000), atSwitch = jump (b, 0);
        check (atSwitch < 1.3 * steady, fmt ("a +12 dB bell switched off while a 1 kHz tone plays: the largest step is %.4f (a steady tone's is %.4f x 4 = %.4f): it fades, there is no click", atSwitch, steady / 4.0, steady));
        const double tailGain = db (rms (b.l, 40000) / rms (tone (1000.0, 8000).l, 0));
        check (std::fabs (tailGain) < 0.05, fmt ("... and after the fade the tone is back at its own level (%+.3f dB)", tailGain));
    }

    // [7] Bad input, and the output gain.
    std::printf("\n[7] Bad samples, output gain\n");
    {
        EngineParams p = one (Type::bell, 1000.0, 6.0);
        Engine e;
        e.prepare (kFs, Phase::minimum);
        auto in = tone (300.0, 24000);
        in.l[3000] = std::nanf ("");
        in.r[4000] = INFINITY;
        const auto out = run (e, in, p);
        bool fine = true;
        for (size_t i = 0; i < out.l.size(); ++i)
            fine = fine && std::isfinite (out.l[i]) && std::isfinite (out.r[i]);
        check (fine, "a NaN and an infinity in the input are silence: the output is finite");
        EngineParams g;
        g.outDb = 6.0;
        check (std::fabs (gainAt (g, 440.0) - 6.0) < 0.02, "OUTPUT +6 dB with every band off is +6.00 dB");
    }

    // [8] Phase mode switch, and the latency it asks the host for.
    std::printf("\n[8] Latency\n");
    {
        Engine e;
        e.prepare (kFs, Phase::minimum);
        check (e.latencySamples.load() == 0, "minimum phase has no latency");
        EngineParams p = one (Type::bell, 1000.0, 3.0, 1.0, Phase::linear);
        Stereo s = tone (440.0, 4096);
        run (e, s, p);
        check (e.latencySamples.load() == linearSizesFor (kFs).latency(), "switching to linear phase moves the latency to the kernel's delay plus the hop");
        for (const double fs : { 44100.0, 48000.0, 88200.0, 96000.0, 192000.0 })
        {
            const auto sz = linearSizesFor (fs);
            check (fs / sz.frame <= 12.1, fmt ("%.0f Hz: frame %d, hop %d: %.1f Hz a bin, latency %.1f ms", fs, sz.frame, sz.hop, fs / sz.frame, 1000.0 * sz.latency() / fs));
        }
    }

    return test::finish ("ApolloTests");
}
