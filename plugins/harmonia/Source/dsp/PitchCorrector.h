// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 The Pantheon authors

#pragma once

#include "Scales.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace harmonia
{
struct CorrectorParams
{
    uint16_t noteMask = kAllNotes; // allowed pitch classes (bit 0 = C)
    float retuneMs = 40.0f;     // time constant for gliding to the target note (0 = instant)
    float strength = 1.0f;      // 0..1, how much of the correction is applied
    float natural = 0.7f;       // 0..1, how much of the singer's own vibrato/drift is kept
    float refA4 = 440.0f;
    float dwellMs = 70.0f;      // a new note must persist this long before it is adopted (0 = at once); undecided, the voice is left alone
};

// Works in cents-from-C-zero ("midi * 100") internally. Called once per analysis hop.
class PitchCorrector
{
public:
    struct Output
    {
        bool active = false;
        float inMidi = 0.0f;     // sung pitch
        float targetMidi = 0.0f; // note being aimed at
        float outMidi = 0.0f;    // corrected pitch
        float correctionCents = 0.0f;
    };

    void prepare (double hopSeconds)
    {
        dt = (float) hopSeconds;
        reset();
    }

    void reset()
    {
        active = false;
        cs = 0.0f;
        unvoicedTime = 0.0f;
        med[0] = med[1] = med[2] = 0.0f;
        medCount = 0;
        pendingCount = 0;
        tick = 0;
        switchTimer = 0.0f;
        dwellTimer = 0.0f;
        ringCount = 0;
        centreCount = 0;
        centreSpeed = 0.0f;
        confirmedTime = 0.0f;
        resetVibrato (0.0f);
    }

    // True while the singer's pitch is oscillating like a vibrato (note target is held steady).
    bool vibratoActive() const { return vibrato; }

    Output process (bool voiced, float f0, const CorrectorParams& p)
    {
        Output out;

        ++tick;

        if (! voiced || f0 <= 0.0f)
        {
            unvoicedTime += dt;
            // Let the correction relax during gaps so a new phrase starts clean.
            cs *= std::exp (-dt / 0.06f);
            medCount = 0;
            if (unvoicedTime > 0.06f)
                active = false;
            out.correctionCents = cs;
            return out;
        }

        unvoicedTime = 0.0f;

        const float midi = 69.0f + 12.0f * std::log2 (f0 / p.refA4);
        float x = midi * 100.0f;

        // Median of three kills single-hop octave blips.
        med[0] = med[1];
        med[1] = med[2];
        med[2] = x;
        if (medCount < 3)
            ++medCount;
        if (medCount == 3)
            x = std::max (std::min (med[0], med[1]), std::min (std::max (med[0], med[1]), med[2]));

        // Ignore a big sudden leap until it has persisted for a few hops: at the end of a
        // note the detector often reports garbage for a moment.
        if (active && std::fabs (x - center) > 700.0f)
        {
            if (pendingCount > 0 && std::fabs (x - pendingX) < 150.0f)
                ++pendingCount;
            else
            {
                pendingCount = 1;
                pendingX = x;
            }

            if (pendingCount < 4)
                x = lastX;
            else
                active = false; // a real leap: treat it as a new note
        }
        else
            pendingCount = 0;
        lastX = x;

        if (! active)
        {
            active = true;
            pendingCount = 0;
            center = x;
            target = nearestAllowed (x / 100.0f, p.noteMask);
            switchTimer = 0.0f;
            ringCount = 0;
            centreCount = 0;
            centreSpeed = 0.0f;
            confirmedTime = 0.0f;
            pushPitch (x);
            resetVibrato (x);
        }
        else
        {
            pushPitch (x);
            trackVibrato (x);

            if (vibrato)
            {
                // Vibrato: the centre is the mean over one whole cycle, which does not depend on
                // where in the cycle we are. The fast "follow the voice" shortcuts below would just
                // chase the oscillation, so they are off.
                vibCentre = cycleMean();
                center += (1.0f - std::exp (-dt / 0.03f)) * (vibCentre - center);
            }
            else
            {
                // "Centre" of the sung note: a slow average (vibrato averages out). It speeds
                // up when the voice moves further than a vibrato would, so that real note
                // changes are recognised quickly.
                const float away = std::clamp ((std::fabs (x - center) - 20.0f) / 30.0f, 0.0f, 1.0f);
                const float tau = 0.07f + away * (0.02f - 0.07f);
                center += (1.0f - std::exp (-dt / tau)) * (x - center);
                const float band = 45.0f;
                center = std::clamp (center, x - band, x + band);
            }
        }

        // A slowly wandering pitch (drift, a note sagging or creeping) is not a new note: the further
        // it creeps past the halfway point the more it takes to give up the current target. A fast
        // move still switches almost at once.
        // Only a target the voice has actually sat on is held this way: while a note is still
        // settling (a scoop, a late-starting vibrato) the target is free to follow the voice.
        updateCentreSpeed();
        if (std::fabs (center - target * 100.0f) < 40.0f)
            confirmedTime += dt;
        const float slow = (1.0f - std::clamp ((centreSpeed - 70.0f) / 130.0f, 0.0f, 1.0f)) * std::clamp ((confirmedTime - 0.15f) / 0.25f, 0.0f, 1.0f);
        const float hysteresis = (vibrato ? 0.25f : 0.10f) + slow * 0.30f;

        // During vibrato the note only changes once the centre has clearly and durably moved on:
        // a swing that straddles a note boundary must not make the target flip back and forth.
        const float candidate = chooseTarget (center / 100.0f, p.noteMask, hysteresis);
        bool undecided = false;
        if (candidate == target)
        {
            switchTimer = 0.0f;
            dwellTimer = 0.0f;
        }
        else if (! vibrato || ! allowed ((int) std::lround (target), p.noteMask))
        {
            // A brief excursion to a neighbouring note (a wobble, a blip of the detector) must not drag the
            // voice there: until the new note has persisted, the singer is left as sung.
            if (p.dwellMs > 0.0f && allowed ((int) std::lround (target), p.noteMask))
            {
                if (candidate != dwellCandidate)
                {
                    dwellCandidate = candidate;
                    dwellTimer = 0.0f;
                }
                dwellTimer += dt;
                undecided = dwellTimer * 1000.0f < p.dwellMs;
            }
            if (! undecided)
            {
                target = candidate;
                switchTimer = 0.0f;
                confirmedTime = 0.0f;
                dwellTimer = 0.0f;
            }
        }
        else
        {
            switchTimer += dt;
            if (switchTimer >= 0.08f)
            {
                target = candidate;
                switchTimer = 0.0f;
                confirmedTime = 0.0f;
            }
        }

        const float dev = x - center;
        float ct = (target * 100.0f - center) - (1.0f - p.natural) * dev;
        ct *= p.strength;
        ct = std::clamp (ct, -400.0f, 400.0f);
        if (undecided || ! anyAllowed (p.noteMask))
            ct = 0.0f; // undecided, or no note to aim at: the voice is left as sung

        const float alpha = p.retuneMs <= 0.5f ? 1.0f : 1.0f - std::exp (-dt * 1000.0f / p.retuneMs);
        cs += alpha * (ct - cs);

        out.active = true;
        out.inMidi = x / 100.0f;
        out.targetMidi = target;
        out.outMidi = (x + cs) / 100.0f;
        out.correctionCents = cs;
        return out;
    }

    float currentCorrectionCents() const { return cs; }
    float centreCents() const { return center; }
    float centreSpeedCentsPerSec() const { return centreSpeed; }

private:
    static bool allowed (int note, uint16_t mask)
    {
        return (mask & (1u << pitchClass (note))) != 0;
    }

    // An empty mask means "no notes to snap to": the voice is left as it is.
    static bool anyAllowed (uint16_t mask) { return (mask & kAllNotes) != 0; }

    static float nearestAllowed (float m, uint16_t mask)
    {
        if (! anyAllowed (mask))
            return m;
        const int base = (int) std::lround (m);
        float best = (float) base;
        float bestDist = 1e9f;
        for (int n = base - 7; n <= base + 7; ++n)
        {
            if (! allowed (n, mask))
                continue;
            const float dist = std::fabs ((float) n - m);
            if (dist < bestDist)
            {
                bestDist = dist;
                best = (float) n;
            }
        }
        return best;
    }

    // The note to aim at for a sung centre `c`: the current target, or a neighbour once `c` is
    // clearly past the halfway point (by `hysteresis` semitones).
    float chooseTarget (float c, uint16_t mask, float hysteresis) const
    {
        if (! anyAllowed (mask))
            return c;
        float t = target;
        if (! allowed ((int) std::lround (t), mask))
            return nearestAllowed (c, mask);

        for (int iter = 0; iter < 6; ++iter)
        {
            int hi = (int) std::lround (t) + 1;
            while (! allowed (hi, mask))
                ++hi;
            int lo = (int) std::lround (t) - 1;
            while (! allowed (lo, mask))
                --lo;

            if (c > 0.5f * (t + (float) hi) + hysteresis)
                t = (float) hi;
            else if (c < 0.5f * (t + (float) lo) - hysteresis)
                t = (float) lo;
            else
                break;
        }
        return t;
    }

    // ---- vibrato detection ----
    // Follows the pitch with a zig-zag filter: a peak/trough is recorded once the pitch has turned
    // by more than `kTurn` cents. Four consecutive, alternating extrema that are evenly spaced
    // (3-10 Hz) and of similar size make a vibrato.
    struct Extremum
    {
        float v = 0.0f;
        int64_t tick = 0;
    };

    void resetVibrato (float x)
    {
        vibrato = false;
        extCount = 0;
        runVal = x;
        runTick = tick;
        runDir = 1;
    }

    void pushExtremum (float v, int64_t t)
    {
        if (extCount == 4)
        {
            for (int i = 0; i < 3; ++i)
                ext[i] = ext[i + 1];
            extCount = 3;
        }
        ext[extCount++] = { v, t };

        vibrato = false;
        if (extCount < 3)
            return;

        // The last four extrema (three swings) make a vibrato; a clearly pronounced pair of swings
        // (up and back down, or the reverse) is already enough for a big one, which would otherwise
        // be taken for a run of different notes while it builds up.
        for (int n = 4; n >= 3 && ! vibrato; --n)
        {
            if (extCount < n)
                continue;
            const bool full = n == 4;
            const Extremum* e = ext + (extCount - n);
            float sMin = 1e9f, sMax = 0.0f, hMin = 1e9f, hMax = 0.0f, hSum = 0.0f;
            for (int i = 1; i < n; ++i)
            {
                const float sw = std::fabs (e[i].v - e[i - 1].v);
                const float h = (float) (e[i].tick - e[i - 1].tick) * dt;
                sMin = std::min (sMin, sw);
                sMax = std::max (sMax, sw);
                hMin = std::min (hMin, h);
                hMax = std::max (hMax, h);
                hSum += h;
            }
            // Full: 1.3 - 12 Hz, up to 6 semitones peak to peak. A drift under the vibrato makes the
            // up and down swings differ, so the swing sizes are only loosely matched.
            const bool ok = full ? (hMin >= 0.04f && hMax <= 0.38f && sMin >= 24.0f && sMax <= 600.0f && sMax <= 3.0f * sMin && hMax <= 2.0f * hMin)
                                 : (hMin >= 0.05f && hMax <= 0.30f && sMin >= 60.0f && sMax <= 600.0f && sMax <= 1.8f * sMin && hMax <= 1.6f * hMin);
            if (ok)
            {
                vibrato = true;
                vibSwingMax = sMax;
                vibHalfPeriod = hSum / (float) (n - 1);
            }
        }
    }

    // ---- recent pitch, for the centre of a vibrato ----
    static constexpr int kRing = 256;

    void pushPitch (float x)
    {
        pitchRing[(size_t) (ringPos & (kRing - 1))] = x;
        ++ringPos;
        ringCount = std::min (ringCount + 1, kRing);
    }

    // Mean pitch over the most recent whole vibrato cycle: the oscillation cancels out.
    float cycleMean() const
    {
        const int n = std::clamp ((int) std::lround (2.0f * vibHalfPeriod / dt), 8, std::min (ringCount, kRing));
        double sum = 0.0;
        for (int i = 1; i <= n; ++i)
            sum += pitchRing[(size_t) ((ringPos - i) & (kRing - 1))];
        return (float) (sum / n);
    }

    // How fast the centre is moving (cents per second), measured over the last 0.2 s.
    void updateCentreSpeed()
    {
        constexpr int kSpan = 50; // hops
        centreRing[(size_t) (centreCount & 63)] = center;
        ++centreCount;
        if (centreCount <= kSpan)
        {
            centreSpeed = 0.0f;
            return;
        }
        const float older = centreRing[(size_t) ((centreCount - 1 - kSpan) & 63)];
        centreSpeed = std::fabs (center - older) / ((float) kSpan * dt);
    }

    void trackVibrato (float x)
    {
        constexpr float kTurn = 12.0f;
        if (runDir > 0)
        {
            if (x >= runVal)
            {
                runVal = x;
                runTick = tick;
            }
            else if (runVal - x > kTurn)
            {
                pushExtremum (runVal, runTick);
                runDir = -1;
                runVal = x;
                runTick = tick;
            }
        }
        else
        {
            if (x <= runVal)
            {
                runVal = x;
                runTick = tick;
            }
            else if (x - runVal > kTurn)
            {
                pushExtremum (runVal, runTick);
                runDir = 1;
                runVal = x;
                runTick = tick;
            }
        }

        if (! vibrato)
            return;

        // The oscillation has stopped, or the voice has gone further from the centre than any
        // swing so far: a real note change. Back to fast tracking.
        const float sinceExtremum = (float) (tick - ext[extCount - 1].tick) * dt;
        const bool stale = sinceExtremum > std::max (0.22f, 1.7f * vibHalfPeriod);
        const bool leapt = std::fabs (x - center) > 0.75f * vibSwingMax + 25.0f;
        if (stale || leapt)
            resetVibrato (x);
    }

    float dt = 0.004f;
    bool active = false;
    float center = 0.0f, target = 0.0f, cs = 0.0f, unvoicedTime = 0.0f;
    float med[3] = { 0, 0, 0 };
    int medCount = 0;
    float lastX = 0.0f, pendingX = 0.0f;
    int pendingCount = 0;

    float pitchRing[kRing] = {};
    int64_t ringPos = 0;
    int ringCount = 0;
    float centreRing[64] = {};
    int centreCount = 0;
    float centreSpeed = 0.0f, confirmedTime = 0.0f;

    int64_t tick = 0;
    float switchTimer = 0.0f, dwellTimer = 0.0f, dwellCandidate = 0.0f;
    bool vibrato = false;
    Extremum ext[4];
    int extCount = 0;
    float runVal = 0.0f, vibCentre = 0.0f, vibSwingMax = 0.0f, vibHalfPeriod = 0.1f;
    int64_t runTick = 0;
    int runDir = 1;
};
} // namespace harmonia
