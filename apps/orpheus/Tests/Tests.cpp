// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 The Pantheon authors

// Headless tests of everything that decides what Orpheus does to a voice: the pitch tracker, the note segmentation, the plan of an
// edit, and the resynthesis. The singer is synthetic (TestSignals.h), so the true pitch of every sample is known and the output
// can be measured against it, not against another tool's opinion.
//
//   cmake --build build --target OrpheusTests && ./build/OrpheusTests_artefacts/Release/OrpheusTests

#include "Check.h"
#include "Dsp/Edits.h"
#include "Dsp/PitchAnalysis.h"
#include "Dsp/Plan.h"
#include "Dsp/Psola.h"
#include "Dsp/Segmenter.h"
#include "TestSignals.h"

#include <chrono>
#include <cstring>
#include <cstdio>

using namespace orpheus;

namespace
{
constexpr double kRate = 48000.0;

bool sameBits (float a, float b) { return std::memcmp (&a, &b, sizeof a) == 0; }

struct Setup
{
    test::Voice voice;
    Contour contour;
    std::vector<double> marks;
    std::vector<Note> notes;
};

Setup analyse (test::Voice v)
{
    Setup s;
    s.voice = std::move (v);
    s.contour = PitchAnalyzer::analyse (s.voice.audio.data(), (int) s.voice.audio.size(), s.voice.rate);
    s.marks = Psola::findMarks (s.voice.audio.data(), (int) s.voice.audio.size(), s.contour);
    for (const auto& span : Segmenter::segment (s.contour))
        s.notes.push_back (makeNote (s.contour, span.start, span.end));
    return s;
}

// Render the whole file with these notes; returns a copy of the input with the edited stretches replaced.
std::vector<float> renderAll (const Setup& s, const std::vector<Note>& notes, double glideMs = 30.0)
{
    const Plan plan = Plan::build (s.contour, notes, glideMs);
    const std::vector<const float*> src { s.voice.audio.data() };
    const auto r = Psola::render (src, (int) s.voice.audio.size(), s.contour, s.marks, plan, 0, (int) s.voice.audio.size());
    std::vector<float> out = s.voice.audio;
    std::copy (r.data[0].begin(), r.data[0].end(), out.begin() + r.start);
    return out;
}

// The pitch of the median voiced frame inside [from, to) frames of an analysis of `x`.
double medianPitch (const Contour& c, int from, int to)
{
    std::vector<double> v;
    for (int i = from; i < to; ++i)
        if (c.isVoiced (i))
            v.push_back (c.midi[(size_t) i]);
    if (v.empty())
        return -1.0;
    std::nth_element (v.begin(), v.begin() + (long) v.size() / 2, v.end());
    return v[v.size() / 2];
}

Contour measure (const std::vector<float>& x, double rate = kRate)
{
    return PitchAnalyzer::analyse (x.data(), (int) x.size(), rate);
}

// A phrase of five notes: steady, vibrato, a drifting one, a short one, a glide up.
std::vector<test::SungNote> phrase()
{
    return {
        { 0.20, 0.70, 57.0 },                       // A3
        { 0.90, 0.80, 60.0, 5.5, 45.0 },            // C4 with vibrato
        { 1.80, 0.70, 62.0, 0.0, 0.0, 35.0 },       // D4 with drift
        { 2.60, 0.40, 64.0 },                       // E4 short
        { 3.00, 0.80, 67.0, 6.0, 30.0 },            // G4, glide from E4
    };
}
} // namespace

int main()
{
    std::printf ("Orpheus tests\n");

    // ---- [1] the tracker ------------------------------------------------------------------------------------------------
    std::printf ("\n[1] pitch tracking against a known pitch\n");
    {
        double worst = 0.0;
        int wrong = 0, total = 0;
        for (const double rate : { 44100.0, 48000.0, 96000.0 })
            for (const double midi : { 41.0, 48.0, 57.0, 64.0, 72.0, 81.0 })
            {
                const auto v = test::sing ({ { 0.0, 1.0, midi } }, 1.0, rate);
                const auto c = measure (v.audio, rate);
                for (int i = 10; i < c.numFrames() - 10; ++i)
                {
                    ++total;
                    if (! c.voiced[(size_t) i])
                    {
                        ++wrong;
                        continue;
                    }
                    worst = std::max (worst, std::fabs (c.midi[(size_t) i] - midi) * 100.0);
                }
            }
        test::check (wrong == 0, test::fmt ("every frame of 18 steady tones (41 to 81 MIDI, 3 rates) is voiced (%d of %d missed)", wrong, total));
        test::check (worst < 0.5, test::fmt ("worst error on a steady tone %.3f cents (< 0.5)", worst));
    }
    {
        const auto v = test::sing ({ { 0.0, 2.0, 57.0, 5.8, 60.0 } }, 2.0);
        const auto c = measure (v.audio);
        double se = 0.0;
        int k = 0;
        bool octave = false;
        for (int i = 20; i < c.numFrames() - 20; ++i)
            if (c.voiced[(size_t) i])
            {
                const double truth = v.midi[(size_t) i * (size_t) c.hop];
                const double e = (c.midi[(size_t) i] - truth) * 100.0;
                se += e * e;
                ++k;
                octave = octave || std::fabs (e) > 300.0;
            }
        test::check (k > 0 && std::sqrt (se / k) < 4.0, test::fmt ("a 5.8 Hz vibrato of +-60 cents is followed to %.2f cents rms (< 4)", std::sqrt (se / std::max (1, k))));
        test::check (! octave, "no octave error under vibrato");
    }
    {
        std::mt19937 rng (3);
        std::uniform_real_distribution<float> d (-1.0f, 1.0f);
        std::vector<float> noise (96000);
        for (auto& v : noise)
            v = 0.2f * d (rng);
        const auto c = measure (noise);
        int voiced = 0;
        for (const auto v : c.voiced)
            voiced += v;
        const auto z = measure (std::vector<float> (48000, 0.0f));
        int zv = 0;
        for (const auto v : z.voiced)
            zv += v;
        test::check (voiced == 0 && zv == 0, test::fmt ("white noise (%d voiced frames) and silence (%d) are never voiced", voiced, zv));
    }
    {
        // a voice with breath: the harmonic tone plus noise at -12 dB
        auto v = test::sing ({ { 0.0, 1.5, 62.0, 5.0, 25.0 } }, 1.5);
        std::mt19937 rng (4);
        std::uniform_real_distribution<float> d (-1.0f, 1.0f);
        const double target = test::rms (v.audio, 0, (int) v.audio.size()) * 0.25;
        for (auto& s : v.audio)
            s += (float) (target * 1.7 * d (rng));
        const auto c = measure (v.audio);
        double worst = 0.0;
        int voiced = 0, n = 0;
        for (int i = 20; i < c.numFrames() - 20; ++i, ++n)
            if (c.voiced[(size_t) i])
            {
                ++voiced;
                worst = std::max (worst, std::fabs (c.midi[(size_t) i] - v.midi[(size_t) i * (size_t) c.hop]) * 100.0);
            }
        test::check (voiced > n * 95 / 100 && worst < 15.0, test::fmt ("with breath noise at -12 dB: %d%% voiced, worst error %.1f cents", voiced * 100 / n, worst));
    }

    // ---- [2] notes ------------------------------------------------------------------------------------------------------
    std::printf ("\n[2] segmentation into notes\n");
    const auto singer = phrase();
    const Setup phraseSetup = analyse (test::sing (singer, 4.2));
    {
        const auto& notes = phraseSetup.notes;
        test::check (notes.size() == singer.size(), test::fmt ("the phrase has %zu notes, found %zu", singer.size(), notes.size()));
        if (notes.size() == singer.size())
        {
            double worstCentre = 0.0, worstEdge = 0.0;
            for (size_t i = 0; i < notes.size(); ++i)
            {
                // the sung centre is what the median of the true pitch over the steady part of the note says (a drifting note has no other)
                std::vector<double> truth;
                const int trim = notes[i].length() / 7;
                for (int f = notes[i].start + trim; f < notes[i].end - trim; ++f)
                    truth.push_back (phraseSetup.voice.midi[(size_t) f * (size_t) phraseSetup.contour.hop]);
                std::nth_element (truth.begin(), truth.begin() + (long) truth.size() / 2, truth.end());
                worstCentre = std::max (worstCentre, std::fabs (notes[i].centre - truth[truth.size() / 2]) * 100.0);
                const double startErr = std::fabs (notes[i].start * phraseSetup.contour.hop / kRate - singer[i].start);
                if (i > 0 && singer[i].start - (singer[i - 1].start + singer[i - 1].duration) < 1.0e-6)
                    worstEdge = std::max (worstEdge, startErr); // only the notes that follow another directly have a boundary to find
            }
            test::check (worstCentre < 5.0, test::fmt ("every note centre is within %.2f cents of the sung pitch (< 5)", worstCentre));
            test::check (worstEdge < 0.08, test::fmt ("the boundary of a legato note is within %.0f ms (< 80)", worstEdge * 1000.0));
        }
    }
    {
        // the same note sung twice with a dip between (la la)
        const auto v = test::sing ({ { 0.1, 0.4, 60.0 }, { 0.5, 0.4, 60.0 } }, 1.2);
        // fade is 20 ms, so notes touch with a short dip of loudness at the join
        const Setup s = analyse (v);
        test::check (s.notes.size() == 2, test::fmt ("one pitch sung twice with a loudness dip is %zu notes (2)", s.notes.size()));
        const Setup glide = analyse (test::sing ({ { 0.1, 0.8, 60.0, 5.5, 100.0 } }, 1.2));
        test::check (glide.notes.size() == 1, test::fmt ("one note with a vibrato of +-100 cents stays one note (%zu)", glide.notes.size()));
    }

    // ---- [3] no edit, no change ----------------------------------------------------------------------------------------
    std::printf ("\n[3] untouched audio stays untouched\n");
    {
        const auto out = renderAll (phraseSetup, phraseSetup.notes);
        test::check (out == phraseSetup.voice.audio, "with no edits the render is bit-exact the input");
        auto notes = phraseSetup.notes;
        notes[1].target += 2.0f; // the second note up two semitones
        const auto edited = renderAll (phraseSetup, notes);
        const auto& c = phraseSetup.contour;
        const int untouchedTo = notes[1].start * c.hop - (int) (0.12 * kRate), untouchedFrom = notes[1].end * c.hop + (int) (0.12 * kRate);
        bool same = true;
        for (int i = 0; i < untouchedTo; ++i)
            same = same && sameBits (edited[(size_t) i], phraseSetup.voice.audio[(size_t) i]);
        for (int i = untouchedFrom; i < (int) edited.size(); ++i)
            same = same && sameBits (edited[(size_t) i], phraseSetup.voice.audio[(size_t) i]);
        test::check (same, "the notes far from an edit are bit-exact the input");
        test::check (edited != phraseSetup.voice.audio, "the edited note does change");
    }

    // ---- [4] pitch accuracy of the result ------------------------------------------------------------------------------
    std::printf ("\n[4] the pitch that comes out is the pitch that was asked for\n");
    {
        // every voiced frame of the output against the pitch the plan asked for
        double worstRms = 0.0, worstMax = 0.0;
        for (const float semis : { -7.0f, -2.0f, -0.37f, 0.5f, 1.0f, 3.0f, 7.0f })
        {
            auto notes = phraseSetup.notes;
            for (auto& n : notes)
                n.target += semis;
            const Plan plan = Plan::build (phraseSetup.contour, notes);
            const auto m = measure (renderAll (phraseSetup, notes));
            for (const auto& n : notes)
            {
                double se = 0.0;
                int k = 0;
                for (int i = n.start + 12; i < n.end - 12; ++i)
                    if (m.isVoiced (i))
                    {
                        const double e = (m.midi[(size_t) i] - plan.outMidi[(size_t) i]) * 100.0;
                        se += e * e;
                        ++k;
                        worstMax = std::max (worstMax, std::fabs (e));
                    }
                worstRms = std::max (worstRms, std::sqrt (se / std::max (1, k)));
            }
        }
        test::check (worstRms < 2.0 && worstMax < 8.0, test::fmt ("phrase moved by -7 ... +7 semitones: the output follows the planned pitch frame by frame within %.2f cents rms, %.2f at worst", worstRms, worstMax));
        auto notes = phraseSetup.notes;
        for (auto& n : notes)
            n.target += 1.37f;
        const Plan plan = Plan::build (phraseSetup.contour, notes);
        double worst = 0.0;
        for (const auto& n : notes)
            worst = std::max (worst, std::fabs (steadyCentre (Contour { kRate, phraseSetup.contour.hop, plan.outMidi, phraseSetup.contour.confidence, phraseSetup.contour.levelDb, phraseSetup.contour.voiced }, n.start, n.end) - n.target) * 100.0);
        test::check (worst < 0.01, test::fmt ("the centre of every planned note is its target (%.4f cents off)", worst));
    }
    {
        // correcting out-of-tune singing to C major
        std::vector<test::SungNote> flat { { 0.1, 0.5, 60.0 - 0.38 }, { 0.7, 0.5, 62.0 + 0.30 }, { 1.3, 0.5, 64.0 - 0.22 }, { 1.9, 0.5, 65.0 + 0.41 }, { 2.5, 0.5, 67.0 - 0.15, 5.5, 35.0 } };
        const Setup s = analyse (test::sing (flat, 3.2));
        Key key;
        key.root = 0;
        key.scale = kMajor;
        auto notes = s.notes;
        for (auto& n : notes)
            edit::snap (n, key);
        const auto m = measure (renderAll (s, notes));
        const Plan plan = Plan::build (s.contour, notes);
        double worst = 0.0;
        bool onGrid = true;
        for (const auto& n : notes)
        {
            std::vector<float> got;
            for (int i = n.start + (n.end - n.start) / 5; i < n.end - (n.end - n.start) / 5; ++i)
                if (m.isVoiced (i))
                    got.push_back (m.midi[(size_t) i] - (plan.outMidi[(size_t) i] - n.target)); // take the sung shape out: what is left is the centre
            std::nth_element (got.begin(), got.begin() + (long) got.size() / 2, got.end());
            worst = std::max (worst, std::fabs (got[got.size() / 2] - std::round (n.target)) * 100.0);
            onGrid = onGrid && std::fabs (n.target - std::round (n.target)) < 1e-4;
        }
        test::check (notes.size() == 5 && onGrid && worst < 2.0, test::fmt ("five notes off by up to 41 cents (one with a vibrato), snapped to C major: all within %.2f cents of the note (< 2)", worst));
    }

    {
        // many separate notes, each starting at a different phase of the waveform, each after a gap: every one has to be rendered
        std::vector<test::SungNote> many;
        for (int k = 0; k < 14; ++k)
            many.push_back ({ 0.1 + 0.4 * k + 0.00137 * k * k, 0.25, 52.0 + 0.7 * k });
        const Setup s = analyse (test::sing (many, 6.0));
        auto notes = s.notes;
        for (auto& n : notes)
            n.target += 2.0f;
        const Plan plan = Plan::build (s.contour, notes);
        const auto m = measure (renderAll (s, notes));
        int rendered = 0;
        for (const auto& n : notes)
        {
            double se = 0.0;
            int k = 0;
            for (int i = n.start + 8; i < n.end - 8; ++i)
                if (m.isVoiced (i))
                {
                    const double e = (m.midi[(size_t) i] - plan.outMidi[(size_t) i]) * 100.0;
                    se += e * e;
                    ++k;
                }
            rendered += k > 0 && std::sqrt (se / k) < 3.0;
        }
        test::check (notes.size() == many.size() && rendered == (int) notes.size(), test::fmt ("%zu notes after gaps, at 14 different onset phases: %d of them rendered at the planned pitch", notes.size(), rendered));
    }

    // ---- [5] what the other controls do ---------------------------------------------------------------------------------
    std::printf ("\n[5] vibrato, drift, formant, gain\n");
    {
        auto notes = phraseSetup.notes;
        notes[1].vibrato = 0.0f; // the note with the vibrato of +-45 cents
        const auto out = renderAll (phraseSetup, notes);
        const auto m = measure (out);
        const auto spread = [&] (const Contour& c, const Note& n)
        {
            double lo = 1e9, hi = -1e9;
            for (int i = n.start + (n.end - n.start) / 3; i < n.end - (n.end - n.start) / 5; ++i)
                if (c.isVoiced (i))
                {
                    lo = std::min<double> (lo, c.midi[(size_t) i]);
                    hi = std::max<double> (hi, c.midi[(size_t) i]);
                }
            return (hi - lo) * 100.0;
        };
        const double before = spread (phraseSetup.contour, phraseSetup.notes[1]), after = spread (m, notes[1]);
        test::check (before > 60.0 && after < 25.0, test::fmt ("vibrato 0 %%: pitch range of the note %.0f -> %.0f cents", before, after));
        auto doubled = phraseSetup.notes;
        doubled[1].vibrato = 2.0f;
        const double wide = spread (measure (renderAll (phraseSetup, doubled)), doubled[1]);
        test::check (wide > 1.6 * before && wide < 2.4 * before, test::fmt ("vibrato 200 %%: %.0f -> %.0f cents", before, wide));
    }
    {
        auto notes = phraseSetup.notes;
        notes[2].drift = 1.0f; // the note that wanders by +-70 cents at 0.7 Hz
        const auto m = measure (renderAll (phraseSetup, notes));
        const auto spread = [&] (const Contour& c, const Note& n)
        {
            double lo = 1e9, hi = -1e9;
            for (int i = n.start + (n.end - n.start) / 6; i < n.end - (n.end - n.start) / 6; ++i)
                if (c.isVoiced (i))
                {
                    lo = std::min<double> (lo, c.midi[(size_t) i]);
                    hi = std::max<double> (hi, c.midi[(size_t) i]);
                }
            return (hi - lo) * 100.0;
        };
        const double before = spread (phraseSetup.contour, phraseSetup.notes[2]), after = spread (m, notes[2]);
        test::check (before > 15.0 && after < 0.35 * before, test::fmt ("drift 100 %%: the wander of the note %.0f -> %.0f cents", before, after));
    }
    {
        // formants stay where they are when the pitch moves, and move when they are asked to
        const Setup s = analyse (test::sing ({ { 0.1, 1.2, 45.0 } }, 1.5)); // 110 Hz: the harmonics are close enough to follow the envelope
        const int a = (int) (0.4 * kRate), b = (int) (1.0 * kRate);
        const double base = test::centroid (s.voice.audio, a, b, kRate);
        auto up = s.notes;
        up[0].target += 4.0f;
        const auto shifted = renderAll (s, up);
        const double moved = test::centroid (shifted, a, b, kRate);
        auto formant = s.notes;
        formant[0].formant = 4.0f;
        const auto warped = renderAll (s, formant);
        const double warpedCentroid = test::centroid (warped, a, b, kRate);
        const auto pitchAfterFormant = medianPitch (measure (warped), (int) (0.5 * kRate / s.contour.hop), (int) (0.9 * kRate / s.contour.hop));
        test::check (std::fabs (moved / base - 1.0) < 0.12, test::fmt ("a pitch shift of +4 semitones moves the spectral centroid %.1f %% (a resampling shifter would move it 26 %%)", 100.0 * (moved / base - 1.0)));
        test::check (warpedCentroid / base > 1.10, test::fmt ("formant +4 semitones moves the centroid %.1f %% up", 100.0 * (warpedCentroid / base - 1.0)));
        test::check (std::fabs (pitchAfterFormant - 45.0) * 100.0 < 3.0, test::fmt ("and leaves the pitch alone (%.2f cents off)", (pitchAfterFormant - 45.0) * 100.0));
    }
    {
        auto notes = phraseSetup.notes;
        notes[0].gainDb = 6.0f;
        const auto out = renderAll (phraseSetup, notes);
        const auto& c = phraseSetup.contour;
        const int a = (notes[0].start + (notes[0].end - notes[0].start) / 3) * c.hop, b = (notes[0].end - (notes[0].end - notes[0].start) / 3) * c.hop;
        const double ratio = test::rms (out, a, b) / test::rms (phraseSetup.voice.audio, a, b);
        test::check (std::fabs (20.0 * std::log10 (ratio) - 6.0) < 0.1, test::fmt ("gain +6 dB reads %.2f dB", 20.0 * std::log10 (ratio)));
    }
    {
        double worst = 0.0;
        for (const float semis : { -5.0f, -2.0f, 2.0f, 5.0f })
        {
            auto notes = phraseSetup.notes;
            for (auto& n : notes)
                n.target += semis;
            const auto out = renderAll (phraseSetup, notes);
            for (const auto& n : notes)
            {
                const int a = (n.start + (n.end - n.start) / 3) * phraseSetup.contour.hop, b = (n.end - (n.end - n.start) / 3) * phraseSetup.contour.hop;
                worst = std::max (worst, std::fabs (20.0 * std::log10 (test::rms (out, a, b) / test::rms (phraseSetup.voice.audio, a, b))));
            }
        }
        test::check (worst < 2.0, test::fmt ("a pitch shift of up to 5 semitones changes the level of a note by at most %.2f dB (< 2)", worst));
    }

    // ---- [6] clicks, partial renders, channels -------------------------------------------------------------------------------
    std::printf ("\n[6] smoothness and consistency\n");
    {
        auto notes = phraseSetup.notes;
        notes[0].target += 1.3f;
        notes[1].target -= 2.0f;
        notes[3].target += 0.4f;
        const auto out = renderAll (phraseSetup, notes);
        double maxIn = 0.0, maxOut = 0.0;
        for (size_t i = 1; i < out.size(); ++i)
        {
            maxIn = std::max (maxIn, (double) std::fabs (phraseSetup.voice.audio[i] - phraseSetup.voice.audio[i - 1]));
            maxOut = std::max (maxOut, (double) std::fabs (out[i] - out[i - 1]));
        }
        test::check (maxOut < 1.6 * maxIn, test::fmt ("largest step between samples %.4f against %.4f in the input (no clicks)", maxOut, maxIn));
    }
    {
        auto notes = phraseSetup.notes;
        notes[1].target += 1.0f;
        notes[2].target -= 1.0f;
        const Plan plan = Plan::build (phraseSetup.contour, notes);
        const std::vector<const float*> src { phraseSetup.voice.audio.data() };
        const int n = (int) phraseSetup.voice.audio.size();
        const auto full = Psola::render (src, n, phraseSetup.contour, phraseSetup.marks, plan, 0, n);
        const int hop = phraseSetup.contour.hop;
        const auto part = Psola::render (src, n, phraseSetup.contour, phraseSetup.marks, plan, notes[2].start * hop, notes[2].end * hop);
        bool same = part.start >= full.start && part.end <= full.end && part.end - part.start > 0;
        for (int i = part.start; same && i < part.end; ++i)
            same = sameBits (part.data[0][(size_t) (i - part.start)], full.data[0][(size_t) (i - full.start)]);
        test::check (same, test::fmt ("rendering only the dirty range gives exactly the samples a full render gives (%d..%d)", part.start, part.end));
    }
    {
        std::vector<const float*> src { phraseSetup.voice.audio.data(), phraseSetup.voice.audio.data() };
        auto notes = phraseSetup.notes;
        notes[1].target += 1.0f;
        const Plan plan = Plan::build (phraseSetup.contour, notes);
        const int n = (int) phraseSetup.voice.audio.size();
        const auto r = Psola::render (src, n, phraseSetup.contour, phraseSetup.marks, plan, 0, n);
        test::check (r.data[0] == r.data[1], "two identical channels give two identical results");
    }

    // ---- [7] keys and tuning -----------------------------------------------------------------------------------------------
    std::printf ("\n[7] key, scale and tuning\n");
    {
        Key key;
        key.scale = kMajor;
        key.root = 2;
        test::check (std::fabs (key.snap (61.6) - 62.0) < 1e-9 && std::fabs (key.snap (60.4) - 61.0) < 1e-9, "D major: 61.6 -> D (62), 60.4 -> C# (61)");
        key.a4Hz = 440.0 * std::exp2 (0.3 / 12.0);
        test::check (std::fabs (key.snap (62.25) - 62.3) < 1e-9, "with a reference 30 cents sharp the grid is 30 cents sharp");
        test::check (noteName (60) == "C4" && noteName (69) == "A4" && noteName (48) == "C3", "note names: C4 = 60, A4 = 69");
        std::vector<Note> melody;
        const auto add = [&] (float midi, int frames) { Note n; n.centre = n.target = midi; n.start = 0; n.end = frames; melody.push_back (n); };
        add (62, 80); add (66, 40); add (64, 40); add (62, 60); add (69, 40); add (67, 30); add (66, 30); add (64, 30); add (61, 30); add (62, 100);
        const auto guess = detectKey (melody);
        test::check (guess.root == 2 && ! guess.minor, test::fmt ("a melody in D major is detected as %s %s", kNoteNames[guess.root], guess.minor ? "minor" : "major"));
        std::vector<Note> sharp = melody;
        for (auto& n : sharp)
            n.centre += 0.30f;
        const auto tuning = detectTuning (sharp);
        test::check (std::fabs (tuning.a4Hz - 440.0 * std::exp2 (0.3 / 12.0)) < 0.2, test::fmt ("notes 30 cents sharp: reference found at %.2f Hz (%.2f expected)", tuning.a4Hz, 440.0 * std::exp2 (0.3 / 12.0)));
    }
    {
        std::vector<Note> notes;
        Contour c;
        c.midi.assign (400, 60.0f);
        c.voiced.assign (400, 1);
        c.confidence.assign (400, 1.0f);
        c.levelDb.assign (400, -20.0f);
        notes.push_back (makeNote (c, 0, 200));
        notes[0].target += 1.5f;
        notes[0].vibrato = 0.5f;
        test::check (edit::splitAt (notes, c, 80) == 0 && notes.size() == 2 && notes[0].end == 80 && notes[1].start == 80, "split cuts a note in two at the frame");
        test::check (std::fabs (notes[0].shift() - 1.5f) < 1e-5 && std::fabs (notes[1].shift() - 1.5f) < 1e-5 && std::fabs (notes[1].vibrato - 0.5f) < 1e-6f, "both halves keep the shift and the settings");
        test::check (edit::splitAt (notes, c, 2) == -1, "a note is not cut within 4 frames of its edge");
        test::check (edit::mergeWithNext (notes, c, 0) && notes.size() == 1 && notes[0].start == 0 && notes[0].end == 200 && std::fabs (notes[0].shift() - 1.5f) < 1e-5, "merge puts them back, shift kept");
    }

    // ---- [8] speed ---------------------------------------------------------------------------------------------------------
    std::printf ("\n[8] speed (informational)\n");
    {
        const auto v = test::sing ({ { 0.0, 20.0, 60.0, 5.5, 40.0 } }, 20.0);
        const auto t0 = std::chrono::steady_clock::now();
        const auto c = measure (v.audio);
        const auto t1 = std::chrono::steady_clock::now();
        const auto marks = Psola::findMarks (v.audio.data(), (int) v.audio.size(), c);
        const auto t2 = std::chrono::steady_clock::now();
        auto notes = std::vector<Note> { makeNote (c, 0, c.numFrames()) };
        notes[0].target += 1.0f;
        const Plan plan = Plan::build (c, notes);
        const std::vector<const float*> src { v.audio.data() };
        const auto r = Psola::render (src, (int) v.audio.size(), c, marks, plan, 0, (int) v.audio.size());
        const auto t3 = std::chrono::steady_clock::now();
        const auto ms = [] (auto a, auto b) { return std::chrono::duration<double, std::milli> (b - a).count(); };
        std::printf ("  20 s of audio: tracking %.0f ms, marks %.0f ms, full resynthesis %.0f ms (%zu samples)\n", ms (t0, t1), ms (t1, t2), ms (t2, t3), r.data[0].size());
    }

    return test::finish ("OrpheusTests");
}
