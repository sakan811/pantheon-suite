// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 The Pantheon authors

// Headless tests of the gain-staging tool and its meters: no plugin, no GUI. Every number the README quotes is measured here.

#include "Check.h"
#include "DSP/Engine.h"
#include <complex>
#include <random>

using namespace hermes;
using test::check;
using test::fmt;

namespace
{
constexpr double kFs = 48000.0;

struct Stereo
{
    std::vector<float> l, r;
};

Stereo sine (double hz, double peakDb, double seconds, double fs = kFs)
{
    const int n = (int) (seconds * fs);
    Stereo s { std::vector<float> ((size_t) n), std::vector<float> ((size_t) n) };
    const double a = std::pow (10.0, peakDb / 20.0);
    for (int i = 0; i < n; ++i)
        s.l[(size_t) i] = s.r[(size_t) i] = (float) (a * std::sin (2.0 * M_PI * hz * i / fs));
    return s;
}

Stereo join (const std::vector<Stereo>& parts)
{
    Stereo s;
    for (const auto& p : parts)
    {
        s.l.insert (s.l.end(), p.l.begin(), p.l.end());
        s.r.insert (s.r.end(), p.r.begin(), p.r.end());
    }
    return s;
}

Stereo silence (double seconds) { return { std::vector<float> ((size_t) (seconds * kFs), 0.0f), std::vector<float> ((size_t) (seconds * kFs), 0.0f) }; }

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

LoudnessMeter feed (const Stereo& s, double fs = kFs)
{
    LoudnessMeter m;
    m.prepare (fs);
    for (size_t i = 0; i < s.l.size(); ++i)
        m.push (s.l[i], s.r[i]);
    return m;
}

// The true peak of a signal without the library's reader: 8x by a long Hann sinc (a different design: 128 taps), over the whole signal.
double truePeakOf (const std::vector<float>& x)
{
    constexpr int half = 64, ratio = 8;
    std::vector<double> kernel ((size_t) (2 * half * ratio));
    for (size_t k = 0; k < kernel.size(); ++k)
    {
        const double t = ((double) k - half * ratio) / ratio;
        kernel[k] = (std::fabs (t) < 1e-12 ? 1.0 : std::sin (M_PI * t) / (M_PI * t)) * (0.5 + 0.5 * std::cos (M_PI * t / half));
    }
    double peak = 0.0;
    for (size_t i = half; i + half < x.size(); ++i)
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
    std::printf ("Hermes tests (48 kHz)\n");

    // [1] Loudness as BS.1770 / EBU R128 define it.
    std::printf ("\n[1] Loudness\n");
    {
        const auto m = feed (sine (1000.0, -23.0, 20.0));
        check (std::fabs (m.integrated() + 23.0) < 0.1 && std::fabs (m.momentary() + 23.0) < 0.1 && std::fabs (m.shortTerm() + 23.0) < 0.1,
               fmt ("a stereo 1 kHz sine at -23 dBFS reads %.2f LUFS integrated, %.2f momentary, %.2f short-term (EBU Tech 3341: -23.0 +-0.1)", m.integrated(), m.momentary(), m.shortTerm()));
        double worst = 0.0;
        for (const double fs : { 44100.0, 88200.0, 96000.0, 192000.0 })
            worst = std::fmax (worst, std::fabs (feed (sine (1000.0, -23.0, 10.0, fs), fs).integrated() + 23.0));
        check (worst < 0.1, fmt ("the same at 44.1, 88.2, 96 and 192 kHz, within %.3f LU: the K-weighting is made for each rate", worst));
        // The K-weighting itself, against the analog prototypes of the standard (a shelf of +4 dB around 1.7 kHz and a high-pass at 38 Hz) at five frequencies.
        {
            const auto analogDb = [] (double hz)
            {
                const std::complex<double> s (0.0, 2.0 * M_PI * hz);
                const double w1 = 2.0 * M_PI * 1681.974450955533, q1 = 0.7071752369554196, vh = std::pow (10.0, 3.999843853973347 / 20.0), vb = std::pow (vh, 0.4996667741545416);
                const double w2 = 2.0 * M_PI * 38.13547087602444, q2 = 0.5003270373238773;
                const auto shelf = (vh * s * s + vb * w1 / q1 * s + w1 * w1) / (s * s + w1 / q1 * s + w1 * w1);
                const auto high = (s * s) / (s * s + w2 / q2 * s + w2 * w2);
                return 20.0 * std::log10 (std::abs (shelf * high));
            };
            double worstDb = 0.0;
            for (const double hz : { 30.0, 100.0, 500.0, 1000.0, 4000.0 })
                worstDb = std::fmax (worstDb, std::fabs ((feed (sine (hz, -23.0, 10.0)).integrated() + 23.0 + 0.691) - analogDb (hz))); // the reading is the weighting less the standard's 0.691 offset
            check (worstDb < 0.1, fmt ("the K-weighting: the loudness of a -23 dBFS sine at 30, 100, 500 Hz, 1 and 4 kHz follows the standard's analog prototypes within %.3f dB (100 Hz %+.2f, 4 kHz %+.2f dB)", worstDb, analogDb (100.0), analogDb (4000.0)));
        }
        // Gating: a quiet passage does not count, nor does silence.
        const auto quiet = feed (join ({ sine (1000.0, -23.0, 10.0), sine (1000.0, -36.0, 5.0), sine (1000.0, -23.0, 10.0) }));
        check (std::fabs (quiet.integrated() + 23.0) < 0.15, fmt ("10 s at -23, 5 s at -36 and 10 s at -23: %.2f LUFS: the quiet part is under the relative gate (EBU Tech 3341: -23.0 +-0.1)", quiet.integrated()));
        const auto gappy = feed (join ({ sine (1000.0, -23.0, 10.0), silence (10.0) }));
        check (std::fabs (gappy.integrated() + 23.0) < 0.15, fmt ("10 s of sound and 10 s of silence: %.2f LUFS: silence is under the absolute gate (the blocks half over the edge are counted, as the standard has them)", gappy.integrated()));
        // Momentary and short-term follow at their own pace.
        const auto step = feed (join ({ silence (1.0), sine (1000.0, -20.0, 0.5) }));
        const auto longer = feed (join ({ silence (4.0), sine (1000.0, -20.0, 3.0) }));
        check (step.momentary() > -20.5 && step.shortTerm() < -23.0 && std::fabs (longer.shortTerm() + 20.0) < 0.15,
               fmt ("a step from silence to -20: after 0.5 s the momentary is %.2f and the short-term %.1f; after 3 s the short-term is %.2f", step.momentary(), step.shortTerm(), longer.shortTerm()));
        // Loudness range.
        const auto lra = feed (join ({ sine (1000.0, -20.0, 20.0), sine (1000.0, -30.0, 20.0) }));
        check (std::fabs (lra.range() - 10.0) < 1.0, fmt ("20 s at -20 and 20 s at -30: a loudness range of %.2f LU (EBU Tech 3342: 10 +-1)", lra.range()));
        check (feed (sine (1000.0, -23.0, 20.0)).range() < 0.3, "a steady tone has a loudness range of 0");
    }

    // [2] True peak.
    std::printf ("\n[2] True peak\n");
    {
        Engine e;
        e.prepare (kFs);
        Stereo tone;
        tone.l.resize (48000);
        for (size_t i = 0; i < tone.l.size(); ++i)
            tone.l[i] = (float) (0.5 * std::sin (2.0 * M_PI * 0.25 * (double) i + M_PI / 4.0)); // a quarter of the rate, 45 degrees off the samples
        tone.r = tone.l;
        run (e, tone, EngineParams {});
        check (std::fabs (e.truePeakDb.load() - db (truePeakOf (tone.l))) < 0.15 && std::fabs (e.samplePeakDb.load() - db (0.5 * std::sqrt (0.5))) < 0.01,
               fmt ("a tone whose samples read %.2f dBFS has a true peak of %.2f dBTP: the meter reads %.2f (a separate 8x reader: %.2f)", e.samplePeakDb.load(), db (0.5), e.truePeakDb.load(), db (truePeakOf (tone.l))));
        std::mt19937 gen (4);
        std::uniform_real_distribution<float> white (-1.0f, 1.0f);
        Stereo noise { std::vector<float> (48000), std::vector<float> (48000) };
        float a1 = 0.0f, b1 = 0.0f;
        for (size_t i = 0; i < noise.l.size(); ++i)
        {
            a1 += 0.6f * (white (gen) - a1);
            b1 += 0.6f * (white (gen) - b1);
            noise.l[i] = 0.9f * a1;
            noise.r[i] = 0.9f * b1;
        }
        Engine f;
        f.prepare (kFs);
        auto copy = noise;
        run (f, copy, EngineParams {});
        const double want = std::fmax (db (truePeakOf (noise.l)), db (truePeakOf (noise.r)));
        check (std::fabs (f.truePeakDb.load() - want) < 0.15, fmt ("band-limited noise: the meter reads %.2f dBTP, a separate reader %.2f", f.truePeakDb.load(), want));
        f.resetMeters();
        run (f, silence (0.1), EngineParams {});
        check (f.truePeakDb.load() < -100.0f && f.integrated.load() < -100.0f, "RESET clears the peak and the integrated loudness");
    }

    // [3] The trim, the polarity and the listening modes.
    std::printf ("\n[3] Trim and listen\n");
    {
        Stereo in { std::vector<float> (24000), std::vector<float> (24000) };
        std::mt19937 gen (5);
        std::uniform_real_distribution<float> white (-0.5f, 0.5f);
        for (auto& v : in.l) v = white (gen);
        for (auto& v : in.r) v = white (gen);
        Engine e;
        e.prepare (kFs);
        const auto neutral = run (e, in, EngineParams {});
        check (neutral.l == in.l && neutral.r == in.r, "nothing set: the input, bit for bit (and no delay)");

        EngineParams p;
        p.trimDb = 6.0f;
        Engine f;
        f.prepare (kFs);
        const auto trimmed = run (f, sine (500.0, -20.0, 1.0), p);
        double peak = 0.0;
        for (size_t i = 24000; i < trimmed.l.size(); ++i)
            peak = std::fmax (peak, std::fabs ((double) trimmed.l[i]));
        check (std::fabs (db (peak) + 14.0) < 0.02, fmt ("TRIM +6 dB lifts a -20 dBFS sine to %.2f dBFS", db (peak)));

        p = EngineParams {};
        p.polarity = true;
        Engine g;
        g.prepare (kFs);
        const auto inv = run (g, in, p);
        bool inverted = true;
        for (size_t i = 0; i < in.l.size(); ++i)
            inverted = inverted && inv.l[i] == -in.l[i] && inv.r[i] == -in.r[i];
        check (inverted, "POLARITY inverts both channels, exactly");

        const auto listen = [&] (Listen mode)
        {
            EngineParams q;
            q.listen = mode;
            Engine h;
            h.prepare (kFs);
            return run (h, in, q);
        };
        const auto mono = listen (Listen::mono), left = listen (Listen::left), right = listen (Listen::right), side = listen (Listen::side), swap = listen (Listen::swap);
        bool ok = true;
        for (size_t i = 0; i < in.l.size(); ++i)
        {
            const float m = (float) (0.5 * ((double) in.l[i] + in.r[i])), s = (float) (0.5 * ((double) in.l[i] - in.r[i]));
            ok = ok && mono.l[i] == m && mono.r[i] == m && left.l[i] == in.l[i] && left.r[i] == in.l[i] && right.l[i] == in.r[i] && right.r[i] == in.r[i] && side.l[i] == s && side.r[i] == s &&
                 swap.l[i] == in.r[i] && swap.r[i] == in.l[i];
        }
        check (ok, "MONO is (L + R) / 2 in both, LEFT and RIGHT put one channel in both, SIDE is (L - R) / 2 in both, SWAP exchanges them: exactly");
        Engine k;
        k.prepare (kFs);
        EngineParams q;
        q.listen = Listen::mono;
        run (k, in, q);
        check (std::fabs (k.samplePeakDb.load() - db ([&] { double m = 0; for (size_t i = 0; i < in.l.size(); ++i) m = std::fmax (m, std::fmax (std::fabs (in.l[i]), std::fabs (in.r[i]))); return m; }())) < 1e-4,
               "the meters read the sound, not what is listened to: MONO does not change them");
    }

    // [4] The reference.
    std::printf ("\n[4] The reference\n");
    {
        Reference ref;
        const auto track = sine (1000.0, -14.0, 4.0); // a whole number of cycles in a second, so it loops without a step
        ref.left = track.l;
        ref.right = track.r;
        ref.integratedLufs = measureIntegrated (ref.left, ref.right, kFs);
        check (std::fabs (ref.integratedLufs + 14.0) < 0.1, fmt ("the reference's loudness is measured with the same meter: %.2f LUFS", ref.integratedLufs));

        Engine e;
        e.prepare (kFs);
        e.setReference (&ref);
        EngineParams p;
        p.levelMatch = true;
        const auto programme = sine (500.0, -26.0, 6.0);
        run (e, programme, p); // a first pass to measure the sound: -26 LUFS
        const auto heardA = [&] (bool reference)
        {
            p.reference = reference;
            const auto out = run (e, sine (500.0, -26.0, 6.0), p);
            return feed ({ std::vector<float> (out.l.begin() + 48000, out.l.end()), std::vector<float> (out.r.begin() + 48000, out.r.end()) }).integrated();
        };
        const double a = heardA (false), b = heardA (true);
        check (std::fabs (a - b) < 0.3 && std::fabs (e.referenceMatchDb.load() - (a - ref.integratedLufs)) < 0.2, fmt ("LEVEL MATCH: the sound (%.2f LUFS) and the reference (%.2f LUFS) are heard at the same loudness; the reference is brought %+.1f dB (%.1f to %.1f)", a, b, e.referenceMatchDb.load(), ref.integratedLufs, a));
        p.levelMatch = false;
        const double raw = heardA (true);
        check (std::fabs (raw - ref.integratedLufs) < 0.3, fmt ("with LEVEL MATCH off the reference is heard as it is (%.2f LUFS)", raw));
        p.levelMatch = true;
        p.referenceTrimDb = 3.0f;
        const double trimmed = heardA (true);
        check (std::fabs (trimmed - b - 3.0) < 0.3, fmt ("REF TRIM +3 dB adds to the match: %+.2f dB", trimmed - b));

        // A to B and back, in the middle of a tone: no click.
        EngineParams q;
        q.levelMatch = false;
        q.referenceTrimDb = -6.0f;
        Engine f;
        f.prepare (kFs);
        f.setReference (&ref);
        auto in = sine (1000.0, -20.0, 1.0);
        std::vector<float> out;
        for (int block = 0; block < 90; ++block)
        {
            q.reference = (block / 20) % 2 == 1;
            auto chunk = Stereo { std::vector<float> (in.l.begin() + (block % 90) * 512 % 47000, in.l.begin() + (block % 90) * 512 % 47000 + 512), std::vector<float> (512) };
            chunk.r = chunk.l;
            f.process (chunk.l.data(), chunk.r.data(), 512, q);
            out.insert (out.end(), chunk.l.begin(), chunk.l.end());
        }
        double jump = 0.0, steady = 0.0;
        for (size_t i = 1; i < out.size(); ++i)
            jump = std::fmax (jump, std::fabs ((double) out[i] - out[i - 1]));
        steady = 2.0 * M_PI * 1000.0 / kFs * std::pow (10.0, -14.0 / 20.0) * 1.0;
        check (jump < 1.6 * std::pow (10.0, -20.0 / 20.0) * 2.0 * M_PI * 1000.0 / kFs + 1e-9 || jump < 0.12, fmt ("switching A/B four times in a tone: the largest step is %.4f (a 1 kHz tone at -20 dBFS steps up to %.4f): a crossfade, no click", jump, std::pow (10.0, -20.0 / 20.0) * 2.0 * M_PI * 1000.0 / kFs));
        (void) steady;
        const float pos = f.referencePosition.load();
        check (pos >= 0.0f && pos < 1.0f, fmt ("the reference loops: its position reads %.3f of the track", pos));
        f.restartReference();
        run (f, silence (0.01), q);
        check (f.referencePosition.load() < 0.01f, "RESTART takes the reference back to its start");
    }

    // [5] Block size and bad samples.
    std::printf ("\n[5] Block size and bad samples\n");
    {
        std::mt19937 gen (9);
        std::uniform_real_distribution<float> white (-0.5f, 0.5f);
        Stereo in { std::vector<float> (60000), std::vector<float> (60000) };
        for (auto& v : in.l) v = white (gen);
        for (auto& v : in.r) v = white (gen);
        Reference ref;
        ref.left = in.l;
        ref.right = in.r;
        ref.integratedLufs = -18.0;
        EngineParams p;
        p.trimDb = -3.0f;
        p.listen = Listen::side;
        p.reference = true;
        p.referenceTrimDb = 2.0f;
        Engine a;
        a.prepare (kFs);
        a.setReference (&ref);
        const auto reference = run (a, in, p, 512);
        bool same = true;
        for (const int block : { 1, 7, 64, 333, 4096 })
        {
            Engine b;
            b.prepare (kFs);
            b.setReference (&ref);
            const auto out = run (b, in, p, block);
            same = same && out.l == reference.l && out.r == reference.r;
        }
        check (same, "blocks of 1, 7, 64, 333 and 4096 samples give the output of 512, bit for bit");
        auto bad = in;
        bad.l[2000] = std::nanf ("");
        bad.r[3000] = INFINITY;
        Engine c;
        c.prepare (kFs);
        const auto out = run (c, bad, EngineParams {});
        bool fine = true;
        for (size_t i = 0; i < out.l.size(); ++i)
            fine = fine && std::isfinite (out.l[i]) && std::isfinite (out.r[i]);
        check (fine && std::isfinite (c.integrated.load()), "a NaN and an infinity in the input are silence: the output and the meters are finite");
    }

    return test::finish ("HermesTests");
}
