// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 The Pantheon authors

#pragma once

#include "Kernels.h"
#include "Rings.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace harmonia
{
// YIN pitch detector tuned for sung vocals. It searches at a decimated rate (~24 kHz) and
// then refines the period at the full rate, which keeps the CPU cost low and the
// resolution well below a cent.
//
// The pitch is measured on a low-passed copy of the voice (4th-order Butterworth, cutoff about
// four times the pitch being tracked). The fundamental and its first harmonics carry the pitch;
// the band above mostly holds breath, sibilance and the residue left in separated stems or by
// bleed, which is not harmonic with the voice and can make YIN hop between two periods.
class PitchDetector
{
public:

    struct Result
    {
        bool voiced = false;
        float period = 0.0f;     // in full-rate samples, fractional
        float confidence = 0.0f; // 0..1
        float rms = 0.0f;
    };

    // minHz is the lowest pitch that will ever be asked for.
    void prepare (double sampleRate, float minHz)
    {
        fs = sampleRate;
        decim = std::max (1, (int) std::lround (fs / 24000.0));
        fsDec = fs / decim;
        tauMaxD = (int) std::ceil (fsDec / minHz) + 2;
        winD = tauMaxD;

        const int decNeeded = winD + tauMaxD + 4;
        int cap = 1;
        while (cap < decNeeded)
            cap <<= 1;
        dec.resize (cap);

        const int fullNeeded = (winD + tauMaxD) * decim + 8;
        cap = 1;
        while (cap < fullNeeded)
            cap <<= 1;
        full.resize (cap);
        fullLp.resize (cap);


        d.assign ((size_t) tauMaxD + 2, 0.0f);
        cm.assign ((size_t) tauMaxD + 2, 1.0f);
        reset();
    }

    void reset()
    {
        dec.clear();
        full.clear();
        fullLp.clear();
        lp[0].clear();
        lp[1].clear();
        setCutoff (kDefaultCutoffHz);
        accum = 0.0f;
        accumCount = 0;
    }

    // Keeps at least `samples` of full-rate input history (see fullRateHistory()).
    void reserveHistory (int samples)
    {
        if (samples > full.capacity())
            full.resize (samples);
    }
    const MirrorRing& fullRateHistory() const { return full; }

    // Number of full-rate samples of history the detector looks at.
    int windowSamples() const { return winD * decim; }
    int maxLagSamples() const { return tauMaxD * decim; }

    void push (float x)
    {
        full.push (x);
        const float y = lp[1].process (lp[0].process (x));
        fullLp.push (y);
        accum += y;
        if (++accumCount == decim)
        {
            dec.push (accum / (float) decim);
            accum = 0.0f;
            accumCount = 0;
        }
    }

    // prevPeriod: last confirmed period in full-rate samples (0 if none).
    Result detect (float minHz, float maxHz, float prevPeriod, bool wasVoiced)
    {
        Result res;

        const int tmin = std::max (2, (int) std::floor (fsDec / maxHz));
        const int tmax = std::min (tauMaxD, (int) std::ceil (fsDec / minHz));
        if (tmax <= tmin + 2)
            return res;

        const int total = winD + tauMaxD;
        const float* x = dec.tail (total);

        // Level gate on the newest window.
        res.rms = std::sqrt (kernels::sumSquares (x + tauMaxD, winD) / (float) winD);
        if (res.rms < 0.0015f) // about -56 dBFS
            return res;

        // Difference function, always comparing against the newest `winD` samples.
        for (int tau = 1; tau <= tmax; ++tau)
            d[(size_t) tau] = kernels::sumSquaredDiff (x + (tauMaxD - tau), x + tauMaxD, winD);

        cm[0] = 1.0f;
        float run = 0.0f;
        for (int tau = 1; tau <= tmax; ++tau)
        {
            run += d[(size_t) tau];
            cm[(size_t) tau] = run > 1e-12f ? d[(size_t) tau] * (float) tau / run : 1.0f;
        }

        // Collect dips below threshold.
        const float thr = wasVoiced ? 0.28f : 0.20f;
        struct Cand { int tau; float v; };
        Cand cands[6];
        int nc = 0;

        int tau = tmin;
        while (tau < tmax && nc < 6)
        {
            if (cm[(size_t) tau] < thr)
            {
                while (tau + 1 <= tmax && cm[(size_t) tau + 1] < cm[(size_t) tau])
                    ++tau;
                cands[nc++] = { tau, cm[(size_t) tau] };
                while (tau <= tmax && cm[(size_t) tau] < thr)
                    ++tau;
            }
            else
                ++tau;
        }

        if (nc == 0)
            return res;

        Cand chosen = cands[0];

        // Octave guard: a strong 2nd/3rd harmonic (e.g. sitting on a formant) can make the
        // half or third period look periodic. If a whole multiple of the first dip is
        // clearly deeper, the true period is that multiple.
        // While the first dip continues the voice being followed, it takes a dip that is no longer
        // periodic by YIN's own measure (0.1) to move away from it. A first formant near f0 / 2 (an
        // "ee" sung around D5) rings at twice the period after every jittered cycle, which deepens the
        // even multiples without the pitch heard being any lower; a real half-period error dips to about 0.2.
        const bool following = prevPeriod > 0.0f && std::fabs (std::log2 ((float) (chosen.tau * decim) / prevPeriod)) < 0.06f;
        if (chosen.v > (following ? 0.1f : 0.035f))
        {
            for (int i = 1; i < nc; ++i)
            {
                const float ratio = (float) cands[i].tau / (float) cands[0].tau;
                const float k = std::round (ratio);
                if (k >= 2.0f && k <= 4.0f && std::fabs (ratio - k) < 0.06f * k && cands[i].v < 0.5f * chosen.v)
                    chosen = cands[i];
            }
        }

        if (prevPeriod > 0.0f)
        {
            float bestDist = 1e9f;
            Cand closest = cands[0];
            for (int i = 0; i < nc; ++i)
            {
                const float dist = std::fabs (std::log2 ((float) (cands[i].tau * decim) / prevPeriod));
                if (dist < bestDist)
                {
                    bestDist = dist;
                    closest = cands[i];
                }
            }
            if (bestDist < 0.06f && closest.v <= chosen.v + 0.06f)
                chosen = closest;
        }

        // Fractional position at the decimated rate.
        float tauD = (float) chosen.tau;
        if (chosen.tau > 1 && chosen.tau < tmax)
        {
            const float c0 = cm[(size_t) chosen.tau - 1], c1 = cm[(size_t) chosen.tau], c2 = cm[(size_t) chosen.tau + 1];
            const float den = c0 - 2.0f * c1 + c2;
            if (std::fabs (den) > 1e-9f)
                tauD += std::clamp (0.5f * (c0 - c2) / den, -1.0f, 1.0f);
        }

        // Refine at the full rate around the coarse estimate.
        const int totalFull = total * decim;
        const float* fx = fullLp.tail (totalFull);
        const int Wf = winD * decim;
        const int maxLagF = tauMaxD * decim;
        const float coarse = tauD * (float) decim;
        const int lo = std::max (2, (int) std::floor (coarse) - decim - 1);
        const int hi = std::min (maxLagF, (int) std::ceil (coarse) + decim + 1);

        float bestD = 1e30f;
        int bestLag = lo;
        float dv[64];
        const int span = std::min (hi - lo + 1, 64);
        for (int i = 0; i < span; ++i)
        {
            const int lag = lo + i;
            const float s = kernels::sumSquaredDiff (fx + (maxLagF - lag), fx + maxLagF, Wf);
            dv[i] = s;
            if (s < bestD)
            {
                bestD = s;
                bestLag = lag;
            }
        }

        float period = (float) bestLag;
        const int bi = bestLag - lo;
        if (bi > 0 && bi < span - 1)
        {
            const float c0 = dv[bi - 1], c1 = dv[bi], c2 = dv[bi + 1];
            const float den = c0 - 2.0f * c1 + c2;
            if (std::fabs (den) > 1e-12f)
                period += std::clamp (0.5f * (c0 - c2) / den, -1.0f, 1.0f);
        }

        // Follow the voice with the filter: low voices keep their harmonics, high ones their fundamental.
        const float fcWanted = std::clamp (4.0f * (float) fs / period, kMinCutoffHz, kMaxCutoffHz);
        setCutoff (cutoffHz + 0.3f * (fcWanted - cutoffHz));

        res.voiced = true;
        res.period = period;
        res.confidence = std::clamp (1.0f - chosen.v, 0.0f, 1.0f);
        return res;
    }

private:
    static constexpr float kDefaultCutoffHz = 1500.0f, kMinCutoffHz = 1000.0f, kMaxCutoffHz = 3500.0f;

    struct LowPass
    {
        double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0, z1 = 0, z2 = 0;

        void design (double w0, double q)
        {
            const double c = std::cos (w0), alpha = std::sin (w0) / (2.0 * q), a0 = 1.0 + alpha;
            b0 = 0.5 * (1.0 - c) / a0;
            b1 = (1.0 - c) / a0;
            b2 = b0;
            a1 = -2.0 * c / a0;
            a2 = (1.0 - alpha) / a0;
        }
        void clear() { z1 = z2 = 0.0; }
        float process (float x)
        {
            const double y = b0 * x + z1;
            z1 = b1 * x - a1 * y + z2;
            z2 = b2 * x - a2 * y;
            return (float) y;
        }
    };

    // Butterworth sections (Q = 1 / (2 cos (pi/8)) and 1 / (2 cos (3 pi/8))).
    void setCutoff (float hz)
    {
        cutoffHz = hz;
        const double w0 = 2.0 * 3.14159265358979323846 * std::min ((double) hz, 0.45 * fs) / fs;
        lp[0].design (w0, 0.5411961);
        lp[1].design (w0, 1.3065630);
    }

    double fs = 48000.0, fsDec = 24000.0;
    float cutoffHz = kDefaultCutoffHz;
    int decim = 2, tauMaxD = 0, winD = 0;
    MirrorRing dec, full, fullLp; // `full` is the raw input; `dec` and `fullLp` come from the low-passed copy
    LowPass lp[2];
    float accum = 0.0f;
    int accumCount = 0;
    std::vector<float> d, cm;
};
} // namespace harmonia
