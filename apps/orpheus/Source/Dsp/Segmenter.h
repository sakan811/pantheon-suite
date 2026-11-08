// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 The Pantheon authors

#pragma once

#include "NoteModel.h"

#include <numeric>

// Cuts the contour into notes. A voiced stretch is one candidate; inside it the notes are where the held pitch changes, or
// where the loudness dips and comes back (the same note sung twice: "la la").
//
// The pitch part is an optimal piecewise-constant fit (a Potts model solved exactly by dynamic programming): the cheapest
// way to describe the stretch as a few constant levels, paying a fixed price for every extra level. The curve is first
// averaged twice over about 125 ms, which is what keeps a vibrato of a semitone from being cut into notes; the edges of the notes
// are then moved back onto the unsmoothed curve, where a real change of note is a step, not a ramp.
namespace orpheus
{
struct SegmenterSettings
{
    int minNoteFrames = 6;        // 30 ms: shorter voiced stretches are not notes
    int smoothFrames = 25;        // 125 ms: the averaging that removes vibrato before the fit
    double levelPenalty = 16000;  // cents^2 * frames a new level has to save (about 40 cents over 10 frames)
    int minSegmentFrames = 8;     // no note from the fit is shorter than 40 ms
    int absorbFrames = 10;        // pieces shorter than this are merged into a neighbour
    int edgeFrames = 24;          // a piece this short at the start or end of a voiced stretch is a fall-off or a scoop (or the vibrato where the smoothing runs out), not a note
    int glideFrames = 20;         // a piece this short whose pitch lies between its neighbours' is the slide from one to the other: merged too
    double dipDb = 10.0;          // a dip in loudness of this much, with the voice back up on both sides, splits a note
};

struct NoteSpan
{
    int start, end;
};

class Segmenter
{
public:
    static std::vector<NoteSpan> segment (const Contour& c, const SegmenterSettings& s = {})
    {
        std::vector<NoteSpan> notes;
        const int nf = c.numFrames();
        for (int f = 0; f < nf; ++f)
        {
            if (! c.isVoiced (f))
                continue;
            int e = f;
            while (e < nf && c.voiced[(size_t) e])
                ++e;
            if (e - f >= s.minNoteFrames)
                segmentRun (c, f, e, s, notes);
            f = e;
        }
        return notes;
    }

private:
    static void segmentRun (const Contour& c, int a, int b, const SegmenterSettings& s, std::vector<NoteSpan>& out)
    {
        const int n = b - a;
        std::vector<double> raw ((size_t) n);
        for (int i = 0; i < n; ++i)
            raw[(size_t) i] = 100.0 * c.midi[(size_t) (a + i)];
        const double origin = raw[0];
        for (auto& v : raw)
            v -= origin;

        const auto med = medianFilter (raw, 2);
        const int radius = std::min (s.smoothFrames, (n % 2 == 0 ? n - 1 : n)) / 2;
        const auto smooth = boxcar (boxcar (med, radius), radius); // twice: the second pass takes what the first leaves of a vibrato

        // ---- exact fit by dynamic programming --------------------------------------------------------------------
        std::vector<int> cuts { 0 };
        if (n >= 2 * s.minSegmentFrames)
        {
            std::vector<double> s1 ((size_t) n + 1, 0.0), s2 ((size_t) n + 1, 0.0);
            for (int i = 0; i < n; ++i)
            {
                s1[(size_t) i + 1] = s1[(size_t) i] + smooth[(size_t) i];
                s2[(size_t) i + 1] = s2[(size_t) i] + smooth[(size_t) i] * smooth[(size_t) i];
            }
            const auto cost = [&] (int i, int j) // squared error of one level over [i, j)
            {
                const double sum = s1[(size_t) j] - s1[(size_t) i];
                return s2[(size_t) j] - s2[(size_t) i] - sum * sum / (double) (j - i);
            };
            const double inf = 1.0e300;
            std::vector<double> best ((size_t) n + 1, inf);
            std::vector<int> from ((size_t) n + 1, 0);
            best[0] = -s.levelPenalty; // the first level is free
            for (int j = s.minSegmentFrames; j <= n; ++j)
                for (int i = 0; i <= j - s.minSegmentFrames; ++i)
                {
                    if (best[(size_t) i] >= inf)
                        continue;
                    const double total = best[(size_t) i] + cost (i, j) + s.levelPenalty;
                    if (total < best[(size_t) j])
                    {
                        best[(size_t) j] = total;
                        from[(size_t) j] = i;
                    }
                }
            cuts.clear();
            for (int j = n; j > 0; j = from[(size_t) j])
                cuts.push_back (from[(size_t) j]);
            std::reverse (cuts.begin(), cuts.end());
        }
        cuts.push_back (n);

        // ---- merge the pieces that are not notes: too short, or only the slide between two notes ----------------------
        const auto level = [&] (int i, int j) { return median (med, i, j); };
        for (bool again = true; again && cuts.size() > 2;)
        {
            again = false;
            for (size_t k = 0; k + 1 < cuts.size(); ++k)
            {
                const int length = cuts[k + 1] - cuts[k];
                const bool hasLeft = k > 0, hasRight = k + 2 < cuts.size();
                if (! hasLeft && ! hasRight)
                    break;
                const double mine = level (cuts[k], cuts[k + 1]);
                bool absorb = length < s.absorbFrames || ((! hasLeft || ! hasRight) && length < s.edgeFrames);
                if (! absorb && hasLeft && hasRight && length < s.glideFrames)
                {
                    const double l = level (cuts[k - 1], cuts[k]), r = level (cuts[k + 1], cuts[k + 2]);
                    absorb = mine > std::min (l, r) - 15.0 && mine < std::max (l, r) + 15.0 && std::fabs (l - r) > 30.0;
                }
                if (! absorb)
                    continue;
                const bool toLeft = hasLeft && (! hasRight || std::fabs (mine - level (cuts[k - 1], cuts[k])) <= std::fabs (mine - level (cuts[k + 1], cuts[k + 2])));
                cuts.erase (cuts.begin() + (long) (toLeft ? k : k + 1));
                again = true;
                break;
            }
        }

        // ---- put each boundary where the unsmoothed pitch actually steps ---------------------------------------------
        for (size_t k = 1; k + 1 < cuts.size(); ++k)
        {
            const int lo = cuts[k - 1], mid = cuts[k], hi = cuts[k + 1];
            const double la = level (std::max (lo, mid - 20), mid), lb = level (mid, std::min (hi, mid + 20));
            int bestCut = mid;
            double bestCost = 1.0e300;
            for (int cand = std::max (lo + 4, mid - 14); cand <= std::min (hi - 4, mid + 14); ++cand)
            {
                double total = 0.0;
                for (int i = std::max (lo, cand - 25); i < cand; ++i)
                    total += std::fabs (med[(size_t) i] - la);
                for (int i = cand; i < std::min (hi, cand + 25); ++i)
                    total += std::fabs (med[(size_t) i] - lb);
                total += 0.01 * std::abs (cand - mid); // a flat valley goes to the middle
                if (total < bestCost)
                {
                    bestCost = total;
                    bestCut = cand;
                }
            }
            cuts[k] = bestCut;
        }

        // ---- loudness dips -------------------------------------------------------------------------------------------
        for (size_t k = 0; k + 1 < cuts.size(); ++k)
            splitAtDips (c, a + cuts[k], a + cuts[k + 1], s, out);
    }

    static void splitAtDips (const Contour& c, int a, int b, const SegmenterSettings& s, std::vector<NoteSpan>& out)
    {
        const int n = b - a;
        const int margin = 6;
        if (n >= 2 * margin + 1)
        {
            std::vector<float> level ((size_t) n);
            for (int i = 0; i < n; ++i) // three-frame mean, so one noisy frame is not a dip
                level[(size_t) i] = (c.levelDb[(size_t) std::max (a, a + i - 1)] + c.levelDb[(size_t) (a + i)] + c.levelDb[(size_t) std::min (b - 1, a + i + 1)]) / 3.0f;
            std::vector<float> left ((size_t) n), right ((size_t) n);
            left[0] = level[0];
            for (int i = 1; i < n; ++i)
                left[(size_t) i] = std::max (left[(size_t) i - 1], level[(size_t) i]);
            right[(size_t) n - 1] = level[(size_t) n - 1];
            for (int i = n - 2; i >= 0; --i)
                right[(size_t) i] = std::max (right[(size_t) i + 1], level[(size_t) i]);
            int cut = -1;
            double deepest = s.dipDb;
            for (int i = margin; i < n - margin; ++i)
            {
                bool localMin = true;
                for (int k = -3; k <= 3 && localMin; ++k)
                    localMin = level[(size_t) (i + k)] >= level[(size_t) i];
                const double dip = std::min (left[(size_t) i], right[(size_t) i]) - level[(size_t) i];
                if (localMin && dip >= deepest)
                {
                    deepest = dip;
                    cut = i;
                }
            }
            if (cut > 0)
            {
                splitAtDips (c, a, a + cut, s, out);
                splitAtDips (c, a + cut, b, s, out);
                return;
            }
        }
        out.push_back ({ a, b });
    }

    static std::vector<double> medianFilter (const std::vector<double>& v, int radius)
    {
        const int n = (int) v.size();
        std::vector<double> out ((size_t) n), window;
        for (int i = 0; i < n; ++i)
        {
            window.assign (v.begin() + std::max (0, i - radius), v.begin() + std::min (n, i + radius + 1));
            std::nth_element (window.begin(), window.begin() + (long) window.size() / 2, window.end());
            out[(size_t) i] = window[window.size() / 2];
        }
        return out;
    }

    static std::vector<double> boxcar (const std::vector<double>& v, int radius)
    {
        const int n = (int) v.size();
        std::vector<double> sums ((size_t) n + 1, 0.0), out ((size_t) n);
        for (int i = 0; i < n; ++i)
            sums[(size_t) i + 1] = sums[(size_t) i] + v[(size_t) i];
        for (int i = 0; i < n; ++i)
        {
            const int lo = std::max (0, i - radius), hi = std::min (n, i + radius + 1);
            out[(size_t) i] = (sums[(size_t) hi] - sums[(size_t) lo]) / (double) (hi - lo);
        }
        return out;
    }

    static double median (const std::vector<double>& v, int i, int j)
    {
        std::vector<double> w (v.begin() + i, v.begin() + std::max (i + 1, j));
        std::nth_element (w.begin(), w.begin() + (long) w.size() / 2, w.end());
        return w[w.size() / 2];
    }
};
} // namespace orpheus
