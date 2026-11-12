// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 The Pantheon authors

#pragma once

#include <pantheon/ui/Controls.h>
#include <pantheon/ui/Hardware.h>
#include <pantheon/ui/HardwareLookAndFeel.h>

// What the shared hardware kit (shared/ui) cannot know about Nemesis: its two words and the scale of its displays.
namespace nemesis::gui
{
// The full name ("Nemesis Hush") is set once, in CMakeLists.txt (PLUGIN_NAME), and reaches the code as JucePlugin_Name;
// UiCheck checks that the two agree.
inline constexpr const char* kProjectWord = "Nemesis";
inline constexpr const char* kPluginWord = "Hush";

inline constexpr float kReductionRangeDb = 24.0f; // the depth of the reduction history
inline constexpr float kBandFloorDb = -36.0f;     // the bottom of the band's response
} // namespace nemesis::gui
