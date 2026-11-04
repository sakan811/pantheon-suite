// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 The Pantheon authors

#pragma once

#include <juce_core/juce_core.h>

#include <cmath>

namespace ui
{
// The logarithmic frequency axis of every spectrum display: 20 Hz to 20 kHz mapped to 0..1 and back.
struct FreqAxis
{
    static constexpr float minHz = 20.0f, maxHz = 20000.0f;

    static float toNormalised (float hz) noexcept
    {
        return juce::jlimit (0.0f, 1.0f, std::log (hz / minHz) / std::log (maxHz / minHz));
    }

    static float toHz (float normalised) noexcept { return minHz * std::pow (maxHz / minHz, normalised); }
};
} // namespace ui
