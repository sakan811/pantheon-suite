// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 The Pantheon authors

#pragma once

#include "Check.h"
#include "Tempo.h"

#include <cmath>
#include <string>

inline bool tempoNear (double a, double b, double tolerance = 1e-6)
{
    return std::abs (a - b) <= tolerance;
}

// Checks for olympus::TempoMap (Source/Tempo.h). 48 kHz throughout.
inline void tempoChecks()
{
    using olympus::Grid;
    using olympus::TempoMap;

    // 120 bpm: one beat is 0.5 s, 24000 samples at 48 kHz.
    {
        TempoMap m;
        test::check (m.beatsToSamples (1.0) == 24000.0, "120 bpm: one beat is 24000 samples");
        test::check (m.beatsToSamples (4.0) == 96000.0, "120 bpm: one bar of 4/4 is 96000 samples");
        test::check (m.samplesToBeats (24000.0) == 1.0, "120 bpm: 24000 samples is one beat");
        test::check (m.bpmAtBeat (3.5) == 120.0 && m.bpmAtSample (1.0e6) == 120.0, "single tempo: bpm everywhere");
    }

    // Samples -> beats -> samples, and beats -> samples -> beats, at many points.
    {
        TempoMap m;
        m.setTempo (8.0, 60.0);
        m.setTempo (12.5, 200.0);

        bool samplesOk = true;
        for (double s : { 0.0, 1.0, 1000.0, 12345.0, 96000.0, 191999.0, 192000.0, 240000.0, 250000.0, 400000.0, 1234567.0 })
            samplesOk = samplesOk && tempoNear (m.beatsToSamples (m.samplesToBeats (s)), s);
        test::check (samplesOk, "round trip samples -> beats -> samples at 11 points, with two tempo changes");

        bool beatsOk = true;
        for (double b : { 0.0, 0.25, 1.0, 7.5, 8.0, 9.0, 12.5, 13.0, 100.0 })
            beatsOk = beatsOk && tempoNear (m.samplesToBeats (m.beatsToSamples (b)), b);
        test::check (beatsOk, "round trip beats -> samples -> beats at 9 points, with two tempo changes");

        bool negativeOk = true;
        for (double b : { -1.0, -0.5, -3.25 })
            negativeOk = negativeOk && tempoNear (m.samplesToBeats (m.beatsToSamples (b)), b);
        test::check (negativeOk, "round trip before the start (linear, at the first tempo)");
    }

    // A tempo change at beat 8: 120 bpm up to there, 60 bpm after.
    {
        TempoMap m;
        m.setTempo (8.0, 60.0);
        test::check (m.beatsToSamples (8.0) == 192000.0, "tempo change: beat 8 is sample 192000 (8 * 24000)");
        test::check (m.beatsToSamples (9.0) == 240000.0, "tempo change: beat 9 is sample 240000 (+48000 at 60 bpm)");
        test::check (m.beatsToSamples (10.0) == 288000.0, "tempo change: beat 10 is sample 288000");
        test::check (m.samplesToBeats (192000.0) == 8.0, "tempo change: sample 192000 is beat 8");
        test::check (m.samplesToBeats (216000.0) == 8.5, "tempo change: sample 216000 is beat 8.5");
        test::check (m.samplesToBeats (240000.0) == 9.0, "tempo change: sample 240000 is beat 9");
        test::check (m.bpmAtBeat (7.99) == 120.0 && m.bpmAtBeat (8.0) == 60.0, "tempo change: bpm switches at beat 8");
        test::check (m.bpmAtSample (191999.0) == 120.0 && m.bpmAtSample (192000.0) == 60.0, "tempo change: bpm at sample switches at 192000");
    }

    // Time signatures.
    {
        TempoMap m;
        m.setSignature (3, 4);
        test::check (m.beatsPerBar() == 3.0, "3/4: three quarter-note beats per bar");
        test::check (m.gridBeats (Grid::Bar) == 3.0, "3/4: the bar grid is three beats");
        test::check (m.sampleAtBar (2) == 72000.0, "3/4: bar 2 starts at sample 72000");

        m.setSignature (6, 8);
        test::check (m.beatsPerBar() == 3.0, "6/8: beatsPerBar is 3");
        test::check (m.sampleAtBar (2) == 72000.0, "6/8: bar 2 starts at sample 72000");

        m.setSignature (4, 3); // invalid denominator: ignored
        test::check (m.numerator == 6 && m.denominator == 8, "invalid denominator 3 is ignored");

        m.setSignature (0, 4); // numerator clamped up to 1
        test::check (m.numerator == 1 && m.denominator == 4, "numerator 0 is clamped to 1");

        m.setSignature (40, 4); // numerator clamped down to 32
        test::check (m.numerator == 32, "numerator 40 is clamped to 32");

        m.setSignature (4, 4);
        test::check (m.beatsPerBar() == 4.0, "4/4 is restored");
    }

    // Snapping: nearest grid line, halfway going up, never before the start.
    {
        TempoMap m; // 120 bpm 4/4: quarter 24000, eighth 12000, sixteenth 6000, 1/32 3000, bar 96000, half 48000
        test::check (m.snapSample (12000.0, Grid::Quarter) == 24000.0, "snap quarter: halfway (12000) goes up to 24000");
        test::check (m.snapSample (11999.0, Grid::Quarter) == 0.0, "snap quarter: just below halfway goes down to 0");
        test::check (m.snapSample (29000.0, Grid::Quarter) == 24000.0, "snap quarter: 29000 is nearer 24000");
        test::check (m.snapSample (36000.0, Grid::Quarter) == 48000.0, "snap quarter: 1.5 beats goes up to beat 2");
        test::check (m.snapSample (24000.0, Grid::Half) == 48000.0, "snap half: beat 1 is halfway between 0 and 2, goes up");
        test::check (m.snapSample (50000.0, Grid::Half) == 48000.0, "snap half: 50000 is nearer 48000");
        test::check (m.snapSample (6000.0, Grid::Eighth) == 12000.0, "snap eighth: halfway (6000) goes up to 12000");
        test::check (m.snapSample (5999.0, Grid::Eighth) == 0.0, "snap eighth: below halfway goes down to 0");
        test::check (m.snapSample (3000.0, Grid::Sixteenth) == 6000.0, "snap sixteenth: halfway (3000) goes up to 6000");
        test::check (m.snapSample (1500.0, Grid::ThirtySecond) == 3000.0, "snap 1/32: halfway (1500) goes up to 3000");
        test::check (m.snapSample (47999.0, Grid::Bar) == 0.0, "snap bar: just below halfway (47999) goes down to 0");
        test::check (m.snapSample (48000.0, Grid::Bar) == 96000.0, "snap bar: halfway (48000) goes up to 96000");
        test::check (m.snapSample (-5000.0, Grid::Quarter) == 0.0, "snap negative input: -5000 goes to 0");
        test::check (m.snapSample (-30000.0, Grid::Quarter) == 0.0, "snap negative input: -30000 goes to 0, never below");
        test::check (m.snapSample (-1.0, Grid::Bar) == 0.0, "snap negative input: -1 at bar grid goes to 0");
        test::check (m.snapSample (0.0, Grid::Eighth) == 0.0, "snap zero stays zero");
    }

    // Floor: the grid line at or before.
    {
        TempoMap m;
        test::check (m.floorSample (30000.0, Grid::Quarter) == 24000.0, "floor quarter: 30000 goes to 24000");
        test::check (m.floorSample (24000.0, Grid::Quarter) == 24000.0, "floor quarter: a line stays on itself");
        test::check (m.floorSample (23999.0, Grid::Quarter) == 0.0, "floor quarter: just before a line goes to the one before");
        test::check (m.floorSample (-100.0, Grid::Quarter) == 0.0, "floor negative input: 0");
    }

    // Bar and beat positions.
    {
        TempoMap m;
        auto at0 = m.barBeatAt (0.0);
        test::check (at0.bar == 1 && at0.beat == 1 && at0.fraction == 0.0, "barBeatAt sample 0 is bar 1 beat 1");

        auto at2 = m.barBeatAt (96000.0);
        test::check (at2.bar == 2 && at2.beat == 1 && at2.fraction == 0.0, "barBeatAt 4 beats (96000) is bar 2 beat 1");

        auto justBefore = m.barBeatAt (95999.0);
        test::check (justBefore.bar == 1 && justBefore.beat == 4, "barBeatAt 95999 is still bar 1 beat 4");

        auto halfBeat = m.barBeatAt (36000.0);
        test::check (halfBeat.bar == 1 && halfBeat.beat == 2 && tempoNear (halfBeat.fraction, 0.5), "barBeatAt 1.5 beats is bar 1 beat 2, fraction 0.5");

        auto third = m.barBeatAt (192000.0);
        test::check (third.bar == 3 && third.beat == 1, "barBeatAt 8 beats is bar 3 beat 1");

        m.setSignature (3, 4);
        auto waltz = m.barBeatAt (72000.0);
        test::check (waltz.bar == 2 && waltz.beat == 1, "3/4: barBeatAt 3 beats is bar 2 beat 1");

        test::check (m.sampleAtBar (1) == 0.0, "sampleAtBar 1 is sample 0");
        test::check (m.sampleAtBar (0) == 0.0, "sampleAtBar below 1 is the first bar");
    }

    // setTempo: replaces at the same beat, inserts in order, beat 0 replaces the first point.
    {
        TempoMap m;
        m.setTempo (8.0, 90.0);
        test::check (m.points.size() == 2 && m.bpmAtBeat (8.0) == 90.0, "setTempo at a new beat adds a point");

        m.setTempo (8.0 + 5e-7, 100.0);
        test::check (m.points.size() == 2 && m.points[1].beat == 8.0 && m.points[1].bpm == 100.0,
                     "setTempo within 1e-6 beats replaces the point, the beat stays");

        m.setTempo (2.0, 150.0);
        test::check (m.points.size() == 3 && m.points[1].beat == 2.0 && m.points[2].beat == 8.0,
                     "setTempo inserts in beat order");

        m.setTempo (0.0, 140.0);
        test::check (m.points.size() == 3 && m.points[0].beat == 0.0 && m.points[0].bpm == 140.0,
                     "setTempo at beat 0 replaces the first point, no new point");

        m.setTempo (-4.0, 110.0);
        test::check (m.points.size() == 3 && m.points[0].beat == 0.0 && m.points[0].bpm == 110.0,
                     "setTempo at a negative beat replaces the first point");
    }

    // removeTempo: never the first point.
    {
        TempoMap m;
        m.setTempo (8.0, 60.0);
        test::check (! m.removeTempo (0) && m.points.size() == 2, "removeTempo refuses index 0");
        test::check (! m.removeTempo (5) && m.points.size() == 2, "removeTempo refuses an index out of range");
        test::check (m.removeTempo (1) && m.points.size() == 1, "removeTempo removes a later point");
        test::check (m.bpmAtBeat (8.0) == 120.0, "after removing, the first tempo runs on");
    }

    // Clamping.
    {
        TempoMap m;
        m.setTempo (0.0, 1000.0);
        test::check (m.points[0].bpm == 300.0, "bpm above 300 is clamped to 300");
        m.setTempo (0.0, 5.0);
        test::check (m.points[0].bpm == 20.0, "bpm below 20 is clamped to 20");
        m.setTempo (4.0, 0.0);
        test::check (m.points.size() == 2 && m.points[1].bpm == 20.0, "a new point's bpm is clamped too");
    }

    // Description and grid names.
    {
        TempoMap m;
        test::check (m.describe() == "120 bpm 4/4", test::fmt ("describe: \"%s\"", m.describe().c_str()).c_str());
        m.setTempo (8.0, 140.0);
        m.setSignature (3, 4);
        test::check (m.describe() == "120-140 bpm 3/4", test::fmt ("describe with a change: \"%s\"", m.describe().c_str()).c_str());
        test::check (std::string (olympus::gridName (Grid::Bar)) == "1 bar"
                         && std::string (olympus::gridName (Grid::Eighth)) == "1/8"
                         && std::string (olympus::gridName (Grid::ThirtySecond)) == "1/32",
                     "gridName gives 1 bar, 1/8 and 1/32");
    }
}
