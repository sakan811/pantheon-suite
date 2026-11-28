// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 The Pantheon authors
#pragma once

#include <pantheon/ui/Hardware.h>

#include <array>

namespace olympus::gui
{
// The twelve colours a track or a group can take from the menu. Five are the shared kit's own colours; the rest are this app's, chosen to
// stay apart from one another on the dark chassis. The order is the automatic palette: a track with no colour of its own takes entry
// (its index % 12), so a new track never looks like its neighbour.
struct TrackPaletteEntry
{
    const char* name;
    juce::uint32 argb;
};

inline std::array<TrackPaletteEntry, 12> trackPalette()
{
    return { {
        { "Teal", colours::accent.getARGB() },
        { "Amber", colours::amber.getARGB() },
        { "Green", colours::good.getARGB() },
        { "Orange", colours::warn.getARGB() },
        { "Red", colours::bad.getARGB() },
        { "Blue", 0xff6fa8ffu },
        { "Violet", 0xffb48cffu },
        { "Pink", 0xffff7eb6u },
        { "Lime", 0xffd6e36bu },
        { "Sky", 0xff5ec8e8u },
        { "Tan", 0xffc9a27eu },
        { "Grey", colours::dim.getARGB() },
    } };
}

// The colour a track is painted in: its own colour, or the palette's entry for its index when it is automatic (0).
inline juce::Colour trackColour (juce::uint32 argb, int index)
{
    if (argb != 0)
        return juce::Colour (argb);
    const auto palette = trackPalette();
    return juce::Colour (palette[(size_t) juce::jmax (0, index) % palette.size()].argb);
}
} // namespace olympus::gui
