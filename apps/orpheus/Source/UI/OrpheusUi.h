// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 The Pantheon authors

#pragma once

#include "../Dsp/Scales.h"

#include <pantheon/ui/Controls.h>
#include <pantheon/ui/Hardware.h>
#include <pantheon/ui/HardwareLookAndFeel.h>
#include <pantheon/ui/ValueKnob.h>

// What the shared hardware kit (shared/ui) cannot know about Orpheus: its words, how a pitch is written, and which colour says a
// note is in tune. The drawing of panels, glass, lamps, knobs and buttons is the kit's (namespace ui).
namespace orpheus::gui
{
inline constexpr const char* kProjectWord = "Orpheus";
inline constexpr const char* kSecondWord = "Pitch Editor";

// "A3", "C#4" and the distance in cents from that note: "A3 +12ct". `grid` is the song's reference offset in semitones (Key::gridOffset()).
inline juce::String noteText (double midi, double grid = 0.0, bool withCents = true)
{
    const int note = (int) std::lround (midi - grid);
    const double cents = (midi - grid - note) * 100.0;
    juce::String s (orpheus::noteName (note));
    if (withCents)
        s << " " << (std::lround (cents) >= 0 ? "+" : "-") << juce::String (std::abs ((int) std::lround (cents))).paddedLeft ('0', 2) << "ct";
    return s;
}

// A number with a fixed count of decimals (juce::String (x, 0) means "as many as it takes", not none).
inline juce::String numberText (double v, int decimals = 0)
{
    return decimals <= 0 ? juce::String (juce::roundToInt (v)) : juce::String (v, decimals);
}

// The width of a line of text in a font.
inline int textWidth (const juce::Font& font, const juce::String& text)
{
    juce::GlyphArrangement glyphs;
    glyphs.addLineOfText (font, text, 0.0f, 0.0f);
    return (int) std::ceil (glyphs.getBoundingBox (0, -1, true).getWidth());
}

// "1:03.215"
inline juce::String timeText (double seconds)
{
    seconds = std::max (0.0, seconds);
    const int minutes = (int) (seconds / 60.0);
    return juce::String (minutes) + ":" + juce::String (seconds - minutes * 60.0, 3).paddedLeft ('0', 6);
}

// How far a note is from being in tune, as a lamp colour: green when it sits on the scale to within a few cents, amber when it is
// audibly off, red when it is between two notes.
inline juce::Colour verdictColour (double centsOff)
{
    const double c = std::fabs (centsOff);
    if (c < 8.0)
        return colours::good;
    if (c < 25.0)
        return colours::warn;
    return colours::bad;
}
} // namespace orpheus::gui
