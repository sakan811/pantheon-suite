// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 The Pantheon authors

#pragma once

#include "Plan.h"

// Pitch-synchronous overlap-add, offline.
//
// findMarks() places one mark per glottal period in every voiced stretch of the recording, once, when the file is analysed.
// A mark is not found by looking for a peak (which jumps between peaks when the voice changes) but by following the waveform:
// the shape around the last mark is matched against the signal one period later, to a fraction of a sample.
//
// render() then rebuilds the stretches that were edited. Output marks are spaced exactly as the plan's pitch says, so the
// pitch that comes out is the pitch that was asked for, whatever the detector made of the input. Each output mark takes
// the grain (two periods under a Hann window) of the nearest input mark. Grains are re-spaced and not resampled, which is why the
// voice keeps its colour; a formant shift reads the grain a little faster or slower on purpose.
// Everything outside the edited stretches is copied from the input, bit for bit. Inside, the result is cross-faded in over 4 ms and
// brought back to the loudness of the input (re-spaced grains add up to a different level).
namespace orpheus
{
struct Rendered
{
    int start = 0, end = 0;                    // the samples this covers
    std::vector<std::vector<float>> data;      // per channel, end - start samples
};

class Psola
{
public:
    static std::vector<double> findMarks (const float* x, int n, const Contour& c)
    {
        std::vector<double> marks;
        const double sr = c.sampleRate;
        const int nf = c.numFrames();
        for (int f = 0; f < nf; ++f)
        {
            if (! c.isVoiced (f))
                continue;
            int e = f;
            while (e < nf && c.voiced[(size_t) e])
                ++e;
            const int runStart = std::max (0, f * c.hop - c.hop / 2), runEnd = std::min (n - 1, (e - 1) * c.hop + c.hop / 2);
            markRun (x, n, c, sr, runStart, runEnd, marks);
            f = e;
        }
        return marks;
    }

    // Rebuilds the stretches that overlap [dirtyStart, dirtyEnd) and returns them with the surrounding untouched audio, so the
    // caller can replace exactly the returned range. The range is the dirty one grown to whole stretches.
    static Rendered render (const std::vector<const float*>& src, int n, const Contour& c, const std::vector<double>& marks, const Plan& plan,
                            int dirtyStart, int dirtyEnd)
    {
        Rendered r;
        const int nf = c.numFrames(), hop = c.hop;
        int a = std::clamp (dirtyStart, 0, n), b = std::clamp (dirtyEnd, 0, n);
        const auto frameStart = [&] (int f) { return std::max (0, f * hop - hop / 2); };
        const auto frameEnd = [&] (int f) { return std::min (n, f * hop + hop / 2); };

        // grow to whole stretches
        for (int f = std::max (0, c.frameAtSample (a) - 1); f <= std::min (nf - 1, c.frameAtSample (b) + 1); ++f)
        {
            if (! plan.active[(size_t) f] || frameEnd (f) <= a - 1 || frameStart (f) >= b + 1)
                continue;
            int s = f, e = f;
            while (s > 0 && plan.active[(size_t) s - 1])
                --s;
            while (e + 1 < nf && plan.active[(size_t) e + 1])
                ++e;
            a = std::min (a, frameStart (s));
            b = std::max (b, frameEnd (e));
            f = e;
        }

        r.start = a;
        r.end = b;
        r.data.resize (src.size());
        for (size_t ch = 0; ch < src.size(); ++ch)
            r.data[ch].assign (src[ch] + a, src[ch] + b);

        for (int f = std::max (0, c.frameAtSample (a) - 1); f <= std::min (nf - 1, c.frameAtSample (b) + 1); ++f)
        {
            if (! plan.active[(size_t) f] || frameStart (f) < a || frameEnd (f) > b)
                continue;
            int e = f;
            while (e + 1 < nf && plan.active[(size_t) e + 1])
                ++e;
            stretch (src, n, c, marks, plan, f, e, r);
            f = e;
        }

        applyGain (c, plan, r);
        return r;
    }

private:
    // ---- analysis marks --------------------------------------------------------------------------------------------
    static void markRun (const float* x, int n, const Contour& c, double sr, int runStart, int runEnd, std::vector<double>& marks)
    {
        const auto period = [&] (double pos) { return sr / midiToHz (c.midiAt (pos)); };
        double T = period (runStart);
        // the template (1.5 periods centred on a mark) has to fit inside the recording, so a run at its very start begins a little later
        runStart = std::max (runStart, (int) std::ceil (1.0 * T) + 2);
        if (runStart + (int) T >= n - 1 || runStart >= runEnd)
            return;
        int p0 = runStart;
        for (int i = runStart; i <= std::min (runEnd, runStart + (int) std::ceil (T)); ++i)
            if (x[i] > x[p0])
                p0 = i;
        double p = parabola (x, n, p0);
        marks.push_back (p);

        while (true)
        {
            T = period (p);
            const double predicted = p + T;
            if (predicted > runEnd + 0.5 * T)
                break;
            const int pi = (int) std::lround (p);
            const double delta = p - pi;
            const int length = std::max (8, ((int) std::lround (1.5 * T) / 2) * 2), half = length / 2;
            const int reach = std::max (2, (int) std::lround (0.15 * T));
            const int centre = (int) std::lround (predicted);
            if (pi - half < 0 || centre + reach + half >= n || centre - reach - half < 0)
                break;
            double templateEnergy = 0.0;
            for (int j = -half; j < half; ++j)
                templateEnergy += (double) x[pi + j] * x[pi + j];
            std::vector<double> ncc ((size_t) (2 * reach + 1), -1.0);
            int best = reach;
            for (int k = -reach; k <= reach; ++k)
            {
                double r = 0.0, e = 0.0;
                const float* seg = x + centre + k - half;
                const float* tpl = x + pi - half;
                for (int j = 0; j < length; ++j)
                {
                    r += (double) tpl[j] * seg[j];
                    e += (double) seg[j] * seg[j];
                }
                ncc[(size_t) (k + reach)] = r / std::sqrt (templateEnergy * e + 1.0e-18);
                if (ncc[(size_t) (k + reach)] > ncc[(size_t) best])
                    best = k + reach;
            }
            double next = predicted;
            if (ncc[(size_t) best] > 0.2)
            {
                double shift = 0.0;
                if (best > 0 && best < 2 * reach)
                {
                    const double l = ncc[(size_t) best - 1], m = ncc[(size_t) best], rr = ncc[(size_t) best + 1];
                    const double denom = l - 2.0 * m + rr;
                    if (denom < -1.0e-12)
                        shift = std::clamp (0.5 * (l - rr) / denom, -0.5, 0.5);
                }
                next = (double) (centre + best - reach) + shift + delta;
            }
            if (next - p < 0.6 * T || next - p > 1.4 * T)
                next = predicted;
            marks.push_back (next);
            p = next;
        }
    }

    static double parabola (const float* x, int n, int i)
    {
        if (i < 1 || i >= n - 1)
            return i;
        const double a = x[i - 1], b = x[i], c = x[i + 1], denom = a - 2.0 * b + c;
        return denom < -1.0e-12 ? i + std::clamp (0.5 * (a - c) / denom, -0.5, 0.5) : i;
    }

    // ---- synthesis -------------------------------------------------------------------------------------------------
    static float cubic (const float* x, int n, double pos) noexcept
    {
        const int i = (int) std::floor (pos);
        const float t = (float) (pos - i);
        const auto at = [&] (int k) { return x[std::clamp (k, 0, n - 1)]; };
        const float p0 = at (i - 1), p1 = at (i), p2 = at (i + 1), p3 = at (i + 2);
        return p1 + 0.5f * t * (p2 - p0 + t * (2.0f * p0 - 5.0f * p1 + 4.0f * p2 - p3 + t * (3.0f * (p1 - p2) + p3 - p0)));
    }

    static constexpr double kMaxPeriodRatio = 16.0; // four octaves, either way

    static void stretch (const std::vector<const float*>& src, int n, const Contour& c, const std::vector<double>& marks, const Plan& plan,
                         int f0, int f1, Rendered& r)
    {
        const double sr = c.sampleRate;
        const int hop = c.hop;
        const int sa = std::max (0, f0 * hop - hop / 2), sb = std::min (n, f1 * hop + hop / 2);
        if (sb - sa < 8 || marks.size() < 3)
            return;
        const int margin = (int) (sr / 40.0) + 8;
        const int bufStart = sa - margin, bufLen = sb - sa + 2 * margin;
        const size_t channels = src.size();
        std::vector<std::vector<float>> y (channels, std::vector<float> ((size_t) bufLen, 0.0f));
        std::vector<float> weight ((size_t) bufLen, 0.0f);

        const auto planAt = [&] (const std::vector<float>& v, double pos)
        {
            const double u = std::clamp (pos / hop, (double) f0, (double) f1);
            const int i = (int) u, j = std::min (i + 1, f1);
            return (double) v[(size_t) i] + (u - i) * (double) (v[(size_t) j] - v[(size_t) i]);
        };

        // Start on the last mark before the stretch if it belongs to the same voiced run (it is within a period and a half), else on the
        // first mark inside it. The previous mark of another run, across a gap, would carry the synthesis away from the stretch.
        const auto first = (size_t) (std::lower_bound (marks.begin(), marks.end(), (double) sa) - marks.begin());
        if (first >= marks.size())
            return;
        size_t j = first;
        if (first > 0 && sa - marks[first - 1] < 1.5 * sr / midiToHz (c.midiAt (sa)))
            j = first - 1;
        double ts = marks[j];
        if (ts > sb)
            return;
        int guard = 0;
        while (guard++ < 10000000)
        {
            const double t1 = sr / midiToHz (planAt (plan.outMidi, ts));
            const double Ts = sr / midiToHz (planAt (plan.outMidi, ts + 0.5 * t1));
            while (j + 1 < marks.size() && std::fabs (marks[j + 1] - ts) <= std::fabs (marks[j] - ts))
                ++j;
            const double expected = sr / midiToHz (c.midiAt (marks[j]));
            double Tin = expected;
            {
                double sum = 0.0;
                int count = 0;
                if (j > 0 && marks[j] - marks[j - 1] > 0.5 * expected && marks[j] - marks[j - 1] < 1.6 * expected)
                {
                    sum += marks[j] - marks[j - 1];
                    ++count;
                }
                if (j + 1 < marks.size() && marks[j + 1] - marks[j] > 0.5 * expected && marks[j + 1] - marks[j] < 1.6 * expected)
                {
                    sum += marks[j + 1] - marks[j];
                    ++count;
                }
                if (count > 0)
                    Tin = sum / count;
            }
            // A pitch planned absurdly far from the sung one (the editor and the project loader keep notes within kMaxShiftSemitones, this is the
            // engine's own limit) makes Ts a few samples long and the loop millions of grains: leave the stretch as it was.
            if (! (Ts >= 2.0) || ! (Tin / Ts <= kMaxPeriodRatio && Tin / Ts >= 1.0 / kMaxPeriodRatio))
                return;
            if (std::fabs (marks[j] - ts) > 2.0 * Tin)
                break; // no input marks here (the voiced stretch ended): nothing to take a grain from
            const double alpha = std::exp2 (planAt (plan.formantSt, ts) / 12.0);
            const double H = std::max (Tin, 0.6 * Ts);
            const int n0 = std::max (bufStart, (int) std::ceil (ts - H)), n1 = std::min (bufStart + bufLen - 1, (int) std::floor (ts + H));
            for (int s = n0; s <= n1; ++s)
            {
                const double d = s - ts;
                const float w = (float) (0.5 + 0.5 * std::cos (M_PI * d / H));
                const double q = marks[j] + d * alpha;
                weight[(size_t) (s - bufStart)] += w;
                for (size_t ch = 0; ch < channels; ++ch)
                    y[ch][(size_t) (s - bufStart)] += w * cubic (src[ch], n, q);
            }
            ts += Ts;
            if (ts - H > sb)
                break;
        }

        const int ramp = std::max (1, std::min ((int) (0.004 * sr), (sb - sa) / 2));
        for (int s = sa; s < sb; ++s)
        {
            const float w = weight[(size_t) (s - bufStart)];
            if (w < 1.0e-3f)
                continue;
            const int edge = std::min (s - sa, sb - 1 - s);
            const float mix = edge >= ramp ? 1.0f : (float) (0.5 - 0.5 * std::cos (M_PI * (edge + 0.5) / ramp));
            for (size_t ch = 0; ch < channels; ++ch)
            {
                float& out = r.data[ch][(size_t) (s - r.start)];
                out += mix * (y[ch][(size_t) (s - bufStart)] / w - out);
            }
        }
        matchLoudness (src, sa, sb, sr, r);
    }

    // Grains that are re-spaced overlap partly out of step, so a pitch shift changes the level (up to 9 dB for five semitones up).
    // The level of the stretch is brought back to the input's: the ratio of the two short-term energies, each averaged over 24 ms
    // (two box passes), so the gain moves too slowly to modulate the waveform. Where the two agree already it is 1.
    static void matchLoudness (const std::vector<const float*>& src, int sa, int sb, double sr, Rendered& r)
    {
        const int len = sb - sa;
        const int radius = std::max (1, (int) (0.012 * sr));
        std::vector<double> ein ((size_t) len, 0.0), eout ((size_t) len, 0.0);
        for (size_t ch = 0; ch < src.size(); ++ch)
            for (int i = 0; i < len; ++i)
            {
                const double a = src[ch][sa + i], b = r.data[ch][(size_t) (sa + i - r.start)];
                ein[(size_t) i] += a * a;
                eout[(size_t) i] += b * b;
            }
        const auto smooth = [&] (std::vector<double>& v)
        {
            for (int pass = 0; pass < 2; ++pass)
            {
                std::vector<double> sums ((size_t) len + 1, 0.0), out ((size_t) len);
                for (int i = 0; i < len; ++i)
                    sums[(size_t) i + 1] = sums[(size_t) i] + v[(size_t) i];
                for (int i = 0; i < len; ++i)
                {
                    const int lo = std::max (0, i - radius), hi = std::min (len, i + radius + 1);
                    out[(size_t) i] = (sums[(size_t) hi] - sums[(size_t) lo]) / (hi - lo);
                }
                v = std::move (out);
            }
        };
        smooth (ein);
        smooth (eout);
        for (int i = 0; i < len; ++i)
        {
            const float g = (float) std::clamp (std::sqrt ((ein[(size_t) i] + 1.0e-12) / (eout[(size_t) i] + 1.0e-12)), 0.25, 4.0);
            for (auto& ch : r.data)
                ch[(size_t) (sa + i - r.start)] *= g;
        }
    }

    static void applyGain (const Contour& c, const Plan& plan, Rendered& r)
    {
        const int nf = c.numFrames(), hop = c.hop;
        for (int s = r.start; s < r.end; ++s)
        {
            const double u = std::clamp ((double) s / hop, 0.0, (double) (nf - 1));
            const int i = (int) u, j = std::min (i + 1, nf - 1);
            const double g = plan.gainDb[(size_t) i] + (u - i) * (plan.gainDb[(size_t) j] - plan.gainDb[(size_t) i]);
            if (std::fabs (g) < 1.0e-4)
                continue;
            const float k = (float) std::pow (10.0, g / 20.0);
            for (auto& ch : r.data)
                ch[(size_t) (s - r.start)] *= k;
        }
    }
};
} // namespace orpheus
