// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 The Pantheon authors

#pragma once

#include <pantheon/ui/Controls.h>
#include <pantheon/ui/Hardware.h>
#include <pantheon/ui/HardwareLookAndFeel.h>

// What the shared hardware kit (shared/ui) cannot know about Hera: its two words and the scale of its displays.
namespace hera::gui
{
// The full name ("Hera Bond") is set once, in CMakeLists.txt (PLUGIN_NAME), and reaches the code as JucePlugin_Name;
// UiCheck checks that the two agree.
inline constexpr const char* kProjectWord = "Hera";
inline constexpr const char* kPluginWord = "Bond";

inline constexpr float kReductionRangeDb = 24.0f;            // the depth of the reduction history
inline constexpr float kCurveMinDb = -60.0f, kCurveMaxDb = 6.0f; // the transfer curve's input and output range
} // namespace hera::gui
