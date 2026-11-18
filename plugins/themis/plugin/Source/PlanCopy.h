// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 The Pantheon authors

#pragma once

#include <pantheon/dsp/BandLink.h>
#include <pantheon/dsp/MusicBands.h>

#include <themis/EqMove.h>
#include <themis/MasterAdvisor.h>
#include <themis/TargetCurve.h>

#include <cstdio>
#include <string>
#include <vector>

// COPY ALL: the whole master plan as ONE pdsp::link text, for the plugins that read it (Iris and Apollo for the bands, Hephaestus
// for a fill band, Aegis for the limiter line, Castor for the width line). Plain C++, no JUCE: the rules are tested in
// tests/test_main.cpp, and the master plan's panel only copies the text and shows the status line.
//
// The rules, all about one clipboard text that is pasted once in each plugin:
//  - every copyable item contributes what its own COPY would: its bands, its limiter setting and its width line; the `limiter` line
//    takes the ceiling and the gain change of the first copyable item that has each one, and later items' values are left out
//    (a doubled setting would be wrong: two items that both take 2.5 dB off must not take 5);
//  - a width line is never written in the same text as a band that touches the side signal (placed side or stereo): both would
//    cut the same side. The per-item COPY of a width item hands both and says so; COPY ALL cannot ask which one to paste, so when
//    such a band is in the text the width lines are left out and counted. The band carries the cut the item's headline states;
//  - the plan has no target curve (its judgements never compare with the reference), so the curve line is written only when a
//    caller passes one; the panel passes none, as the per-item COPY does;
//  - no amount key is written: each band and each limiter setting takes all of its change, as the per-item COPY does.
namespace themis
{
namespace plancopy
{
struct Result
{
    std::string text;   // the clipboard text; empty when there is nothing to copy
    std::string reason; // why there is nothing to copy (the COPY ALL control says it); empty when there is a text
    int items = 0;      // plan items that contributed something
    int bands = 0, fillBands = 0;
    int curves = 0;     // 0 or 1
    int limiters = 0;   // 0 or 1: one limiter line
    bool ceiling = false, gain = false;
    float ceilingDb = 0.0f, gainDb = 0.0f;
    int widthBands = 0;        // bands the width line sets (0 = no width line)
    int widthItemsLeftOut = 0; // width items whose width line is left out (a band on the side carries the same cut)

    bool empty() const { return text.empty(); }
};

// One suggested band as the text the other plugins paste (its kind, its place and its numbers).
inline pdsp::LinkBand toLinkBand (const EqMove& m)
{
    using K = EqMove::Kind;
    pdsp::LinkBand b;
    b.kind = m.kind == K::lowShelf ? pdsp::LinkBand::Kind::lowShelf : m.kind == K::highShelf ? pdsp::LinkBand::Kind::highShelf
           : m.kind == K::lowCut ? pdsp::LinkBand::Kind::lowCut : m.kind == K::highCut ? pdsp::LinkBand::Kind::highCut
           : m.kind == K::harmonicFill ? pdsp::LinkBand::Kind::fill : pdsp::LinkBand::Kind::bell;
    b.place = m.place == EqMove::Place::mid ? pdsp::LinkBand::Place::mid
            : m.place == EqMove::Place::side ? pdsp::LinkBand::Place::side : pdsp::LinkBand::Place::stereo;
    b.freq = m.freqHz;
    b.gainDb = m.gainDb;
    b.q = m.q;
    if (m.kind == K::harmonicFill)
    {
        b.loHz = m.loHz;
        b.hiHz = m.hiHz;
    }
    return b;
}

// True when a band changes the side signal (a band placed on the side, or in the stereo image, which changes the side too).
inline bool touchesSide (const pdsp::LinkBand& b)
{
    return b.place == pdsp::LinkBand::Place::side || b.place == pdsp::LinkBand::Place::stereo;
}

// The whole plan as one text. `target`, when given, adds the curve line (the reference Iris and Apollo draw).
inline Result copyAll (const MasterAdvice& advice, const TargetCurve* target = nullptr)
{
    Result r;
    if (advice.items.empty())
    {
        r.reason = advice.measuring ? "the plan is still measuring: play the track, then COPY ALL"
                                    : "the plan has no items, so there is nothing to copy";
        return r;
    }

    std::vector<pdsp::LinkBand> bands;
    pdsp::LinkLimiter limiter;
    pdsp::LinkWidth width;
    int widthItems = 0;
    for (const auto& item : advice.items)
    {
        if (!item.canCopy())
            continue;
        ++r.items;
        for (const auto& m : item.moves)
            bands.push_back (toLinkBand (m));
        if (item.limiter.setsCeiling && !limiter.hasCeiling)
        {
            limiter.hasCeiling = true;
            limiter.ceilingDb = item.limiter.ceilingDb;
        }
        if (item.limiter.changesGain && !limiter.hasGainChange)
        {
            limiter.hasGainChange = true;
            limiter.gainChangeDb = item.limiter.gainChangeDb;
        }
        if (item.width.active())
        {
            ++widthItems;
            for (size_t b = 0; b < pdsp::bands::kCount; ++b)
                if (item.width.percent[b].has_value() && !width.percent[b].has_value())
                    width.percent[b] = item.width.percent[b]; // the first item of the plan (worst first) that names the band
        }
    }

    bool sideBand = false;
    for (const auto& b : bands)
        sideBand = sideBand || touchesSide (b);
    pdsp::LinkWidth wide;
    if (widthItems > 0)
    {
        if (sideBand)
            r.widthItemsLeftOut = widthItems;
        else
            wide = width;
    }

    pdsp::LinkCurve curve;
    if (target != nullptr)
    {
        curve.f0Hz = BandGrid::kF0;
        curve.perOctave = BandGrid::kPerOctave;
        curve.name = target->name;
        curve.db.assign (target->medianDb.begin(), target->medianDb.end());
    }
    const bool haveCurve = target != nullptr && curve.valid();

    if (r.items == 0 || (bands.empty() && !limiter.valid() && !wide.valid() && !haveCurve))
    {
        r.reason = "none of the plan's items has a band, a limiter setting or a width line to hand over: their fixes are in your DAW";
        return r;
    }

    r.text = pdsp::link::encode (bands, haveCurve ? &curve : nullptr, limiter.valid() ? &limiter : nullptr, wide.valid() ? &wide : nullptr);
    r.bands = static_cast<int> (bands.size());
    for (const auto& b : bands)
        r.fillBands += b.kind == pdsp::LinkBand::Kind::fill ? 1 : 0;
    r.curves = haveCurve ? 1 : 0;
    r.limiters = limiter.valid() ? 1 : 0;
    r.ceiling = limiter.hasCeiling;
    r.gain = limiter.hasGainChange;
    r.ceilingDb = static_cast<float> (limiter.ceilingDb);
    r.gainDb = static_cast<float> (limiter.gainChangeDb);
    for (const auto& p : wide.percent)
        r.widthBands += p.has_value() ? 1 : 0;
    return r;
}

// The status line after COPY ALL: what is inside (counts of bands, curve, limiter and width) and where to paste it.
inline std::string statusText (const Result& r)
{
    if (r.empty())
        return "Nothing to copy: " + r.reason + ".";
    char buf[128];
    std::string out = "Copied the whole plan (" + std::to_string (r.items) + " item(s)): bands " + std::to_string (r.bands);
    if (r.fillBands > 0)
        out += " (" + std::to_string (r.fillBands) + " fill)";
    out += ", curve " + std::to_string (r.curves) + ", limiter " + std::to_string (r.limiters) + ", width " + std::to_string (r.widthBands) + " band(s)";
    if (r.limiters > 0)
    {
        std::string set;
        if (r.ceiling)
        {
            std::snprintf (buf, sizeof (buf), "ceiling %.1f dBTP", static_cast<double> (r.ceilingDb));
            set += buf;
        }
        if (r.gain)
        {
            std::snprintf (buf, sizeof (buf), "%sgain %+.1f dB", set.empty() ? "" : ", ", static_cast<double> (r.gainDb));
            set += buf;
        }
        out += " (" + set + ")";
    }
    out += ". ";
    if (r.widthItemsLeftOut > 0)
        out += "The width line of " + std::to_string (r.widthItemsLeftOut) + " item(s) is left out: a band on the side already makes the same cut. ";
    out += "Paste it once in each plugin: Iris or Apollo take the bands, Hephaestus the fill, Aegis the limiter, Castor the width; each skips the lines it does not read.";
    return out;
}
} // namespace plancopy
} // namespace themis
