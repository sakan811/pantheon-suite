// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 The Pantheon authors

#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <limits>
#include <string>
#include <vector>

// The project's tempo and time signature, plain C++ so the tests need no JUCE.
// Positions on the tempo side are quarter-note beats; positions on the timeline
// are samples at the session rate. Tempo is piecewise constant: a change takes
// effect at its beat and audio never follows it (the grid does). Beats before 0
// run at the first tempo, so the mapping is linear there and invertible.
//
// Beat numbers in barBeatAt count quarter notes, whatever the denominator: 6/8
// has three beats in a bar, the same as 3/4, because the metronome clicks on
// every quarter. That keeps one beat unit for the whole map.

namespace olympus
{
enum class Grid { Bar, Half, Quarter, Eighth, Sixteenth, ThirtySecond };

inline const char* gridName (Grid g)
{
    switch (g)
    {
        case Grid::Bar:          return "1 bar";
        case Grid::Half:         return "1/2";
        case Grid::Quarter:      return "1/4";
        case Grid::Eighth:       return "1/8";
        case Grid::Sixteenth:    return "1/16";
        case Grid::ThirtySecond: return "1/32";
    }
    return "1/4";
}

struct TempoPoint
{
    double beat = 0.0; // quarter notes from the start of the project
    double bpm = 120.0;
};

class TempoMap
{
public:
    static constexpr double kMinBpm = 20.0;
    static constexpr double kMaxBpm = 300.0;
    // Two tempo points closer than this are the same point (setTempo).
    static constexpr double kBeatEps = 1e-6;
    // Samples that land exactly on a grid line come out as that line, not the
    // one before it, even though beat arithmetic on samples is not exact.
    static constexpr double kGridEps = 1e-9;

    double sampleRate = 48000.0;
    std::vector<TempoPoint> points { { 0.0, 120.0 } }; // sorted by beat, points[0].beat == 0
    int numerator = 4, denominator = 4;

    double beatsPerBar() const
    {
        return numerator * 4.0 / denominator;
    }

    double gridBeats (Grid g) const
    {
        switch (g)
        {
            case Grid::Bar:          return beatsPerBar();
            case Grid::Half:         return 2.0;
            case Grid::Quarter:      return 1.0;
            case Grid::Eighth:       return 0.5;
            case Grid::Sixteenth:    return 0.25;
            case Grid::ThirtySecond: return 0.125;
        }
        return 1.0;
    }

    double bpmAtBeat (double beat) const
    {
        double bpm = firstBpm();
        for (const auto& p : points)
        {
            if (p.beat > beat)
                break;
            bpm = p.bpm;
        }
        return bpm;
    }

    double bpmAtSample (double sample) const
    {
        return bpmAtBeat (samplesToBeats (sample));
    }

    double beatsToSamples (double beats) const
    {
        if (beats < 0.0)
            return beats * samplesPerBeat (firstBpm());

        // Walk the segments up to `beats`, adding each one's exact length.
        double samples = 0.0;
        for (std::size_t i = 0; i < points.size() && beats > points[i].beat; ++i)
        {
            const double end = i + 1 < points.size() ? std::min (beats, points[i + 1].beat) : beats;
            samples += (end - points[i].beat) * samplesPerBeat (points[i].bpm);
        }
        return samples;
    }

    double samplesToBeats (double samples) const
    {
        if (samples < 0.0)
            return samples / samplesPerBeat (firstBpm());

        // The inverse: find the segment the sample falls in, with the same
        // arithmetic beatsToSamples uses, so the two round-trip exactly.
        double elapsed = 0.0;
        for (std::size_t i = 0; i < points.size(); ++i)
        {
            const double spb = samplesPerBeat (points[i].bpm);
            const bool last = i + 1 == points.size();
            const double length = last ? std::numeric_limits<double>::infinity()
                                       : (points[i + 1].beat - points[i].beat) * spb;

            if (samples <= elapsed + length)
                return points[i].beat + (samples - elapsed) / spb;

            elapsed += length;
        }
        return 0.0; // not reached: the last segment is unbounded
    }

    // Adds a tempo point, or replaces the bpm of the one at the same beat.
    // Beat 0 (or anything within kBeatEps of it) always replaces the first point.
    void setTempo (double beat, double bpm)
    {
        if (! std::isfinite (beat) || ! std::isfinite (bpm))
            return;

        bpm = std::clamp (bpm, kMinBpm, kMaxBpm);
        beat = std::max (0.0, beat);

        if (beat <= kBeatEps)
        {
            if (points.empty())
                points.push_back ({ 0.0, bpm });
            points.front().bpm = bpm;
            return;
        }

        for (auto& p : points)
        {
            if (std::abs (p.beat - beat) <= kBeatEps)
            {
                p.bpm = bpm;
                return;
            }
        }

        points.push_back ({ beat, bpm });
        std::sort (points.begin(), points.end(), [] (const TempoPoint& a, const TempoPoint& b) { return a.beat < b.beat; });
    }

    // The first point is the project's start and cannot go. Returns false for it
    // and for an index out of range.
    bool removeTempo (int index)
    {
        if (index <= 0 || index >= static_cast<int> (points.size()))
            return false;

        points.erase (points.begin() + index);
        return true;
    }

    // An invalid denominator is ignored (the old one stays); the numerator is clamped to 1..32.
    void setSignature (int num, int den)
    {
        if (den != 1 && den != 2 && den != 4 && den != 8 && den != 16 && den != 32)
            return;

        numerator = std::clamp (num, 1, 32);
        denominator = den;
    }

    // The nearest grid line, halfway going up. Result is never before the start.
    double snapSample (double sample, Grid g) const
    {
        const double step = gridBeats (g);
        const double lines = std::floor (samplesToBeats (sample) / step + 0.5 + kGridEps);
        return gridLineSample (lines, step);
    }

    // The grid line at or before the sample. Result is never before the start.
    double floorSample (double sample, Grid g) const
    {
        const double step = gridBeats (g);
        const double lines = std::floor (samplesToBeats (sample) / step + kGridEps);
        return gridLineSample (lines, step);
    }

    struct BarBeat
    {
        int bar = 1;        // 1-based; 0 and below are the bars before the start
        int beat = 1;       // 1-based quarter note within the bar
        double fraction = 0.0; // 0..1 through that beat
    };

    BarBeat barBeatAt (double sample) const
    {
        const double bpb = beatsPerBar();
        const double beats = samplesToBeats (sample);
        const double barIndex = std::floor (beats / bpb + kGridEps);
        const double pos = std::max (0.0, beats - barIndex * bpb);
        const int beatsInBar = std::max (1, static_cast<int> (std::ceil (bpb - kGridEps)));

        const int beatIndex = std::clamp (static_cast<int> (std::floor (pos + kGridEps)), 0, beatsInBar - 1);

        BarBeat result;
        // barIndex is unbounded (a timeline position can be huge either way): saturate both ends before the cast so it cannot overflow int
        result.bar = static_cast<int> (std::clamp (barIndex, static_cast<double> (std::numeric_limits<int>::min() + 1),
                                                   static_cast<double> (std::numeric_limits<int>::max() - 1))) + 1;
        result.beat = beatIndex + 1;
        result.fraction = std::clamp (pos - beatIndex, 0.0, 1.0);
        return result;
    }

    // Bar is 1-based; anything below 1 is the first bar.
    double sampleAtBar (int bar) const
    {
        return beatsToSamples ((std::max (1, bar) - 1) * beatsPerBar());
    }

    // "120 bpm 4/4", or "120-140 bpm 4/4" when the tempo changes.
    std::string describe() const
    {
        double lo = firstBpm();
        double hi = lo;
        for (const auto& p : points)
        {
            lo = std::min (lo, p.bpm);
            hi = std::max (hi, p.bpm);
        }

        char bpmText[64];
        if (hi - lo < 1e-9)
            std::snprintf (bpmText, sizeof bpmText, "%g", lo);
        else
            std::snprintf (bpmText, sizeof bpmText, "%g-%g", lo, hi);

        char out[112];
        std::snprintf (out, sizeof out, "%s bpm %d/%d", bpmText, numerator, denominator);
        return out;
    }

private:
    double firstBpm() const
    {
        return points.empty() ? 120.0 : points.front().bpm;
    }

    double samplesPerBeat (double bpm) const
    {
        return 60.0 / bpm * sampleRate;
    }

    double gridLineSample (double lines, double step) const
    {
        return std::max (0.0, beatsToSamples (std::max (0.0, lines * step)));
    }
};
}
