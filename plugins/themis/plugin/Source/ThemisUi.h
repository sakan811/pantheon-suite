// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 The Pantheon authors

#pragma once

#include <pantheon/ui/Controls.h>
#include <pantheon/ui/Hardware.h>
#include <pantheon/ui/HardwareLookAndFeel.h>
#include <pantheon/ui/MeterCell.h>

#include <pantheon/dsp/BandLink.h>

#include <themis/Common.h>
#include <themis/EqMove.h>
#include <themis/Translation.h>

// What the shared hardware kit (shared/ui) cannot know about: Themis' own words, number formats and verdict type. The
// drawing itself (panels, lamps, knobs, glass, meters) lives in shared/ui and is used from here, not redrawn.
namespace colours
{
// The one mapping from a verdict to its lamp colour.
inline juce::Colour forLevel (themis::Level l)
{
    return l == themis::Level::good ? good : l == themis::Level::warn ? warn : bad;
}
} // namespace colours

namespace ui
{
// The two words of the plugin's name. The full name ("Themis Verdict") is set once, in CMakeLists.txt (PLUGIN_NAME), and
// reaches the code as JucePlugin_Name; the smoke test checks that the two agree.
inline constexpr const char* kProjectWord = "Themis";
inline constexpr const char* kPluginWord = "Verdict";

inline juce::String signedDb (float v) { return juce::String (themis::signedDbText (v)); }

inline juce::String hzText (double hz) { return juce::String (themis::hzText (hz)); }

// Suggested bands as the text the other plugins of the repository paste (pdsp::link): equaliser bands go to the equaliser,
// a harmonic-fill band to the harmonic generator.
// The band list for the other plugins, with the target curve the bands were fitted to when there is one (Iris and Apollo draw it as their reference).
inline juce::String linkText (const themis::EqMoves& moves, const themis::TargetCurve* target = nullptr, const themis::LimiterMove& limiter = {})
{
    using K = themis::EqMove::Kind;
    std::vector<pdsp::LinkBand> bands;
    for (const auto& m : moves)
    {
        pdsp::LinkBand b;
        b.kind = m.kind == K::lowShelf ? pdsp::LinkBand::Kind::lowShelf : m.kind == K::highShelf ? pdsp::LinkBand::Kind::highShelf
               : m.kind == K::lowCut ? pdsp::LinkBand::Kind::lowCut : m.kind == K::highCut ? pdsp::LinkBand::Kind::highCut
               : m.kind == K::harmonicFill ? pdsp::LinkBand::Kind::fill : pdsp::LinkBand::Kind::bell;
        b.place = m.place == themis::EqMove::Place::mid ? pdsp::LinkBand::Place::mid
                : m.place == themis::EqMove::Place::side ? pdsp::LinkBand::Place::side : pdsp::LinkBand::Place::stereo;
        b.freq = m.freqHz;
        b.gainDb = m.gainDb;
        b.q = m.q;
        b.loHz = m.kind == K::harmonicFill ? m.loHz : b.loHz;
        b.hiHz = m.kind == K::harmonicFill ? m.hiHz : b.hiHz;
        bands.push_back (b);
    }
    pdsp::LinkCurve curve;
    if (target != nullptr)
    {
        curve.f0Hz = themis::BandGrid::kF0;
        curve.perOctave = themis::BandGrid::kPerOctave;
        curve.name = target->name;
        curve.db.assign (target->medianDb.begin(), target->medianDb.end());
    }
    pdsp::LinkLimiter set;
    set.hasCeiling = limiter.setsCeiling;
    set.ceilingDb = limiter.ceilingDb;
    set.hasGainChange = limiter.changesGain;
    set.gainChangeDb = limiter.gainChangeDb;
    return juce::String (pdsp::link::encode (bands, target != nullptr ? &curve : nullptr, limiter.active() ? &set : nullptr));
}

// Where a copied fix goes, for the status line.
inline juce::String linkDestination (const themis::EqMoves& moves, const themis::LimiterMove& limiter = {})
{
    if (moves.empty())
        return limiter.active() ? "Aegis" : "";
    bool fill = false, eq = false;
    for (const auto& m : moves)
        (m.kind == themis::EqMove::Kind::harmonicFill ? fill : eq) = true;
    return fill && eq ? "Iris or Apollo (EQ) and Hephaestus (fill)" : fill ? "Hephaestus" : "Iris or Apollo";
}
} // namespace ui
