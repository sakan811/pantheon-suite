// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 The Pantheon authors

// Headless tests of everything under Source/DSP: no plugin, no GUI. Every number the README quotes comes from here.
//
//   IrisTests

#include "Check.h"
#include "DSP/Engine.h"
#include "DSP/Masking.h"
#include "DSP/ReferenceMatch.h"
#include "DSP/ResonanceFinder.h"
#include "TestSignals.h"

#include <chrono>
#include <functional>

using namespace iris;
using namespace test;

namespace
{
struct Stereo
{
    std::vector<float> l, r;
};

// Runs a fresh or existing engine over a signal in blocks of `block` samples.
Stereo run (Engine& e, const EngineParams& p, const std::vector<float>& inL, const std::vector<float>& inR, int block = 512)
{
    Stereo out { inL, inR };
    const int total = (int) inL.size();
    juce::AudioBuffer<float> buffer (2, block);
    for (int start = 0; start < total; start += block)
    {
        const int n = std::min (block, total - start);
        buffer.setSize (2, n, false, false, true);
        for (int i = 0; i < n; ++i)
        {
            buffer.setSample (0, i, inL[(size_t) (start + i)]);
            buffer.setSample (1, i, inR[(size_t) (start + i)]);
        }
        e.process (buffer, p);
        for (int i = 0; i < n; ++i)
        {
            out.l[(size_t) (start + i)] = buffer.getSample (0, i);
            out.r[(size_t) (start + i)] = buffer.getSample (1, i);
        }
    }
    return out;
}

Stereo runMono (Engine& e, const EngineParams& p, const std::vector<float>& in, int block = 512)
{
    Stereo out { in, in };
    juce::AudioBuffer<float> buffer (1, block);
    for (int start = 0; start < (int) in.size(); start += block)
    {
        const int n = std::min (block, (int) in.size() - start);
        buffer.setSize (1, n, false, false, true);
        for (int i = 0; i < n; ++i)
            buffer.setSample (0, i, in[(size_t) (start + i)]);
        e.process (buffer, p);
        for (int i = 0; i < n; ++i)
            out.l[(size_t) (start + i)] = buffer.getSample (0, i);
    }
    return out;
}

Stereo runBoth (const EngineParams& p, const std::vector<float>& inL, const std::vector<float>& inR, int block = 512)
{
    Engine e;
    e.prepare (fs, block);
    return run (e, p, inL, inR, block);
}

Stereo runSame (const EngineParams& p, const std::vector<float>& in)
{
    return runBoth (p, in, in);
}

void setBand (EngineParams& p, int i, BandType type, double freq, double gainDb = 0.0, double q = 1.0, Placement place = Placement::stereo, int slope = 1)
{
    auto& b = p.bands[(size_t) i];
    b.on = true;
    b.type = type;
    b.freq = freq;
    b.gainDb = gainDb;
    b.q = q;
    b.place = place;
    b.slope = slope;
}

constexpr int oneSecond = 48000;

// Steady-state gain of a band arrangement for a sine at `freq`, in dB (measured over the last second).
double gainAt (const EngineParams& p, double freq, double amp = 0.1)
{
    const int n = oneSecond + 12000;
    const auto in = sine (freq, amp, n);
    const auto out = runSame (p, in);
    return toDb (amplitudeAt (out.l, freq, 12000, oneSecond) / amplitudeAt (in, freq, 12000, oneSecond));
}

bool identical (const Stereo& a, const Stereo& b)
{
    return a.l == b.l && a.r == b.r;
}

bool allFinite (const Stereo& s, double limit = 1000.0)
{
    for (const auto* ch : { &s.l, &s.r })
        for (float v : *ch)
            if (! std::isfinite (v) || std::fabs (v) > limit)
                return false;
    return true;
}

// ----------------------------------------------------------------------------------------------
void testTransparent()
{
    std::printf ("\n[1] With nothing switched on, the audio passes bit for bit\n");
    const auto l = noise (1, 0.7, 20000), r = noise (2, 0.7, 20000);

    EngineParams p;
    check (identical (runBoth (p, l, r), { l, r }), "CLEAN, no bands: output is the input");
    p.outGainDb = 1.0e-6; // what a host's 0.0 dB looks like after JUCE's parameter snapping
    check (identical (runBoth (p, l, r), { l, r }), "... also with the output gain at 0.000001 dB");
    p.outGainDb = 0.0;
    p.sat = true;
    p.driveDb = 30.0;
    check (identical (runBoth (p, l, r), { l, r }), "SAT at 30 dB drive, no bands: output is the input");

    p = {};
    setBand (p, 0, BandType::bell, 1000.0, 0.0);
    setBand (p, 1, BandType::highShelf, 5000.0, 0.0);
    setBand (p, 2, BandType::bell, 300.0, 0.0, 1.0, Placement::side);
    check (identical (runBoth (p, l, r), { l, r }), "bands on at 0 dB: output is the input");
}

void testBell()
{
    std::printf("\n[2] Bell, shelves and notch: the measured response is the designed one\n");
    EngineParams p;
    setBand (p, 0, BandType::bell, 1000.0, 6.0, 1.0);
    const double centre = gainAt (p, 1000.0), far = gainAt (p, 100.0);
    check (std::fabs (centre - 6.0) < 0.05, fmt ("bell +6 dB at 1 kHz reads %+.3f dB at 1 kHz", centre));
    check (std::fabs (far) < 0.15, fmt ("... and %+.3f dB a decade below", far));

    double worst = 0.0;
    for (double f : { 200.0, 500.0, 800.0, 1250.0, 2000.0, 5000.0 })
    {
        const double expected = bandResponseDb (p.bands[0], 0.0, fs, f);
        worst = std::max (worst, std::fabs (gainAt (p, f) - expected));
    }
    check (worst < 0.05, fmt ("six more frequencies agree with bandResponseDb to %.3f dB", worst));

    EngineParams q;
    setBand (q, 0, BandType::lowShelf, 200.0, 6.0, 0.7071);
    check (std::fabs (gainAt (q, 20.0) - 6.0) < 0.3 && std::fabs (gainAt (q, 5000.0)) < 0.1,
           fmt ("low shelf +6 dB at 200 Hz: %+.2f dB at 20 Hz, %+.2f dB at 5 kHz", gainAt (q, 20.0), gainAt (q, 5000.0)));
    q = {};
    setBand (q, 0, BandType::highShelf, 4000.0, -6.0, 0.7071);
    check (std::fabs (gainAt (q, 18000.0) + 6.0) < 0.5 && std::fabs (gainAt (q, 200.0)) < 0.1,
           fmt ("high shelf -6 dB at 4 kHz: %+.2f dB at 18 kHz, %+.2f dB at 200 Hz", gainAt (q, 18000.0), gainAt (q, 200.0)));
    q = {};
    setBand (q, 0, BandType::notch, 1000.0, 0.0, 4.0);
    check (gainAt (q, 1000.0, 0.5) < -40.0 && std::fabs (gainAt (q, 4000.0)) < 0.2,
           fmt ("notch at 1 kHz: %.1f dB at the notch, %+.2f dB at 4 kHz", gainAt (q, 1000.0, 0.5), gainAt (q, 4000.0)));
}

void testCuts()
{
    std::printf("\n[3] Low and high cut: Butterworth slopes of 12, 24, 36 and 48 dB per octave\n");
    const double expected[4] = { -12.30, -24.08, -36.12, -48.16 }; // 10 log10 (1 / (1 + 2^2N)) at half the corner
    for (int slope = 0; slope < 4; ++slope)
    {
        EngineParams p;
        setBand (p, 0, BandType::lowCut, 400.0, 0.0, 1.0, Placement::stereo, slope);
        const double half = gainAt (p, 200.0, 0.5), corner = gainAt (p, 400.0, 0.5), pass = gainAt (p, 4000.0, 0.5);
        check (std::fabs (half - expected[slope]) < 0.3 && std::fabs (corner + 3.01) < 0.1 && std::fabs (pass) < 0.01,
               fmt ("low cut %d dB/oct: %.2f dB at half the corner (want %.2f), %.2f at the corner, %+.3f in the pass band",
                    12 * (slope + 1), half, expected[slope], corner, pass));
    }
    EngineParams p;
    setBand (p, 0, BandType::highCut, 8000.0, 0.0, 1.0, Placement::stereo, 1);
    // The digital filter is the analogue one with its frequency axis warped: |H|^2 = 1 / (1 + (tan (pi f / fs) / tan (pi fc / fs))^(2 N)).
    const double warp = std::tan (M_PI * 16000.0 / fs) / std::tan (M_PI * 8000.0 / fs), want = -10.0 * std::log10 (1.0 + std::pow (warp, 8.0));
    const double half = gainAt (p, 16000.0, 0.5);
    check (std::fabs (half - want) < 0.3 && std::fabs (gainAt (p, 800.0, 0.5)) < 0.01,
           fmt ("high cut 24 dB/oct at 8 kHz: %.2f dB at 16 kHz (the warped Butterworth gives %.2f)", half, want));
}

void testPlacement()
{
    std::printf("\n[4] Left, Right, Mid and Side: a band touches only what it is placed on\n");
    const int n = oneSecond + 12000;
    const auto s = sine (1000.0, 0.2, n);
    const auto neg = scaled (s, -1.0);
    const auto other = sine (1700.0, 0.2, n);

    const auto level = [&] (const std::vector<float>& x) { return amplitudeAt (x, 1000.0, 12000, oneSecond); };
    const double ref = level (s);

    EngineParams p;
    setBand (p, 0, BandType::bell, 1000.0, 6.0, 1.0, Placement::mid);
    auto mid = runBoth (p, s, s);
    auto side = runBoth (p, s, neg);
    check (std::fabs (toDb (level (mid.l) / ref) - 6.0) < 0.05 && std::fabs (toDb (level (mid.r) / ref) - 6.0) < 0.05,
           fmt ("Mid +6 dB on a mono signal: %+.2f / %+.2f dB", toDb (level (mid.l) / ref), toDb (level (mid.r) / ref)));
    check (side.l == s && side.r == neg, "Mid band on a pure side signal: untouched, bit for bit");

    p = {};
    setBand (p, 0, BandType::bell, 1000.0, 6.0, 1.0, Placement::side);
    side = runBoth (p, s, neg);
    mid = runBoth (p, s, s);
    check (std::fabs (toDb (level (side.l) / ref) - 6.0) < 0.05 && std::fabs (toDb (level (side.r) / ref) - 6.0) < 0.05,
           fmt ("Side +6 dB on a pure side signal: %+.2f / %+.2f dB", toDb (level (side.l) / ref), toDb (level (side.r) / ref)));
    check (mid.l == s && mid.r == s, "Side band on a mono signal: untouched, bit for bit");

    p = {};
    setBand (p, 0, BandType::bell, 1000.0, 6.0, 1.0, Placement::left);
    auto lr = runBoth (p, s, other);
    check (std::fabs (toDb (level (lr.l) / ref) - 6.0) < 0.05 && lr.r == other, "Left band: left +6 dB, right untouched bit for bit");
    p.bands[0].place = Placement::right;
    lr = runBoth (p, other, s);
    check (std::fabs (toDb (level (lr.r) / ref) - 6.0) < 0.05 && lr.l == other, "Right band: right +6 dB, left untouched bit for bit");

    // A mid and a side band together are two independent equalisers.
    p = {};
    setBand (p, 0, BandType::bell, 1000.0, 6.0, 1.0, Placement::mid);
    setBand (p, 1, BandType::bell, 1000.0, -6.0, 1.0, Placement::side);
    const auto mixed = runBoth (p, s, other); // L = s, R = other (neither pure mid nor pure side)
    check (allFinite (mixed), "a mid band and a side band together stay finite");
}

void testDynamics()
{
    std::printf("\n[5] Dynamic bands\n");
    DynamicsParams d;
    d.thresholdDb = -30.0;
    d.ratio = 4.0;
    d.rangeDb = -12.0;
    check (std::fabs (dynamicOffsetDb (d, -50.0)) < 1.0e-9 && std::fabs (dynamicOffsetDb (d, -26.0) + 3.0) < 1.0e-9 && std::fabs (dynamicOffsetDb (d, -5.0) + 12.0) < 1.0e-9,
           fmt ("gain computer: 0 dB well below, %.2f dB at 4 dB over (4:1), %.1f dB at the range", dynamicOffsetDb (d, -26.0), dynamicOffsetDb (d, -5.0)));
    d.mode = DynMode::below;
    d.rangeDb = 6.0;
    check (std::fabs (dynamicOffsetDb (d, -10.0)) < 1.0e-9 && std::fabs (dynamicOffsetDb (d, -80.0) - 6.0) < 1.0e-9, "gain computer, Below: engages as the level falls under the threshold");

    EngineParams p;
    setBand (p, 0, BandType::bell, 1000.0, 0.0, 1.0);
    p.bands[0].dynamic = true;
    p.bands[0].dyn = d = { DynMode::above, -30.0, 4.0, -12.0, 2.0, 80.0 };

    const double quiet = gainAt (p, 1000.0, std::pow (10.0, -40.0 / 20.0));
    const double mid = gainAt (p, 1000.0, std::pow (10.0, -26.0 / 20.0));
    const double loud = gainAt (p, 1000.0, std::pow (10.0, -10.0 / 20.0));
    check (std::fabs (quiet) < 0.05, fmt ("Above: a signal under the threshold passes at %+.3f dB", quiet));
    check (std::fabs (mid + 3.0) < 0.3, fmt ("Above: 4 dB over the threshold at 4:1 is %+.2f dB (want -3.00)", mid));
    check (std::fabs (loud + 12.0) < 0.3, fmt ("Above: far over the threshold stops at the range, %+.2f dB (want -12.00)", loud));

    check (std::fabs (gainAt (p, 100.0, 0.3)) < 0.05, "a loud signal far from the band does not move it");

    p.bands[0].dyn = { DynMode::below, -30.0, 4.0, 6.0, 2.0, 80.0 };
    const double below = gainAt (p, 1000.0, std::pow (10.0, -60.0 / 20.0)), above = gainAt (p, 1000.0, std::pow (10.0, -10.0 / 20.0));
    check (std::fabs (below - 6.0) < 0.3 && std::fabs (above) < 0.05, fmt ("Below: quiet material gets %+.2f dB, loud material %+.3f dB", below, above));

    // Attack and release: gain follows the level with the times that were asked for.
    p.bands[0].dyn = { DynMode::above, -30.0, 20.0, -12.0, 100.0, 400.0 };
    const int n = oneSecond * 3;
    auto in = sine (1000.0, 0.001, n); // a bed far under the threshold
    const auto loudTone = sine (1000.0, std::pow (10.0, -10.0 / 20.0), n);
    for (int i = oneSecond / 2; i < oneSecond * 3 / 2 + oneSecond / 2; ++i) // loud from 0.5 s to 2.0 s
        in[(size_t) i] = loudTone[(size_t) i];
    const auto out = runSame (p, in);
    const auto windowGain = [&] (double t) { const int a = (int) (t * fs); return toDb (amplitudeAt (out.l, 1000.0, a, 960) / amplitudeAt (in, 1000.0, a, 960)); };
    const double early = windowGain (0.51), later = windowGain (0.95), settled = windowGain (1.9);
    const double wantEarly = -12.0 * (1.0 - std::exp (-0.015 / 0.1)); // the window is centred 15 ms after the start
    check (std::fabs (early - wantEarly) < 0.6 && later < -10.0 && settled < -11.5,
           fmt ("attack 100 ms: %+.1f dB after 15 ms (an exponential gives %+.1f), %+.1f dB at 450 ms, %+.1f dB at 1.4 s", early, wantEarly, later, settled));
    const double afterEarly = windowGain (2.03), afterLate = windowGain (2.9); // the signal stops at 2.0 s
    const double wantLate = -12.0 * std::exp (-0.9 / 0.4);
    check (afterEarly < -6.0 && std::fabs (afterLate - wantLate) < 0.7,
           fmt ("release 400 ms: still %+.1f dB 30 ms after the signal stops, %+.2f dB 900 ms later (an exponential gives %+.2f)", afterEarly, afterLate, wantLate));

    // The editor draws the live gain from these.
    Engine e;
    e.prepare (fs, 512);
    run (e, p, loudTone, loudTone);
    check (e.dynamicDb[0].load() < -5.0 && e.detectedDb[0].load() > -20.0,
           fmt ("the engine reports the dynamic gain (%.1f dB) and the level it hears (%.1f dB)", e.dynamicDb[0].load(), e.detectedDb[0].load()));
}

void testSaturation()
{
    std::printf("\n[6] CLEAN and SAT\n");
    const auto harmonic = [&] (const Stereo& o, int k) { return toDb (amplitudeAt (o.l, 1000.0 * k, 12000, oneSecond) / amplitudeAt (o.l, 1000.0, 12000, oneSecond)); };

    EngineParams clean;
    setBand (clean, 0, BandType::bell, 1000.0, 12.0, 1.0);
    const auto loud = sine (1000.0, 0.3, oneSecond + 12000);
    const auto cleanOut = runSame (clean, loud);
    check (harmonic (cleanOut, 3) < -110.0 && harmonic (cleanOut, 2) < -110.0,
           fmt ("CLEAN adds no harmonics: 2nd %.0f dB, 3rd %.0f dB", harmonic (cleanOut, 2), harmonic (cleanOut, 3)));

    EngineParams tape = clean;
    tape.sat = true;
    tape.style = SatStyle::tape;
    const auto tapeOut = runSame (tape, loud);
    check (harmonic (tapeOut, 3) > -45.0 && harmonic (tapeOut, 3) < -10.0 && harmonic (tapeOut, 2) < -80.0,
           fmt ("SAT, Tape: odd harmonics (3rd %.1f dB, 5th %.1f dB), no even ones (2nd %.0f dB)", harmonic (tapeOut, 3), harmonic (tapeOut, 5), harmonic (tapeOut, 2)));

    EngineParams tube = tape;
    tube.style = SatStyle::tube;
    const auto tubeOut = runSame (tube, loud);
    check (harmonic (tubeOut, 2) > -50.0 && harmonic (tubeOut, 2) < -10.0,
           fmt ("SAT, Tube: even harmonics appear (2nd %.1f dB, 3rd %.1f dB)", harmonic (tubeOut, 2), harmonic (tubeOut, 3)));

    EngineParams console = tape;
    console.style = SatStyle::console;
    const auto consoleOut = runSame (console, loud);
    check (harmonic (consoleOut, 3) > -60.0 && harmonic (consoleOut, 3) < harmonic (tapeOut, 3) + 0.01,
           fmt ("SAT, Console: gentler than Tape (3rd %.1f dB against %.1f dB)", harmonic (consoleOut, 3), harmonic (tapeOut, 3)));

    // Small signals stay linear: SAT only shows when the boost gets loud.
    const auto small = sine (1000.0, 0.003, oneSecond + 12000);
    const double smallClean = toDb (amplitudeAt (runSame (clean, small).l, 1000.0, 12000, oneSecond)), smallSat = toDb (amplitudeAt (runSame (tape, small).l, 1000.0, 12000, oneSecond));
    check (std::fabs (smallClean - smallSat) < 0.01, fmt ("a -50 dBFS signal: CLEAN and SAT differ by %.4f dB", smallSat - smallClean));

    // Drive moves the knee: with a moderate boost, more drive means more harmonics.
    {
        EngineParams gentle;
        setBand (gentle, 0, BandType::bell, 1000.0, 6.0, 1.0);
        gentle.sat = true;
        const auto tone = sine (1000.0, 0.2, oneSecond + 12000);
        double last = -300.0;
        bool rising = true;
        std::string line;
        for (double drive : { 0.0, 6.0, 12.0 })
        {
            gentle.driveDb = drive;
            const double h3 = harmonic (runSame (gentle, tone), 3);
            rising = rising && h3 > last + 3.0;
            last = h3;
            line += fmt (" %.0f dB: %.1f dB;", drive, h3);
        }
        check (rising, "a +6 dB boost: the 3rd harmonic rises with Drive (" + line.substr (1) + ")");
    }

    // Only boosts saturate: cuts and cut filters are the same in both modes, bit for bit.
    EngineParams cuts;
    setBand (cuts, 0, BandType::bell, 1000.0, -12.0, 1.0);
    setBand (cuts, 1, BandType::lowCut, 80.0, 0.0);
    setBand (cuts, 2, BandType::highCut, 14000.0, 0.0);
    setBand (cuts, 3, BandType::notch, 3000.0, 0.0, 3.0);
    const auto bell = sine (1000.0, 0.8, 20000);
    EngineParams cutsSat = cuts;
    cutsSat.sat = true;
    check (identical (runSame (cuts, bell), runSame (cutsSat, bell)), "cuts, cut filters and notches do not saturate: same output in both modes");

    // Why the shaper is anti-aliased: a 3rd harmonic of 9.7 kHz is at 29.1 kHz, which folds down to 18.9 kHz.
    {
        const int n = 48000;
        const auto x = sine (9700.0, 0.5, n);
        Shaper adaa;
        adaa.reset (SatStyle::tape);
        adaa.setSampleRate (fs);
        std::vector<float> a ((size_t) n), naive ((size_t) n);
        for (int i = 0; i < n; ++i)
        {
            a[(size_t) i] = (float) (x[(size_t) i] + adaa.residual (SatStyle::tape, x[(size_t) i], 4.0));
            naive[(size_t) i] = (float) (std::tanh (4.0 * x[(size_t) i]) / 4.0);
        }
        const double aliasA = toDb (amplitudeAt (a, 18900.0, 0, n)), aliasNaive = toDb (amplitudeAt (naive, 18900.0, 0, n));
        check (aliasA < aliasNaive - 8.0, fmt ("anti-aliasing: the folded 3rd harmonic is %.1f dB with ADAA, %.1f dB without", aliasA, aliasNaive));
    }
}

void testDelta()
{
    std::printf("\n[7] Delta and gain match\n");
    const auto l = noise (3, 0.3, 30000), r = noise (4, 0.3, 30000);
    EngineParams p;
    setBand (p, 0, BandType::bell, 800.0, 9.0, 1.2);
    setBand (p, 1, BandType::highShelf, 6000.0, -4.0, 0.7);
    setBand (p, 2, BandType::bell, 2000.0, 5.0, 2.0, Placement::side);
    p.bands[2].dynamic = true;
    p.bands[2].dyn.thresholdDb = -30.0;

    const auto wet = runBoth (p, l, r);
    Engine e;
    e.prepare (fs, 512);
    e.monitor.delta = true;
    const auto delta = run (e, p, l, r);
    double worst = 0.0;
    for (size_t i = 0; i < l.size(); ++i)
        worst = std::max ({ worst, (double) std::fabs (delta.l[i] - (wet.l[i] - l[i])), (double) std::fabs (delta.r[i] - (wet.r[i] - r[i])) });
    check (worst < 1.0e-6, fmt ("DELTA is the processed signal minus the input (largest difference %.1e)", worst));

    EngineParams none;
    Engine flat;
    flat.prepare (fs, 512);
    flat.monitor.delta = true;
    const auto zero = run (flat, none, l, r);
    check (maxAbs (zero.l, 0, (int) l.size()) == 0.0 && maxAbs (zero.r, 0, (int) l.size()) == 0.0, "DELTA with no bands is silence");

    // Gain match.
    const int n = oneSecond * 6;
    const auto bed = noise (5, 0.2, n);
    EngineParams loudEq;
    setBand (loudEq, 0, BandType::lowShelf, 3000.0, 9.0, 0.7);
    const auto plain = runSame (loudEq, bed);
    loudEq.match = true;
    Engine m;
    m.prepare (fs, 512);
    const auto matched = run (m, loudEq, bed, bed);
    const double before = toDb (rms (plain.l, n - oneSecond, oneSecond) / rms (bed, n - oneSecond, oneSecond));
    const double after = toDb (rms (matched.l, n - oneSecond, oneSecond) / rms (bed, n - oneSecond, oneSecond));
    check (before > 2.0 && std::fabs (after) < 0.3, fmt ("GAIN MATCH: a +9 dB low shelf is %+.1f dB louder, %+.2f dB with the match on (applying %.1f dB)", before, after, m.matchDb.load()));

    EngineParams extreme;
    setBand (extreme, 0, BandType::bell, 500.0, 24.0, 0.3);
    setBand (extreme, 1, BandType::bell, 2500.0, 24.0, 0.3);
    extreme.match = true;
    Engine x;
    x.prepare (fs, 512);
    run (x, extreme, bed, bed);
    check (x.matchDb.load() >= -12.1, fmt ("GAIN MATCH never takes off more than 12 dB (%.1f dB)", x.matchDb.load()));
}

void testSolo()
{
    std::printf("\n[8] Solo: frequency regions, bands and curves\n");
    // The middle of each band in octaves; Low-Mid (250 - 500 Hz) is the one band only an octave wide.
    double mids[numRegions];
    for (int r = 0; r < numRegions; ++r)
        mids[r] = std::sqrt (pdsp::bands::kBands[(size_t) r].loHz * pdsp::bands::kBands[(size_t) r].hiHz);
    constexpr int narrow = 2;
    constexpr unsigned allRegions = (1u << numRegions) - 1u;

    // Each region lets its own range through and keeps the others out.
    bool ownOk = true, othersOk = true;
    double worstOwn = 0.0, worstNarrow = 0.0, loudestOther = -200.0, loudestNeighbour = -200.0, loudestNarrowNeighbour = -200.0;
    for (int region = 0; region < numRegions; ++region)
        for (int tone = 0; tone < numRegions; ++tone)
        {
            Engine e;
            e.prepare (fs, 512);
            e.monitor.regions = 1u << region;
            const auto in = sine (mids[tone], 0.3, oneSecond + 12000);
            const auto out = run (e, EngineParams {}, in, in);
            const double g = toDb (amplitudeAt (out.l, mids[tone], 12000, oneSecond) / amplitudeAt (in, mids[tone], 12000, oneSecond));
            if (region == tone)
            {
                (region == narrow ? worstNarrow : worstOwn) = std::max (region == narrow ? worstNarrow : worstOwn, std::fabs (g));
                ownOk = ownOk && std::fabs (g) < (region == narrow ? 4.3 : 2.5); // an octave-wide band's middle is 1.9 dB down at each crossover
            }
            else if (std::abs (region - tone) == 1)
            {
                const bool besideNarrow = tone == narrow || region == narrow;
                (besideNarrow ? loudestNarrowNeighbour : loudestNeighbour) = std::max (besideNarrow ? loudestNarrowNeighbour : loudestNeighbour, g);
                othersOk = othersOk && g < (besideNarrow ? -13.0 : -15.0); // a tone in the octave-wide band leaks 14 dB into the bands beside it
            }
            else
            {
                loudestOther = std::max (loudestOther, g);
                othersOk = othersOk && g < -35.0;
            }
        }
    check (ownOk, fmt ("a region passes a tone in the middle of its own range (within %.2f dB; %.2f dB for the octave-wide low-mid)", worstOwn, worstNarrow));
    check (othersOk, fmt ("... and keeps the next range's tone %.1f dB down at most (%.1f dB beside the low-mid), and the others %.1f dB", loudestNeighbour, loudestNarrowNeighbour, loudestOther));

    // All six together are the original magnitude.
    double worst = 0.0;
    for (double f : { 30.0, 60.0, 100.0, 250.0, 350.0, 500.0, 1200.0, 2000.0, 4500.0, 6000.0, 15000.0 })
    {
        Engine e;
        e.prepare (fs, 512);
        e.monitor.regions = allRegions;
        const auto in = sine (f, 0.3, oneSecond + 12000);
        const auto out = run (e, EngineParams {}, in, in);
        worst = std::max (worst, std::fabs (toDb (amplitudeAt (out.l, f, 12000, oneSecond) / amplitudeAt (in, f, 12000, oneSecond))));
    }
    check (worst < 0.05, fmt ("all six regions together are flat (largest deviation %.3f dB over eleven tones, crossovers included)", worst));

    // Moving the crossovers moves the regions; crossovers out of order are put back in order.
    {
        EngineParams p;
        p.crossover = { 1000.0, 3000.0, 100.0, 50.0, 20.0 };
        Engine e;
        e.prepare (fs, 512);
        e.monitor.regions = allRegions;
        const auto in = noise (9, 0.3, 30000);
        check (allFinite (run (e, p, in, in)), "crossovers given out of order are sorted, and the output stays finite");
    }

    // Soloing then releasing leaves the audio exactly as it was.
    {
        const auto in = noise (7, 0.5, 40000);
        Engine e;
        e.prepare (fs, 512);
        e.monitor.regions = 2u;
        run (e, EngineParams {}, in, in);
        e.monitor.regions = 0u;
        run (e, EngineParams {}, in, in);
        const auto after = run (e, EngineParams {}, in, in);
        check (identical (after, { in, in }), "after a region solo is released the output is the input again, bit for bit");
    }

    // Band solo, BAND: the band's own region, after the whole chain.
    EngineParams p;
    setBand (p, 0, BandType::bell, 1000.0, 0.0, 4.0);
    const auto probe = [&] (const EngineParams& pp, SoloKind kind, int band, double f, double amp = 0.2)
    {
        Engine e;
        e.prepare (fs, 512);
        e.monitor.soloBand = band;
        e.monitor.soloKind = (int) kind;
        const auto in = sine (f, amp, oneSecond + 12000);
        const auto out = run (e, pp, in, in);
        return std::make_pair (toDb (amplitudeAt (out.l, f, 12000, oneSecond) / amplitudeAt (in, f, 12000, oneSecond)), out);
    };
    const double on = probe (p, SoloKind::band, 0, 1000.0).first, off = probe (p, SoloKind::band, 0, 100.0).first, offHigh = probe (p, SoloKind::band, 0, 10000.0).first;
    check (std::fabs (on) < 0.2 && off < -30.0 && offHigh < -30.0,
           fmt ("band solo (BAND), bell at 1 kHz: %+.2f dB at 1 kHz, %.0f dB at 100 Hz, %.0f dB at 10 kHz", on, off, offHigh));

    EngineParams shelf;
    setBand (shelf, 0, BandType::lowShelf, 200.0, 4.0, 0.7);
    const double below = probe (shelf, SoloKind::band, 0, 50.0).first, above = probe (shelf, SoloKind::band, 0, 2000.0).first;
    check (below > -1.0 && above < -30.0, fmt ("band solo, low shelf at 200 Hz: hears %+.1f dB at 50 Hz and %.0f dB at 2 kHz (the region it shapes)", below, above));

    // Band solo, CURVE: only what the band adds.
    EngineParams curve;
    setBand (curve, 0, BandType::bell, 1000.0, 6.0, 1.0);
    setBand (curve, 1, BandType::bell, 5000.0, 6.0, 4.0);
    const auto [curveGain, curveOut] = probe (curve, SoloKind::curve, 0, 1000.0);
    const double expectedAdd = toDb (std::pow (10.0, 6.0 / 20.0) - 1.0) + 0.0; // gain of the added part for a 6 dB bell at its centre, ~ +5.98 dB re the input
    (void) curveOut;
    check (std::fabs (curveGain - expectedAdd) < 0.1, fmt ("band solo (CURVE): a +6 dB bell at its centre adds %.2f dB re the input, i.e. 0.995 of it (want %.2f)", curveGain, expectedAdd));
    const double otherBand = probe (curve, SoloKind::curve, 0, 5000.0).first;
    check (otherBand < -12.0, fmt ("... and little of the other band's range (%.0f dB at 5 kHz, where only the first band's own skirt is left)", otherBand));

    // A curve solo of a Side band is the side channel's change, in both ears with opposite signs.
    {
        EngineParams sideBand;
        setBand (sideBand, 0, BandType::bell, 1000.0, 6.0, 1.0, Placement::side);
        Engine e;
        e.prepare (fs, 512);
        e.monitor.soloBand = 0;
        e.monitor.soloKind = (int) SoloKind::curve;
        const auto in = sine (1000.0, 0.2, oneSecond + 12000);
        const auto out = run (e, sideBand, in, scaled (in, -1.0));
        check (std::fabs (out.l[40000] + out.r[40000]) < 1.0e-6f && maxAbs (out.l, 30000, 4000) > 0.1, "curve solo of a Side band: left and right are opposites");
    }

    // A band that is not on cannot be soloed; releasing a solo restores the signal exactly.
    {
        EngineParams only;
        setBand (only, 2, BandType::bell, 500.0, 5.0);
        Engine e;
        e.prepare (fs, 512);
        e.monitor.soloBand = 4; // not on
        const auto in = noise (11, 0.3, 20000);
        const auto a = run (e, only, in, in);
        Engine ref;
        ref.prepare (fs, 512);
        check (identical (a, run (ref, only, in, in)), "soloing a band that is off changes nothing");
        e.monitor.soloBand = 2;
        run (e, only, in, in);
        e.monitor.soloBand = -1;
        run (e, only, in, in);
        const auto restored = run (e, only, in, in);
        Engine ref2;
        ref2.prepare (fs, 512);
        run (ref2, only, in, in); run (ref2, only, in, in);
        check (identical (restored, run (ref2, only, in, in)), "after a band solo is released the output is the plain output again, bit for bit");
    }
}

void testRobust()
{
    std::printf("\n[9] Robustness\n");
    const auto l = noise (21, 0.9, 40000), r = noise (22, 0.9, 40000);

    // Block-size independence.
    {
        EngineParams p;
        setBand (p, 0, BandType::lowCut, 40.0, 0.0, 1.0, Placement::stereo, 3);
        setBand (p, 1, BandType::bell, 300.0, 7.0, 2.0, Placement::mid);
        setBand (p, 2, BandType::highShelf, 6000.0, 4.0, 0.7, Placement::side);
        setBand (p, 3, BandType::bell, 2500.0, 0.0, 3.0);
        p.bands[3].dynamic = true;
        p.bands[3].dyn = { DynMode::above, -34.0, 6.0, -9.0, 3.0, 90.0 };
        p.sat = true;
        p.match = true;
        p.outGainDb = -2.0;
        Engine a, b, c;
        a.prepare (fs, 4096); b.prepare (fs, 64); c.prepare (fs, 1);
        a.monitor.delta = b.monitor.delta = c.monitor.delta = false;
        const auto big = run (a, p, l, r, 4096), small = run (b, p, l, r, 37), single = run (c, p, l, r, 1);
        double d1 = 0.0, d2 = 0.0;
        size_t first = 0;
        for (size_t i = 0; i < big.l.size(); ++i)
        {
            const double a1 = std::fabs (big.l[i] - small.l[i]), a2 = std::fabs (big.l[i] - single.l[i]);
            if (a1 > 0.0 && d1 == 0.0) first = i;
            d1 = std::max (d1, a1);
            d2 = std::max (d2, a2);
        }
        check (identical (big, small) && identical (big, single), fmt ("the output is the same whether the host sends blocks of 4096, 37 or 1 samples (largest difference %.1e / %.1e, first at %zu)", d1, d2, first));
    }

    // Extremes at every sample rate.
    for (double rate : { 22050.0, 44100.0, 48000.0, 96000.0, 192000.0 })
    {
        EngineParams p;
        setBand (p, 0, BandType::lowCut, 20.0, 0.0, 1.0, Placement::stereo, 3);
        setBand (p, 1, BandType::bell, 20.0, 24.0, 18.0);
        setBand (p, 2, BandType::highShelf, 20000.0, 24.0, 0.1);
        setBand (p, 3, BandType::highCut, 20000.0, 0.0, 1.0, Placement::mid, 3);
        setBand (p, 4, BandType::bell, 19000.0, -24.0, 18.0, Placement::side);
        setBand (p, 5, BandType::lowShelf, 20.0, 24.0, 18.0);
        setBand (p, 6, BandType::notch, 20000.0, 0.0, 18.0);
        setBand (p, 7, BandType::bell, 50.0, 12.0, 0.1);
        for (int i : { 1, 2, 5, 7 })
        {
            p.bands[(size_t) i].dynamic = true;
            p.bands[(size_t) i].dyn = { DynMode::above, -60.0, 20.0, -24.0, 0.1, 5.0 };
        }
        p.sat = true;
        p.driveDb = 30.0;
        p.match = true;
        p.outGainDb = 24.0;
        p.crossover = { 20.0, 21.0, 22.0, 20000.0, 20000.0 };
        Engine e;
        e.prepare (rate, 256);
        e.monitor.regions = 0b10101;
        const int n = (int) rate;
        std::vector<float> in = noise (23, 0.9, n);
        const auto out = run (e, p, in, in, 256);
        check (allFinite (out, 100000.0), fmt ("%.0f Hz: every band at its extreme with SAT, dynamics and a region solo stays finite", rate));
    }

    // A bad sample must not poison the filters for good.
    {
        EngineParams p;
        setBand (p, 0, BandType::bell, 300.0, 6.0);
        setBand (p, 1, BandType::lowCut, 30.0);
        p.bands[0].dynamic = true;
        auto in = noise (24, 0.3, 30000);
        in[5000] = std::nanf ("");
        in[5001] = INFINITY;
        const auto out = runSame (p, in);
        check (std::isfinite (out.l.back()) && std::isfinite (out.r.back()) && maxAbs (out.l, 20000, 10000) < 2.0, "a NaN and an inf in the input do not leave the output broken");
    }

    // Moving settings and switching bands does not click.
    {
        EngineParams p;
        setBand (p, 0, BandType::bell, 100.0, 0.0, 1.0);
        Engine e;
        e.prepare (fs, 512);
        const auto in = sine (100.0, 0.2, oneSecond * 2);
        double widest = 0.0;
        const auto step = [&] (int from, int to)
        {
            const auto chunk = std::vector<float> (in.begin() + from, in.begin() + to);
            const auto out = run (e, p, chunk, chunk);
            for (size_t i = 1; i < out.l.size(); ++i)
                widest = std::max (widest, (double) std::fabs (out.l[i] - out.l[i - 1]));
        };
        step (0, 24000);
        p.bands[0].gainDb = 12.0;       // jumps from 0 to +12 dB
        step (24000, 48000);
        p.bands[0].on = false;          // switched off while boosting
        step (48000, 72000);
        p.bands[0].on = true;           // and on again
        p.bands[0].freq = 4000.0;
        step (72000, 96000);
        check (widest < 0.02, fmt ("a 12 dB jump, an off/on and a frequency jump never step by more than %.4f (a click would be over 0.05)", widest));
    }

    // Mono.
    {
        EngineParams p;
        setBand (p, 0, BandType::bell, 1000.0, 6.0, 1.0, Placement::mid);
        setBand (p, 1, BandType::bell, 1000.0, 6.0, 1.0, Placement::side);
        Engine e;
        e.prepare (fs, 512);
        const auto in = sine (1000.0, 0.1, oneSecond + 12000);
        const auto out = runMono (e, p, in);
        check (std::fabs (toDb (amplitudeAt (out.l, 1000.0, 12000, oneSecond) / amplitudeAt (in, 1000.0, 12000, oneSecond)) - 6.0) < 0.05, "a one-channel buffer is treated as mid: the Mid band applies, the Side band does nothing");
    }

    // Cost.
    {
        EngineParams p;
        for (int i = 0; i < maxBands; ++i)
        {
            setBand (p, i, i % 2 ? BandType::bell : BandType::highShelf, 200.0 * (i + 1), 3.0, 1.5, (Placement) (i % 5));
            p.bands[(size_t) i].dynamic = true;
        }
        p.sat = true;
        Engine e;
        e.prepare (fs, 512);
        const auto in = noise (25, 0.3, oneSecond * 10);
        const auto t0 = std::chrono::steady_clock::now();
        run (e, p, in, in);
        const double secs = std::chrono::duration<double> (std::chrono::steady_clock::now() - t0).count();
        std::printf ("  8 dynamic bands, SAT, all placements: %.2f s for 10 s of audio -> %.1f %% of one core\n", secs, secs * 10.0);
        check (secs < 3.0, "the worst case runs in real time with a wide margin");
    }
}
// Settings that change while the engine runs must end up exactly where a fresh start with them would.
void testLiveChanges()
{
    std::printf("\n[10] Settings changed while running behave as if they had been there from the start\n");

    const auto slice = [] (const std::vector<float>& x, int from, int to) { return std::vector<float> (x.begin() + from, x.begin() + to); };
    const auto sum = [] (std::vector<float> a, const std::vector<float>& b)
    {
        for (size_t i = 0; i < a.size(); ++i)
            a[i] += b[i];
        return a;
    };
    // Runs `first` for samples [0, split) and `second` from there on, on one engine.
    const auto runChanged = [&] (const EngineParams& first, const EngineParams& second, int split, const std::vector<float>& l, const std::vector<float>& r)
    {
        Engine e;
        e.prepare (fs, 512);
        const int n = (int) l.size();
        auto a = run (e, first, slice (l, 0, split), slice (r, 0, split));
        const auto b = run (e, second, slice (l, split, n), slice (r, split, n));
        a.l.insert (a.l.end(), b.l.begin(), b.l.end());
        a.r.insert (a.r.end(), b.r.begin(), b.r.end());
        return a;
    };

    // A cut's slope changed on a running band.
    {
        EngineParams p;
        setBand (p, 0, BandType::lowCut, 400.0, 0.0, 1.0, Placement::stereo, 0);
        Engine e;
        e.prepare (fs, 512);
        const auto tone = sine (200.0, 0.5, oneSecond + 12000);
        run (e, p, tone, tone);
        p.bands[0].slope = 3;
        const auto out = run (e, p, tone, tone);
        const double g = toDb (amplitudeAt (out.l, 200.0, 12000, oneSecond) / amplitudeAt (tone, 200.0, 12000, oneSecond));
        check (std::fabs (g + 48.18) < 0.5, fmt ("a low cut moved from 12 to 48 dB/oct while running: %.2f dB at half the corner (want -48.18)", g));
    }

    // Dynamic switched on while the band runs: its detector must listen to the band's own region, not to everything.
    {
        const int n = oneSecond * 2;
        const auto loudBass = sine (100.0, 0.3, n), quietMid = sine (1000.0, 0.003, n);
        const auto in = sum (loudBass, quietMid);
        EngineParams p;
        setBand (p, 0, BandType::bell, 1000.0, 0.0, 1.0);
        const DynamicsParams below { DynMode::below, -30.0, 20.0, 6.0, 5.0, 50.0 };

        auto measure = [&] (const Stereo& out)
        {
            return toDb (amplitudeAt (out.l, 1000.0, n - oneSecond, oneSecond) / amplitudeAt (in, 1000.0, n - oneSecond, oneSecond));
        };
        EngineParams dynamicFromStart = p;
        dynamicFromStart.bands[0].dynamic = true;
        dynamicFromStart.bands[0].dyn = below;
        const double reference = measure (runSame (dynamicFromStart, in));

        EngineParams later = p;
        later.bands[0].dynamic = true;
        later.bands[0].dyn = below;
        const double switchedOn = measure (runChanged (p, later, oneSecond / 2, in, in));
        check (std::fabs (reference - 6.0) < 0.3 && std::fabs (switchedOn - reference) < 0.3,
               fmt ("Below, loud bass and a quiet 1 kHz tone: %+.2f dB when dynamic from the start, %+.2f dB when switched on while running", reference, switchedOn));

        // Above: nothing in the band's own region is over the threshold, so the gain must not move at all, not even for the first few ms.
        EngineParams above = p;
        above.bands[0].dynamic = true;
        above.bands[0].dyn = { DynMode::above, -30.0, 20.0, -12.0, 5.0, 50.0 };
        Engine e;
        e.prepare (fs, 512);
        run (e, p, slice (in, 0, oneSecond / 2), slice (in, 0, oneSecond / 2));
        double deepest = 0.0;
        for (int start = oneSecond / 2; start < oneSecond / 2 + 4800; start += 32)
        {
            run (e, above, slice (in, start, start + 32), slice (in, start, start + 32), 32);
            deepest = std::min (deepest, (double) e.dynamicDb[0].load());
        }
        check (deepest > -0.05, fmt ("a band made dynamic while running listens to its own region from the first tick (gain moved by at most %.2f dB)", std::fabs (deepest)));
    }

    // Changing the sample rate: prepare() must leave nothing of the old rate behind (the region crossovers are set only when they change).
    {
        EngineParams p;
        setBand (p, 0, BandType::bell, 500.0, 6.0, 1.0);
        const auto in = sine (200.0, 0.3, 96000);
        Engine fresh;
        fresh.prepare (96000.0, 512);
        fresh.monitor.regions = 1u;
        const auto want = run (fresh, p, in, in);

        Engine moved;
        moved.prepare (44100.0, 512);
        moved.monitor.regions = 1u;
        run (moved, p, in, in);
        moved.prepare (96000.0, 512);
        check (identical (run (moved, p, in, in), want), "a region solo after prepare() at a new sample rate is the same as on a fresh engine");
    }

    // A change that reaches the engine in the middle of a tick waits for the next tick, like everything else that varies.
    {
        const auto l = noise (41, 0.5, 20000), r = noise (42, 0.5, 20000);
        EngineParams a;
        setBand (a, 0, BandType::bell, 1000.0, 6.0, 1.0);
        EngineParams b = a;
        b.bands[0].place = Placement::mid;
        check (identical (runChanged (a, b, 6416, l, r), runChanged (a, b, 6432, l, r)),
               "a placement change arriving 16 samples into a tick takes effect at the tick, not before");

        EngineParams c = a;
        c.sat = true;
        c.driveDb = 24.0;
        c.style = SatStyle::tape;
        EngineParams d = c;
        d.style = SatStyle::tube;
        check (identical (runChanged (c, d, 6416, l, r), runChanged (c, d, 6432, l, r)),
               "... and so does a change of saturation style");
    }

    // Gain Match works both ways: a band that takes most of the level away is made up, but never by more than 12 dB.
    {
        EngineParams p;
        setBand (p, 0, BandType::highCut, 200.0, 0.0, 1.0, Placement::stereo, 3);
        p.match = true;
        Engine e;
        e.prepare (fs, 512);
        const auto bed = noise (43, 0.2, oneSecond * 6);
        run (e, p, bed, bed);
        check (e.matchDb.load() > 11.8 && e.matchDb.load() < 12.1, fmt ("GAIN MATCH makes up at most 12 dB (%.2f dB)", e.matchDb.load()));
    }

    // What a dynamic band hears is the level of its own channel, as the peak of a sine, whatever it is placed on.
    {
        const int n = oneSecond;
        const auto tone = sine (1000.0, std::pow (10.0, -20.0 / 20.0), n), silence = std::vector<float> ((size_t) n, 0.0f);
        const struct { Placement place; const std::vector<float>* l; const std::vector<float>* r; const char* name; } cases[] =
        {
            { Placement::stereo, &tone, &tone, "Stereo" }, { Placement::left, &tone, &silence, "Left" }, { Placement::right, &silence, &tone, "Right" },
            { Placement::mid, &tone, &tone, "Mid" }, { Placement::side, &tone, nullptr, "Side" },
        };
        std::string line;
        bool ok = true;
        for (const auto& c : cases)
        {
            EngineParams p;
            setBand (p, 0, BandType::bell, 1000.0, 0.0, 1.0, c.place);
            p.bands[0].dynamic = true;
            p.bands[0].dyn.thresholdDb = -60.0;
            Engine e;
            e.prepare (fs, 512);
            const auto negated = scaled (tone, -1.0);
            run (e, p, *c.l, c.r != nullptr ? *c.r : negated);
            const double level = e.detectedDb[0].load();
            ok = ok && std::fabs (level + 20.0) < 0.3;
            line += fmt (" %s %.2f;", c.name, level);
        }
        check (ok, "a dynamic band reads a -20 dB sine as -20 dB on every placement:" + line);
    }
}
void testDynamicsUsage()
{
    std::printf ("\n[11] Dynamics usage: a dynamic band that never moves, or never lets go, is named\n");
    const DynamicsParams d; // threshold -24, ratio 4, range -6
    const auto run = [&] (auto levelAt)
    {
        DynamicsUsage u;
        for (int i = 0; i < 30 * 10; ++i) // ten seconds of the editor's 30 Hz timer
        {
            const double level = levelAt (i);
            u.push (level, dynamicOffsetDb (d, level), d.rangeDb, 1.0 / 30.0);
        }
        return u.summary();
    };
    const auto quiet = run ([] (int) { return -40.0; });
    const auto loud = run ([] (int) { return 0.0; });
    const auto bursts = run ([] (int i) { return i % 30 < 5 ? -10.0 : -40.0; });
    const auto silent = run ([] (int) { return -120.0; });
    check (quiet.verdict == DynamicsUsage::Verdict::idle && std::fabs (quiet.maxLevelDb + 40.0) < 1.0e-9,
           fmt ("16 dB under the threshold all the time: idle, loudest %.1f dB", quiet.maxLevelDb));
    check (loud.verdict == DynamicsUsage::Verdict::pinned, "24 dB over the threshold all the time: pinned at its range");
    check (bursts.verdict == DynamicsUsage::Verdict::working, "bursts over the threshold between quiet passages: working");
    check (silent.verdict == DynamicsUsage::Verdict::measuring, "digital silence (a stopped transport) gives no verdict");
}
} // namespace

void testResonances()
{
    std::printf ("\n[12] Resonance finder: narrow peaks that stand out, not broad tilts or silence\n");
    const double binHz = 48000.0 / 8192.0;
    const auto spectrum = [&] (double f0, double peakDb, double q)
    {
        std::vector<float> db (4096);
        for (size_t i = 0; i < db.size(); ++i)
        {
            const double f = std::max (1.0, (double) i * binHz);
            const double x = q * (f / f0 - f0 / f);
            db[i] = (float) (-40.0 - 3.0 * std::log2 (f / 1000.0) + peakDb / (1.0 + x * x)); // a pink-ish tilt plus the peak
        }
        return db;
    };
    const auto run = [&] (const std::vector<float>& db)
    {
        ResonanceFinder r;
        r.reset();
        for (int i = 0; i < 90; ++i)
            r.update (db.data(), (int) db.size(), binHz, 1.0f / 30.0f);
        return r.find();
    };
    const auto ring = run (spectrum (1000.0, 10.0, 8.0));
    check (! ring.empty() && std::fabs (std::log2 (ring[0].freqHz / 1000.0)) < 1.0 / 24.0 && ring[0].prominenceDb > 7.0f && ring[0].q > 4.0 && ring[0].q < 16.0,
           fmt ("a +10 dB, Q 8 peak at 1 kHz: found at %.0f Hz, %.1f dB above its neighbourhood, Q %.1f",
                ring.empty() ? 0.0 : ring[0].freqHz, ring.empty() ? 0.0 : ring[0].prominenceDb, ring.empty() ? 0.0 : ring[0].q));
    check (run (spectrum (1000.0, 6.0, 0.7)).empty(), "a broad +6 dB bump (Q 0.7) is tone, not a resonance");
    check (run (spectrum (1000.0, 0.0, 1.0)).empty(), "a smooth spectrum has none");
    std::vector<float> silent (4096, -140.0f);
    check (run (silent).empty(), "silence (a stopped transport) reports none");
}

void testMasking()
{
    std::printf ("\n[13] Masking: where this track and the sidechain fight for the same frequencies\n");
    const double binHz = 48000.0 / 8192.0;
    // Levels per bin: a floor, plus +20 dB inside [lo, hi].
    const auto shape = [&] (double lo, double hi, float floorDb, float boostDb)
    {
        std::vector<float> db (4096);
        for (size_t i = 0; i < db.size(); ++i)
        {
            const double f = (double) i * binHz;
            db[i] = floorDb + (f >= lo && f <= hi ? boostDb : 0.0f);
        }
        return db;
    };
    const auto run = [&] (const std::vector<float>& own, const std::vector<float>& other)
    {
        MaskingMeter m;
        for (int i = 0; i < 90; ++i)
            m.update (own.data(), other.data(), (int) own.size(), binHz, 1.0f / 30.0f);
        return m.clashes();
    };
    // This track: loud 200 - 500 Hz and 3 - 6 kHz. The other: loud 200 - 500 Hz only, 2 dB louder there.
    auto own = shape (200.0, 500.0, -70.0f, 30.0f);
    const auto top = shape (3000.0, 6000.0, 0.0f, 30.0f);
    for (size_t i = 0; i < own.size(); ++i)
        own[i] = std::max (own[i], top[i] - 70.0f);
    const auto other = shape (200.0, 500.0, -70.0f, 32.0f);
    const auto c = run (own, other);
    check (c.size() == 1 && c[0].loHz > 150.0 && c[0].hiHz < 700.0,
           fmt ("one clash zone, in the shared low mids: %.0f - %.0f Hz (%d zones)", c.empty() ? 0.0 : c[0].loHz, c.empty() ? 0.0 : c[0].hiHz, (int) c.size()));
    if (! c.empty())
        check (std::fabs (c[0].cutDb + 4.0f) < 0.5f, fmt ("the cut puts the other track 6 dB clear: %.1f dB (2 dB louder already, so -4)", c[0].cutDb));
    check (run (own, shape (8000.0, 12000.0, -70.0f, 30.0f)).empty(), "tracks loud in different ranges do not clash");
    std::vector<float> silent (4096, -140.0f);
    check (run (own, silent).empty(), "a silent sidechain reports nothing");
}

void testReferenceMatch()
{
    std::printf ("\n[14] Reference: the long-term shape of a track, and the bands that make one follow another\n");
    const double fs = 48000.0, binHz = fs / 8192.0;
    // A spectrum in dB per bin: a slope of `slopeDbPerOct` around 1 kHz plus an offset, and whatever `extra` adds at that frequency.
    const auto spectrum = [&] (float slopeDbPerOct, float offsetDb, const std::function<float (double)>& extra = {})
    {
        std::vector<float> db (4096);
        for (size_t i = 1; i < db.size(); ++i)
        {
            const double f = (double) i * binHz;
            db[i] = offsetDb + slopeDbPerOct * (float) std::log2 (f / 1000.0) + (extra ? extra (f) : 0.0f);
        }
        db[0] = db[1];
        return db;
    };
    const auto profileOf = [&] (const std::vector<float>& db, float seconds)
    {
        SpectrumProfile p;
        for (int i = 0; i < (int) (seconds * 30.0f); ++i)
            p.update (db.data(), (int) db.size(), binHz, 1.0f / 30.0f);
        return p;
    };
    const auto shapeOf = [&] (const SpectrumProfile& p, ShapeMask& valid)
    {
        Shape s {};
        check (p.shape (fs * 0.45, s, valid), "a profile with enough audio gives a shape");
        return s;
    };

    // The shape has the level taken out and keeps the slope.
    ShapeMask valid {}, valid2 {};
    const auto quiet = shapeOf (profileOf (spectrum (-3.0f, -60.0f), 5.0f), valid);
    const auto loud = shapeOf (profileOf (spectrum (-3.0f, -40.0f), 5.0f), valid2);
    float worst = 0.0f;
    for (size_t i = 0; i < quiet.size(); ++i)
        if (valid[i] && valid2[i])
            worst = std::max (worst, std::fabs (quiet[i] - loud[i]));
    check (worst < 0.01f, fmt ("the same shape at two levels agrees to %.4f dB", worst));
    double anchor = 0.0;
    int na = 0;
    for (int i = 0; i < ProfileGrid::kBands; ++i)
        if (ProfileGrid::inAnchor (i))
        {
            anchor += quiet[(size_t) i];
            ++na;
        }
    check (std::fabs (anchor / na) < 0.01, fmt ("the mean over 200 Hz - 5 kHz is 0 dB: %.4f", anchor / na));
    const double slope = quiet[(size_t) ProfileGrid::kBands - 10] - quiet[(size_t) ProfileGrid::kBands - 22]; // two octaves apart
    check (std::fabs (slope + 6.0) < 0.7, fmt ("a -3 dB/octave tilt reads -6 dB over two octaves: %.2f dB", slope));
    check (! valid[(size_t) ProfileGrid::kBands - 1] || ProfileGrid::centreHz (ProfileGrid::kBands - 1) <= fs * 0.45, "bands the sample rate cannot carry are not valid");

    // Silence is not averaged, and a short listen is not a profile.
    SpectrumProfile p;
    const std::vector<float> silent (4096, -140.0f);
    for (int i = 0; i < 300; ++i)
        p.update (silent.data(), (int) silent.size(), binHz, 1.0f / 30.0f);
    check (! p.hasData() && p.seconds() < 1.0e-9, "silence adds nothing");
    Shape sh {};
    ShapeMask vm {};
    p = profileOf (spectrum (0.0f, -50.0f), 2.0f);
    check (! p.hasData() && ! p.shape (fs * 0.45, sh, vm), "two seconds is not enough");

    // The reference as text, and a curve from another plugin on another grid.
    Shape round {};
    check (shapeFromText (shapeToText (quiet), round), "the reference survives the session text");
    float roundWorst = 0.0f;
    for (size_t i = 0; i < round.size(); ++i)
        roundWorst = std::max (roundWorst, std::fabs (round[i] - quiet[i]));
    check (roundWorst <= 0.05f, fmt ("... to the one decimal it keeps: %.3f dB", roundWorst));
    check (! shapeFromText ("1,2,3", round) && ! shapeFromText ("", round), "text with the wrong number of points is refused");
    pdsp::LinkCurve curve;
    curve.f0Hz = 25.0;
    curve.perOctave = 3.0;
    curve.name = "Modern Pop";
    for (int i = 0; i < 30; ++i)
        curve.db.push_back (4.0 - 0.5 * i + (i == 15 ? 3.0 : 0.0));
    std::vector<pdsp::LinkBand> bands;
    pdsp::LinkBand b;
    b.kind = pdsp::LinkBand::Kind::bell;
    b.freq = 300.0;
    b.gainDb = -2.0;
    bands.push_back (b);
    const auto text = pdsp::link::encode (bands, &curve);
    std::vector<pdsp::LinkBand> back;
    pdsp::LinkCurve backCurve;
    check (pdsp::link::decode (text, back, &backCurve) && back.size() == 1 && backCurve.valid() && backCurve.name == "Modern Pop"
               && backCurve.db.size() == curve.db.size() && std::fabs (backCurve.f0Hz - 25.0) < 1.0e-9 && std::fabs (backCurve.perOctave - 3.0) < 1.0e-9,
           "a curve line travels with the bands, name and grid included");
    check (pdsp::link::decode (text, back) && back.size() == 1, "a reader that does not know curves reads the bands and skips the line");
    check (pdsp::link::decode (pdsp::link::kHeader + std::string ("\ncurve f0=20 oct=6 db=1,2,x"), back, &backCurve) && ! backCurve.valid(), "a curve with a bad number is dropped");
    Shape fromCurve {};
    check (shapeFromCurve (curve, fromCurve), "a curve becomes a shape");
    double ca = 0.0;
    for (int i = 0; i < ProfileGrid::kBands; ++i)
        if (ProfileGrid::inAnchor (i))
            ca += fromCurve[(size_t) i];
    check (std::fabs (ca / na) < 1.0e-4, "... levelled at the anchor");

    // Matching. The reference differs from the track by a bell at 3 kHz and a low shelf; the match finds both.
    const auto live = shapeOf (profileOf (spectrum (-3.0f, -50.0f), 5.0f), valid);
    const auto withBands = [&] (const std::vector<match::Band>& made)
    {
        std::vector<float> db;
        Shape ref = live;
        for (int i = 0; i < ProfileGrid::kBands; ++i)
            for (const auto& m : made)
            {
                BandParams bp;
                bp.type = m.type;
                bp.freq = m.freqHz;
                bp.q = m.q;
                bp.gainDb = m.gainDb;
                ref[(size_t) i] += (float) bandResponseDb (bp, 0.0, fs, ProfileGrid::centreHz (i));
            }
        return ref;
    };
    const auto bellRef = withBands ({ { BandType::bell, 3000.0, 6.0, 1.4 } });
    const auto found = match::matchBands (live, bellRef, valid, fs);
    check (! found.empty() && found[0].type == BandType::bell && std::fabs (std::log2 (found[0].freqHz / 3000.0)) < 0.35 && found[0].gainDb > 4.0 && found[0].gainDb < 7.5,
           fmt ("a +6 dB bell at 3 kHz is found: %s at %.0f Hz, %+.1f dB, Q %.2f", found.empty() ? "nothing" : found[0].type == BandType::bell ? "bell" : "other",
                found.empty() ? 0.0 : found[0].freqHz, found.empty() ? 0.0 : found[0].gainDb, found.empty() ? 0.0 : found[0].q));
    const float before = match::remainingDb (live, bellRef, valid, fs, {}), after = match::remainingDb (live, bellRef, valid, fs, found);
    check (before > 1.5f && after < 0.5f * before, fmt ("it takes the difference from %.2f dB to %.2f dB (RMS over 30 Hz - 18 kHz)", before, after));

    const auto shelfRef = withBands ({ { BandType::lowShelf, 120.0, -5.0, 0.7071 }, { BandType::bell, 800.0, -3.0, 1.0 } });
    const auto two = match::matchBands (live, shelfRef, valid, fs);
    bool shelf = false, dip = false;
    for (const auto& m : two)
    {
        shelf |= m.type == BandType::lowShelf && m.gainDb < -3.0 && m.gainDb > -7.0;
        dip |= m.type == BandType::bell && m.gainDb < -1.5 && std::fabs (std::log2 (m.freqHz / 800.0)) < 0.4;
    }
    const float afterTwo = match::remainingDb (live, shelfRef, valid, fs, two);
    check (shelf && dip, fmt ("a low shelf of -5 dB and a -3 dB dip at 800 Hz are both found (%d bands)", (int) two.size()));
    check (afterTwo < 0.8f, fmt ("... leaving %.2f dB", afterTwo));
    check (std::is_sorted (two.begin(), two.end(), [] (const match::Band& x, const match::Band& y) { return x.freqHz < y.freqHz; }), "bands come back in frequency order");

    // Nothing to match, nothing past the limits.
    check (match::matchBands (live, live, valid, fs).empty(), "the same shape needs no bands");
    Shape wild = live;
    for (int i = 0; i < ProfileGrid::kBands; ++i)
        wild[(size_t) i] += (i % 2 ? 9.0f : -9.0f) * (i % 7 == 0 ? 2.0f : 1.0f);
    const auto many = match::matchBands (live, wild, valid, fs, 4);
    bool inLimits = (int) many.size() <= 4;
    for (const auto& m : many)
        inLimits &= std::fabs (m.gainDb) <= match::kMaxDb + 0.05 && m.freqHz >= match::kLoHz && m.freqHz <= match::kHiHz && m.q >= match::kMinQ && m.q <= match::kMaxQ;
    check (inLimits, fmt ("a ragged reference gives at most the bands asked for (%d), each inside the limits", (int) many.size()));
    ShapeMask none {};
    check (match::matchBands (live, bellRef, none, fs).empty(), "no valid band, no match");
}

int main()
{
    testTransparent();
    testBell();
    testCuts();
    testPlacement();
    testDynamics();
    testSaturation();
    testDelta();
    testSolo();
    testRobust();
    testLiveChanges();
    testDynamicsUsage();
    testResonances();
    testMasking();
    testReferenceMatch();
    return finish ("IrisTests");
}
