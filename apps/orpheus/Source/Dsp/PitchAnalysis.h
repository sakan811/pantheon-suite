// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 The Pantheon authors

#pragma once

#include "Pitch.h"

#include <array>
#include <functional>

// Offline pitch tracking of a single voice, built for accuracy rather than latency (the whole recording is known, so every
// decision can look at both sides of a frame):
//
//   1. low-pass and decimate to about 16 kHz;
//   2. per frame, the YIN difference function with cumulative-mean normalisation, and up to four candidate periods
//      (local minima), parabolically interpolated;
//   3. Viterbi over the whole recording: the cheapest path through candidates and "unvoiced", where a jump in pitch costs
//      in proportion to its size. An octave error is a jump up and a jump back, so it is only taken when it is much
//      better than staying put, which is what a single-frame decision cannot know;
//   4. each voiced frame refined at the full sample rate: the normalised correlation at integer lags around the period,
//      with a parabola through its peak (a few hundredths of a cent on a clean harmonic tone);
//   5. a gap of a couple of frames inside a voiced stretch is bridged (glottal irregularity), a blip of two or three
//      voiced frames is dropped.
namespace orpheus
{
struct AnalysisSettings
{
    double minHz = 65.0;       // the lowest pitch looked for (a bass voice reaches about 80 Hz)
    double maxHz = 1100.0;     // the highest (a soprano's top C is 1046 Hz)
    double silenceDb = -62.0;  // frames quieter than this are never voiced
    double frameMs = 5.0;
};

class PitchAnalyzer
{
public:
    using Progress = std::function<bool (float)>; // 0..1; return false to cancel (the result is then empty)

    static Contour analyse (const float* x, int n, double sampleRate, const AnalysisSettings& settings = {}, const Progress& progress = {})
    {
        Contour out;
        out.sampleRate = sampleRate;
        const int D = std::max (1, (int) std::lround (sampleRate / 16000.0));
        const double dsr = sampleRate / D;
        const int hopD = std::max (1, (int) std::lround (sampleRate * settings.frameMs * 0.001 / D));
        out.hop = hopD * D;
        const int numFrames = n / out.hop + 1;
        out.midi.assign ((size_t) numFrames, 69.0f);
        out.confidence.assign ((size_t) numFrames, 0.0f);
        out.levelDb.assign ((size_t) numFrames, -120.0f);
        out.voiced.assign ((size_t) numFrames, 0);
        if (n < 64)
            return out;

        const std::vector<float> xd = decimate (x, n, D);
        const int W = 8 * (int) std::lround (dsr * 0.032 / 8.0);                       // difference window, 32 ms, a multiple of 8
        const int Wlevel = 2 * (int) std::lround (dsr * 0.008);                        // loudness window, 16 ms: short enough to see two syllables apart
        const int tauMax = std::min ((int) std::ceil (dsr / settings.minHz) + 2, W);
        const int tauMin = std::max (2, (int) std::floor (dsr / settings.maxHz));
        const int pad = W / 2 + tauMax / 2 + 8;
        std::vector<float> xp ((size_t) xd.size() + 2 * (size_t) pad, 0.0f);
        std::copy (xd.begin(), xd.end(), xp.begin() + pad);

        struct Candidate { float tau, cost; };
        constexpr int maxCandidates = 4, states = maxCandidates + 1;
        std::vector<std::array<Candidate, maxCandidates>> cands ((size_t) numFrames);
        std::vector<uint8_t> numCands ((size_t) numFrames, 0);
        std::vector<float> dtau ((size_t) tauMax + 2), cmnd ((size_t) tauMax + 2);

        // ---- 1-2: levels and candidates ----------------------------------------------------------------------------------
        for (int f = 0; f < numFrames; ++f)
        {
            if ((f & 255) == 0 && progress && ! progress (0.55f * (float) f / (float) numFrames))
                return Contour {};
            const int c = f * hopD;
            const float* base = xp.data() + pad + c - W / 2;
            double energy = 0.0;
            for (int j = W / 2 - Wlevel / 2; j < W / 2 + Wlevel / 2; ++j)
                energy += (double) base[j] * base[j];
            const double rmsDb = 10.0 * std::log10 (energy / Wlevel + 1.0e-12);
            out.levelDb[(size_t) f] = (float) rmsDb;
            if (rmsDb < settings.silenceDb)
                continue;

            dtau[0] = 0.0f;
            for (int tau = 1; tau <= tauMax; ++tau)
                dtau[(size_t) tau] = squaredDifference (base - tau / 2, base + (tau - tau / 2), W);
            cmnd[0] = 1.0f;
            double running = 0.0;
            for (int tau = 1; tau <= tauMax; ++tau)
            {
                running += dtau[(size_t) tau];
                cmnd[(size_t) tau] = running > 1.0e-12 ? (float) ((double) dtau[(size_t) tau] * (double) tau / running) : 1.0f;
            }

            auto& list = cands[(size_t) f];
            int count = 0;
            for (int tau = tauMin; tau < tauMax; ++tau)
            {
                const float v = cmnd[(size_t) tau];
                if (v >= 0.45f || v > cmnd[(size_t) tau - 1] || v >= cmnd[(size_t) tau + 1])
                    continue;
                const float a = cmnd[(size_t) tau - 1], b = v, d = cmnd[(size_t) tau + 1];
                const float denom = a - 2.0f * b + d;
                const float shift = denom > 1.0e-9f ? std::clamp (0.5f * (a - d) / denom, -0.5f, 0.5f) : 0.0f;
                const float cost = v + 0.04f * std::log2 ((float) tau / (float) tauMin);
                const Candidate cand { (float) tau + shift, cost };
                if (count < maxCandidates)
                    list[(size_t) count++] = cand;
                else
                {
                    auto worst = std::max_element (list.begin(), list.end(), [] (const Candidate& p, const Candidate& q) { return p.cost < q.cost; });
                    if (cand.cost < worst->cost)
                        *worst = cand;
                }
            }
            numCands[(size_t) f] = (uint8_t) count;
        }

        // ---- 3: Viterbi ---------------------------------------------------------------------------------------------------
        constexpr float unvoicedCost = 0.32f, switchCost = 0.12f, jumpCost = 0.12f;
        std::vector<float> cost ((size_t) numFrames * states, 0.0f);
        std::vector<int8_t> back ((size_t) numFrames * states, -1);
        const auto stateCount = [&] (int f) { return (int) numCands[(size_t) f] + 1; }; // candidates, then unvoiced as the last
        const auto observation = [&] (int f, int s) { return s == (int) numCands[(size_t) f] ? unvoicedCost : cands[(size_t) f][(size_t) s].cost; };
        for (int s = 0; s < stateCount (0); ++s)
            cost[(size_t) s] = observation (0, s);
        for (int f = 1; f < numFrames; ++f)
        {
            if ((f & 1023) == 0 && progress && ! progress (0.55f + 0.1f * (float) f / (float) numFrames))
                return Contour {};
            const int prevStates = stateCount (f - 1), nowStates = stateCount (f);
            for (int s = 0; s < nowStates; ++s)
            {
                const bool voicedNow = s < nowStates - 1;
                float best = 1.0e30f;
                int bestFrom = 0;
                for (int p = 0; p < prevStates; ++p)
                {
                    const bool voicedBefore = p < prevStates - 1;
                    float step = 0.0f;
                    if (voicedNow && voicedBefore)
                        step = jumpCost * std::min (14.0f, 12.0f * std::fabs (std::log2 (cands[(size_t) f][(size_t) s].tau / cands[(size_t) f - 1][(size_t) p].tau)));
                    else if (voicedNow != voicedBefore)
                        step = switchCost;
                    const float total = cost[(size_t) (f - 1) * states + (size_t) p] + step;
                    if (total < best)
                    {
                        best = total;
                        bestFrom = p;
                    }
                }
                cost[(size_t) f * states + (size_t) s] = best + observation (f, s);
                back[(size_t) f * states + (size_t) s] = (int8_t) bestFrom;
            }
        }
        std::vector<int> path ((size_t) numFrames);
        {
            int s = 0;
            float best = 1.0e30f;
            for (int k = 0; k < stateCount (numFrames - 1); ++k)
                if (cost[(size_t) (numFrames - 1) * states + (size_t) k] < best)
                {
                    best = cost[(size_t) (numFrames - 1) * states + (size_t) k];
                    s = k;
                }
            for (int f = numFrames - 1; f >= 0; --f)
            {
                path[(size_t) f] = s;
                s = back[(size_t) f * states + (size_t) s];
            }
        }

        // ---- 4: refine at the full rate -----------------------------------------------------------------------------------
        for (int f = 0; f < numFrames; ++f)
        {
            if ((f & 255) == 0 && progress && ! progress (0.65f + 0.3f * (float) f / (float) numFrames))
                return Contour {};
            const int s = path[(size_t) f];
            if (s >= (int) numCands[(size_t) f])
                continue;
            const double coarse = (double) cands[(size_t) f][(size_t) s].tau * D;
            double period = coarse, conf = std::clamp (1.0 - cands[(size_t) f][(size_t) s].cost, 0.0, 1.0);
            refine (x, n, sampleRate, f * out.hop, coarse, D + 1, period, conf);
            out.midi[(size_t) f] = (float) hzToMidi (sampleRate / period);
            out.confidence[(size_t) f] = (float) conf;
            out.voiced[(size_t) f] = 1;
        }

        // ---- 5: bridge short gaps, drop short blips -----------------------------------------------------------------------
        const int nf = numFrames;
        for (int f = 1; f < nf - 1; ++f)
        {
            if (out.voiced[(size_t) f])
                continue;
            int e = f;
            while (e < nf && ! out.voiced[(size_t) e])
                ++e;
            if (e < nf && e - f <= 2 && out.voiced[(size_t) f - 1] && std::fabs (out.midi[(size_t) e] - out.midi[(size_t) f - 1]) < 1.0f
                && out.levelDb[(size_t) f] > settings.silenceDb)
                for (int k = f; k < e; ++k)
                {
                    const float t = (float) (k - f + 1) / (float) (e - f + 1);
                    out.midi[(size_t) k] = out.midi[(size_t) f - 1] + t * (out.midi[(size_t) e] - out.midi[(size_t) f - 1]);
                    out.confidence[(size_t) k] = 0.3f;
                    out.voiced[(size_t) k] = 1;
                }
            f = e;
        }
        for (int f = 0; f < nf; ++f)
        {
            if (! out.voiced[(size_t) f])
                continue;
            int e = f;
            while (e < nf && out.voiced[(size_t) e])
                ++e;
            if (e - f <= 3)
                for (int k = f; k < e; ++k)
                    out.voiced[(size_t) k] = 0;
            f = e;
        }
        if (progress)
            progress (1.0f);
        return out;
    }

private:
    // Low-pass at 0.45 of the new Nyquist and keep every D-th sample. A windowed sinc, 8 D + 1 taps.
    static std::vector<float> decimate (const float* x, int n, int D)
    {
        if (D == 1)
            return std::vector<float> (x, x + n);
        const int M = 4 * D;
        std::vector<double> h ((size_t) (2 * M + 1));
        const double fc = 0.45 / D;
        double sum = 0.0;
        for (int m = -M; m <= M; ++m)
        {
            const double a = 2.0 * fc * m;
            const double sinc = m == 0 ? 1.0 : std::sin (M_PI * a) / (M_PI * a);
            const double win = 0.5 + 0.5 * std::cos (M_PI * m / (M + 1));
            h[(size_t) (m + M)] = 2.0 * fc * sinc * win;
            sum += h[(size_t) (m + M)];
        }
        for (auto& v : h)
            v /= sum;
        const int nd = n / D + 1;
        std::vector<float> y ((size_t) nd);
        for (int k = 0; k < nd; ++k)
        {
            double acc = 0.0;
            for (int m = -M; m <= M; ++m)
            {
                const int i = k * D + m;
                if (i >= 0 && i < n)
                    acc += h[(size_t) (m + M)] * x[i];
            }
            y[(size_t) k] = (float) acc;
        }
        return y;
    }

    static float squaredDifference (const float* a, const float* b, int n) noexcept
    {
        float s0 = 0, s1 = 0, s2 = 0, s3 = 0;
        for (int j = 0; j < n; j += 4)
        {
            const float d0 = a[j] - b[j], d1 = a[j + 1] - b[j + 1], d2 = a[j + 2] - b[j + 2], d3 = a[j + 3] - b[j + 3];
            s0 += d0 * d0;
            s1 += d1 * d1;
            s2 += d2 * d2;
            s3 += d3 * d3;
        }
        return (s0 + s1) + (s2 + s3);
    }

    // The period near `coarse` (in samples) that maximises the normalised correlation of the signal with itself one period
    // later, over a window of three periods centred on `centre`, to a fraction of a sample. Leaves period and conf alone where
    // the window runs off the recording or the peak is at the edge of the search.
    static void refine (const float* x, int n, double sampleRate, int centre, double coarse, int reach, double& period, double& conf)
    {
        const int window = std::max (8, ((int) std::lround (std::clamp (3.0 * coarse, 0.02 * sampleRate, 0.04 * sampleRate)) / 2) * 2);
        const int centreLag = (int) std::lround (coarse);
        const int lags = 2 * reach + 1;
        std::array<double, 64> ncc {};
        if (lags > (int) ncc.size())
            return;
        for (int k = 0; k < lags; ++k)
        {
            const int lag = centreLag - reach + k;
            const int a = centre - window / 2 - lag / 2, b = centre - window / 2 + (lag - lag / 2);
            if (a < 0 || b + window > n)
                return;
            double r = 0.0, ea = 0.0, eb = 0.0;
            for (int j = 0; j < window; ++j)
            {
                const double u = x[a + j], v = x[b + j];
                r += u * v;
                ea += u * u;
                eb += v * v;
            }
            ncc[(size_t) k] = r / std::sqrt(ea * eb + 1.0e-18);
        }
        int best = 0;
        for (int k = 1; k < lags; ++k)
            if (ncc[(size_t) k] > ncc[(size_t) best])
                best = k;
        if (best == 0 || best == lags - 1)
            return;
        const double a = ncc[(size_t) best - 1], b = ncc[(size_t) best], c = ncc[(size_t) best + 1];
        const double denom = a - 2.0 * b + c;
        const double shift = denom < -1.0e-12 ? std::clamp (0.5 * (a - c) / denom, -0.5, 0.5) : 0.0;
        period = (double) (centreLag - reach + best) + shift;
        conf = std::clamp (b, 0.0, 1.0);
    }
};
} // namespace orpheus
