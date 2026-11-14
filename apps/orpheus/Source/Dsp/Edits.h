// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 The Pantheon authors

#pragma once

#include "Scales.h"

// The operations the editor offers on a list of notes, without the editor: each takes the notes and changes them. A note that
// is split or merged keeps what was done to it (the same shift, the same drift, vibrato, formant and gain), so cutting a note
// in two does not change how it sounds.
namespace orpheus::edit
{
// Moves the note `amount` (0..1) of the way from where it was sung to the nearest note of the scale.
inline void snap (Note& n, const Key& key, float amount = 1.0f) noexcept
{
    n.target = n.centre + amount * (float) (key.snap (n.centre) - n.centre);
}

// How far, 0..1, the note has been pulled to the scale (what the CORRECT knob shows); 1 when it is already in tune.
inline float correctionOf (const Note& n, const Key& key) noexcept
{
    const float goal = (float) (key.snap (n.centre) - n.centre);
    return std::fabs (goal) < 1.0e-4f ? 1.0f : std::clamp ((n.target - n.centre) / goal, -1.0f, 2.0f);
}

constexpr int kMinSplitFrames = 4;

// The index of the note that can be cut at `frame` (both halves at least kMinSplitFrames long), or -1. Nothing is changed.
inline int splitIndex (const std::vector<Note>& notes, int frame) noexcept
{
    for (size_t i = 0; i < notes.size(); ++i)
        if (frame - notes[i].start >= kMinSplitFrames && notes[i].end - frame >= kMinSplitFrames)
            return (int) i;
    return -1;
}

// Cuts the note that contains `frame` in two there. Returns the index of the first half, or -1 if no note can be cut there.
inline int splitAt (std::vector<Note>& notes, const Contour& c, int frame)
{
    if (const int i = splitIndex (notes, frame); i >= 0)
    {
        const Note whole = notes[(size_t) i];
        Note a = whole, b = whole;
        a.end = frame;
        b.start = frame;
        a.centre = steadyCentre (c, a.start, a.end);
        b.centre = steadyCentre (c, b.start, b.end);
        a.target = a.centre + whole.shift();
        b.target = b.centre + whole.shift();
        a.clampTarget();
        b.clampTarget();
        notes[(size_t) i] = a;
        notes.insert (notes.begin() + (long) i + 1, b);
        return i;
    }
    return -1;
}

// Joins note i and the one after it (they keep the settings of the longer one). Returns false if there is no next note.
inline bool mergeWithNext (std::vector<Note>& notes, const Contour& c, int i)
{
    if (i < 0 || i + 1 >= (int) notes.size())
        return false;
    Note m = notes[(size_t) i].length() >= notes[(size_t) i + 1].length() ? notes[(size_t) i] : notes[(size_t) i + 1];
    const float shift = m.shift();
    m.start = notes[(size_t) i].start;
    m.end = notes[(size_t) i + 1].end;
    m.centre = steadyCentre (c, m.start, m.end);
    m.target = m.centre + shift;
    m.clampTarget();
    notes[(size_t) i] = m;
    notes.erase (notes.begin() + i + 1);
    return true;
}

// Index of the note holding `frame`, or -1.
inline int noteAt (const std::vector<Note>& notes, int frame) noexcept
{
    for (size_t i = 0; i < notes.size(); ++i)
        if (frame >= notes[i].start && frame < notes[i].end)
            return (int) i;
    return -1;
}
} // namespace orpheus::edit
