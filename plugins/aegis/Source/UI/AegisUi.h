// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 The Pantheon authors

#pragma once

#include <pantheon/ui/Controls.h>
#include <pantheon/ui/Hardware.h>
#include <pantheon/ui/HardwareLookAndFeel.h>

// What the shared hardware kit (shared/ui) cannot know about Aegis: its two words and the scale of its displays.
namespace aegis::gui
{
// The full name ("Aegis Ceiling") is set once, in CMakeLists.txt (PLUGIN_NAME), and reaches the code as JucePlugin_Name;
// UiCheck checks that the two agree.
inline constexpr const char* kProjectWord = "Aegis";
inline constexpr const char* kPluginWord = "Ceiling";

inline constexpr float kReductionRangeDb = 18.0f; // the depth of the reduction display
inline constexpr float kLevelFloorDb = -36.0f;    // the bottom of the level bars
} // namespace aegis::gui
