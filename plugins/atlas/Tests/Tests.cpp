// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 The Pantheon authors

// DSP tests for Atlas: no plugin, no GUI. Every number the README quotes is printed here.

#include "Check.h"
#include "DSP/Engine.h"
#include <pantheon/dsp/SpectrumIssues.h>
#include <random>

using namespace atlas;
using test::check;
using test::fmt;

namespace
{
constexpr double fs = 48000.0;
constexpr double pi = 3.14159265358979323846;
struct Stereo { std::vector<float> l, r; };

std::vector<float> sine (double freq, double amp, int n)
{
    std::vector<float> x ((size_t) n);
    for (int i = 0; i < n; ++i)
        x[(size_t) i] = (float) (amp * std::sin (2.0 * pi * freq * i / fs));
    return x;
}

std::vector<float> noise (unsigned seed, double amp, int n)
{
    std::mt19937 gen (seed);
    std::uniform_real_distribution<float> d (-1.0f, 1.0f);
    std::vector<float> x ((size_t) n);
    for (auto& v : x) v = (float) amp * d (gen);
    return x;
}

double toDb (double ratio) { return 20.0 * std::log10 (std::max (ratio, 1.0e-12)); }

// Amplitude of the component at `freq` over a window (a Hann-weighted correlation).
double amplitudeAt (const std::vector<float>& x, double freq, int from, int n)
{
    double re = 0.0, im = 0.0, sum = 0.0;
    for (int i = 0; i < n; ++i)
    {
        const double w = 0.5 - 0.5 * std::cos (2.0 * pi * (i + 0.5) / n), ph = 2.0 * pi * freq * (from + i) / fs;
        re += w * x[(size_t) (from + i)] * std::cos (ph);
        im += w * x[(size_t) (from + i)] * std::sin (ph);
        sum += w;
    }
    return 2.0 * std::sqrt (re * re + im * im) / sum;
}

double rms (const std::vector<float>& x, int from, int n)
{
    double s = 0.0;
    for (int i = 0; i < n; ++i) s += (double) x[(size_t) (from + i)] * x[(size_t) (from + i)];
    return std::sqrt (s / n);
}

double maxDiff (const std::vector<float>& a, const std::vector<float>& b, int from, int n, int delay = 0)
{
    double m = 0.0;
    for (int i = from; i < from + n; ++i)
        m = std::max (m, (double) std::fabs (a[(size_t) (i + delay)] - b[(size_t) i]));
    return m;
}

Stereo run (Engine& e, const EngineParams& p, const std::vector<float>& l, const std::vector<float>& r, int block = 512)
{
    Stereo out { l, r };
    for (int start = 0; start < (int) l.size(); start += block)
        e.process (out.l.data() + start, out.r.data() + start, std::min (block, (int) l.size() - start), p);
    return out;
}

Stereo run (const EngineParams& p, const std::vector<float>& l, const std::vector<float>& r, int block = 512)
{
    Engine e;
    e.prepare (fs, p.phase, p.oversampling);
    return run (e, p, l, r, block);
}

// The same, with only the bands in `mask` heard: a band's own gain, without what its neighbours carry of the same tone.
Stereo runSolo (const EngineParams& p, unsigned mask, const std::vector<float>& l, const std::vector<float>& r)
{
    Engine e;
    e.prepare (fs, p.phase, p.oversampling);
    e.monitor.solo = mask;
    return run (e, p, l, r);
}

EngineParams allOff()
{
    EngineParams p;
    for (auto& b : p.bands) b.on = false;
    return p;
}

// Band 3 (Midrange, 500 Hz to 2 kHz) set to compress hard and fast; the rest off.
EngineParams midrangeOnly (double thresholdDb = -30.0, double ratio = 4.0)
{
    auto p = allOff();
    p.bands[3] = { true, Channels::stereo, thresholdDb, ratio, 1.0, 50.0, 0.0, false };
    return p;
}

const int second = (int) fs;

// ----------------------------------------------------------------------------------------------
void testSplit()
{
    std::printf ("\n[1] The split: six bands that add back to the input\n");
    const auto l = noise (1, 0.5, second), r = noise (2, 0.5, second);

    // Zero latency: the bands add to an all-pass of the input (same magnitude at every frequency).
    auto p = allOff();
    double worst = 0.0;
    for (double f : { 30.0, 60.0, 125.0, 250.0, 500.0, 1000.0, 2000.0, 6000.0, 12000.0 })
    {
        const auto in = sine (f, 0.5, second);
        const auto out = run (p, in, in);
        worst = std::max (worst, std::fabs (toDb (amplitudeAt (out.l, f, second / 2, second / 4) / 0.5)));
    }
    check (worst < 0.01, fmt ("zero latency, every band off: flat within %.4f dB from 30 Hz to 12 kHz (crossovers included)", worst));

    // Linear: the bands add to the input, delayed, and nothing else.
    p.phase = PhaseMode::linear;
    Engine e;
    e.prepare (fs, p.phase, 0);
    const int latency = e.latencySamples.load();
    const auto out = run (e, p, l, r);
    const double err = std::max (maxDiff (out.l, l, 0, second - latency, latency), maxDiff (out.r, r, 0, second - latency, latency));
    check (latency == LinearSplit::latencyFor (fs) && err < 1.0e-5,
           fmt ("linear, every band off: the input %d samples (%.0f ms) later, within %.1f dB", latency, 1000.0 * latency / fs, toDb (err / 0.5)));

    // The two modes cut the spectrum at the same places: solo a band, measure a tone inside it and one in the next band.
    for (auto phase : { PhaseMode::zeroLatency, PhaseMode::linear })
    {
        auto q = allOff();
        q.phase = phase;
        const auto level = [&] (double f)
        {
            Engine s;
            s.prepare (fs, phase, 0);
            s.monitor.solo = 1u << 3;
            const auto in = sine (f, 0.5, second);
            const auto o = run (s, q, in, in);
            return toDb (amplitudeAt (o.l, f, second / 2, second / 4) / 0.5);
        };
        const double inside = level (1000.0), edge = level (2000.0), outside = level (8000.0), below = level (125.0);
        check (std::fabs (inside + 1.08) < 0.2 && std::fabs (edge + 6.0) < 0.3 && outside < -40.0 && below < -40.0,
               fmt ("%s, Midrange soloed: 1 kHz %+.2f dB, the 2 kHz edge %+.2f dB, 8 kHz %+.1f dB, 125 Hz %+.1f dB",
                    phase == PhaseMode::linear ? "linear" : "zero latency", inside, edge, outside, below));
    }

    // A linear band is delayed and not turned: a click through a soloed band stays symmetric around its peak.
    {
        auto q = allOff();
        q.phase = PhaseMode::linear;
        Engine s;
        s.prepare (fs, q.phase, 0);
        s.monitor.solo = 1u << 1;
        std::vector<float> click ((size_t) second, 0.0f);
        click[1000] = 1.0f;
        const auto o = run (s, q, click, click);
        const int centre = 1000 + latency;
        double asym = 0.0, peak = 0.0;
        for (int k = 1; k < 2000; ++k)
        {
            asym = std::max (asym, (double) std::fabs (o.l[(size_t) (centre + k)] - o.l[(size_t) (centre - k)]));
            peak = std::max (peak, (double) std::fabs (o.l[(size_t) (centre + k)]));
        }
        check (asym < 1.0e-5 && peak > 1.0e-3, fmt ("linear, Bass soloed: a click comes out symmetric around its centre (pre and post differ by %.1e)", asym));
    }
}

void testCompression()
{
    std::printf ("\n[2] The compressor\n");
    check (std::fabs (reductionDb (-40.0, -30.0, 0.75)) < 1.0e-12 && std::fabs (reductionDb (-10.0, -30.0, 0.75) - 15.0) < 1.0e-12
               && std::fabs (reductionDb (-30.0, -30.0, 0.75) - 0.75 * 9.0 / 12.0) < 1.0e-12,
           fmt ("gain computer at 4:1: 0 dB under the knee, %.2f dB at the threshold (the knee), 15 dB at 20 dB over", reductionDb (-30.0, -30.0, 0.75)));

    const auto gainAt = [] (const EngineParams& p, double f, double ampDb)
    {
        const double amp = std::pow (10.0, ampDb / 20.0);
        const auto in = sine (f, amp, second);
        const auto out = run (p, in, in);
        return toDb (amplitudeAt (out.l, f, second / 2, second / 4) / amp);
    };
    auto p = midrangeOnly (-30.0, 4.0);
    // The band's own gain is read with the band soloed: the whole output also has what the neighbouring bands carry of the tone.
    const auto tone = sine (1000.0, std::pow (10.0, -10.0 / 20.0), second);
    const auto bandIn = runSolo (allOff(), 1u << 3, tone, tone), bandOut = runSolo (p, 1u << 3, tone, tone);
    const double levelDb = toDb (amplitudeAt (bandIn.l, 1000.0, second / 2, second / 4));
    const double taken = toDb (amplitudeAt (bandOut.l, 1000.0, second / 2, second / 4)) - levelDb, law = -0.75 * (levelDb + 30.0);
    const double quiet = gainAt (p, 1000.0, -45.0), whole = gainAt (p, 1000.0, -10.0), other = gainAt (p, 100.0, -10.0);
    check (std::fabs (quiet) < 0.05 && std::fabs (taken - law) < 0.3 && std::fabs (other) < 0.05,
           fmt ("Midrange at -30 dB, 4:1: a 1 kHz tone at -45 dB passes at %+.3f dB; at -10 dB it is %.2f dB in the band, which is taken down %+.2f dB (the law gives %+.2f); 100 Hz at -10 dB %+.3f dB",
                quiet, levelDb, taken, law, other));
    check (whole > taken + 1.0, fmt ("... and the whole output falls %+.2f dB: the neighbouring bands carry some of the tone, as in any multiband compressor", whole));

    // Attack and release: the times asked for.
    p = midrangeOnly (-30.0, 20.0);
    p.bands[3].attackMs = 100.0;
    p.bands[3].releaseMs = 400.0;
    Engine e;
    e.prepare (fs);
    e.monitor.solo = 1u << 3;
    auto in = sine (1000.0, 0.001, 3 * second);
    const auto loudTone = sine (1000.0, std::pow (10.0, -10.0 / 20.0), 3 * second);
    for (int i = second / 2; i < 2 * second; ++i) in[(size_t) i] = loudTone[(size_t) i];
    const auto out = run (e, p, in, in);
    const auto dry = runSolo (allOff(), 1u << 3, in, in);
    const auto g = [&] (double t) { const int a = (int) (t * fs); return toDb (amplitudeAt (out.l, 1000.0, a, 960) / amplitudeAt (dry.l, 1000.0, a, 960)); };
    const double full = -0.95 * (20.0 + levelDb + 10.0);
    const double wantEarly = full * (1.0 - std::exp (-0.015 / 0.1)), wantLate = full * std::exp (-0.9 / 0.4);
    check (std::fabs (g (0.51) - wantEarly) < 1.0 && std::fabs (g (1.9) - full) < 0.5 && g (2.03) < 0.8 * full && std::fabs (g (2.9) - wantLate) < 0.7,
           fmt ("attack 100 ms, release 400 ms (band soloed): %+.1f dB after 15 ms (an exponential gives %+.1f), %+.1f dB settled (%+.1f), %+.1f dB 30 ms after the tone stops, %+.2f dB 900 ms after (%+.2f)",
                g (0.51), wantEarly, g (1.9), full, g (2.03), g (2.9), wantLate));

    // A band that is off is its input: with every band off but one below its threshold, the output is the all-off output bit for bit.
    const auto l = noise (3, 0.01, second), r = noise (4, 0.01, second);
    const auto a = run (allOff(), l, r), b = run (midrangeOnly (-20.0, 4.0), l, r);
    check (a.l == b.l && a.r == b.r, "a band on but under its threshold changes nothing, bit for bit");
}

void testChannels()
{
    std::printf ("\n[3] Stereo, L/R, M/S and one signal only\n");
    // Left: a loud 1 kHz tone. Right: the same tone, quiet (under the threshold).
    const double loud = std::pow (10.0, -10.0 / 20.0), quiet = std::pow (10.0, -45.0 / 20.0);
    const auto l = sine (1000.0, loud, second), r = sine (1000.0, quiet, second);
    const auto gains = [&] (Channels c, const std::vector<float>& a, const std::vector<float>& b)
    {
        auto p = midrangeOnly (-30.0, 4.0);
        p.bands[3].channels = c;
        p.bands[3].releaseMs = 2000.0;
        const auto ref = runSolo (allOff(), 1u << 3, a, b);
        const auto out = runSolo (p, 1u << 3, a, b);
        const int from = second / 2, n = second / 4;
        return std::pair { toDb (amplitudeAt (out.l, 1000.0, from, n) / amplitudeAt (ref.l, 1000.0, from, n)),
                           toDb (amplitudeAt (out.r, 1000.0, from, n) / std::max (amplitudeAt (ref.r, 1000.0, from, n), 1.0e-9)) };
    };
    const double want = -0.75 * (-10.0 - 1.08 + 30.0); // the tone is 1.08 dB down in the band (test [2])
    const auto st = gains (Channels::stereo, l, r), dual = gains (Channels::dual, l, r), left = gains (Channels::left, l, r), right = gains (Channels::right, l, r);
    check (std::fabs (st.first - want) < 0.5 && std::fabs (st.first - st.second) < 0.01, fmt ("Stereo: one gain for both, %+.2f / %+.2f dB (the image does not move)", st.first, st.second));
    check (std::fabs (dual.first - want) < 0.5 && std::fabs (dual.second) < 0.05, fmt ("L/R: each side its own gain, %+.2f / %+.3f dB", dual.first, dual.second));
    check (std::fabs (left.first - want) < 0.5 && std::fabs (left.second) < 1.0e-6 && std::fabs (right.first) < 1.0e-6,
           fmt ("Left: %+.2f / %+.6f dB; Right (quiet there): %+.6f dB on the left", left.first, left.second, right.first));

    // A loud centre and a quiet side: Mid compresses the centre and leaves the side alone, bit for bit; Side the reverse.
    const auto centre = sine (1000.0, loud, second), side = sine (1500.0, quiet, second);
    std::vector<float> ml (centre), mr (centre);
    for (int i = 0; i < second; ++i) { ml[(size_t) i] += side[(size_t) i]; mr[(size_t) i] -= side[(size_t) i]; }
    const auto measure = [&] (Channels c)
    {
        auto p = midrangeOnly (-30.0, 4.0);
        p.bands[3].channels = c;
        p.bands[3].releaseMs = 2000.0;
        const auto ref = runSolo (allOff(), 1u << 3, ml, mr);
        const auto out = runSolo (p, 1u << 3, ml, mr);
        std::vector<float> m ((size_t) second), s ((size_t) second), m0 ((size_t) second), s0 ((size_t) second);
        for (int i = 0; i < second; ++i)
        {
            m[(size_t) i] = 0.5f * (out.l[(size_t) i] + out.r[(size_t) i]);  s[(size_t) i] = 0.5f * (out.l[(size_t) i] - out.r[(size_t) i]);
            m0[(size_t) i] = 0.5f * (ref.l[(size_t) i] + ref.r[(size_t) i]); s0[(size_t) i] = 0.5f * (ref.l[(size_t) i] - ref.r[(size_t) i]);
        }
        const int from = second / 2, n = second / 4;
        return std::pair { toDb (amplitudeAt (m, 1000.0, from, n) / amplitudeAt (m0, 1000.0, from, n)), toDb (amplitudeAt (s, 1500.0, from, n) / amplitudeAt (s0, 1500.0, from, n)) };
    };
    const auto mid = measure (Channels::mid), sd = measure (Channels::side), both = measure (Channels::midSide);
    check (std::fabs (mid.first - want) < 0.5 && std::fabs (mid.second) < 0.001, fmt ("Mid: the centre %+.2f dB, the side %+.4f dB", mid.first, mid.second));
    check (std::fabs (sd.first) < 0.001 && std::fabs (sd.second) < 0.05, fmt ("Side (quiet there): the centre %+.4f dB, the side %+.3f dB", sd.first, sd.second));
    check (std::fabs (both.first - want) < 0.5 && std::fabs (both.second) < 0.05, fmt ("M/S: each its own gain, the centre %+.2f dB, the side %+.3f dB", both.first, both.second));
}

void testMonitoring()
{
    std::printf ("\n[4] Band solo and delta\n");
    const auto l = noise (5, 0.3, second), r = noise (6, 0.3, second);
    auto p = midrangeOnly (-40.0, 4.0);
    const auto plain = run (p, l, r);

    // Solo of every band is the plain output; solo of some bands and of the others add to it.
    Engine a, b;
    a.prepare (fs); b.prepare (fs);
    a.monitor.solo = 0b001011u;
    b.monitor.solo = 0b110100u;
    const auto x = run (a, p, l, r), y = run (b, p, l, r);
    double err = 0.0;
    for (int i = 0; i < second; ++i)
        err = std::max (err, (double) std::fabs (x.l[(size_t) i] + y.l[(size_t) i] - plain.l[(size_t) i]));
    check (err < 1.0e-5, fmt ("three bands soloed plus the other three soloed is the plain output (within %.1e)", err));

    // Delta of a band is that band's output minus its input: plain output = all-off output + delta.
    Engine d;
    d.prepare (fs);
    d.monitor.solo = 1u << 3;
    d.monitor.delta = 1u << 3;
    const auto delta = run (d, p, l, r), dry = run (allOff(), l, r);
    err = 0.0;
    for (int i = 0; i < second; ++i)
        err = std::max (err, (double) std::fabs (dry.l[(size_t) i] + delta.l[(size_t) i] - plain.l[(size_t) i]));
    check (err < 1.0e-5 && rms (delta.l, second / 2, second / 4) > 1.0e-3, fmt ("delta of the Midrange is what its compressor changes: input + delta = output (within %.1e)", err));

    Engine q;
    q.prepare (fs);
    q.monitor.delta = 1u << 0; // a band that does nothing
    const auto silent = run (q, allOff(), l, r);
    Engine s;
    s.prepare (fs);
    s.monitor.solo = 0b111110u;
    const auto rest = run (s, allOff(), l, r);
    check (maxDiff (silent.l, rest.l, 0, second) < 1.0e-6, "the delta of a band that does nothing is silence (the other bands play on)");

    // Releasing a solo returns to the plain output.
    Engine t;
    t.prepare (fs);
    t.monitor.solo = 1u << 2;
    run (t, p, l, r);
    t.monitor.solo = 0u;
    Engine u;
    u.prepare (fs);
    run (u, p, l, r);
    const auto afterSolo = run (t, p, l, r), never = run (u, p, l, r);
    check (maxDiff (afterSolo.l, never.l, second / 10, second - second / 10) <= 0.0, "after a solo is released the output is the plain output again, bit for bit (past the 10 ms fade)");
}

void testGainMatch()
{
    std::printf ("\n[5] Gain match per band\n");
    const auto in = sine (1000.0, std::pow (10.0, -10.0 / 20.0), 4 * second);
    auto p = midrangeOnly (-25.0, 4.0);
    p.bands[3].releaseMs = 2000.0;
    const auto loss = toDb (rms (run (p, in, in).l, 3 * second, second / 2) / rms (in, 3 * second, second / 2));
    p.bands[3].match = true;
    Engine e;
    e.prepare (fs);
    const auto out = run (e, p, in, in);
    const double matched = toDb (rms (out.l, 3 * second, second / 2) / rms (in, 3 * second, second / 2));
    check (loss < -6.0 && std::fabs (matched) < 0.1 && e.matchMeter[3].load() > 9.0 && e.matchMeter[3].load() < 11.5,
           fmt ("a tone the compressor takes down (the output falls %.1f dB): with MATCH the band gets %+.2f dB and the output is within %+.2f dB of the input", -loss, e.matchMeter[3].load(), matched));
    p.bands[3].thresholdDb = -50.0;
    Engine limited;
    limited.prepare (fs);
    run (limited, p, in, in);
    check (std::fabs (limited.matchMeter[3].load() - 12.0) < 1.0e-4, fmt ("... and it never adds more than %.0f dB, however far the band is taken down", limited.matchMeter[3].load()));

    p.bands[3].thresholdDb = -20.0;
    Engine f;
    f.prepare (fs);
    const auto out2 = run (f, p, in, in);
    const double matched2 = toDb (rms (out2.l, 3 * second, second / 2) / rms (in, 3 * second, second / 2));
    check (std::fabs (matched2) < 0.1 && f.matchMeter[3].load() > 6.0 && f.matchMeter[3].load() < 9.0 && std::fabs (f.matchMeter[2].load()) < 1.0e-6,
           fmt ("threshold -20 dB: MATCH adds %+.2f dB and the band is at %+.2f dB; a band without it gets none", f.matchMeter[3].load(), matched2));
}

void testOversampling()
{
    std::printf ("\n[6] Oversampling and latency\n");
    const auto l = noise (7, 0.3, second), r = noise (8, 0.3, second);
    for (int os = 1; os < kNumOsModes; ++os)
        for (auto phase : { PhaseMode::zeroLatency, PhaseMode::linear })
        {
            auto p = allOff();
            p.oversampling = os;
            p.phase = phase;
            Engine e;
            e.prepare (fs, phase, os);
            const int latency = e.latencySamples.load();
            const auto out = run (e, p, l, r);
            auto q = allOff();
            q.phase = phase;
            const auto ref = run (q, l, r);
            const int base = phase == PhaseMode::linear ? LinearSplit::latencyFor (fs) : 0;
            // With nothing compressing, oversampling is a delay of the plain output and nothing else: only the difference is oversampled.
            check (latency > base && maxDiff (out.l, ref.l, 0, second - latency, latency - base) <= 0.0,
                   fmt ("%dx, %s, bands off: the plain output %d samples later, bit for bit (reported latency %d)", 1 << os, phase == PhaseMode::linear ? "linear" : "zero latency", latency - base, latency));
        }

    // What oversampling is for. A fast compressor on a high tone moves its gain inside the cycle, which makes the tone's odd harmonics;
    // for 11 kHz they are at 33 and 55 kHz, above Nyquist, and at 48 kHz they fold back to 15 and 7 kHz, where there is nothing else.
    const auto folded = [] (int os)
    {
        auto p = allOff();
        p.oversampling = os;
        p.bands[5] = { true, Channels::stereo, -30.0, 20.0, 0.1, 5.0, 0.0, false };
        const auto in = sine (11000.0, 0.5, second);
        const auto out = run (p, in, in);
        const double tone = amplitudeAt (out.l, 11000.0, second / 2, second / 4);
        return std::pair { toDb (amplitudeAt (out.l, 15000.0, second / 2, second / 4) / tone), toDb (amplitudeAt (out.l, 7000.0, second / 2, second / 4) / tone) };
    };
    const auto off = folded (0), x2 = folded (1), x4 = folded (2);
    check (x2.first < off.first - 10.0 && x4.first < off.first - 10.0 && x4.second < off.second - 10.0,
           fmt ("11 kHz, fastest times: the folded 3rd / 5th harmonics are %.1f / %.1f dB under the tone without oversampling, %.1f / %.1f dB at 2x, %.1f / %.1f dB at 4x",
                off.first, off.second, x2.first, x2.second, x4.first, x4.second));
}

void testRobust()
{
    std::printf ("\n[7] Block sizes, bad samples, sample rates\n");
    const auto l = noise (9, 0.4, second / 2), r = noise (10, 0.4, second / 2);
    for (auto phase : { PhaseMode::zeroLatency, PhaseMode::linear })
        for (int os : { 0, 1 })
        {
            auto p = midrangeOnly (-40.0, 4.0);
            p.bands[1] = { true, Channels::midSide, -40.0, 3.0, 5.0, 80.0, 3.0, true };
            p.phase = phase;
            p.oversampling = os;
            const auto a = run (p, l, r, 512), b = run (p, l, r, 1), c = run (p, l, r, 377);
            check (a.l == b.l && a.r == b.r && a.l == c.l && a.r == c.r,
                   fmt ("%s, oversampling %s: blocks of 512, 377 and 1 give the same output, bit for bit", phase == PhaseMode::linear ? "linear" : "zero latency", os == 0 ? "off" : "2x"));
        }

    auto bad = l;
    bad[1000] = std::numeric_limits<float>::infinity();
    bad[2000] = std::numeric_limits<float>::quiet_NaN();
    const auto out = run (midrangeOnly (-40.0, 4.0), bad, r);
    bool finite = true;
    for (float v : out.l) finite = finite && std::isfinite (v) && std::fabs (v) < 10.0f;
    check (finite, "an inf and a NaN in the input: the output stays finite");

    for (double rate : { 44100.0, 96000.0, 192000.0 })
    {
        Engine e;
        e.prepare (rate, PhaseMode::linear, 2);
        auto p = midrangeOnly (-40.0, 4.0);
        p.phase = PhaseMode::linear;
        p.oversampling = 2;
        auto x = l, y = r;
        e.process (x.data(), y.data(), (int) x.size(), p);
        bool ok = true;
        for (float v : x) ok = ok && std::isfinite (v) && std::fabs (v) < 10.0f;
        check (ok && e.latencySamples.load() >= LinearSplit::latencyFor (rate),
               fmt ("%.0f Hz, linear, 4x: runs, latency %d samples (%.1f ms)", rate, e.latencySamples.load(), 1000.0 * e.latencySamples.load() / rate));
    }

    // Switching the modes while playing: no blow-up, and the reported latency follows.
    Engine e;
    e.prepare (fs);
    auto p = midrangeOnly (-40.0, 4.0);
    auto x = l, y = r;
    bool ok = true;
    for (int k = 0; k < 6; ++k)
    {
        p.phase = k % 2 == 0 ? PhaseMode::linear : PhaseMode::zeroLatency;
        p.oversampling = k % 3;
        e.process (x.data(), y.data(), 4096, p);
        ok = ok && e.latencySamples.load() == e.latencyFor (p.phase, p.oversampling);
        for (int i = 0; i < 4096; ++i) ok = ok && std::isfinite (x[(size_t) i]) && std::fabs (x[(size_t) i]) < 10.0f;
    }
    check (ok, "phase and oversampling switched while playing: finite output, and the latency reported is the mode's");
}
void testFreeBands()
{
    std::printf ("\n[8] One to six bands\n");
    check (bandOwners ({ true, true, true, true, true }) == std::array<int, kBands> { 0, 1, 2, 3, 4, 5 }
               && bandOwners ({ false, false, false, false, false }) == std::array<int, kBands> { 0, 0, 0, 0, 0, 0 }
               && bandOwners ({ true, false, false, true, false }) == std::array<int, kBands> { 0, 1, 1, 1, 4, 4 },
           "a split that is off joins the band above it to the one below");

    // No splits: one band, a plain compressor. The whole output follows the law (nothing leaks into a neighbour).
    auto p = allOff();
    p.split = { false, false, false, false, false };
    p.bands[0] = { true, Channels::stereo, -30.0, 4.0, 1.0, 50.0, 0.0, false };
    const auto tone = sine (1000.0, std::pow (10.0, -10.0 / 20.0), second);
    const auto one = run (p, tone, tone);
    const double taken = toDb (amplitudeAt (one.l, 1000.0, second / 2, second / 4)) + 10.0;
    check (std::fabs (taken + 15.0) < 0.2, fmt ("no splits, -30 dB, 4:1: a 1 kHz tone at -10 dB is taken down %+.2f dB on the whole output (the law gives -15.00)", taken));

    // Two bands split at 250 Hz only: the low band's compressor does not touch 2 kHz, the upper band's does, with strip 2's settings.
    p = allOff();
    p.split = { false, true, false, false, false };
    p.bands[2] = { true, Channels::stereo, -30.0, 4.0, 1.0, 50.0, 0.0, false };
    const auto high = sine (4000.0, std::pow (10.0, -10.0 / 20.0), second), low = sine (60.0, std::pow (10.0, -10.0 / 20.0), second);
    const auto h = run (p, high, high), lo = run (p, low, low);
    const double takenHigh = toDb (amplitudeAt (h.l, 4000.0, second / 2, second / 4)) + 10.0, takenLow = toDb (amplitudeAt (lo.l, 60.0, second / 2, second / 4)) + 10.0;
    check (std::fabs (takenHigh + 15.0) < 0.2 && std::fabs (takenLow) < 0.05,
           fmt ("one split at 250 Hz, the upper band compressing: 4 kHz %+.2f dB (it spans 250 Hz to 20 kHz), 60 Hz %+.3f dB", takenHigh, takenLow));

    // Joined bands still add back to the input, in both phase modes, and the output does not depend on the host's block.
    const auto l = noise (11, 0.4, second / 2), r = noise (12, 0.4, second / 2);
    auto q = allOff();
    q.split = { true, false, false, true, false };
    q.phase = PhaseMode::linear;
    Engine e;
    e.prepare (fs, q.phase, 0);
    const int latency = e.latencySamples.load();
    const auto out = run (e, q, l, r);
    q.bands[1] = { true, Channels::midSide, -40.0, 3.0, 5.0, 80.0, 2.0, true };
    const auto a = run (q, l, r, 512), b = run (q, l, r, 1);
    check (maxDiff (out.l, l, 0, second / 2 - latency, latency) < 1.0e-5 && a.l == b.l && a.r == b.r,
           "three bands, linear: with the bands off the output is the input, later; compressing, blocks of 512 and 1 give the same output bit for bit");
}

void testIssueFinder()
{
    std::printf ("\n[9] The display's finder: peaks and jumps (shared/dsp SpectrumIssues.h)\n");
    using pdsp::IssueFinder;
    using pdsp::SpectrumGrid;
    const float dt = 1.0f / 30.0f;
    std::mt19937 gen (21);
    std::uniform_real_distribution<float> jitter (-1.5f, 1.5f);
    // A programme that slopes down 3 dB per octave from -30 dB, every step wobbling a little.
    const auto base = [&] (int i) { return -30.0f - 3.0f * (float) i / (float) SpectrumGrid::kPerOctave + jitter (gen); };

    IssueFinder steady;
    for (int frame = 0; frame < 240; ++frame)
    {
        SpectrumGrid::Column c;
        for (int i = 0; i < SpectrumGrid::kSteps; ++i) c[(size_t) i] = base (i);
        steady.push (c, dt);
    }
    std::array<IssueFinder::Peak, IssueFinder::kMaxPeaks> peaks;
    std::array<IssueFinder::Swing, IssueFinder::kMaxSwings> swings;
    check (steady.peaks (peaks) == 0 && steady.swings (swings) == 0, "a steady, smooth programme: no peaks, no jumps");

    // The same with a resonance at 3 kHz (12 dB, narrow) and a region around 80 Hz that leaps 18 dB twice a second.
    IssueFinder f;
    const int resonance = (int) std::lround (SpectrumGrid::stepAt (3000.0)), kick = (int) std::lround (SpectrumGrid::stepAt (80.0));
    for (int frame = 0; frame < 240; ++frame)
    {
        SpectrumGrid::Column c;
        for (int i = 0; i < SpectrumGrid::kSteps; ++i)
        {
            c[(size_t) i] = base (i);
            if (std::abs (i - resonance) <= 1) c[(size_t) i] += i == resonance ? 12.0f : 6.0f;
            if (std::abs (i - kick) <= 8 && frame % 15 < 3) c[(size_t) i] += 18.0f;
        }
        f.push (c, dt);
        if (frame == 30)
            check (f.peaks (peaks) == 0 && f.swings (swings) == 0, "nothing is reported in the first two seconds");
    }
    const int numPeaks = f.peaks (peaks), numSwings = f.swings (swings);
    check (numPeaks == 1 && std::fabs (std::log2 (peaks[0].freqHz / 3000.0)) < 1.0 / 24.0 && peaks[0].prominenceDb > 8.0f,
           fmt ("a 12 dB resonance at 3 kHz: %d peak, at %.0f Hz, %.1f dB over its neighbourhood", numPeaks, peaks[0].freqHz, peaks[0].prominenceDb));
    check (numSwings == 1 && swings[0].loHz < 70.0 && swings[0].hiHz > 92.0 && swings[0].loHz > 55.0 && swings[0].hiHz < 115.0 && swings[0].swingDb > 10.0f
               && swings[0].thresholdDb() < swings[0].peakDb && swings[0].thresholdDb() > swings[0].peakDb - swings[0].swingDb,
           fmt ("a region around 80 Hz that leaps 18 dB: %d region, %.0f to %.0f Hz, jumping %.1f dB to %.1f dB (a threshold half way: %.1f dB)",
                numSwings, swings[0].loHz, swings[0].hiHz, swings[0].swingDb, swings[0].peakDb, swings[0].thresholdDb()));

    IssueFinder silent;
    SpectrumGrid::Column quiet;
    quiet.fill (-140.0f);
    for (int frame = 0; frame < 240; ++frame) silent.push (quiet, dt);
    check (silent.peaks (peaks) == 0 && silent.swings (swings) == 0, "silence says nothing");
}
} // namespace

int main()
{
    std::printf ("Atlas DSP tests (48 kHz)\n");
    testSplit();
    testCompression();
    testChannels();
    testMonitoring();
    testGainMatch();
    testOversampling();
    testRobust();
    testFreeBands();
    testIssueFinder();
    return test::finish ("AtlasTests");
}
