// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 The Pantheon authors

#pragma once

#include <pantheon/ui/Controls.h>
#include <pantheon/ui/Hardware.h>
#include <pantheon/ui/HardwareLookAndFeel.h>

// What the shared hardware kit (shared/ui) cannot know about Hestia: its two words, the scale of its cut display and the colour of each stem.
namespace hestia::gui
{
// The full name ("Hestia Space") is set once, in CMakeLists.txt (PLUGIN_NAME), and reaches the code as JucePlugin_Name;
// UiCheck checks that the two agree.
inline constexpr const char* kProjectWord = "Hestia";
inline constexpr const char* kPluginWord = "Space";

inline constexpr float kSpectraFloorDb = -90.0f, kSpectraCeilingDb = 0.0f; // the spectra
inline constexpr float kCutRangeDb = 18.0f;                  // the bottom of the cut pane (the largest CARVE)

// The colour of another stem, by the slot it holds on the bus: the same stem keeps its colour while it plays.
inline juce::Colour stemColour (int slot) { return juce::Colour::fromHSV (std::fmod (0.58f + 0.137f * (float) slot, 1.0f), 0.55f, 0.95f, 1.0f); }
} // namespace hestia::gui
