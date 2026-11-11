// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 The Pantheon authors

// Offline accuracy tests for the Harmonia DSP core (no JUCE needed).
//   clang++ -std=c++17 -O2 Tests/dsp_test.cpp -o build/dsp_test && build/dsp_test

#include "TestSignals.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <limits>

using namespace testsig;
using harmonia::AutoTuneEngine;
using harmonia::EngineParams;

static EngineParams makeParams (uint16_t mask, float retuneMs, float natural, float strength = 1.0f)
{
    EngineParams p;
    p.corrector.noteMask = mask;
    p.corrector.retuneMs = retuneMs;
    p.corrector.natural = natural;
    p.corrector.strength = strength;
    return p;
}

int main()
{
    const double fs = 48000.0;
    const uint16_t chromatic = 0x0FFF;

    // ------------------------------------------------------------------ 1. steady note
    std::printf ("1. Steady note, 35 cents sharp of A3 (chromatic)\n");
    {
        const double sung = 57.0 + 0.35;
        auto in = makeVoice (fs, 3.0, [&] (double) { return midiToHz (sung); });

        for (int mode = 0; mode < 2; ++mode)
        {
            AutoTuneEngine eng;
            eng.prepare (fs, 1);
            eng.setParams (mode == 0 ? makeParams (chromatic, 0.0f, 0.0f) : makeParams (chromatic, 40.0f, 0.7f));
            auto out = runEngine (eng, in);
            auto ps = measurePitch (out, fs);
            auto s = stats (centsVs (ps, 1.0 + eng.latencySamples() / fs, 2.8, 57.0));
            std::printf ("  mode: %s   (latency %d samples = %.1f ms, starved %d)\n", mode == 0 ? "hard (speed 0, natural 0)" : "soft (speed 40ms, natural 70%)",
                         eng.latencySamples(), 1000.0 * eng.latencySamples() / fs, eng.starvedEvents());
            check (std::fabs (s.mean) < 2.0, "mean error vs A3", s.mean, "cents");
            check (s.stddev < 2.0, "pitch wobble (stddev)", s.stddev, "cents");
            check (eng.starvedEvents() == 0, "no starvation", eng.starvedEvents());
        }
    }

    // ------------------------------------------------------------------ 2. vibrato
    std::printf ("2. Vibrato +-40 cents @5.5 Hz around a note 30 cents flat of C4\n");
    {
        auto f0 = [] (double t) {
            const double m = 60.0 - 0.30 + 0.40 * std::sin (2.0 * kPi * 5.5 * t);
            return midiToHz (m);
        };
        auto in = makeVoice (fs, 4.0, f0);
        auto inPs = measurePitch (in, fs);
        auto inStats = stats (centsVs (inPs, 1.0, 3.5, 60.0));

        struct Case { const char* name; float speed, natural; double minKeep, maxKeep; };
        const Case cases[] = {
            { "natural 100%, speed 40ms", 40.0f, 1.0f, 0.70, 1.3 },
            { "natural 0%, speed 0 (hard)", 0.0f, 0.0f, 0.0, 0.15 },
        };
        for (auto& c : cases)
        {
            AutoTuneEngine eng;
            eng.prepare (fs, 1);
            eng.setParams (makeParams (chromatic, c.speed, c.natural));
            auto out = runEngine (eng, in);
            auto ps = measurePitch (out, fs);
            auto s = stats (centsVs (ps, 1.0 + eng.latencySamples() / fs, 3.5, 60.0));
            const double keep = s.stddev / inStats.stddev;
            std::printf ("  case: %s\n", c.name);
            check (std::fabs (s.mean) < 5.0, "mean error vs C4", s.mean, "cents");
            check (keep >= c.minKeep && keep <= c.maxKeep, "vibrato depth kept (out/in)", keep);
        }
    }

    // ------------------------------------------------------------------ 2b. vibrato straddling a note boundary
    std::printf ("2b. Wide vibrato centred on the boundary between two notes must not flip the target\n");
    {
        struct Result { int flips = 0; double outDepth = 0, outMean = 0; };
        // Drives the corrector with a synthetic f0 contour (4 ms hops); midiAt(t) is the sung pitch.
        auto drive = [] (const std::function<double (double)>& midiAt, double seconds, const harmonia::CorrectorParams& cp,
                         double settle, std::vector<double>* targets = nullptr) {
            harmonia::PitchCorrector pc;
            pc.prepare (0.004);
            std::mt19937 rng (7);
            std::normal_distribution<double> nd (0.0, 1.0);
            Result r;
            std::vector<double> outs;
            float lastTarget = -1.0f;
            for (double t = 0.0; t < seconds; t += 0.004)
            {
                const double m = midiAt (t) + 0.03 * nd (rng); // 3 cents of detector/voice jitter
                const auto o = pc.process (true, (float) midiToHz (m), cp);
                if (targets)
                    targets->push_back (o.targetMidi);
                if (t >= settle)
                {
                    if (lastTarget >= 0.0f && o.targetMidi != lastTarget)
                        ++r.flips;
                    outs.push_back (o.outMidi * 100.0);
                }
                lastTarget = o.targetMidi;
            }
            const auto s = stats (outs);
            r.outDepth = s.stddev;
            r.outMean = s.mean;
            return r;
        };

        struct Case { const char* name; uint16_t mask; double centre, depth, rate; };
        const Case cases[] = {
            { "chromatic, +-50c @5.5Hz on C4/C#4 boundary", chromatic, 60.50, 0.50, 5.5 },
            { "chromatic, +-70c @6.5Hz on C4/C#4 boundary", chromatic, 60.50, 0.70, 6.5 },
            { "chromatic, +-35c @4.5Hz slightly past boundary", chromatic, 60.56, 0.35, 4.5 },
            { "C major, +-90c @5Hz on D4/E4 boundary", 0x0AB5, 63.00, 0.90, 5.0 },
        };
        for (auto& c : cases)
        {
            harmonia::CorrectorParams cp;
            cp.noteMask = c.mask;
            cp.retuneMs = 40.0f;
            cp.natural = 1.0f;
            auto contour = [&] (double t) { return c.centre + c.depth * std::sin (2.0 * kPi * c.rate * t); };
            const auto r = drive (contour, 4.0, cp, 0.6);
            char label[128];
            std::snprintf (label, sizeof label, "%s: target flips", c.name);
            check (r.flips == 0, label, r.flips);
            // natural 100%: the vibrato itself should survive (not be chopped up by flips).
            const double inDepth = c.depth * 100.0 / std::sqrt (2.0);
            std::snprintf (label, sizeof label, "%s: depth kept", c.name);
            check (r.outDepth > 0.6 * inDepth && r.outDepth < 1.3 * inDepth, label, r.outDepth / inDepth);
        }

        // The same, at natural 0: the output must be (nearly) a single flat note, not jumping between two.
        {
            harmonia::CorrectorParams cp;
            cp.retuneMs = 0.0f;
            cp.natural = 0.0f;
            auto contour = [] (double t) { return 60.50 + 0.50 * std::sin (2.0 * kPi * 5.5 * t); };
            const auto r = drive (contour, 4.0, cp, 0.6);
            check (r.flips == 0, "natural 0%, hard: target flips", r.flips);
            check (r.outDepth < 20.0, "natural 0%, hard: output wobble (stddev)", r.outDepth, "cents");
        }

        // Vibrato that fades in, with a drifting rate, on a held note at a boundary.
        {
            harmonia::CorrectorParams cp;
            cp.retuneMs = 40.0f;
            cp.natural = 1.0f;
            auto contour = [] (double t) {
                const double depth = 0.55 * std::clamp (t / 1.0, 0.0, 1.0);
                const double phase = 2.0 * kPi * (4.5 * t + 0.4 * t * t); // 4.5 -> ~6.1 Hz over 2 s
                return 60.50 + depth * std::sin (phase);
            };
            const auto r = drive (contour, 4.0, cp, 1.4);
            check (r.flips == 0, "fade-in vibrato with drifting rate: target flips", r.flips);
        }

        // A slow slide through vibrato (portamento) must arrive at the new note without dithering.
        {
            harmonia::CorrectorParams cp;
            cp.retuneMs = 40.0f;
            cp.natural = 0.7f;
            auto contour = [] (double t) {
                const double slide = std::clamp ((t - 1.5) / 0.5, 0.0, 1.0); // C4 -> D4 over 0.5 s
                return 60.0 + 2.0 * slide + 0.40 * std::sin (2.0 * kPi * 5.5 * t);
            };
            std::vector<double> targets;
            const auto r = drive (contour, 3.5, cp, 0.0, &targets);
            check (r.flips <= 2 && std::fabs (targets.back() - 62.0) < 0.01, "portamento C4 -> D4 under vibrato: target changes (<= 2)", r.flips);
        }

        // End to end through the real engine: the rendered output keeps the vibrato around ONE note.
        {
            auto f0 = [] (double t) { return midiToHz (60.50 + 0.50 * std::sin (2.0 * kPi * 5.5 * t)); };
            auto in = makeVoice (fs, 4.0, f0);
            AutoTuneEngine eng;
            eng.prepare (fs, 1);
            eng.setParams (makeParams (chromatic, 40.0f, 1.0f));
            auto out = runEngine (eng, in);
            auto ps = measurePitch (out, fs);
            auto sm = stats (centsVs (ps, 1.0 + eng.latencySamples() / fs, 3.6, 60.0));
            auto sIn = stats (centsVs (measurePitch (in, fs), 1.0, 3.6, 60.0));
            const double nearest = std::fabs (sm.mean) < std::fabs (sm.mean - 100.0) ? 0.0 : 100.0;
            check (std::fabs (sm.mean - nearest) < 10.0, "engine: output centred on a single note (mean vs C4 or C#4)", sm.mean - nearest, "cents");
            check (sm.stddev > 0.8 * sIn.stddev && sm.stddev < 1.25 * sIn.stddev, "engine: vibrato depth kept (out/in)", sm.stddev / sIn.stddev);
        }

        // Huge vibrato: a swing of more than a semitone either side must still be one note.
        {
            struct Wide { const char* name; uint16_t mask; double centre, depth, rate; };
            const Wide wides[] = {
                { "chromatic, +-100c @5Hz, 25c sharp of C4", chromatic, 60.25, 1.00, 5.0 },
                { "chromatic, +-150c @4.5Hz, 20c flat of E4", chromatic, 63.80, 1.50, 4.5 },
                { "chromatic, +-80c @2.5Hz (slow), 10c sharp of D4", chromatic, 62.10, 0.80, 2.5 },
                { "C major, +-120c @5.5Hz around the E4/F4 boundary", 0x0AB5, 64.50, 1.20, 5.5 },
            };
            for (auto& w : wides)
            {
                harmonia::CorrectorParams cp;
                cp.noteMask = w.mask;
                cp.retuneMs = 40.0f;
                cp.natural = 0.7f;
                auto contour = [&] (double t) {
                    const double rise = std::clamp (t / 0.5, 0.0, 1.0); // the vibrato grows in
                    return w.centre + rise * w.depth * std::sin (2.0 * kPi * w.rate * t);
                };
                std::vector<double> targets;
                const auto r = drive (contour, 5.0, cp, 1.5, &targets);
                char label[128];
                std::snprintf (label, sizeof label, "%s: target flips", w.name);
                check (r.flips == 0, label, r.flips);
                std::snprintf (label, sizeof label, "%s: output centred on the note", w.name);
                const double note = targets.back();
                check (std::fabs (r.outMean - note * 100.0) < 12.0, label, r.outMean - note * 100.0, "cents");
            }
        }

        // Drift: a held note that creeps away slowly is not a new note.
        for (double vib : { 0.0, 0.25 })
        {
            harmonia::CorrectorParams cp;
            cp.retuneMs = 40.0f;
            cp.natural = 0.7f;
            auto contour = [vib] (double t) { return 60.10 + 0.30 * t + vib * std::min (1.0, t) * std::sin (2.0 * kPi * 5.5 * t); };
            std::vector<double> targets;
            drive (contour, 3.5, cp, 0.0, &targets); // 30 c/s: reaches 61.15 by the end
            int early = 0;
            for (size_t i = 0; i < targets.size(); ++i)
                if (i * 0.004 > 0.6 && i * 0.004 < 2.1 && std::fabs (targets[i] - 60.0) > 0.01)
                    ++early; // up to 60.73: still C4
            char label[128];
            std::snprintf (label, sizeof label, "drift 30c/s on C4, vibrato +-%.0fc: target held past the halfway point", vib * 100.0);
            check (early == 0, label, early);
            std::snprintf (label, sizeof label, "drift 30c/s, vibrato +-%.0fc: gives way to C#4 once the pitch has clearly left", vib * 100.0);
            check (std::fabs (targets.back() - 61.0) < 0.01, label, targets.back());
        }

        // Real note changes must still be recognised quickly, vibrato or not.
        struct Step { const char* name; double from, to, depth; };
        const Step steps[] = {
            { "C4 -> E4, +-40c vibrato", 60.0, 64.0, 0.40 },
            { "C4 -> D4, +-40c vibrato", 60.0, 62.0, 0.40 },
            { "C4 -> C#4, +-30c vibrato", 60.0, 61.0, 0.30 },
            { "C4 -> D4, no vibrato", 60.0, 62.0, 0.0 },
        };
        for (auto& s : steps)
        {
            harmonia::CorrectorParams cp;
            cp.retuneMs = 40.0f;
            cp.natural = 0.7f;
            const double tStep = 1.5;
            auto contour = [&] (double t) {
                // a fast 40 ms slide between the notes, as a singer would do
                const double slide = std::clamp ((t - tStep) / 0.04, 0.0, 1.0);
                return s.from + (s.to - s.from) * slide + s.depth * std::sin (2.0 * kPi * 5.5 * t);
            };
            std::vector<double> targets;
            drive (contour, 3.0, cp, 0.0, &targets);
            double tReach = -1.0;
            for (size_t i = 0; i < targets.size(); ++i)
                if (i * 0.004 > tStep && std::fabs (targets[i] - s.to) < 0.01) { tReach = (double) i * 0.004 - tStep; break; }
            int wrongLate = 0; // after settling, the target must be exactly the new note
            for (size_t i = 0; i < targets.size(); ++i)
                if (i * 0.004 > tStep + 0.5 && std::fabs (targets[i] - s.to) > 0.01)
                    ++wrongLate;
            char label[128];
            std::snprintf (label, sizeof label, "%s: seconds until new target", s.name);
            check (tReach >= 0.0 && tReach < 0.22 && wrongLate == 0, label, tReach);
        }
    }

    // ------------------------------------------------------------------ 3. melody with gaps
    std::printf ("3. Melody A3 +30c, C4 -25c, E4 +20c, G3 +40c with breath gaps (C major)\n");
    {
        const double notes[] = { 57.30, 59.75, 63.20, 55.40 }; // sung midi
        const double targets[] = { 57.0, 60.0, 64.0, 55.0 };   // C major: A3, C4, E4, G3 (59.75 -> 60, 63.2 -> 64)
        auto f0 = [&] (double t) {
            const double seg = 0.8;
            const int k = (int) (t / seg);
            const double within = t - k * seg;
            if (k >= 4 || within > seg - 0.06)
                return 0.0;
            return midiToHz (notes[k]);
        };
        auto in = makeVoice (fs, 3.2, f0);
        AutoTuneEngine eng;
        eng.prepare (fs, 1);
        eng.setParams (makeParams (0x0AB5, 30.0f, 0.5f)); // C major bits: C D E F G A B
        auto out = runEngine (eng, in);
        auto ps = measurePitch (out, fs);
        const double lat = eng.latencySamples() / fs;
        for (int k = 0; k < 4; ++k)
        {
            auto s = stats (centsVs (ps, k * 0.8 + 0.35 + lat, k * 0.8 + 0.72, targets[k]));
            char label[64];
            std::snprintf (label, sizeof label, "note %d settled error (n=%d)", k + 1, s.n);
            check (s.n > 5 && std::fabs (s.mean) < 6.0 && s.stddev < 6.0, label, s.mean, "cents");
        }
        check (eng.starvedEvents() == 0, "no starvation", eng.starvedEvents());
    }

    // ------------------------------------------------------------------ 4. range of voices
    std::printf ("4. Different voices, 28 cents flat, hard tune\n");
    {
        const struct { const char* name; double midi; } voices[] = { { "bass E2 (82 Hz)", 40.0 }, { "baritone A2", 45.0 }, { "alto E4", 64.0 }, { "soprano C6", 84.0 } };
        for (auto& v : voices)
        {
            const double sung = v.midi - 0.28;
            auto in = makeVoice (fs, 2.5, [&] (double) { return midiToHz (sung); }, v.midi > 70 ? kEe : kAh);
            AutoTuneEngine eng;
            eng.prepare (fs, 1);
            auto vp = makeParams (chromatic, 0.0f, 0.0f);
            vp.maxHz = 1300.0f;
            eng.setParams (vp);
            auto out = runEngine (eng, in);
            auto ps = measurePitch (out, fs, 60, 1300);
            auto s = stats (centsVs (ps, 1.0 + eng.latencySamples() / fs, 2.3, v.midi));
            char label[96];
            std::snprintf (label, sizeof label, "%s: mean err (stddev %.2f)", v.name, s.stddev);
            check (s.n > 20 && std::fabs (s.mean) < 3.0 && s.stddev < 3.0, label, s.mean, "cents");
        }
    }

    // ------------------------------------------------------------------ 5. transparency
    std::printf ("5. Transparency: already in tune => output is the delayed input (PSOLA may shift timing by < 1 period)\n");
    {
        auto in = makeVoice (fs, 2.0, [] (double) { return midiToHz (57.0); });
        AutoTuneEngine eng;
        eng.prepare (fs, 1);
        eng.setParams (makeParams (chromatic, 40.0f, 0.7f));
        auto out = runEngine (eng, in);
        const int lat = eng.latencySamples();
        double best = -1.0;
        for (int lag = -300; lag <= 300; ++lag)
        {
            double num = 0, da = 0, db = 0;
            for (size_t i = (size_t) (fs * 1.0); i < (size_t) (fs * 1.8); ++i)
            {
                const double a = in[(size_t) ((long) i - lat + lag)], b = out[i];
                num += a * b;
                da += a * a;
                db += b * b;
            }
            best = std::max (best, num / std::sqrt (da * db));
        }
        check (best > 0.97, "waveform correlation with delayed dry", best);
    }

    // ------------------------------------------------------------------ 6. formants kept
    std::printf ("6. Formants stay put while pitch moves up ~ 3 semitones\n");
    {
        auto in = makeVoice (fs, 2.5, [] (double) { return midiToHz (55.0 + 0.0); }); // G3
        // force a 300 cent upward shift by only allowing B-flat... (A#3 = 58)
        AutoTuneEngine eng;
        eng.prepare (fs, 1);
        eng.setParams (makeParams ((uint16_t) (1u << 10), 0.0f, 0.0f)); // only A#
        auto out = runEngine (eng, in);
        auto spectrumCentroid = [&] (const std::vector<float>& x, size_t from, size_t len) {
            // energy-weighted centroid of the Hann-windowed power spectrum, 100 Hz .. 4 kHz
            double num = 0, den = 0;
            for (double f = 100; f < 4000; f += 2.0)
            {
                const double w = 2.0 * kPi * f / fs;
                const double cw = std::cos (w), sw = std::sin (w);
                double cr = 1.0, ci = 0.0, re = 0.0, im = 0.0;
                for (size_t i = 0; i < len; ++i)
                {
                    const double hann = 0.5 - 0.5 * std::cos (2.0 * kPi * (double) i / (double) len);
                    const double v = x[from + i] * hann;
                    re += v * cr;
                    im += v * ci;
                    const double nr = cr * cw - ci * sw;
                    ci = cr * sw + ci * cw;
                    cr = nr;
                }
                const double p = re * re + im * im;
                num += f * p;
                den += p;
            }
            return num / den;
        };
        const size_t from = (size_t) (fs * 1.5), len = (size_t) (fs * 0.5);
        const double cIn = spectrumCentroid (in, from, len);
        const double cOut = spectrumCentroid (out, from, len);
        auto ps = measurePitch (out, fs);
        auto s = stats (centsVs (ps, 1.2, 2.3, 58.0));
        check (std::fabs (s.mean) < 3.0, "output pitch = A#3", s.mean, "cents");
        // a naive resample by +3 semitones would move the centroid by +19%
        check (std::fabs (cOut / cIn - 1.0) < 0.07, "spectral centroid ratio out/in (resample would be 1.19)", cOut / cIn);
    }

    // ------------------------------------------------------------------ 7. silence / noise safety
    std::printf ("7. Silence, noise and clipping safety\n");
    {
        std::mt19937 rng (3);
        std::normal_distribution<float> nd (0.0f, 0.3f);
        std::vector<float> noise ((size_t) (fs * 2));
        for (auto& s : noise)
            s = std::clamp (nd (rng), -1.0f, 1.0f);
        AutoTuneEngine eng;
        eng.prepare (fs, 1);
        eng.setParams (makeParams (chromatic, 20.0f, 0.5f));
        auto out = runEngine (eng, noise);
        bool finite = true;
        double peak = 0;
        for (float s : out)
        {
            finite = finite && std::isfinite (s);
            peak = std::max (peak, (double) std::fabs (s));
        }
        check (finite, "output is finite on white noise", finite ? 1 : 0);
        check (peak < 2.0, "output level sane on white noise", peak);

        std::vector<float> silent ((size_t) fs, 0.0f);
        AutoTuneEngine eng2;
        eng2.prepare (fs, 1);
        eng2.setParams (makeParams (chromatic, 20.0f, 0.5f));
        auto out2 = runEngine (eng2, silent);
        double maxAbs = 0;
        for (float s : out2)
            maxAbs = std::max (maxAbs, (double) std::fabs (s));
        check (maxAbs == 0.0, "silence in -> silence out", maxAbs);
    }

    // ------------------------------------------------------------------ 7b. pitch sweep
    std::printf ("7b. Detection sweep: every semitone E2..C6, two vowels, 0.4%% jitter\n");
    {
        int bad = 0;
        for (int v = 0; v < 2; ++v)
            for (int midi = 40; midi <= 84; ++midi)
            {
                const double sung = midi - 0.28;
                auto in = makeVoice (fs, 1.5, [&] (double) { return midiToHz (sung); }, v == 0 ? kAh : kEe, 7u + (unsigned) midi, 0.004);
                auto ps = measurePitch (in, fs, 60, 1300);
                int ok = 0, n = 0;
                for (auto& p : ps)
                {
                    if (p.t < 0.4)
                        continue;
                    ++n;
                    if (std::fabs ((p.midi - sung) * 100.0) < 15.0)
                        ++ok;
                }
                if (ok < n * 0.97)
                    ++bad;
            }
        check (bad == 0, "pitches/vowels with octave or tracking errors", bad);
    }

    // ------------------------------------------------------------------ 7b2. inharmonic residue
    std::printf ("7b2. Detection ignores a strong inharmonic tone high above the voice (stem-separation residue, bleed)\n");
    {
        int bad = 0;
        double worst = 1.0;
        for (double tone : { 2230.0, 3010.0, 2600.0 })
            for (int midi : { 52, 57, 60, 64, 69 })
            {
                const double sung = midi + 0.22;
                auto in = makeVoice (fs, 1.5, [&] (double) { return midiToHz (sung); }, kAh, 3u + (unsigned) midi, 0.004);
                for (size_t i = 0; i < in.size(); ++i)
                    in[i] += (float) (0.15 * std::sin (2.0 * kPi * tone * (double) i / fs));
                auto ps = measurePitch (in, fs, 60, 1300);
                int ok = 0, n = 0;
                for (auto& p : ps)
                {
                    if (p.t < 0.4)
                        continue;
                    ++n;
                    if (std::fabs ((p.midi - sung) * 100.0) < 15.0)
                        ++ok;
                }
                worst = std::min (worst, n ? (double) ok / n : 0.0);
                if (ok < n * 0.97)
                    ++bad;
            }
        check (bad == 0, "voice + inharmonic tone: cases with wrong pitch frames", bad);
        check (worst > 0.97, "voice + inharmonic tone: worst fraction of frames within 15 cents", worst);
    }

    // ------------------------------------------------------------------ 7b3. octave guard
    std::printf ("7b3. A 2nd harmonic far stronger than the fundamental still reads as the fundamental (octave guard)\n");
    {
        std::mt19937 rng (3);
        std::normal_distribution<double> nd (0.0, 1.0);
        double worst = 1.0;
        for (double f0 : { 90.0, 140.0, 220.0, 330.0, 500.0 })
            for (double h1 : { 0.08, 0.15, 0.3 }) // the fundamental relative to the 2nd harmonic: -22 .. -10 dB
            {
                std::vector<float> in ((size_t) (1.5 * fs));
                double ph = 0.0, jit = 1.0;
                for (auto& v : in)
                {
                    ph += f0 * jit / fs;
                    if (ph >= 1.0)
                    {
                        ph -= 1.0;
                        jit = 1.0 + 0.004 * nd (rng);
                    }
                    const double w = 2.0 * kPi * ph;
                    v = (float) (0.3 * (h1 * std::sin (w) + std::sin (2.0 * w) + 0.3 * std::sin (3.0 * w) + 0.1 * std::sin (4.0 * w)));
                }
                auto ps = measurePitch (in, fs, 60, 1300);
                int ok = 0, n = 0;
                for (auto& p : ps)
                {
                    if (p.t < 0.4)
                        continue;
                    ++n;
                    if (std::fabs ((p.midi - hzToMidi (f0)) * 100.0) < 15.0)
                        ++ok;
                }
                worst = std::min (worst, n ? (double) ok / n : 0.0);
            }
        check (worst > 0.97, "strong 2nd harmonic: worst fraction of frames at the fundamental", worst);
    }

    // ------------------------------------------------------------------ 7b4. voice ranges follow the tuning
    std::printf ("7b4. Each voice range tracks its top and bottom notes (+-40 cent vibrato) at A4 = 415, 440 and 466 Hz\n");
    {
        struct Edge { int range, midi; };
        // Top notes G4 (baritone), F5 (alto), C6 (soprano, also for Auto); bottom notes E2 (bass), C3 (tenor), C4 (soprano).
        const Edge edges[] = { { 0, 84 }, { 0, 40 }, { 1, 67 }, { 1, 40 }, { 2, 77 }, { 2, 48 }, { 3, 84 }, { 3, 60 } };
        int bad = 0;
        double worst = 1.0;
        for (const double a4 : { 415.0, 440.0, 466.0 })
            for (const auto& e : edges)
            {
                harmonia::EngineParams ep;
                ep.setVoiceRange (e.range, (float) a4);
                const double base = a4 * std::pow (2.0, (e.midi - 69) / 12.0);
                auto f0 = [&] (double t) { return base * std::pow (2.0, 0.40 / 12.0 * std::sin (2.0 * kPi * 5.5 * t)); };
                auto in = makeVoice (fs, 1.5, f0, kAh, 11u + (unsigned) e.midi, 0.004);
                auto ps = measurePitch (in, fs, ep.minHz, ep.maxHz);
                int ok = 0;
                for (auto& p : ps)
                    if (p.t >= 0.4 && std::fabs ((p.midi - hzToMidi (f0 (p.t))) * 100.0) < 15.0)
                        ++ok;
                const double fraction = ok / (1.1 / 0.005); // every 5 ms hop after the first 0.4 s should be there
                worst = std::min (worst, fraction);
                if (fraction < 0.97)
                {
                    ++bad;
                    std::printf ("    %s, MIDI %d at A4 = %.0f Hz: %.0f %% tracked\n", harmonia::kVoiceRanges[e.range].name, e.midi, a4, 100.0 * fraction);
                }
            }
        check (bad == 0, "range edges not tracked", bad);
        check (worst > 0.97, "range edges: worst fraction of frames tracked", worst);
    }

    // ------------------------------------------------------------------ 7c. sample rates
    std::printf ("7c. Other sample rates and low voices: 35 cents sharp, hard tune\n");
    {
        const struct { double rate, midi; } cases[] = {
            { 22050, 57 }, { 44100, 57 }, { 88200, 57 }, { 96000, 57 }, { 192000, 57 }, { 96000, 41 }, { 192000, 41 },
        };
        for (auto& c : cases)
        {
            auto in = makeVoice (c.rate, 2.5, [&] (double) { return midiToHz (c.midi + 0.35); });
            AutoTuneEngine eng;
            eng.prepare (c.rate, 1);
            eng.setParams (makeParams (chromatic, 0.0f, 0.0f));
            auto out = runEngine (eng, in);
            auto s = stats (centsVs (measurePitch (out, c.rate), 1.0 + eng.latencySamples() / c.rate, 2.3, c.midi));
            char label[96];
            std::snprintf (label, sizeof label, "%.0f Hz, midi %.0f: mean err (stddev %.2f, starved %d)", c.rate, c.midi, s.stddev,
                           eng.starvedEvents());
            check (s.n > 20 && std::fabs (s.mean) < 2.0 && s.stddev < 2.0 && eng.starvedEvents() == 0, label, s.mean, "cents");
        }
    }

    // ------------------------------------------------------------------ 7d. determinism
    std::printf ("7d. Output depends only on the input: not on block size, and reset() restores a clean state\n");
    {
        auto in = makeVoice (fs, 3.0, [] (double t) { return t < 0.3 ? 0.0 : midiToHz (55.3 + 4.0 * std::sin (t * 1.7) + 0.3 * std::sin (t * 35.0)); });
        auto render = [&] (AutoTuneEngine& eng, int block) {
            eng.setParams (makeParams (chromatic, 30.0f, 0.5f));
            return runEngine (eng, in, block);
        };
        AutoTuneEngine eng;
        eng.prepare (fs, 1);
        const auto reference = render (eng, 480);

        for (int block : { 1, 7, 4096 })
        {
            AutoTuneEngine other;
            other.prepare (fs, 1);
            char label[64];
            std::snprintf (label, sizeof label, "block size %d gives identical output", block);
            check (render (other, block) == reference, label, block);
        }

        eng.reset();
        check (render (eng, 480) == reference, "same output after reset()", 0);

        // Both channels of a stereo pair must be treated identically.
        AutoTuneEngine stereo;
        stereo.prepare (fs, 2);
        stereo.setParams (makeParams (chromatic, 30.0f, 0.5f));
        auto l = in, r = in;
        for (size_t pos = 0; pos + 480 <= l.size(); pos += 480)
        {
            float* ch[2] = { l.data() + pos, r.data() + pos };
            stereo.process (ch, 2, 480);
        }
        check (l == r, "identical stereo channels stay identical", 0);
    }

    // ------------------------------------------------------------------ 7d2. Note hold
    std::printf ("7d2. Note hold: a brief excursion is not followed, a real note change still is\n");
    {
        auto run = [&] (float dwellMs, double blipSeconds, double toMidi, int& targetChanges, double& followedAt) {
            harmonia::PitchCorrector pc;
            pc.prepare (0.004);
            harmonia::CorrectorParams cp;
            cp.noteMask = chromatic;
            cp.dwellMs = dwellMs;
            cp.retuneMs = 40.0f;
            targetChanges = 0;
            followedAt = -1.0;
            float last = -1.0f;
            const double tStep = 1.0, tEnd = tStep + blipSeconds;
            for (double t = 0.0; t < 2.5; t += 0.004)
            {
                double midi = 60.2 + 0.05 * std::sin (2.0 * kPi * 4.0 * t);
                if (t >= tStep && t < tEnd)
                    midi = toMidi;
                else if (t >= tEnd && blipSeconds > 1.0)
                    midi = toMidi;
                const auto o = pc.process (true, (float) midiToHz (midi), cp);
                if (t > 0.5 && last >= 0.0f && o.targetMidi != last)
                    ++targetChanges;
                if (followedAt < 0.0 && t >= tStep && std::fabs (o.targetMidi - toMidi) < 0.6)
                    followedAt = t - tStep;
                last = o.targetMidi;
            }
        };
        int changes = 0;
        double at = 0.0;
        run (70.0f, 0.045, 61.3, changes, at);
        check (changes == 0, "a 45 ms excursion a semitone away is ignored (hold 70 ms)", changes);
        run (0.0f, 0.045, 61.3, changes, at);
        check (changes > 0, "(without hold the same excursion does move the target)", changes);
        run (70.0f, 5.0, 64.0, changes, at);
        check (at >= 0.0 && at < 0.35, "a real change to a new note is followed within 350 ms", at * 1000.0, "ms");
        double atNoHold = 0.0;
        run (0.0f, 5.0, 64.0, changes, atNoHold);
        check (at - atNoHold < 0.1, "hold delays a real note change by under 100 ms", (at - atNoHold) * 1000.0, "ms");
    }

    // ------------------------------------------------------------------ 7e. Auto key
    std::printf ("7e. Auto key: finds the key by itself, ignores passing notes, follows a modulation\n");
    {
        // A tune that circles around its tonic; `shift` transposes it. The singer is a little off: +-20 cents.
        const int tune[] = { 0, 2, 4, 2, 4, 5, 7, 5, 4, 2, 0, 2, 4, 7, 4, 2, 0, -1, 0, 2, 4, 5, 4, 2, 0 };
        const int numTune = (int) (sizeof (tune) / sizeof (tune[0]));
        auto note = [&] (double t, int shift) {
            const int i = (int) (t / 0.35) % numTune;
            const int pc = (tune[i] % 12 + 12) % 12;
            const double detune = ((i * 7) % 5 - 2) * 0.10;
            return midiToHz (60.0 + shift + (tune[i] < 0 ? tune[i] : pc) + detune);
        };
        const double sectionA = 24.0, sectionB = 30.0;
        auto in = makeVoice (fs, sectionA + sectionB, [&] (double t) { return t < sectionA ? note (t, 0) : note (t - sectionA, 6); });

        EngineParams p = makeParams (chromatic, 30.0f, 0.6f);
        p.autoKey = true;
        AutoTuneEngine eng;
        eng.prepare (fs, 1);
        eng.setParams (p);
        check (eng.activeNoteMask() == chromatic, "before it knows the key the chosen notes are used", eng.activeNoteMask());

        uint16_t beforeMod = 0, early = 0, end = 0;
        int firstKeyAt = -1;
        auto out = in;
        for (size_t pos = 0; pos + 480 <= out.size(); pos += 480)
        {
            float* ch[1] = { out.data() + pos };
            eng.process (ch, 1, 480);
            const double t = (double) (pos + 480) / fs;
            if (firstKeyAt < 0 && eng.trackedKey().valid)
                firstKeyAt = (int) t;
            if (t > 9.0 && t < 9.1)
                early = eng.activeNoteMask();
            if (t > sectionA - 0.1 && t < sectionA)
                beforeMod = eng.activeNoteMask();
        }
        end = eng.activeNoteMask();

        const uint16_t cMajor = harmonia::scaleMask (0, harmonia::kMajorScale);
        const uint16_t fsMajor = harmonia::scaleMask (6, harmonia::kMajorScale);
        check (firstKeyAt >= 0 && firstKeyAt <= 8, "key known within 8 s of singing", firstKeyAt, "s");
        check (early == cMajor, "section 1 is C major (despite the chromatic passing note)", early);
        check (beforeMod == cMajor, "still C major at the end of section 1", beforeMod);
        check (end == fsMajor, "follows the modulation to F# major", end);

        AutoTuneEngine manual;
        manual.prepare (fs, 1);
        manual.setParams (makeParams (cMajor, 30.0f, 0.6f)); // autoKey off
        auto copy = in;
        for (size_t pos = 0; pos + 480 <= copy.size(); pos += 480)
        {
            float* ch[1] = { copy.data() + pos };
            manual.process (ch, 1, 480);
        }
        check (manual.activeNoteMask() == cMajor && ! manual.trackedKey().valid, "auto key off: the chosen notes stay in force", manual.activeNoteMask());
    }

    // ------------------------------------------------------------------ 8. CPU
    std::printf ("8. CPU cost\n");
    {
        auto in = makeVoice (fs, 10.0, [] (double t) { return midiToHz (50.0 + 7.0 * std::sin (t)); });
        AutoTuneEngine eng;
        eng.prepare (fs, 2);
        eng.setParams (makeParams (chromatic, 30.0f, 0.5f));
        std::vector<float> l = in, r = in;
        const auto t0 = std::chrono::steady_clock::now();
        for (size_t pos = 0; pos + 512 <= l.size(); pos += 512)
        {
            float* ch[2] = { l.data() + pos, r.data() + pos };
            eng.process (ch, 2, 512);
        }
        const auto t1 = std::chrono::steady_clock::now();
        const double secs = std::chrono::duration<double> (t1 - t0).count();
        const double load = secs / 10.0 * 100.0;
        check (load < 10.0, "CPU load (stereo, 48 kHz, one core)", load, "%");
    }

    // ------------------------------------------------------------------ 9. bad samples
    std::printf ("9. A NaN and an inf in the input\n");
    {
        const auto in = makeVoice (fs, 4.0, [] (double) { return midiToHz (57.35); });
        auto bad = in;
        bad[20000] = std::numeric_limits<float>::quiet_NaN();
        bad[30000] = std::numeric_limits<float>::infinity();
        AutoTuneEngine withBad, clean;
        for (auto* e : { &withBad, &clean })
        {
            e->prepare (fs, 1);
            e->setParams (makeParams (chromatic, 40.0f, 0.7f));
        }
        const auto a = runEngine (withBad, bad), b = runEngine (clean, in);
        int nonFinite = 0;
        double worst = 0.0;
        for (size_t i = 0; i < a.size(); ++i)
        {
            if (! std::isfinite (a[i]))
                ++nonFinite;
            else if (i + 48000 >= a.size())
                worst = std::max (worst, (double) std::fabs (a[i] - b[i]));
        }
        check (nonFinite == 0, "the output stays finite", nonFinite, "bad samples");
        check (worst < 0.01, "a second later the voice is what it would have been", worst, "max difference");
    }

    std::printf ("\n%s (%d failure%s)\n", failures == 0 ? "ALL TESTS PASSED" : "SOME TESTS FAILED", failures, failures == 1 ? "" : "s");
    return failures == 0 ? 0 : 1;
}
