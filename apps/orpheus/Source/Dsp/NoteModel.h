// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 The Pantheon authors

#pragma once

#include "Pitch.h"

#include <string>

// A note is a stretch of the contour that the singer held at one pitch, plus what has been done to it. Everything the editor
// can change is one of the fields below, so an edit is one value and an undo is a copy of the vector.
namespace orpheus
{
struct Note
{
    int start = 0, end = 0;   // frames [start, end)
    float centre = 69.0f;     // the detected pitch of the note: the median of its steady part, MIDI
    float target = 69.0f;     // where the centre should be: equal to `centre` = untouched
    float drift = 0.0f;       // 0 keeps the slow wander of the pitch, 1 removes it
    float vibrato = 1.0f;     // the fast modulation (vibrato, jitter) scaled: 0 = flat, 1 = as sung, 2 = twice
    float formant = 0.0f;     // formant shift in semitones, the pitch left alone
    float gainDb = 0.0f;

    int length() const noexcept { return end - start; }
    // Exactly the same note, field for field (what undo compares; floats are compared on purpose, not with a tolerance).
    bool identicalTo (const Note& o) const noexcept
    {
        const auto same = [] (float a, float b) { return ! (a < b) && ! (b < a); };
        return start == o.start && end == o.end && same (centre, o.centre) && same (target, o.target) && same (drift, o.drift) && same (vibrato, o.vibrato)
               && same (formant, o.formant) && same (gainDb, o.gainDb);
    }
    float shift() const noexcept { return target - centre; } // semitones
    bool edited() const noexcept
    {
        return std::fabs (target - centre) > 1.0e-4f || drift > 1.0e-4f || std::fabs (vibrato - 1.0f) > 1.0e-4f
               || std::fabs (formant) > 1.0e-4f || std::fabs (gainDb) > 1.0e-4f;
    }
    void reset() noexcept
    {
        target = centre;
        drift = 0.0f;
        vibrato = 1.0f;
        formant = 0.0f;
        gainDb = 0.0f;
    }
};

// The median of the voiced pitch over the steady middle of the note (its first and last 15 % are where the voice glides in and out).
inline float steadyCentre (const Contour& c, int start, int end)
{
    std::vector<float> v;
    const int trim = (end - start) / 7;
    for (int pass = 0; pass < 2 && v.empty(); ++pass)
        for (int i = start + (pass == 0 ? trim : 0); i < end - (pass == 0 ? trim : 0); ++i)
            if (c.isVoiced (i))
                v.push_back (c.midi[(size_t) i]);
    if (v.empty())
        return 69.0f;
    std::nth_element (v.begin(), v.begin() + (long) v.size() / 2, v.end());
    return v[v.size() / 2];
}

inline Note makeNote (const Contour& c, int start, int end)
{
    Note n;
    n.start = start;
    n.end = end;
    n.centre = n.target = steadyCentre (c, start, end);
    return n;
}
} // namespace orpheus
