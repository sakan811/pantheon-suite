// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 The Pantheon authors

#pragma once

#include "Hardware.h"

#include <vector>

namespace ui
{
// A spectrogram on glass: frequency across (whatever grid the caller's columns are on), time running down, the newest
// moment at the bottom. The caller pushes one row of levels per display tick and draws it into any rectangle; what the
// levels mean (which analyzer, which grid) is the caller's. The image is a ring of rows, so a push costs one row.
class Spectrogram
{
public:
    Spectrogram (int columns, int rows) : image (juce::Image::RGB, columns, rows, true) { clear(); }

    int columns() const noexcept { return image.getWidth(); }
    int rows() const noexcept { return image.getHeight(); }

    void clear()
    {
        image.clear (image.getBounds(), colourFor (0.0f));
        next = 0;
    }

    // One moment: `levels` holds columns() values; `floor` and `ceiling` are the levels drawn as dark glass and as full light.
    void push (const float* levels, float floor, float ceiling)
    {
        juce::Image::BitmapData data (image, 0, next, image.getWidth(), 1, juce::Image::BitmapData::writeOnly);
        for (int x = 0; x < image.getWidth(); ++x)
            data.setPixelColour (x, 0, colourFor ((levels[x] - floor) / (ceiling - floor)));
        next = (next + 1) % image.getHeight();
    }

    void draw (juce::Graphics& g, juce::Rectangle<float> area) const
    {
        // The ring starts at `next` (the oldest row): that part goes on top, the rest under it.
        const int h = image.getHeight(), w = image.getWidth(), older = h - next;
        const float split = area.getHeight() * (float) older / (float) h;
        g.setImageResamplingQuality (juce::Graphics::mediumResamplingQuality);
        const auto piece = [&] (int fromRow, int numRows, float y, float height)
        {
            if (numRows > 0)
                g.drawImage (image, juce::roundToInt (area.getX()), juce::roundToInt (y), juce::roundToInt (area.getWidth()), juce::roundToInt (height) + 1,
                             0, fromRow, w, numRows);
        };
        juce::Graphics::ScopedSaveState state (g);
        g.reduceClipRegion (area.toNearestInt());
        piece (next, older, area.getY(), split);
        piece (0, next, area.getY() + split, area.getHeight() - split);
    }

    // Dark glass, through the phosphor colour, to amber and a near-white for the loudest.
    static juce::Colour colourFor (float t) noexcept
    {
        t = juce::jlimit (0.0f, 1.0f, t);
        // Most of a programme sits in the middle of the range: that part stays dark, so what is loud is what lights up.
        struct Stop { float at; juce::Colour colour; };
        const Stop stops[] = { { 0.0f, colours::glass }, { 0.35f, colours::accent.withMultipliedBrightness (0.22f) }, { 0.6f, colours::accent.withMultipliedBrightness (0.55f) },
                               { 0.78f, colours::accent }, { 0.92f, colours::amber }, { 1.0f, colours::text } };
        int i = 0;
        while (i < 4 && t > stops[i + 1].at)
            ++i;
        return stops[i].colour.interpolatedWith (stops[i + 1].colour, (t - stops[i].at) / (stops[i + 1].at - stops[i].at));
    }

private:
    juce::Image image;
    int next = 0;
};
} // namespace ui
