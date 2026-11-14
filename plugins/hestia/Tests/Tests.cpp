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
        e.params.carveDb = 0.0f;
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
            p.distance = 0.6f;
            std::vector<float> l ((size_t) kFs), r ((size_t) kFs);
            for (size_t i = 0; i < l.size(); ++i)
                l[i] = 0.4f * noise ((long long) i) * (i < 20000 ? 1.0f : 0.1f), r[i] = 0.4f * noise ((long long) i, 5);
            std::vector<float> out;
            for (size_t start = 0; start < l.size(); start += (size_t) block)
            {
                const int n = (int) std::min<size_t> ((size_t) block, l.size() - start);
                // The neighbour is a bass, there all the time: its frame is kept fresh at the same clock.
                bus.publish (neighbour, 1, Role::bass, 0.5f, 0.0f, levels, 5000);
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
        check (changed, "... and it is not the input at -3 dB: the cut and the depth (60%) were working");
    }

    // ---- [9] the monitor and the output ------------------------------------------------------------------------------------------------------------
    std::printf ("[9] the monitor, and the output gain\n");
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
        check (std::fabs (tailDb (delta.out[1], 0.2) - toDb (0.125)) < 1.2, fmt ("DELTA is what Hestia takes away: %.2f dB (a 6 dB cut of 0.25 takes %.2f)", tailDb (delta.out[1], 0.2), toDb (0.125)));
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
            bus.publish (slots[i], i < 3 ? 1 : 2, Role::keys, 0.5f, 0.0f, lv, 100);
        check (bus.liveCount (1, 100) == 3 && bus.liveCount (2, 100) == 2 && bus.liveCount (3, 100) == 0 && bus.liveCount (0, 100) == 0, "stems see only their own mix, and the mix Off sees nobody");
        Stem got[kMaxStems];
        check (bus.gather (slots[0], 1, 100, got) == 2, "a stem does not gather itself");
        check (bus.liveCount (1, 100 + kFreshMs - 1) == 3 && bus.liveCount (1, 100 + kFreshMs) == 0, "a stem that has not published for 500 ms is gone");
        bus.publish (slots[0], 1, Role::kick, 0.8f, 0.0f, lv, 4294967200u);
        bus.publish (slots[2], 1, Role::keys, 0.5f, 0.0f, lv, 4294967250u);
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

    // ---- [13] depth ---------------------------------------------------------------------------------------------------------------------------------
    std::printf ("[13] depth: how far back a stem sits\n");
    {
        const auto front = cuesFor (0.0f), back = cuesFor (1.0f);
        check (front.airDb == 0.0f && front.levelGain == 1.0f && front.widthLoss == 0.0f && front.reflections == 0.0f, "at the front every cue is exactly nothing");
        check (std::fabs (back.airDb + kAirMaxCutDb) < 1e-6f && std::fabs (20.0f * std::log10 (back.levelGain) + kLevelMaxCutDb) < 1e-4f
                   && std::fabs (back.widthLoss - kWidthMaxLoss) < 1e-6f && std::fabs (back.reflections - kReflectMax) < 1e-6f && cuesFor (7.0f).airDb == back.airDb,
               "at the back they are what Depth.h says (9 dB of air, 4 dB of level, 30% of the sides, 0.45 of room), and a distance past 1 is 1");

        // The impulse response of a stem that sits at the back, stereo, alone in its mix.
        Bus bus;
        Engine engine (bus);
        engine.prepare (kFs);
        EngineParams p;
        p.distance = 1.0f;
        p.group = 0;
        const int total = (int) (1.0 * kFs), at = (int) (0.5 * kFs);
        std::vector<float> l ((size_t) total, 0.0f), r ((size_t) total, 0.0f);
        l[(size_t) at] = 1.0f; // left only: the width shows
        for (int start = 0; start < total; start += 480)
            engine.process (l.data() + start, r.data() + start, std::min (480, total - start), p, 1000);
        // After half a second the depth has settled; the impulse comes through the settled stem. Run it again from there: the first run was the settling.
        std::vector<float> il ((size_t) total, 0.0f), ir ((size_t) total, 0.0f);
        il[(size_t) 2000] = 1.0f;
        for (int start = 0; start < total; start += 480)
            engine.process (il.data() + start, ir.data() + start, std::min (480, total - start), p, 1000);
        const double fsd = kFs;
        pdsp::Biquad shelf;
        shelf.setHighShelf (fsd, (double) kAirHz, 0.7071, -(double) kAirMaxCutDb);
        const double side = 0.5 * (1.0 - 0.0), loss = (double) kWidthMaxLoss * side;
        const double expectL = (shelf.b0 - loss * shelf.b0) * back.levelGain, expectR = (loss * shelf.b0) * back.levelGain;
        check (std::fabs (il[2000] - expectL) < 1e-4 && std::fabs (ir[2000] - expectR) < 1e-4,
               fmt ("the direct sound at the back: left %.4f (expected %.4f), right %.4f (expected %.4f): the high shelf, the narrowing and the level", il[2000], expectL, ir[2000], expectR));

        // The reflections: each tap, on the channel it belongs to, at its gain times the room, through the one-pole.
        const double a = 1.0 - std::exp (-2.0 * kPi * EarlyReflections::kDampHz / kFs);
        bool taps = true;
        double worst = 0.0;
        for (int t = 0; t < EarlyReflections::kTaps; ++t)
        {
            const int index = 2000 + (int) std::lround (EarlyReflections::kDelayMs[0][t] * 0.001 * kFs);
            // The room is fed the stem before the shelf, and what comes out is multiplied by the level like everything else.
            const double expect = (double) kReflectMax * (double) EarlyReflections::kTapGain[t] * a * (double) back.levelGain;
            worst = std::fmax (worst, std::fabs (il[(size_t) index] - expect) / expect);
            taps = taps && std::fabs (il[(size_t) index] - expect) < 0.05 * expect;
        }
        double rightRoom = 0.0;
        for (int t = 0; t < EarlyReflections::kTaps; ++t)
            rightRoom = std::fmax (rightRoom, std::fabs (ir[(size_t) (2000 + (int) std::lround (EarlyReflections::kDelayMs[1][t] * 0.001 * kFs))]));
        check (taps && rightRoom < 1e-5, fmt ("a left impulse: its four reflections come on the left at 6.3, 13.1, 21.7 and 31.9 ms, at 0.45 x (0.5, 0.4, 0.3, 0.2) x the damping pole x the level (worst %.1f%% off), and the right has none", 100.0 * worst));
        double after = 0.0;
        for (int i = 2000 + (int) (0.045 * kFs); i < total; ++i)
            after = std::fmax (after, std::fmax (std::fabs (il[(size_t) i]), std::fabs (ir[(size_t) i])));
        check (after < 1e-4, fmt ("no reverb tail: 45 ms after the impulse there is nothing left (%.1e)", after));
        check (EarlyReflections::longestMs() < 40.0, "the longest reflection is under 40 ms");

        // At the front the stem is untouched, whatever it was before.
        p.distance = 0.0f;
        std::vector<float> fl ((size_t) total), fr ((size_t) total);
        for (int i = 0; i < total; ++i) fl[(size_t) i] = fr[(size_t) i] = 0.3f * noise (i);
        const auto keep = fl;
        for (int start = 0; start < total; start += 480)
            engine.process (fl.data() + start, fr.data() + start, std::min (480, total - start), p, 1000);
        bool back0 = true;
        for (int i = (int) (0.6 * kFs); i < total; ++i)
            back0 = back0 && fl[(size_t) i] == keep[(size_t) i] && fr[(size_t) i] == keep[(size_t) i];
        check (back0, "taken back to 0% the depth ends exactly: bit for bit the input again, after it has settled");

        // A stem at the back gives way to one at the front.
        Bus bus2;
        Stem1 near (bus2, Role::guitar), far (bus2, Role::guitar);
        far.params.distance = 0.8f;
        std::uint32_t clock = 1000;
        const auto pair = run ({ &near, &far }, 2.0, 256, [&] (int, long long n) { return sine (bandCentreHz (17), 0.25, n); }, clock);
        Bus bus3;
        Stem1 alone (bus3, Role::guitar);
        alone.params.distance = 0.8f;
        alone.params.group = 0;
        std::uint32_t clock3 = 1000;
        const auto lone = run ({ &alone }, 2.0, 256, [&] (int, long long n) { return sine (bandCentreHz (17), 0.25, n); }, clock3);
        const double carved = (tailDb (pair.out[1], 0.5) - tailDb (pair.in[1], 0.5)) - (tailDb (lone.out[0], 0.5) - tailDb (lone.in[0], 0.5));
        // The far one has 0.8 x 0.35 = 28% less claim: 0.224 of a span of 0.5 is 45% of the carve (6 dB), 2.7 dB.
        check (tailDb (pair.out[0], 0.5) - tailDb (pair.in[0], 0.5) > -0.2 && std::fabs (carved + 2.7) < 0.7,
               fmt ("two guitars on one tone, one at 80%% depth: the near one is untouched, the far one gives up %.1f dB (expected 2.7) on top of what its depth does by itself", -carved));
        Stem got[kMaxStems];
        check (bus2.gather (near.engine.slotIndex(), 1, clock, got) == 1 && std::fabs (got[0].distance - 0.8f) < 1e-6f, "a stem's depth is published to the others");
        check (std::fabs (claims (Role::guitar, 0.5f, 1.0f)[10] - (1.0f - kDistanceYield) * claims (Role::guitar, 0.5f)[10]) < 1e-6f, "a claim at the back is kDistanceYield less");
    }

    // ---- [14] the sidechain -----------------------------------------------------------------------------------------------------------------------
    std::printf ("[14] the sidechain: any track, with or without Hestia on it\n");
    {
        const double lowTone = bandCentreHz (5);
        const auto runWith = [&] (Engine& e, EngineParams p, bool connected, double seconds, int block, double keyAmp, std::vector<float>& out)
        {
            const int total = (int) (seconds * kFs);
            std::vector<float> l ((size_t) total), r ((size_t) total), key ((size_t) total);
            for (int i = 0; i < total; ++i)
            {
                l[(size_t) i] = r[(size_t) i] = sine (lowTone, 0.25, i);
                key[(size_t) i] = sine (lowTone, keyAmp, i);
            }
            for (int start = 0; start < total; start += block)
            {
                const int n = std::min (block, total - start);
                e.process (l.data() + start, r.data() + start, n, p, 1000, connected ? key.data() + start : nullptr, connected ? key.data() + start : nullptr);
            }
            out = l;
        };
        std::vector<float> out;
        std::vector<float> input ((size_t) kFs * 2);
        for (size_t i = 0; i < input.size(); ++i) input[i] = sine (lowTone, 0.25, (long long) i);

        Bus bus;
        Engine voice (bus);
        voice.prepare (kFs);
        EngineParams p;
        p.role = Role::leadVocal;
        p.scRole = Role::bass;
        runWith (voice, p, true, 2.0, 256, 0.25, out);
        const double sidechainCut = tailDb (out, 0.5) - tailDb (input, 0.5);
        check (tailDb (out, 0.5) - tailDb (input, 0.5) < -5.0 && voice.linkedStems() == 1 && voice.sidechainActive.load(),
               fmt ("a vocal with a bass on the sidechain, on the same tone: cut %.2f dB, and it counts the sidechain as the one stem it sees", tailDb (out, 0.5) - tailDb (input, 0.5)));
        check (voice.sidechainLevel[5].load() > -14.0f && voice.sidechainLevel[5].load() < -10.0f, fmt ("what the sidechain hears is published for the display: %.1f dB (a 0.25 sine is -12)", voice.sidechainLevel[5].load()));

        Bus bus2;
        Engine none (bus2);
        none.prepare (kFs);
        runWith (none, p, false, 2.0, 256, 0.25, out);
        check (out == input && ! none.sidechainActive.load() && none.linkedStems() == 0, "with no sidechain connected the stem passes bit for bit");

        Bus bus3;
        Engine off (bus3);
        off.prepare (kFs);
        EngineParams q = p;
        q.group = 0;
        runWith (off, q, true, 2.0, 256, 0.25, out);
        check (out == input, "with the mix Off the sidechain is not listened to: the stem is not linked to anything");

        Bus bus4;
        Engine quiet (bus4);
        quiet.prepare (kFs);
        runWith (quiet, p, true, 2.0, 256, 0.0, out);
        check (out == input, "a silent sidechain takes nothing");

        // The sidechain that goes away is forgotten, and comes back from clean filters.
        Bus bus5;
        Engine gone (bus5);
        gone.prepare (kFs);
        runWith (gone, p, true, 1.5, 256, 0.25, out);
        const float cutWith = gone.cutShown[5].load();
        runWith (gone, p, false, 3.0, 256, 0.25, out);
        check (cutWith < -5.0f && std::fabs (gone.cutShown[5].load()) < 0.05f && gone.sidechainLevel[5].load() == kFloorDb, "a sidechain that is disconnected ends its cut and its level on the display");

        // Same output for any host block size.
        std::vector<std::vector<float>> results;
        for (int block : { 1, 7, 64, 100, 512, 4096 })
        {
            Bus b;
            Engine e (b);
            e.prepare (kFs);
            EngineParams z = p;
            z.distance = 0.5f;
            std::vector<float> o;
            runWith (e, z, true, 1.0, block, 0.2, o);
            results.push_back (o);
        }
        bool same = true;
        for (size_t i = 1; i < results.size(); ++i) same = same && results[i] == results[0];
        check (same, "with a sidechain and a depth of 50%, blocks of 1, 7, 64, 100, 512 and 4096 samples give the very same output");

        // A track with Hestia on it and a track on the sidechain are the same to the rule: the same tone, the same cut.
        Bus bus6;
        Stem1 bass (bus6, Role::bass), vocal (bus6, Role::leadVocal);
        std::uint32_t clock = 1000;
        const auto viaBus = run ({ &bass, &vocal }, 2.0, 256, [&] (int, long long n) { return sine (lowTone, 0.25, n); }, clock);
        const double busCut = tailDb (viaBus.out[1], 0.5) - tailDb (viaBus.in[1], 0.5);
        check (std::fabs (busCut - sidechainCut) < 0.3, fmt ("the same cut by the bus (%.2f dB) as by the sidechain (%.2f dB)", busCut, sidechainCut));
    }

    // ---- [15] track names, HOLD, and COPY TO MIX -------------------------------------------------------------------------------------------------
    std::printf ("[15] names, hold, copy to mix\n");
    {
        Bus bus;
        const int a = bus.claim(), b = bus.claim();
        char name[kNameChars + 1];
        bus.nameOf (a, name);
        check (name[0] == '\0', "a new slot has no name");
        bus.setName (a, "Lead Vox 2");
        bus.setName (b, "A track name that is far too long to keep whole");
        char other[kNameChars + 1];
        bus.nameOf (a, name);
        bus.nameOf (b, other);
        check (std::strcmp (name, "Lead Vox 2") == 0 && std::strlen (other) == (size_t) kNameChars && std::strncmp (other, "A track name that is far too long", (size_t) kNameChars) == 0,
               fmt ("a slot keeps the host's name for its track ('%s'), cut at %d characters ('%s')", name, kNameChars, other));
        bus.release (a);
        const int again = bus.claim();
        bus.nameOf (again, name);
        check (again == a && name[0] == '\0', "a slot that is given back and claimed again has no name");

        // HOLD: the vocal gives up the bass's band while the bass plays; held, it keeps that cut after the bass has stopped.
        const double lowTone = bandCentreHz (5);
        for (const bool hold : { false, true })
        {
            Bus hb;
            Stem1 bass (hb, Role::bass), vocal (hb, Role::leadVocal);
            std::uint32_t clock = 1000;
            run ({ &bass, &vocal }, 2.0, 256, [&] (int, long long n) { return sine (lowTone, 0.25, n); }, clock);
            const float before = vocal.engine.cutShown[5].load();
            vocal.params.hold = hold;
            const auto after = run ({ &bass, &vocal }, 3.0, 256, [&] (int s, long long n) { return s == 0 ? 0.0f : sine (lowTone, 0.25, n); }, clock); // the bass goes quiet
            const double level = tailDb (after.out[1], 0.5) - tailDb (after.in[1], 0.5);
            if (hold)
                check (before < -5.0f && std::fabs (vocal.engine.cutShown[5].load() - before) < 0.01f && level < -5.0,
                       fmt ("HOLD: the cut of %.2f dB stays while the bass is silent for 3 s (the tone still comes out %.2f dB down)", before, level));
            else
                check (before < -5.0f && std::fabs (vocal.engine.cutShown[5].load()) < 0.05f && level > -0.3, fmt ("without HOLD the same cut lets go once the bass stops (%.2f dB)", level));
        }
        // Held from the start there is no cut to keep: the stem is the input, bit for bit, even with the bass playing under it.
        {
            Bus hb;
            Stem1 bass (hb, Role::bass), vocal (hb, Role::leadVocal);
            vocal.params.hold = true;
            std::uint32_t clock = 1000;
            const auto r = run ({ &bass, &vocal }, 1.5, 256, [&] (int, long long n) { return sine (lowTone, 0.25, n); }, clock);
            check (r.out[1] == r.in[1] && vocal.engine.cutShown[5].load() == 0.0f, "HOLD before any cut was made keeps no cut: the output is the input, bit for bit");
        }

        // COPY TO MIX: the others of the mix take the values; the sender, another mix and an instance made later do not.
        Bus cb;
        Stem1 sender (cb), mate (cb), stranger (cb);
        sender.params.group = 2;
        mate.params.group = 2;
        stranger.params.group = 3;
        std::uint32_t clock = 1000;
        run ({ &sender, &mate, &stranger }, 0.5, 256, [&] (int, long long n) { return sine (1000.0, 0.1, n); }, clock);
        Shared sent;
        sent.carveDb = 11.0f;
        sent.reachDb = 20.0f;
        sent.attackMs = 5.0f;
        sent.releaseMs = 400.0f;
        sender.engine.copyToMix (2, sent);
        Stem1 later (cb);
        later.params.group = 2;
        run ({ &sender, &mate, &stranger, &later }, 0.1, 256, [&] (int, long long n) { return sine (1000.0, 0.1, n); }, clock);
        Shared got;
        check (mate.engine.broadcastPending() && mate.engine.takeBroadcast (got) && got.carveDb == 11.0f && got.reachDb == 20.0f && got.attackMs == 5.0f && got.releaseMs == 400.0f,
               "another stem of the mix is told, and gets CARVE 11, REACH 20, ATTACK 5 and RELEASE 400");
        check (! mate.engine.broadcastPending(), "... once");
        check (! sender.engine.broadcastPending() && ! stranger.engine.broadcastPending() && ! later.engine.broadcastPending(),
               "the sender, a stem of another mix and an instance made after the copy are not");
    }

    return test::finish ("HestiaTests");
}
