// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 The Pantheon authors

#pragma once

#include "Hardware.h"
#include "Note.h"

namespace ui
{
// The notes of a panel: a lamp-coloured dot (amber for a warning, dim for information) and one line each, the first `maxLines`
// that fit `area`. Draw them where the panel has room under its controls.
inline void drawNotes (juce::Graphics& g, juce::Rectangle<int> area, const std::vector<Note>& notes, int maxLines = 3)
{
    constexpr int lineHeight = 14;
    int y = area.getY(), shown = 0;
    g.setFont (juce::FontOptions (11.0f));
    for (const auto& note : notes)
    {
        if (shown++ == maxLines)
            break;
        g.setColour (note.severity == Severity::warning ? colours::warn : colours::dim);
        g.fillEllipse ((float) area.getX(), (float) y + 4.0f, 5.0f, 5.0f);
        g.drawFittedText (juce::String (note.text), area.getX() + 12, y, area.getWidth() - 12, lineHeight,
                          juce::Justification::centredLeft, 1, 0.85f);
        y += lineHeight;
    }
}

// A window of glass along the foot of an editor for plugins whose panels are too narrow for a line of notes: up to two notes, or,
// when there is nothing to say, a reminder that GUIDE says what the plugin is for.
inline constexpr int kNoteStripHeight = 36;

inline void drawNoteStrip (juce::Graphics& g, juce::Rectangle<int> area, const std::vector<Note>& notes)
{
    hw::drawGlass (g, area.toFloat(), 3.0f, colours::amber);
    const auto inside = area.reduced (14, 4);
    if (notes.empty())
    {
        g.setColour (colours::dim);
        g.setFont (juce::FontOptions (11.0f));
        g.drawText ("Nothing to warn about. GUIDE says what this plugin is for, and what it is not for.", inside, juce::Justification::centredLeft, false);
    }
    else
        drawNotes (g, inside, notes, 2);
    hw::drawGlassOverlay (g, area.toFloat(), 3.0f, false);
}

// The GUIDE button: says what the plugin is for and, as much, what it is not for (the plugin's own text), in a message box.
class GuideButton : public juce::TextButton
{
public:
    GuideButton (juce::String title, juce::String text) : juce::TextButton ("GUIDE")
    {
        setTooltip ("What " + title + " is for, and what it is not for.");
        onClick = [title, text] { juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::InfoIcon, title, text); };
    }
};
}
