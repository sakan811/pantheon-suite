// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 The Pantheon authors

#pragma once

#include "NoteModel.h"

#include <array>

// Keys, scales and the reference tuning: where a note may land, which key a melody is in, and how far from 440 Hz a song is
// tuned. Plain C++.
namespace orpheus
{
struct ScaleInfo
{
    const char* name;
    uint16_t mask; // bit k set = k semitones above the root is in the scale
};

constexpr uint16_t bits (std::initializer_list<int> semitones)
{
    uint16_t m = 0;
    for (int s : semitones)
        m = (uint16_t) (m | (1u << s));
    return m;
}

inline constexpr ScaleInfo kScales[] = {
    { "Chromatic", 0x0FFF },
    { "Major", bits ({ 0, 2, 4, 5, 7, 9, 11 }) },
    { "Minor", bits ({ 0, 2, 3, 5, 7, 8, 10 }) },
    { "Harmonic Minor", bits ({ 0, 2, 3, 5, 7, 8, 11 }) },
    { "Melodic Minor", bits ({ 0, 2, 3, 5, 7, 9, 11 }) },
    { "Dorian", bits ({ 0, 2, 3, 5, 7, 9, 10 }) },
    { "Phrygian", bits ({ 0, 1, 3, 5, 7, 8, 10 }) },
    { "Lydian", bits ({ 0, 2, 4, 6, 7, 9, 11 }) },
    { "Mixolydian", bits ({ 0, 2, 4, 5, 7, 9, 10 }) },
    { "Major Pentatonic", bits ({ 0, 2, 4, 7, 9 }) },
    { "Minor Pentatonic", bits ({ 0, 3, 5, 7, 10 }) },
    { "Blues", bits ({ 0, 3, 5, 6, 7, 10 }) },
};
inline constexpr int kNumScales = (int) (sizeof (kScales) / sizeof (kScales[0]));
inline constexpr int kChromatic = 0, kMajor = 1, kMinor = 2;

inline constexpr const char* kNoteNames[12] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };

inline int pitchClassOf (int midiNote) noexcept { return ((midiNote % 12) + 12) % 12; }

// "A3", "C#4": the note a whole MIDI number names (octave numbering where 60 = C4).
inline std::string noteName (int midiNote)
{
    const int octave = (midiNote - pitchClassOf (midiNote)) / 12 - 1; // floor division
    return std::string (kNoteNames[pitchClassOf (midiNote)]) + std::to_string (octave);
}

// The reference tuning a song can have: a semitone either side of 440 Hz. The TUNING knob, a loaded project and DETECT A4 all stay inside it.
inline constexpr double kMinA4Hz = 415.0, kMaxA4Hz = 466.0;

struct Key
{
    int root = 0;                 // pitch class, 0 = C
    int scale = kChromatic;       // index into kScales
    double a4Hz = 440.0;          // the song's reference tuning

    // How far the song's grid sits above the 440 Hz one, in semitones (a fraction of one).
    double gridOffset() const noexcept { return 12.0 * std::log2 (a4Hz / 440.0); }

    bool allows (int gridNote) const noexcept
    {
        return (kScales[std::clamp (scale, 0, kNumScales - 1)].mask >> ((pitchClassOf (gridNote) - root + 12) % 12) & 1u) != 0;
    }

    // The note of the scale nearest to `midi`, as an absolute pitch (the grid offset is put back).
    double snap (double midi) const noexcept
    {
        const double u = midi - gridOffset();
        const int centre = (int) std::lround (u);
        double best = u;
        double bestDistance = 1.0e9;
        for (int k = centre - 6; k <= centre + 6; ++k)
            if (allows (k) && std::fabs (u - k) < bestDistance)
            {
                bestDistance = std::fabs (u - k);
                best = k;
            }
        return best + gridOffset();
    }

    // Whole grid note nearest to a pitch, in scale or not (for names).
    int nearestGridNote (double midi) const noexcept { return (int) std::lround (midi - gridOffset()); }
};

struct KeyGuess
{
    int root = 0;
    bool minor = false;
    double score = 0.0; // correlation with the winning profile, -1..1
};

struct TuningGuess
{
    double a4Hz = 440.0;
    double strength = 0.0; // 0 = the notes sit anywhere between semitones, 1 = all on one grid
};

namespace analysis
{
// The weight of a note in the statistics: how long it is, more if the pitch was clear.
inline double weightOf (const Note& n) noexcept { return (double) n.length(); }
} // namespace analysis

// Krumhansl-Schmuckler: the pitch-class histogram of the notes, weighted by length, against the major and minor profiles in all 12 rotations.
inline KeyGuess detectKey (const std::vector<Note>& notes, double gridOffset = 0.0)
{
    static constexpr std::array<double, 12> major { 6.35, 2.23, 3.48, 2.33, 4.38, 4.09, 2.52, 5.19, 2.39, 3.66, 2.29, 2.88 };
    static constexpr std::array<double, 12> minor { 6.33, 2.68, 3.52, 5.38, 2.60, 3.53, 2.54, 4.75, 3.98, 2.69, 3.34, 3.17 };
    std::array<double, 12> hist {};
    for (const auto& n : notes)
        hist[(size_t) pitchClassOf ((int) std::lround (n.centre - gridOffset))] += analysis::weightOf (n);
    const auto correlate = [&] (const std::array<double, 12>& profile, int rotation)
    {
        double mh = 0, mp = 0;
        for (int i = 0; i < 12; ++i)
        {
            mh += hist[(size_t) i];
            mp += profile[(size_t) i];
        }
        mh /= 12;
        mp /= 12;
        double num = 0, dh = 0, dp = 0;
        for (int i = 0; i < 12; ++i)
        {
            const double h = hist[(size_t) ((i + rotation) % 12)] - mh, p = profile[(size_t) i] - mp;
            num += h * p;
            dh += h * h;
            dp += p * p;
        }
        return dh > 0 ? num / std::sqrt (dh * dp) : 0.0;
    };
    KeyGuess best;
    best.score = -2.0;
    for (int root = 0; root < 12; ++root)
        for (const bool isMinor : { false, true })
        {
            const double r = correlate (isMinor ? minor : major, root);
            if (r > best.score)
                best = { root, isMinor, r };
        }
    return best;
}

// The grid the notes sit on: the circular mean of their distance from a semitone. A song recorded at A4 = 432 Hz has its notes
// 32 cents below the 440 grid; so does a singer who is flat all song, which is also what retuning to the song's own grid fixes.
inline TuningGuess detectTuning (const std::vector<Note>& notes)
{
    double c = 0.0, s = 0.0, total = 0.0;
    for (const auto& n : notes)
    {
        const double w = analysis::weightOf (n), phase = 2.0 * M_PI * n.centre;
        c += w * std::cos (phase);
        s += w * std::sin (phase);
        total += w;
    }
    if (total <= 0.0)
        return {};
    const double offset = std::atan2 (s, c) / (2.0 * M_PI); // semitones, -0.5..0.5
    return { 440.0 * std::exp2 (offset / 12.0), std::sqrt (c * c + s * s) / total };
}

// How clearly the notes sit on one off-grid before it is believed (0 = anywhere, 1 = all on one grid). A singer who is 30 cents flat all
// the way through gives about 0.95; notes that are each a little sharp or flat in their own way give 0.2 to 0.4 and say nothing about a reference.
inline constexpr double kTuningStrengthNeeded = 0.5;

// The key of a melody, rounding each note on the grid its singer used if there is a clear one.
inline KeyGuess detectKeyAuto (const std::vector<Note>& notes)
{
    const auto tuning = detectTuning (notes);
    return detectKey (notes, tuning.strength >= kTuningStrengthNeeded ? 12.0 * std::log2 (tuning.a4Hz / 440.0) : 0.0);
}
} // namespace orpheus
