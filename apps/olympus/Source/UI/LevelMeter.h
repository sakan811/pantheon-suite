// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 The Pantheon authors
#pragma once

#include <algorithm>
#include <cmath>

#include <pantheon/ui/MeterCell.h>

// The level meter of a track and of the master: the same ballistics and the same colours, drawn with the kit's meter cell.
namespace olympus::gui
{
// The level of a linear peak in dBFS; -100 below -100 dB (the silence of a meter).
inline double dbOf (double linear) { return linear > 1.0e-5 ? 20.0 * std::log10 (linear) : -100.0; }

// The ballistics of one meter bar: the level falls back by 3 dB between reads, the held mark more slowly (0.4 dB between reads).
struct MeterBallistics
{
    float level = -100.0f, hold = -100.0f;
    void add (float db)
    {
        level = std::max (db, level - 3.0f);
        hold = std::max (db, hold - 0.4f);
    }
};

// Paints a meter bar across `bar` (-60 to 0 dBFS): the level in green, amber above -12 dB, red above -3 dB, and the held mark.
inline void paintLevelBar (juce::Graphics& g, const juce::Rectangle<float>& bar, const MeterBallistics& meter)
{
    const auto x = [&bar] (float db) { return bar.getX() + bar.getWidth() * juce::jlimit (0.0f, 1.0f, (db + 60.0f) / 60.0f); };
    const juce::Colour colour = meter.level > -3.0f ? colours::bad : meter.level > -12.0f ? colours::warn : colours::good;
    ui::MeterCell::fillSpan (g, bar, bar.getX(), x (meter.level), colour);
    if (meter.hold > -60.0f) ui::MeterCell::drawMarker (g, bar, x (meter.hold), colour);
}
} // namespace olympus::gui
