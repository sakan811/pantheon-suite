// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 The Pantheon authors

// Stress scenarios for the pitch corrector: large vibrato, slow drift, sagging note ends.
// Every scenario knows which note the singer meant, so we can score what the corrector decided.
//   stress_corrector            run all scenarios and print a table
//   stress_corrector --csv dir  also write one CSV per scenario (t, in, out, target, truth)
//   stress_corrector --dwell ms try the note-switch dwell
//   stress_corrector --leap     run with LEAP HOLD on (the hold shrinks with the jump)

#include "../Source/dsp/PitchCorrector.h"
#include "TestSignals.h"

#include <cstdlib>
#include <cstring>
#include <string>

using namespace testsig;
using harmonia::CorrectorParams;
using harmonia::PitchCorrector;

namespace
{
struct Note
{
    double midi;    // the note meant
    double seconds; // how long it is held
    double offset;  // how far off the singer is (cents), constant
    double vibDepth;// cents (zero-to-peak)
    double vibRate; // Hz
    double drift;   // cents per second, linear over the note
    double sagCents;// extra fall-off over the last 0.3 s (negative = flat)
    double gap;     // silence after the note (seconds)
};

struct Scenario
{
    const char* name;
    std::vector<Note> notes;
    uint16_t mask;
    float natural;
};

struct Frame
{
    double t;
    bool voiced;
    double midi;  // sung
    double truth; // note meant
    bool steady;  // away from the note edges: the corrector is expected to be right here
    int note;     // index of the note meant
};

std::vector<Frame> render (const Scenario& s, unsigned seed)
{
    std::mt19937 rng (seed);
    std::normal_distribution<double> nd (0.0, 1.0);
    std::vector<Frame> out;
    const double dt = 0.004;
    double t = 0.0;
    double prevPitch = 0.0;
    bool first = true;
    int noteIndex = -1;

    for (const Note& n : s.notes)
    {
        ++noteIndex;
        // The vibrato rate and depth wander the way a real singer's do.
        double ratePhase = 0.0, rateWander = 0.0, depthWander = 0.0;
        double driftWalk = 0.0;
        const int steps = (int) std::lround (n.seconds / dt);
        for (int i = 0; i < steps; ++i, t += dt)
        {
            const double tn = i * dt;
            rateWander += (0.02 * nd (rng) - 0.04 * rateWander);
            depthWander += (0.02 * nd (rng) - 0.04 * depthWander);
            driftWalk += 0.4 * nd (rng) - 0.01 * driftWalk;
            ratePhase += 2.0 * kPi * n.vibRate * (1.0 + 0.5 * rateWander) * dt;

            const double onset = std::clamp ((tn - 0.25) / 0.35, 0.0, 1.0); // vibrato grows in
            const double vib = n.vibDepth * onset * (1.0 + 0.8 * depthWander) * std::sin (ratePhase);
            const double sag = tn > n.seconds - 0.3 ? n.sagCents * (tn - (n.seconds - 0.3)) / 0.3 : 0.0;
            const double scoop = tn < 0.12 ? -80.0 * (1.0 - tn / 0.12) : 0.0; // arrive from below
            double pitch = n.midi * 100.0 + n.offset + n.drift * tn + driftWalk + sag + scoop + vib + 3.0 * nd (rng);

            // Slide in from the previous note over 60 ms when there is no gap.
            if (! first && prevPitch > 0.0 && tn < 0.06)
                pitch = prevPitch + (pitch - prevPitch) * (tn / 0.06);

            Frame f;
            f.t = t;
            f.voiced = true;
            f.midi = pitch / 100.0;
            f.truth = n.midi;
            f.steady = tn > 0.35 && tn < n.seconds - 0.05;
            f.note = noteIndex;
            out.push_back (f);
            prevPitch = pitch;
        }
        first = false;
        const int gapSteps = (int) std::lround (n.gap / dt);
        for (int i = 0; i < gapSteps; ++i, t += dt)
            out.push_back ({ t, false, 0.0, n.midi, false, noteIndex });
        if (gapSteps > 0)
            prevPitch = 0.0;
    }
    return out;
}

struct Score
{
    double wrongPct = 0, flips = 0, centreErr = 0, depthRatio = 0, maxJump = 0;
};

Score run (const Scenario& s, unsigned seed, const char* csvPath, float dwellMs, bool leap)
{
    const auto frames = render (s, seed);
    PitchCorrector pc;
    pc.prepare (0.004);
    CorrectorParams cp;
    cp.noteMask = s.mask;
    cp.retuneMs = 40.0f;
    cp.natural = s.natural;
    cp.dwellMs = dwellMs;
    cp.leapHold = leap;

    FILE* csv = csvPath ? std::fopen (csvPath, "w") : nullptr;
    if (csv)
        std::fprintf (csv, "t,in,out,target,truth,voiced,centre\n");

    int steadyN = 0, wrong = 0, flips = 0;
    double lastTarget = -1.0;
    std::vector<double> inDev, outDev;
    double maxJump = 0.0, lastOut = 0.0;
    std::vector<double> noteSum (s.notes.size(), 0.0);
    std::vector<int> noteN (s.notes.size(), 0);
    for (const Frame& f : frames)
    {
        const auto o = pc.process (f.voiced, f.voiced ? (float) midiToHz (f.midi) : 0.0f, cp);
        if (csv)
            std::fprintf (csv, "%.4f,%.3f,%.3f,%.3f,%.3f,%d,%.3f\n", f.t, f.voiced ? f.midi : 0.0, o.active ? o.outMidi : 0.0, o.active ? o.targetMidi : 0.0, f.truth, f.voiced, pc.centreCents() / 100.0);
        if (f.voiced && o.active && f.steady)
        {
            ++steadyN;
            if (std::fabs (o.targetMidi - f.truth) > 0.01)
                ++wrong;
            if (lastTarget >= 0.0 && o.targetMidi != lastTarget)
                ++flips;
            noteSum[(size_t) f.note] += (o.outMidi - f.truth) * 100.0;
            ++noteN[(size_t) f.note];
            inDev.push_back ((f.midi - f.truth) * 100.0);
            outDev.push_back ((o.outMidi - f.truth) * 100.0);
            if (lastOut > 0.0)
                maxJump = std::max (maxJump, std::fabs (o.outMidi - lastOut) * 100.0);
            lastOut = o.outMidi;
        }
        else
            lastOut = 0.0;
        lastTarget = f.voiced && o.active && f.steady ? o.targetMidi : -1.0;
    }
    if (csv)
        std::fclose (csv);

    Score sc;
    sc.wrongPct = steadyN ? 100.0 * wrong / steadyN : 0.0;
    sc.flips = flips;
    // bias of the output about the note meant, per note (vibrato averages out)
    for (size_t i = 0; i < noteSum.size(); ++i)
        sc.centreErr += noteN[i] ? std::fabs (noteSum[i] / noteN[i]) / (double) noteSum.size() : 0.0;
    const auto sIn = stats (inDev), sOut = stats (outDev);
    // centre-removed spreads: how much of the movement is left
    sc.depthRatio = sIn.stddev > 0.0 ? sOut.stddev / sIn.stddev : 0.0;
    sc.maxJump = maxJump;
    return sc;
}
} // namespace

int main (int argc, char** argv)
{
    std::string csvDir;
    float dwellMs = harmonia::CorrectorParams{}.dwellMs;
    bool leap = false;
    for (int i = 1; i + 1 < argc; ++i)
    {
        if (! std::strcmp (argv[i], "--csv"))
            csvDir = argv[i + 1];
        else if (! std::strcmp (argv[i], "--dwell"))
            dwellMs = (float) std::atof (argv[i + 1]);
    }
    for (int i = 1; i < argc; ++i)
        if (! std::strcmp (argv[i], "--leap"))
            leap = true;

    const uint16_t chromatic = 0x0FFF;
    const uint16_t cMajor = 0x0AB5;

    //          midi  sec  off  vibD vibR drift  sag  gap
    const std::vector<Scenario> scenarios = {
        { "vibrato +-100c @5Hz, chromatic", { { 60, 2.0, 20, 100, 5.0, 0, 0, 0 }, { 64, 2.0, -25, 100, 5.0, 0, 0, 0.3 }, { 62, 2.0, 15, 100, 5.0, 0, 0, 0 } }, chromatic, 0.7f },
        { "vibrato +-150c @4.5Hz, chromatic", { { 60, 2.5, 0, 150, 4.5, 0, 0, 0.3 }, { 65, 2.5, 20, 150, 4.5, 0, 0, 0.3 }, { 62, 2.5, -20, 150, 4.5, 0, 0, 0 } }, chromatic, 0.7f },
        { "vibrato +-100c @5Hz, C major", { { 60, 2.0, 20, 100, 5.0, 0, 0, 0 }, { 64, 2.0, -25, 100, 5.0, 0, 0, 0.3 }, { 67, 2.0, 15, 100, 5.0, 0, 0, 0 } }, cMajor, 0.7f },
        { "slow wide vibrato +-80c @2.5Hz, chromatic", { { 60, 2.5, 10, 80, 2.5, 0, 0, 0.3 }, { 63, 2.5, -10, 80, 2.5, 0, 0, 0.3 } }, chromatic, 0.7f },
        { "vibrato +-60c @6Hz, 38c sharp of the note", { { 60, 2.0, 38, 60, 6.0, 0, 0, 0.3 }, { 62, 2.0, -38, 60, 6.0, 0, 0, 0.3 } }, chromatic, 0.7f },
        { "drift +25c/s, light vibrato, chromatic", { { 60, 2.5, 0, 30, 5.5, 25, 0, 0.3 }, { 64, 2.5, 0, 30, 5.5, -25, 0, 0.3 } }, chromatic, 0.7f },
        { "drift +50c/s, light vibrato, chromatic", { { 60, 2.5, 0, 30, 5.5, 50, 0, 0.3 }, { 64, 2.5, 0, 30, 5.5, -50, 0, 0.3 } }, chromatic, 0.7f },
        { "drift 25c/s + vibrato +-100c, chromatic", { { 60, 2.5, 0, 100, 5.0, 25, 0, 0.3 }, { 64, 2.5, 0, 100, 5.0, -25, 0, 0.3 } }, chromatic, 0.7f },
        { "sag -150c at note end, light vibrato", { { 60, 2.0, 10, 30, 5.5, 0, -150, 0.3 }, { 62, 2.0, -10, 30, 5.5, 0, -150, 0.3 } }, chromatic, 0.7f },
        { "wandering held notes (no vibrato), C major", { { 60, 2.0, 35, 0, 5.0, 12, 0, 0.3 }, { 62, 2.0, -35, 0, 5.0, -12, 0, 0.3 }, { 64, 2.0, 40, 0, 5.0, 0, 0, 0.3 } }, cMajor, 0.7f },
    };

    std::printf ("%-52s %8s %6s %9s %8s %9s\n", "scenario (corrector only, natural 0.7, retune 40 ms)", "wrong%", "flips", "bias c", "depth", "maxstep c");
    double totalWrong = 0.0, totalFlips = 0.0, totalErr = 0.0;
    for (const Scenario& s : scenarios)
    {
        double wrong = 0, flips = 0, err = 0, depth = 0, jump = 0;
        const int seeds = 5;
        for (int seed = 1; seed <= seeds; ++seed)
        {
            std::string path = csvDir.empty() ? "" : csvDir + "/" + std::to_string (&s - scenarios.data()) + ".csv";
            const Score sc = run (s, (unsigned) seed, seed == 1 && ! csvDir.empty() ? path.c_str() : nullptr, dwellMs, leap);
            wrong += sc.wrongPct / seeds;
            flips += sc.flips / seeds;
            err += sc.centreErr / seeds;
            depth += sc.depthRatio / seeds;
            jump = std::max (jump, sc.maxJump);
        }
        std::printf ("%-52s %8.1f %6.1f %9.1f %8.2f %9.1f\n", s.name, wrong, flips, err, depth, jump);
        totalWrong += wrong;
        totalFlips += flips;
        totalErr += err;
    }
    std::printf ("%-52s %8.1f %6.1f %9.1f\n", "TOTAL (lower is better)", totalWrong, totalFlips, totalErr);
    return 0;
}
