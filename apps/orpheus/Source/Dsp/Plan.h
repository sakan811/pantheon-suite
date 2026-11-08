// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 The Pantheon authors

#pragma once

#include "NoteModel.h"

// The edits, as numbers per frame: what pitch every voiced frame should have, how far the formants move, how loud it should be.
// This is the only thing the renderer reads of the notes, so the question "what will this edit do" has one answer in one place.
//
// Inside a note the sung pitch is split into its centre, a slow part (drift: the wander over a few hundred milliseconds)
// and a fast part (vibrato and jitter). The planned pitch is
//
//     target + (1 - drift) * slow + vibrato * fast
//
// so with the defaults (drift 0, vibrato 1) the whole sung shape is moved rigidly and its centre lands exactly on the target.
// The shift that results is then smoothed over `glide` milliseconds, so two neighbouring notes moved by different amounts
// are joined by a slide, not a step.
namespace orpheus
{
struct Plan
{
    int hop = 240;
    double sampleRate = 48000.0;
    std::vector<float> outMidi;      // the pitch to produce (voiced frames)
    std::vector<float> shiftCents;   // outMidi - detected, in cents
    std::vector<float> formantSt;    // formant shift, semitones
    std::vector<float> gainDb;
    std::vector<uint8_t> active;     // the frame has to be resynthesised (pitch or formant moved)

    static constexpr double activeCents = 0.02; // below this the shift is inaudible and the original is kept untouched

    int numFrames() const noexcept { return (int) outMidi.size(); }

    static Plan build (const Contour& c, const std::vector<Note>& notes, double glideMs = 30.0)
    {
        Plan p;
        p.hop = c.hop;
        p.sampleRate = c.sampleRate;
        const int nf = c.numFrames();
        p.outMidi = c.midi;
        p.shiftCents.assign ((size_t) nf, 0.0f);
        p.formantSt.assign ((size_t) nf, 0.0f);
        p.gainDb.assign ((size_t) nf, 0.0f);
        p.active.assign ((size_t) nf, 0);

        constexpr int slowRadius = 12; // two passes of 25 frames: first null at 8 Hz, so a vibrato is almost entirely "fast"
        for (const auto& note : notes)
        {
            const int a = std::max (0, note.start), b = std::min (nf, note.end), len = b - a;
            if (len <= 0 || ! note.edited())
                continue;
            std::vector<double> dev ((size_t) len, 0.0), weight ((size_t) len, 0.0);
            for (int i = 0; i < len; ++i)
                if (c.voiced[(size_t) (a + i)])
                {
                    dev[(size_t) i] = c.midi[(size_t) (a + i)] - note.centre;
                    weight[(size_t) i] = 1.0;
                }
            const auto slow1 = boxcar (dev, weight, slowRadius);
            const std::vector<double> ones ((size_t) len, 1.0);
            const auto slow = boxcar (slow1, ones, slowRadius);
            for (int i = 0; i < len; ++i)
            {
                const size_t f = (size_t) (a + i);
                if (! c.voiced[f])
                    continue;
                const double fast = dev[(size_t) i] - slow[(size_t) i];
                const double wanted = note.target + (1.0 - note.drift) * slow[(size_t) i] + note.vibrato * fast;
                p.shiftCents[f] = (float) ((wanted - c.midi[f]) * 100.0);
                p.formantSt[f] = note.formant;
                p.gainDb[f] = note.gainDb;
            }
        }

        const int radius = std::max (0, (int) std::lround (glideMs * 0.001 * c.sampleRate / c.hop / 2.0));
        if (radius > 0)
            for (int f = 0; f < nf; ++f)
            {
                if (! c.voiced[(size_t) f])
                    continue;
                int e = f;
                while (e < nf && c.voiced[(size_t) e])
                    ++e;
                smoothRun (p.shiftCents, f, e, radius);
                smoothRun (p.formantSt, f, e, radius);
                smoothRun (p.gainDb, f, e, radius);
                f = e;
            }

        for (int f = 0; f < nf; ++f)
        {
            if (! c.voiced[(size_t) f])
                continue;
            p.outMidi[(size_t) f] = c.midi[(size_t) f] + p.shiftCents[(size_t) f] * 0.01f;
            p.active[(size_t) f] = std::fabs (p.shiftCents[(size_t) f]) > activeCents || std::fabs (p.formantSt[(size_t) f]) > 1.0e-3f;
        }
        return p;
    }

    // The frame range a note's edit can touch: its own frames and the glide on both sides.
    static void affectedFrames (const Note& n, double glideMs, const Contour& c, int& first, int& last)
    {
        const int radius = (int) std::lround (glideMs * 0.001 * c.sampleRate / c.hop / 2.0) + 2;
        first = std::max (0, n.start - radius);
        last = std::min (c.numFrames(), n.end + radius);
    }

private:
    static std::vector<double> boxcar (const std::vector<double>& v, const std::vector<double>& w, int radius)
    {
        const int n = (int) v.size();
        std::vector<double> sv ((size_t) n + 1, 0.0), sw ((size_t) n + 1, 0.0), out ((size_t) n, 0.0);
        for (int i = 0; i < n; ++i)
        {
            sv[(size_t) i + 1] = sv[(size_t) i] + v[(size_t) i] * w[(size_t) i];
            sw[(size_t) i + 1] = sw[(size_t) i] + w[(size_t) i];
        }
        for (int i = 0; i < n; ++i)
        {
            const int lo = std::max (0, i - radius), hi = std::min (n, i + radius + 1);
            const double weight = sw[(size_t) hi] - sw[(size_t) lo];
            out[(size_t) i] = weight > 0.0 ? (sv[(size_t) hi] - sv[(size_t) lo]) / weight : 0.0;
        }
        return out;
    }

    // Box filter over frames [a, b) with the edge values continued, so a note at the edge of a voiced stretch keeps its full shift.
    static void smoothRun (std::vector<float>& v, int a, int b, int radius)
    {
        const int n = b - a;
        const auto at = [&] (int i) { return (double) v[(size_t) (a + std::clamp (i, 0, n - 1))]; };
        std::vector<float> out ((size_t) n);
        double acc = 0.0;
        for (int k = -radius; k <= radius; ++k)
            acc += at (k);
        for (int i = 0; i < n; ++i)
        {
            out[(size_t) i] = (float) (acc / (2 * radius + 1));
            acc += at (i + radius + 1) - at (i - radius);
        }
        std::copy (out.begin(), out.end(), v.begin() + a);
    }
};
} // namespace orpheus
