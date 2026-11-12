// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 The Pantheon authors

#pragma once

#include <pantheon/ui/Controls.h>
#include <pantheon/ui/Hardware.h>
#include <pantheon/ui/HardwareLookAndFeel.h>

// What the shared hardware kit (shared/ui) cannot know about Dionysus: its two words and the scale of its displays.
namespace dionysus::gui
{
// The full name ("Dionysus Warmth") is set once, in CMakeLists.txt (PLUGIN_NAME), and reaches the code as JucePlugin_Name;
// UiCheck checks that the two agree.
inline constexpr const char* kProjectWord = "Dionysus";
inline constexpr const char* kPluginWord = "Warmth";

inline constexpr float kHarmonicFloorDb = -60.0f; // the bottom of the harmonic bars
} // namespace dionysus::gui
