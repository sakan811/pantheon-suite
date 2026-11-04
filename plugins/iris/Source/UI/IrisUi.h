// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 The Pantheon authors

#pragma once

#include "../DSP/Band.h"

#include <pantheon/ui/Controls.h>
#include <pantheon/ui/Hardware.h>
#include <pantheon/ui/HardwareLookAndFeel.h>

// What the shared hardware kit (shared/ui) cannot know about Iris: its two words, and which colour a band placement is lit in.
// The drawing itself (panels, glass, lamps, knobs, buttons) is the kit's (namespace ui); Iris' own view code is in iris::gui.
namespace iris::gui
{
// The two words of the plugin's name. The full name ("Iris Prism") is set once, in CMakeLists.txt (PLUGIN_NAME), and
// reaches the code as JucePlugin_Name; UiCheck checks that the two agree.
inline constexpr const char* kProjectWord = "Iris";
inline constexpr const char* kPluginWord = "Prism";

// A band on both channels is amber (the colour of a digital read-out); one channel or one half of the image has its own colour,
// so the curve and the node of a Mid band can be told from a Side band at a glance.
inline juce::Colour forPlacement (Placement place)
{
    switch (place)
    {
        case Placement::stereo: return colours::amber;
        case Placement::left:   return juce::Colour (0xff7fb2ff);
        case Placement::right:  return juce::Colour (0xffff8fa3);
        case Placement::mid:    return colours::accent;
        case Placement::side:   return juce::Colour (0xffb48cff);
    }
    return colours::amber;
}

// One letter for the node: S(tereo) is the default and goes without.
inline const char* placementLetter (Placement place) noexcept
{
    switch (place)
    {
        case Placement::stereo: return "";
        case Placement::left:   return "L";
        case Placement::right:  return "R";
        case Placement::mid:    return "M";
        case Placement::side:   return "S";
    }
    return "";
}
} // namespace iris::gui
