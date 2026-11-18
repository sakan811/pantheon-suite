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
#include <themis/MasterAdvisor.h>
#include <themis/Translation.h>

#include "PlanCopy.h"

// What the shared hardware kit (shared/ui) cannot know about: Themis' own words, number formats and verdict type. The
// drawing itself (panels, lamps, knobs, glass, meters) lives in shared/ui and is used from here, not redrawn.
namespace colours
{
// The one mapping from a verdict to its lamp colour.
inline juce::Colour forLevel (themis::Level l)
{
    return l == themis::Level::good ? good : l == themis::Level::warn ? warn : bad;
}

// The colour a level is written in on the faceplate. The plain fix red is 3.9:1 there, under the 4.5:1 AA level for small text, so the
// words use this brighter red (4.7:1 on the faceplate, 6.4:1 on the glass); the lamps and the bars keep the plain colour.
inline const juce::Colour badText { 0xfff06a5e };
inline juce::Colour forLevelText (themis::Level l)
{
    return l == themis::Level::bad ? badText : forLevel (l);
}

// The lamp of the plan's verdict word (the word says it too: a colour is never the only coding). Measuring states are unlit.
inline juce::Colour forVerdict (themis::plan::Verdict v)
{
    using V = themis::plan::Verdict;
    return v == V::faults ? bad : v == V::check ? warn : v == V::clear ? good : dim;
}

// What the scope and the spectrogram draw that the kit has no colour for: the bezel of the round scope (metal, lit and in shade) and the dark teal
// the spectrogram's ramp passes through between the glass and its accent.
inline const juce::Colour bezelLit { 0xff6b675d }, bezelShade { 0xff25231f }, rampTeal { 0xff0f3a40 };
} // namespace colours

namespace gui
{
// The silkscreen captions of the header's three groups. The control rows are the same in both rows; a caption sits above its group.
inline constexpr const char* kTargetCaption = "TARGET";
inline constexpr const char* kJudgeCaption = "JUDGE";
inline constexpr const char* kSessionCaption = "SESSION";

// An engraved rule: a dark groove with its lit edge beside it, the way the header plate is grooved.
inline void drawEngravedRule (juce::Graphics& g, juce::Line<float> line)
{
    g.setColour (juce::Colours::black.withAlpha (0.65f));
    g.drawLine (line, 1.0f);
    g.setColour (juce::Colours::white.withAlpha (0.07f));
    g.drawLine (line.getStartX() + 1.0f, line.getStartY(), line.getEndX() + 1.0f, line.getEndY(), 1.0f);
}

// The readout cells' two groups: a silkscreen caption over each row, with an engraved rule running on from it. Row 1 is the tonal
// balance and its dynamics, row 2 how the balance translates and the stereo and peak checks.
inline constexpr const char* kToneGroupCaption = "TONE AND DYNAMICS";
inline constexpr const char* kTranslationGroupCaption = "TRANSLATION, STEREO AND PEAKS";
inline constexpr float kGroupCaptionHeight = 12.0f, kGroupCaptionFontHeight = 9.0f;

// The level word beside a lamp or a colour (the words are the core's, so the plan text and the screen say the same). One function
// for every place the screen shows a level.
inline const char* levelMark (themis::Level l) { return themis::plan::levelWord (l); }
inline constexpr float kLevelWordFontHeight = 10.0f, kCellTitleFontHeight = 10.0f, kLevelWordGap = 6.0f; // the kit sets the cell titles at this size

// The width of a line of text in a font (JUCE's own measure, as the glyph arrangement lays it out).
inline float textWidth (const juce::Font& font, const juce::String& text)
{
    juce::GlyphArrangement glyphs;
    glyphs.addLineOfText (font, text, 0.0f, 0.0f);
    return glyphs.getBoundingBox (0, -1, false).getWidth();
}

// The level word on the title line of a readout cell, right-aligned (the title is left-aligned on the same line). The kit draws the
// title without fitting it, so the word is left out when the two would overlap: the cell's colour and read-out still say it.
inline void drawLevelWord (juce::Graphics& g, const ui::MeterCell& cell, const juce::String& title, themis::Level level)
{
    const auto line = cell.area.withHeight (ui::MeterCell::kTitleHeight);
    const auto font = ui::labelFont (kLevelWordFontHeight);
    const float wordWidth = textWidth (font, levelMark (level));
    if (textWidth (ui::labelFont (kCellTitleFontHeight), title.toUpperCase()) + kLevelWordGap + wordWidth > line.getWidth())
        return;
    g.setFont (font);
    g.setColour (colours::forLevelText (level));
    g.drawText (levelMark (level), line.toNearestInt(), juce::Justification::centredRight);
}

// A group caption: the silkscreen words, then the engraved rule from just after them to the right edge of the band.
inline void drawGroupCaption (juce::Graphics& g, juce::Rectangle<float> band, const char* text)
{
    const auto font = ui::labelFont (kGroupCaptionFontHeight);
    juce::GlyphArrangement glyphs;
    glyphs.addLineOfText (font, text, 0.0f, 0.0f);
    const float textWidth = glyphs.getBoundingBox (0, -1, false).getWidth();
    g.setFont (font);
    g.setColour (colours::text);
    g.drawText (text, band.toNearestInt(), juce::Justification::centredLeft);
    const float y = band.getCentreY() + 1.0f, x0 = band.getX() + textWidth + 6.0f;
    drawEngravedRule (g, { x0, y, band.getRight(), y });
}

// The two words of the plugin's name. The full name ("Themis Verdict") is set once, in CMakeLists.txt (PLUGIN_NAME), and
// reaches the code as JucePlugin_Name; the smoke test checks that the two agree.
inline constexpr const char* kProjectWord = "Themis";
inline constexpr const char* kPluginWord = "Verdict";

inline juce::String signedDb (float v) { return juce::String (themis::signedDbText (v)); }

inline juce::String hzText (double hz) { return juce::String (themis::hzText (hz)); }

// The DEVICE LOSS meter's title. While a device is chosen the meter is labelled as not part of the master plan (the plan never
// judges the device, it is the track against the target).
inline juce::String deviceLossTitle (bool deviceChosen) { return deviceChosen ? "DEVICE LOSS: NOT IN PLAN" : "DEVICE LOSS"; }

// Suggested bands as the text the other plugins of the repository paste (pdsp::link): equaliser bands go to the equaliser,
// a harmonic-fill band to the harmonic generator.
// The band list for the other plugins, with the target curve the bands were fitted to when there is one (Iris and Apollo draw it as their reference).
inline juce::String linkText (const themis::EqMoves& moves, const themis::TargetCurve* target = nullptr, const themis::LimiterMove& limiter = {},
                             const themis::WidthMove& width = {})
{
    std::vector<pdsp::LinkBand> bands;
    for (const auto& m : moves)
        bands.push_back (themis::plancopy::toLinkBand (m));
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
    pdsp::LinkWidth wide;
    for (size_t b = 0; b < pdsp::bands::kCount; ++b)
        wide.percent[b] = width.percent[b];
    return juce::String (pdsp::link::encode (bands, target != nullptr ? &curve : nullptr, limiter.active() ? &set : nullptr,
                                             width.active() ? &wide : nullptr));
}

// Where a copied fix goes, for the status line: the bands, the limiter setting and the width line each have their own reader.
inline juce::String linkDestination (const themis::EqMoves& moves, const themis::LimiterMove& limiter = {}, const themis::WidthMove& width = {})
{
    juce::String to;
    if (moves.empty())
        to = limiter.active() ? "Aegis" : "";
    else
    {
        bool fill = false, eq = false;
        for (const auto& m : moves)
            (m.kind == themis::EqMove::Kind::harmonicFill ? fill : eq) = true;
        to = fill && eq ? "Iris or Apollo (EQ) and Hephaestus (fill)" : fill ? "Hephaestus" : "Iris or Apollo";
    }
    if (width.active())
        to = to.isEmpty() ? juce::String ("Castor") : to + " and Castor (width)";
    return to;
}
} // namespace gui
