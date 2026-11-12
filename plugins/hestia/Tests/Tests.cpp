// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 The Pantheon authors

// Headless tests of the DSP: the bands, the roles, the level bank, the cut, and stems that see each other through the bus. No plugin, no GUI.

#include "Check.h"
#include "DSP/Engine.h"
#include <cstring>
#include <functional>
#include <vector>

// Bit-for-bit comparisons are the point of several tests here: "nothing to cut" is the identity exactly, and a block size changes nothing.
#pragma GCC diagnostic ignored "-Wfloat-equal"

using namespace hestia;
using test::check;
using test::fmt;

namespace
{
constexpr double kFs = 48000.0;
constexpr double kPi = 3.14159265358979323846;

double toDb (double x) { return 20.0 * std::log10 (std::fmax (x, 1e-12)); }

struct Stem1
{
    Bus& bus;
    Engine engine;
    EngineParams params;
    std::vector<float> l, r;
    long long position = 0;
    explicit Stem1 (Bus& b, Role role = Role::keys) : bus (b), engine (b)
    {
        params.role = role;
        engine.prepare (kFs);
    }
};

// Runs the stems together for `seconds`, block by block, each block through each stem in turn on a clock that advances with the audio. `source` gives a
// stem's sample at a position. Returns each stem's output; `input` is kept to compare with.
struct Run
{
    std::vector<std::vector<float>> out, in;
};

Run run (std::vector<Stem1*> stems, double seconds, int block, const std::function<float (int stem, long long n)>& source, std::uint32_t& clockMs)
{
    Run result;
    result.out.resize (stems.size());
    result.in.resize (stems.size());
    const long long total = (long long) (seconds * kFs);
    std::vector<float> l, r;
    for (long long start = 0; start < total; start += block)
    {
        const int n = (int) std::min<long long> (block, total - start);
        for (size_t s = 0; s < stems.size(); ++s)
        {
            l.resize ((size_t) n);
            r.resize ((size_t) n);
            for (int i = 0; i < n; ++i)
                l[(size_t) i] = r[(size_t) i] = source ((int) s, stems[s]->position + i);
            result.in[s].insert (result.in[s].end(), l.begin(), l.end());
            stems[s]->engine.process (l.data(), r.data(), n, stems[s]->params, clockMs);
            result.out[s].insert (result.out[s].end(), l.begin(), l.end());
            stems[s]->position += n;
        }
        clockMs += (std::uint32_t) std::lround (1000.0 * n / kFs);
    }
    return result;
}

// The level of the last `seconds` of a signal, as the peak of a sine (rms * sqrt 2), in dB.
double tailDb (const std::vector<float>& x, double seconds)
{
    const size_t n = std::min (x.size(), (size_t) (seconds * kFs));
    double sum = 0.0;
    for (size_t i = x.size() - n; i < x.size(); ++i)
        sum += (double) x[i] * x[i];
    return toDb (std::sqrt (2.0 * sum / (double) n));
}

float sine (double hz, double amplitude, long long n) { return (float) (amplitude * std::sin (2.0 * kPi * hz * (double) n / kFs)); }

// A repeatable noise: every test that wants it gets the same one.
float noise (long long n, std::uint32_t seed = 1)
{
    std::uint32_t x = (std::uint32_t) n * 2654435761u + seed * 40503u;
    x ^= x >> 15; x *= 2246822519u; x ^= x >> 13; x *= 3266489917u; x ^= x >> 16;
    return (float) ((double) x / 2147483648.0 - 1.0);
}
} // namespace

int main()
{
    std::printf ("Hestia DSP tests\n");

    // ---- [1] the bands --------------------------------------------------------------------------------------------------------------------------
    std::printf ("[1] the thirty bands\n");
    {
        bool rising = true;
        for (int k = 1; k < kBands; ++k)
            rising = rising && bandCentreHz (k) > bandCentreHz (k - 1);
        check (std::fabs (bandCentreHz (0) - 25.0) < 1e-9 && bandCentreHz (kBands - 1) > 19000.0 && bandCentreHz (kBands - 1) < 20000.0 && rising,
               fmt ("centres rise from %.0f Hz to %.0f Hz", bandCentreHz (0), bandCentreHz (kBands - 1)));
        check (std::fabs (bandCentreHz (10) / bandCentreHz (0) - 10.0) < 1e-9 && std::fabs (bandCentreHz (13) / bandCentreHz (10) - 1.9953) < 1e-3, "ten bands are a decade; three are an octave (to a third of a percent)");
        check (bandForHz (79.0) == 5 && bandForHz (1995.0) == 19 && bandForHz (1.0) == 0 && bandForHz (1e6) == kBands - 1, "bandForHz picks the nearest band and clamps at both ends");
    }

    // ---- [2] the roles --------------------------------------------------------------------------------------------------------------------------
    std::printf ("[2] the claims of a role\n");
    {
        bool inRange = true;
        for (int r = 0; r < kNumRoles; ++r)
            for (int k = 0; k < kBands; ++k)
            {
                const double c = claimAt ((Role) r, bandCentreHz (k));
                inRange = inRange && c >= 0.0 && c <= 1.0;
            }
        check (inRange, "every role's claim is between 0 and 1 on every band");
        check (claimAt (Role::bass, 79.0) > claimAt (Role::leadVocal, 79.0) + 0.4 && claimAt (Role::leadVocal, 1995.0) > claimAt (Role::bass, 1995.0) + 0.4,
               "bass has the low end, a lead vocal the upper mids");
        check (claimAt (Role::kick, 60.0) > claimAt (Role::bass, 60.0) && claimAt (Role::bass, 200.0) > claimAt (Role::kick, 200.0), "the kick owns the thump, the bass the notes above it");
        check (claimAt (Role::cymbals, 10000.0) >= 0.99 && claimAt (Role::cymbals, 100.0) < 0.01, "hats and cymbals live at the top");
        const auto low = claims (Role::guitar, 0.0f), mid = claims (Role::guitar, 0.5f), high = claims (Role::guitar, 1.0f);
        check (std::fabs (mid[10] - (float) claimAt (Role::guitar, bandCentreHz (10))) < 1e-6f && std::fabs (low[10] - 0.5f * mid[10]) < 1e-6f && std::fabs (high[10] - 1.5f * mid[10]) < 1e-6f,
               "importance 0.5 leaves the claim as it is, 0 halves it, 1 makes it one and a half times");
    }

    // ---- [3] the level bank ----------------------------------------------------------------------------------------------------------------------
    std::printf ("[3] the level bank\n");
    {
        LevelBank bank;
        bank.prepare (kFs);
        const int centre = bandForHz (1995.0);
        for (long long n = 0; n < (long long) (kFs * 1.0); ++n)
            bank.push ((double) sine (bandCentreHz (centre), 0.25, n));
        float level[kBands];
        bank.read (level);
        check (std::fabs (level[centre] - toDb (0.25)) < 0.5, fmt ("a sine of 0.25 on the centre of band %d reads %.2f dB (should be %.2f)", centre, level[centre], toDb (0.25)));
        float worstNear = -200.0f, worstFar = -200.0f;
        for (int k = 0; k < kBands; ++k)
        {
            if (std::abs (k - centre) == 3) worstNear = std::fmax (worstNear, level[k]);
            if (std::abs (k - centre) >= 6) worstFar = std::fmax (worstFar, level[k]);
        }
        check (worstNear < level[centre] - 20.0f, fmt ("three bands away: %.1f dB under it", level[centre] - worstNear));
        check (worstFar < level[centre] - 40.0f, fmt ("six or more bands away: %.1f dB under it", level[centre] - worstFar));

        // Every band reads its own centre.
        bool all = true;
        for (int k = 0; k < kBands; ++k)
        {
            LevelBank b;
            b.prepare (kFs);
            for (long long n = 0; n < (long long) (kFs * 0.8); ++n)
                b.push ((double) sine (bandCentreHz (k), 0.5, n));
            float lv[kBands];
            b.read (lv);
            int top = 0;
            for (int j = 1; j < kBands; ++j)
                if (lv[j] > lv[top]) top = j;
            all = all && top == k && std::fabs (lv[k] - toDb (0.5)) < 0.8;
        }
        check (all, "a sine on any band's centre is loudest in that band and reads within 0.8 dB");

        bank.reset();
        bank.read (level);
        check (level[0] == kFloorDb && level[centre] == kFloorDb, "silence reads the floor, -120 dB");
    }

    // ---- [4] the cut -----------------------------------------------------------------------------------------------------------------------------
    std::printf ("[4] the cut\n");
    {
        Carver carver;
        carver.prepare (kFs);
        float t[kBands];
        std::fill (t, t + kBands, -6.0f);
        carver.setTarget (t);
        double worst = 0.0;
        for (int k = 0; k < kBands; ++k)
            worst = std::fmax (worst, std::fabs (carver.responseDb (bandCentreHz (k)) + 6.0));
        check (worst < 0.4, fmt ("a flat -6 dB is -6 dB at every centre (worst error %.2f dB; the bells overlap, the inverse takes that out)", worst));

        double worstNotch = 0.0, worstNeighbour = 0.0, worstFar = 0.0;
        for (int k = 0; k < kBands; ++k)
        {
            Carver c;
            c.prepare (kFs);
            std::fill (t, t + kBands, 0.0f);
            t[k] = -9.0f;
            c.setTarget (t);
            worstNotch = std::fmax (worstNotch, std::fabs (c.responseDb (bandCentreHz (k)) + 9.0));
            for (int j = 0; j < kBands; ++j)
            {
                if (std::abs (j - k) == 1) worstNeighbour = std::fmax (worstNeighbour, std::fabs (c.responseDb (bandCentreHz (j))));
                if (std::abs (j - k) >= 3) worstFar = std::fmax (worstFar, std::fabs (c.responseDb (bandCentreHz (j))));
            }
        }
        check (worstNotch < 0.4, fmt ("one band cut by 9 dB is 9 dB down at its own centre, whichever band it is (worst error %.2f dB)", worstNotch));
        check (worstFar < 0.4, fmt ("... and three or more bands away it is untouched (worst %.2f dB)", worstFar));
        std::printf ("      (the next band is %.2f dB off at the worst: that is the target at the centres, the overlap is inverted, so zero)\n", worstNeighbour);

        // In the time domain, through the filters.
        Carver c;
        c.prepare (kFs);
        std::fill (t, t + kBands, 0.0f);
        t[bandForHz (1995.0)] = -9.0f;
        c.setTarget (t);
        double peakIn = 0.0, peakOut = 0.0;
        for (long long n = 0; n < (long long) kFs; ++n)
        {
            double l = (double) sine (bandCentreHz (bandForHz (1995.0)), 0.5, n), r = l;
            c.process (l, r);
            if (n > (long long) (kFs * 0.5))
            {
                peakIn = std::fmax (peakIn, 0.5);
                peakOut = std::fmax (peakOut, std::fabs (l));
            }
        }
        check (std::fabs (toDb (peakOut / peakIn) + 9.0) < 0.5, fmt ("a sine through it comes out %.2f dB (asked -9)", toDb (peakOut / peakIn)));

        // Nothing wanted: bit for bit the input.
        Carver idle;
        idle.prepare (kFs);
        std::fill (t, t + kBands, 0.0f);
        idle.setTarget (t);
        bool exact = true;
        for (long long n = 0; n < 5000; ++n)
        {
            const float x = noise (n);
            double l = x, r = x * 0.5;
            idle.process (l, r);
            exact = exact && l == (double) x && r == (double) (x * 0.5);
        }
        check (exact, "with no cut wanted the thirty bells are the identity, bit for bit");

        // Taking a cut back returns to the identity.
        std::fill (t, t + kBands, -6.0f);
        idle.setTarget (t);
        std::fill (t, t + kBands, 0.0f);
        idle.setTarget (t);
        idle.reset();
        exact = true;
        for (long long n = 0; n < 5000; ++n)
        {
            const float x = noise (n, 3);
            double l = x, r = x;
            idle.process (l, r);
            exact = exact && l == (double) x;
        }
        check (exact, "a cut set and then taken back leaves the identity again");

        // Another sample rate: the bands above what it can reach are left alone and the rest still work.
        Carver low;
        low.prepare (32000.0);
        std::fill (t, t + kBands, -6.0f);
        low.setTarget (t);
        check (std::fabs (low.responseDb (1000.0) + 6.0) < 0.6 && std::isfinite (low.responseDb (15000.0)), "at 32 kHz the cut still works under what it can reach");
    }

    // ---- [5] one stem on its own -----------------------------------------------------------------------------------------------------------------
    std::printf ("[5] a stem with nobody to see\n");
    {
        Bus bus;
        Stem1 a (bus, Role::bass);
        std::uint32_t clock = 1000;
        const auto out = run ({ &a }, 1.0, 256, [] (int, long long n) { return 0.3f * noise (n) + sine (80.0, 0.2, n); }, clock);
        check (out.out[0] == out.in[0], "alone in its mix the stem passes bit for bit");
        check (a.engine.linkedStems() == 0, "... and it knows it is alone");

        Bus bus2;
        Stem1 b (bus2, Role::bass), c (bus2, Role::leadVocal);
        b.params.group = 0;
        c.params.group = 0;
        clock = 1000;
        const auto off = run ({ &b, &c }, 1.0, 256, [] (int, long long n) { return sine (79.06, 0.25, n); }, clock);
        check (off.out[0] == off.in[0] && off.out[1] == off.in[1] && b.engine.linkedStems() == 0, "with the mix Off two stems do not see each other and neither is changed");

        Bus bus3;
        Stem1 d (bus3, Role::bass), e (bus3, Role::leadVocal);
        e.params.depthDb = 0.0f;
        clock = 1000;
        const auto zero = run ({ &d, &e }, 1.0, 256, [] (int, long long n) { return sine (79.06, 0.25, n); }, clock);
        check (zero.out[1] == zero.in[1], "a depth of 0 dB cuts nothing, bit for bit");
    }

    // ---- [6] two stems in one mix ------------------------------------------------------------------------------------------------------------------
    std::printf ("[6] two stems make room for each other\n");
    {
        Bus bus;
        Stem1 bass (bus, Role::bass), vocal (bus, Role::leadVocal);
        std::uint32_t clock = 1000;
        const double lowTone = bandCentreHz (5), highTone = bandCentreHz (19);
        const auto r1 = run ({ &bass, &vocal }, 2.0, 256, [&] (int, long long n) { return sine (lowTone, 0.25, n); }, clock);
        const double bassCut = tailDb (r1.out[0], 0.5) - tailDb (r1.in[0], 0.5), vocalCut = tailDb (r1.out[1], 0.5) - tailDb (r1.in[1], 0.5);
        check (std::fabs (vocalCut + 6.0) < 0.7, fmt ("both on %.0f Hz: the vocal gives it up, %.2f dB (depth 6)", lowTone, vocalCut));
        check (bassCut > -0.2, fmt ("... and the bass, which the band belongs to, is left alone (%.2f dB)", bassCut));
        check (bass.engine.linkedStems() == 1 && vocal.engine.linkedStems() == 1, "each sees the other");

        Bus bus2;
        Stem1 bass2 (bus2, Role::bass), vocal2 (bus2, Role::leadVocal);
        clock = 1000;
        const auto r2 = run ({ &bass2, &vocal2 }, 2.0, 256, [&] (int, long long n) { return sine (highTone, 0.25, n); }, clock);
        const double bassCut2 = tailDb (r2.out[0], 0.5) - tailDb (r2.in[0], 0.5), vocalCut2 = tailDb (r2.out[1], 0.5) - tailDb (r2.in[1], 0.5);
        check (std::fabs (bassCut2 + 6.0) < 0.7 && vocalCut2 > -0.2, fmt ("both on %.0f Hz: now the bass gives way (%.2f dB) and the vocal does not (%.2f dB)", highTone, bassCut2, vocalCut2));

        // Different tones: nothing to make room for.
        Bus bus3;
        Stem1 bass3 (bus3, Role::bass), vocal3 (bus3, Role::leadVocal);
        clock = 1000;
        const auto r3 = run ({ &bass3, &vocal3 }, 2.0, 256, [&] (int s, long long n) { return s == 0 ? sine (lowTone, 0.25, n) : sine (highTone, 0.25, n); }, clock);
        check (tailDb (r3.out[1], 0.5) - tailDb (r3.in[1], 0.5) > -0.3 && tailDb (r3.out[0], 0.5) - tailDb (r3.in[0], 0.5) > -0.3,
               "a bass at 79 Hz and a voice at 2 kHz are each where they belong: neither is cut");
        // ... but a stem cut in a band where the other is, is cut for what the other holds, not for what it does: the cut is armed by the other's level there.
        check (std::fabs (vocal3.engine.cutShown[19].load()) < 0.05f, "a stem is cut only in the bands where the other has something");

        // Same role, same importance: neither claim is bigger.
        Bus bus4;
        Stem1 g1 (bus4, Role::guitar), g2 (bus4, Role::guitar);
        clock = 1000;
        const auto r4 = run ({ &g1, &g2 }, 1.5, 256, [&] (int, long long n) { return sine (bandCentreHz (17), 0.25, n); }, clock);
        check (r4.out[0] == r4.in[0] && r4.out[1] == r4.in[1], "two stems of the same role and importance leave each other alone: the one that matters more is yours to say");
        g2.params.importance = 0.9f;
        clock += 1000;
        const auto r5 = run ({ &g1, &g2 }, 2.0, 256, [&] (int, long long n) { return sine (bandCentreHz (17), 0.25, n); }, clock);
        check (tailDb (r5.out[0], 0.5) - tailDb (r5.in[0], 0.5) < -1.0 && tailDb (r5.out[1], 0.5) - tailDb (r5.in[1], 0.5) > -0.2,
               "raise one's IMPORTANCE and the other gives way to it");

        // Reach: how much quieter than itself a stem can let the other be and still make room. The bass is 12 dB under the vocal.
        Bus bus5;
        Stem1 loud (bus5, Role::leadVocal), quietBass (bus5, Role::bass);
        const auto reachSource = [&] (int s, long long n) { return s == 0 ? sine (lowTone, 0.25, n) : sine (lowTone, 0.0625, n); };
        loud.params.reachDb = 6.0f;
        clock = 1000;
        const auto r6 = run ({ &loud, &quietBass }, 2.0, 256, reachSource, clock);
        check (tailDb (r6.out[0], 0.5) - tailDb (r6.in[0], 0.5) > -0.2, "a vocal 12 dB louder than the bass in a band is not cut for it with a REACH of 6 dB: the bass is outside it");
        loud.params.reachDb = 24.0f;
        clock += 1000;
        const auto r7 = run ({ &loud, &quietBass }, 2.0, 256, reachSource, clock);
        check (std::fabs (tailDb (r7.out[0], 0.5) - tailDb (r7.in[0], 0.5) + 3.0) < 0.8, fmt ("... with a REACH of 24 dB it is half way in: %.2f dB of the 6", tailDb (r7.out[0], 0.5) - tailDb (r7.in[0], 0.5)));

        // The cut shown is the cut made.
        check (vocal.engine.carverRef().responseDb (lowTone) < -5.0, "the carver the editor draws holds the cut");
    }

    // ---- [7] a stem that has stopped ---------------------------------------------------------------------------------------------------------------
    std::printf ("[7] a stem that stops is forgotten\n");
    {
        Bus bus;
        Stem1 bass (bus, Role::bass), vocal (bus, Role::leadVocal);
        std::uint32_t clock = 1000;
        const double tone = bandCentreHz (5);
        run ({ &bass, &vocal }, 1.5, 256, [&] (int, long long n) { return sine (tone, 0.25, n); }, clock);
        const float cutWith = vocal.engine.cutShown[5].load();
        // The bass stops being processed (the transport stopped, the track was muted); the vocal goes on.
        const auto r = run ({ &vocal }, 3.0, 256, [&] (int, long long n) { return sine (tone, 0.25, n); }, clock);
        check (cutWith < -5.0f && vocal.engine.linkedStems() == 0 && std::fabs (vocal.engine.cutShown[5].load()) < 0.05f,
               fmt ("the cut was %.1f dB; half a second after the bass stops it is gone and the vocal sees nobody", cutWith));
        check (tailDb (r.out[0], 0.3) - tailDb (r.in[0], 0.3) > -0.2, "... and the vocal is as it came in");
    }

    // ---- [8] any block size -----------------------------------------------------------------------------------------------------------------------
    std::printf ("[8] the same for any host block size\n");
    {
        Bus bus;
        const int neighbour = bus.claim();
        float levels[kBands];
        for (int k = 0; k < kBands; ++k)
            levels[k] = -20.0f - 0.5f * (float) k;
        std::vector<std::vector<float>> results;
        for (int block : { 1, 7, 64, 100, 512, 4096 })
        {
            Engine engine (bus);
            engine.prepare (kFs);
            EngineParams p;
            p.role = Role::leadVocal;
            p.outDb = -3.0f;
            std::vector<float> l ((size_t) kFs), r ((size_t) kFs);
            for (size_t i = 0; i < l.size(); ++i)
                l[i] = 0.4f * noise ((long long) i) * (i < 20000 ? 1.0f : 0.1f), r[i] = 0.4f * noise ((long long) i, 5);
            std::vector<float> out;
            for (size_t start = 0; start < l.size(); start += (size_t) block)
            {
                const int n = (int) std::min<size_t> ((size_t) block, l.size() - start);
                // The neighbour is a bass, there all the time: its frame is kept fresh at the same clock.
                bus.publish (neighbour, 1, Role::bass, 0.5f, levels, 5000);
                engine.process (l.data() + start, r.data() + start, n, p, 5000);
            }
            out = l;
            out.insert (out.end(), r.begin(), r.end());
            results.push_back (std::move (out));
        }
        bool same = true;
        for (size_t i = 1; i < results.size(); ++i)
            same = same && results[i] == results[0];
        check (same, "blocks of 1, 7, 64, 100, 512 and 4096 samples give the very same output, bit for bit, while a neighbour is being cut for");
        bool changed = false;
        for (size_t i = 0; i < 20000; ++i)
            changed = changed || std::fabs (results[0][i] - 0.4f * noise ((long long) i) * 0.70794578f) > 1e-3f;
        check (changed, "... and it is not the input at -3 dB: the cut was working");
    }

    // ---- [9] the monitor and the output ------------------------------------------------------------------------------------------------------------
    std::printf ("[9] the cut alone, and the output gain\n");
    {
        Bus bus;
        Stem1 bass (bus, Role::bass), vocal (bus, Role::leadVocal);
        std::uint32_t clock = 1000;
        const double tone = bandCentreHz (5);
        run ({ &bass, &vocal }, 1.5, 256, [&] (int, long long n) { return sine (tone, 0.25, n); }, clock);
        const auto normal = run ({ &bass, &vocal }, 0.5, 256, [&] (int, long long n) { return sine (tone, 0.25, n); }, clock);
        vocal.engine.deltaOn = true;
        const auto delta = run ({ &bass, &vocal }, 0.5, 256, [&] (int, long long n) { return sine (tone, 0.25, n); }, clock);
        // The cut alone: what it takes away. 0.25 sine cut by 6 dB takes 0.25 * (1 - 0.5) = 0.125 away.
        check (std::fabs (tailDb (delta.out[1], 0.2) - toDb (0.125)) < 1.2, fmt ("DELTA is what the cut takes away: %.2f dB (a 6 dB cut of 0.25 takes %.2f)", tailDb (delta.out[1], 0.2), toDb (0.125)));
        check (tailDb (normal.out[1], 0.2) < tailDb (normal.in[1], 0.2) - 4.0, "... and without it the stem is the cut stem");
        vocal.engine.deltaOn = false;
        vocal.params.outDb = -6.0f;
        const auto quiet = run ({ &bass, &vocal }, 0.5, 256, [&] (int, long long n) { return sine (tone, 0.25, n); }, clock);
        check (std::fabs (tailDb (quiet.out[1], 0.2) - (tailDb (normal.out[1], 0.2) - 6.0)) < 0.3, "OUTPUT -6 dB lowers the stem by 6 dB");
    }

    // ---- [10] bad input --------------------------------------------------------------------------------------------------------------------------
    std::printf ("[10] bad samples\n");
    {
        Bus bus;
        Stem1 a (bus);
        std::vector<float> l (4096, 0.1f), r (4096, 0.1f);
        l[100] = INFINITY;
        r[200] = NAN;
        a.engine.process (l.data(), r.data(), 4096, a.params, 1000);
        bool fine = true;
        for (size_t i = 0; i < l.size(); ++i)
            fine = fine && std::isfinite (l[i]) && std::isfinite (r[i]);
        check (fine && l[100] == 0.0f && r[200] == 0.0f, "an infinite or NaN sample is replaced by silence on the way in: the output stays finite");
        float level[kBands];
        a.engine.ownLevel[10].load();
        for (int k = 0; k < kBands; ++k) level[k] = a.engine.ownLevel[(size_t) k].load();
        check (std::isfinite (level[10]), "... and so are the levels it publishes");
    }

    // ---- [11] the bus ----------------------------------------------------------------------------------------------------------------------------
    std::printf ("[11] the bus\n");
    {
        Bus bus;
        int slots[kMaxStems + 1];
        bool distinct = true;
        for (int i = 0; i < kMaxStems; ++i)
        {
            slots[i] = bus.claim();
            for (int j = 0; j < i; ++j)
                distinct = distinct && slots[i] != slots[j];
        }
        check (distinct && slots[0] >= 0, fmt ("%d stems each get their own slot", kMaxStems));
        check (bus.claim() == -1, "a thirty-third has none, and is refused rather than shared");
        bus.release (slots[3]);
        const int again = bus.claim();
        check (again == slots[3], "a released slot is the next one given");

        float lv[kBands];
        std::fill (lv, lv + kBands, -30.0f);
        for (int i = 0; i < 5; ++i)
            bus.publish (slots[i], i < 3 ? 1 : 2, Role::keys, 0.5f, lv, 100);
        check (bus.liveCount (1, 100) == 3 && bus.liveCount (2, 100) == 2 && bus.liveCount (3, 100) == 0 && bus.liveCount (0, 100) == 0, "stems see only their own mix, and the mix Off sees nobody");
        Stem got[kMaxStems];
        check (bus.gather (slots[0], 1, 100, got) == 2, "a stem does not gather itself");
        check (bus.liveCount (1, 100 + kFreshMs - 1) == 3 && bus.liveCount (1, 100 + kFreshMs) == 0, "a stem that has not published for 500 ms is gone");
        bus.publish (slots[0], 1, Role::kick, 0.8f, lv, 4294967200u);
        bus.publish (slots[2], 1, Role::keys, 0.5f, lv, 4294967250u);
        const int seen = bus.gather (slots[1], 1, 4294967200u + 300u, got); // 4294967500 wraps to 204
        bool foundKick = false;
        for (int i = 0; i < seen; ++i)
            foundKick = foundKick || (got[i].slot == slots[0] && got[i].role == Role::kick && std::fabs (got[i].importance - 0.8f) < 1e-6f);
        check (seen == 2 && foundKick, "across the wrap of the 32-bit millisecond counter a fresh stem is still fresh, and its role and importance come with it");
    }

    // ---- [12] the pressure -----------------------------------------------------------------------------------------------------------------------
    std::printf ("[12] the rule\n");
    {
        check (pressureOn (0.5f, 0.5f, -20.0f, -20.0f, 12.0f) == 0.0f && pressureOn (0.9f, 0.5f, -20.0f, -20.0f, 12.0f) == 0.0f, "a stem with the bigger or equal claim is never in the way");
        check (std::fabs (pressureOn (0.1f, 0.9f, -20.0f, -20.0f, 12.0f) - 1.0f) < 1e-6f, "a bigger claim, there and as loud: all of the depth");
        check (pressureOn (0.1f, 0.9f, -20.0f, -90.0f, 12.0f) == 0.0f, "a bigger claim that is not there (-90 dB) takes nothing");
        check (std::fabs (pressureOn (0.1f, 0.35f, -20.0f, -20.0f, 12.0f) - 0.5f) < 1e-6f, "a claim 0.25 bigger takes half");
        check (std::fabs (pressureOn (0.1f, 0.9f, -20.0f, -26.0f, 12.0f) - 0.5f) < 1e-6f, "an owner 6 dB under with a reach of 12 takes half");
    }

    return test::finish ("HestiaTests");
}
