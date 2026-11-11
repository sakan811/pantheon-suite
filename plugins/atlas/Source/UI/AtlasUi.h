// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 The Pantheon authors

#pragma once

#include <pantheon/ui/Controls.h>
#include <pantheon/ui/Hardware.h>
#include <pantheon/ui/HardwareLookAndFeel.h>

// What the shared hardware kit (shared/ui) cannot know about Atlas: its two words and the scale of its reduction meters.
namespace atlas::gui
{
// The full name ("Atlas Span") is set once, in CMakeLists.txt (PLUGIN_NAME), and reaches the code as JucePlugin_Name;
// UiCheck checks that the two agree.
inline constexpr const char* kProjectWord = "Atlas";
inline constexpr const char* kPluginWord = "Span";

inline constexpr float kMeterRangeDb = 24.0f; // a full reduction meter
} // namespace atlas::gui
