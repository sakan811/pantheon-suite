// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 The Pantheon authors

#pragma once

#include <pantheon/ui/Controls.h>
#include <pantheon/ui/Hardware.h>
#include <pantheon/ui/HardwareLookAndFeel.h>

// What the shared hardware kit (shared/ui) cannot know about Selene Poly: its nameplate words and display scales/colours.
namespace selene::gui
{
inline constexpr const char* kProjectWord = "Selene";
inline constexpr const char* kPluginWord = "Poly";

inline constexpr float kFilterMinHz = 20.0f;
inline constexpr float kFilterMaxHz = 20000.0f;
inline constexpr float kFilterFloorDb = -36.0f;
inline constexpr float kFilterCeilDb = 18.0f;

inline const juce::Colour kFilterCurve { 0xff37e3c5 };   // teal phosphor filter curve
inline const juce::Colour kScopeTrace { 0xffffb347 };    // warm amber scope trace
inline const juce::Colour kChorusGlow { 0xffb388ff };    // twilight City Pop violet accent
} // namespace selene::gui
