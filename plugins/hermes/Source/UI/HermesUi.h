// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 The Pantheon authors

#pragma once

#include <pantheon/ui/Controls.h>
#include <pantheon/ui/Hardware.h>
#include <pantheon/ui/HardwareLookAndFeel.h>

// What the shared hardware kit (shared/ui) cannot know about Hermes: its two words and the scales of its meters.
namespace hermes::gui
{
// The full name ("Hermes Level") is set once, in CMakeLists.txt (PLUGIN_NAME), and reaches the code as JucePlugin_Name;
// UiCheck checks that the two agree.
inline constexpr const char* kProjectWord = "Hermes";
inline constexpr const char* kPluginWord = "Level";

inline constexpr float kGraphTopLufs = -5.0f, kGraphBottomLufs = -50.0f; // the loudness graph
inline constexpr float kNothing = -150.0f;                                // a reading under this is "no reading yet"
} // namespace hermes::gui
