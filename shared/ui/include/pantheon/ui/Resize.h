// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 The Pantheon authors

#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include "Aspect.h"
#include "Hardware.h"

#include <algorithm>
#include <cmath>

namespace ui
{
// The four corners of an editor as handles: drag any of them and the window grows or shrinks with its proportions kept. The window cannot move (the host owns where it is),
// so the two left corners grow it when pulled left, the two top corners when pulled up, and the bottom right when pulled right or down, by the longer of the two moves.
// The bottom right corner stays under the pointer; for the others the window grows from its fixed top left, so the handle you hold does not follow the pointer (it
// stays at the window's edge) and the pointer ends up outside the window: that is the price of a window the plugin cannot move. Double-clicking a corner brings the window back to its opening size. The host's own edge drag still works (the constrainer
// keeps the same ratio and limits). It sits over the editor and answers the mouse only in the corners; the editor calls `setBounds (getLocalBounds())` on it in `resized()`.
//
//   resizeGrips (*this, minWidth, openWidth, maxWidth) ... addAndMakeVisible (resizeGrips);   // after every other child, so it is on top
class ResizeGrips : public juce::Component
{
public:
    static constexpr int kGripSize = 16;

    ResizeGrips (juce::AudioProcessorEditor& editorToResize, int minWidth, int openWidth, int maxWidth)
        : editor (editorToResize), minimumWidth (minWidth), openingWidth (openWidth), maximumWidth (maxWidth)
    {
        setInterceptsMouseClicks (true, false);
        setRepaintsOnMouseActivity (false);
        editor.setResizable (true, false); // the host may resize it; the corner handle is this one, not JUCE's (bottom right only)
        editor.setResizeLimits (minWidth, heightFor (minWidth), maxWidth, heightFor (maxWidth));
        if (auto* limits = editor.getConstrainer())
            limits->setFixedAspectRatio (kEditorAspect);
    }

    // The corner under a point, or -1: 0 top left, 1 top right, 2 bottom left, 3 bottom right.
    static int cornerAt (juce::Point<int> p, int width, int height, int size = kGripSize)
    {
        const bool left = p.x < size, right = p.x >= width - size, top = p.y < size, bottom = p.y >= height - size;
        if (top && left) return 0;
        if (top && right) return 1;
        if (bottom && left) return 2;
        if (bottom && right) return 3;
        return -1;
    }

    bool hitTest (int x, int y) override { return cornerAt ({ x, y }, getWidth(), getHeight()) >= 0; }

    void paint (juce::Graphics& g) override
    {
        for (int corner = 0; corner < 4; ++corner)
        {
            const bool hot = corner == hovered || corner == dragged;
            g.setColour (colours::dim.withAlpha (hot ? 0.95f : 0.4f));
            const float w = (float) getWidth(), h = (float) getHeight(), s = (float) kGripSize - 3.0f, m = 3.0f; // three short diagonal strokes, as on a hardware corner
            const float cx = (corner & 1) ? w - m : m, cy = (corner & 2) ? h - m : m;
            const float sx = (corner & 1) ? -1.0f : 1.0f, sy = (corner & 2) ? -1.0f : 1.0f;
            for (int i = 0; i < 3; ++i)
            {
                const float k = s * (0.4f + 0.3f * (float) i);
                g.drawLine (cx + sx * k, cy, cx, cy + sy * k, 1.2f);
            }
        }
    }

    juce::MouseCursor getMouseCursor() override
    {
        switch (cornerAt (getMouseXYRelative(), getWidth(), getHeight()))
        {
            case 0: return juce::MouseCursor::TopLeftCornerResizeCursor;
            case 1: return juce::MouseCursor::TopRightCornerResizeCursor;
            case 2: return juce::MouseCursor::BottomLeftCornerResizeCursor;
            case 3: return juce::MouseCursor::BottomRightCornerResizeCursor;
            default: return juce::MouseCursor::NormalCursor;
        }
    }

    void mouseMove (const juce::MouseEvent& e) override { hover (cornerAt (e.getPosition(), getWidth(), getHeight())); }
    void mouseExit (const juce::MouseEvent&) override { hover (-1); }

    void mouseDown (const juce::MouseEvent& e) override
    {
        dragged = cornerAt (e.getPosition(), getWidth(), getHeight());
        startWidth = editor.getWidth();
        repaint();
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (dragged < 0)
            return;
        const auto moved = e.getScreenPosition() - e.getMouseDownScreenPosition(); // the screen: the window moves under a corner as it is resized
        const double dx = (dragged & 1) ? moved.x : -moved.x, dy = (dragged & 2) ? moved.y : -moved.y;
        const double byWidth = dx, byHeight = dy * kEditorAspect;
        resizeTo (startWidth + (int) std::lround (std::abs (byWidth) >= std::abs (byHeight) ? byWidth : byHeight));
    }

    void mouseUp (const juce::MouseEvent&) override
    {
        dragged = -1;
        repaint();
    }

    void mouseDoubleClick (const juce::MouseEvent&) override { resizeTo (openingWidth); }

    // The size a width gives, within the limits (also what the tests ask).
    int clampedWidth (int width) const { return std::clamp (width, minimumWidth, maximumWidth); }
    static int heightFor (int width) { return (int) std::lround ((double) width / kEditorAspect); }

private:
    void resizeTo (int width)
    {
        const int w = clampedWidth (width);
        editor.setSize (w, heightFor (w));
    }

    void hover (int corner)
    {
        if (corner != hovered)
        {
            hovered = corner;
            repaint();
        }
    }

    juce::AudioProcessorEditor& editor;
    int minimumWidth, openingWidth, maximumWidth;
    int hovered = -1, dragged = -1, startWidth = 0;
};
} // namespace ui
