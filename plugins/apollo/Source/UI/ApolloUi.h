// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 The Pantheon authors

#pragma once

#include <pantheon/ui/Controls.h>
#include <pantheon/ui/Hardware.h>
#include <pantheon/ui/HardwareLookAndFeel.h>

// What the shared hardware kit (shared/ui) cannot know about Apollo: its two words, the scale of its curve and the colour of each band.
namespace apollo::gui
{
// The full name ("Apollo Curve") is set once, in CMakeLists.txt (PLUGIN_NAME), and reaches the code as JucePlugin_Name;
// UiCheck checks that the two agree.
inline constexpr const char* kProjectWord = "Apollo";
inline constexpr const char* kPluginWord = "Curve";

inline constexpr float kCurveRangeDb = 18.0f; // the display shows -18 to +18 dB

// A band keeps its colour: the number on its strip, its node and its own curve.
inline juce::Colour bandColour (int band) { return juce::Colour::fromHSV (std::fmod (0.08f + 0.125f * (float) band, 1.0f), 0.6f, 0.95f, 1.0f); }
} // namespace apollo::gui
