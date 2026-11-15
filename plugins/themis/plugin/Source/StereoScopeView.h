// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 The Pantheon authors

#pragma once

#include "PluginProcessor.h"
#include "ThemisUi.h"

#include <array>
#include <functional>

// Vectorscope (goniometer) of the stereo image with a phase-correlation meter underneath.
// Vertical axis = mid, horizontal = side: mono is a vertical line, a wide mix is a fat cloud, an
// out-of-phase signal lies along the horizontal axis. The plot auto-scales to the recent peak so the shape
// stays readable at any level (click it to switch to a fixed 0 dBFS scale); correlation does not depend on level.
class StereoScopeView : public juce::Component, public juce::SettableTooltipClient
{
public:
    explicit StereoScopeView (ThemisProcessor& p) : proc (p)
    {
        setTooltip ("Vectorscope: mid (vertical) vs side (horizontal). Click to switch between auto-scale and fixed 0 dBFS scale.");
        setMouseCursor (juce::MouseCursor::PointingHandCursor);
    }

    // Pull fresh samples (message thread, ~30 Hz). Repaints only while audio is flowing or the readout changed.
    void update()
    {
        const auto& scope = proc.stereoScope();
        const bool haveNow = scope.hasData();
        const float corr = scope.correlation();
        const uint64_t seq = scope.sequence();
        if (seq == lastSequence && haveNow == haveData && std::equal_to<float>() (corr, correlation)) // exact: only skips an unchanged readout
            return;

        if (seq != lastSequence)
        {
            std::array<float, kPoints> l {}, r {};
            numPoints = scope.copyRecent (l.data(), r.data(), kPoints);
            float radius = 0.0f;
            for (int i = 0; i < numPoints; ++i)
            {
                const auto s = static_cast<size_t> (i);
                themis::StereoScope::toXY (l[s], r[s], points[s].x, points[s].y);
                radius = std::max (radius, std::hypot (points[s].x, points[s].y));
            }
            peakRadius = std::max (radius, peakRadius * kPeakRelease);
        }
        lastSequence = seq;
        haveData = haveNow;
        correlation = corr;
        repaint();
    }

    void paint (juce::Graphics& g) override
    {
        auto area = getLocalBounds().toFloat();
        ui::hw::drawModule (g, area);
        area.reduce (10.0f, 8.0f);

        ui::hw::drawLabel (g, "STEREO", area.removeFromTop (14.0f).toNearestInt());

        const auto meter = area.removeFromBottom (kMeterHeight);
        area.removeFromBottom (6.0f);
        const float side = std::max (0.0f, std::min (area.getWidth(), area.getHeight()) - 24.0f);
        const auto square = juce::Rectangle<float> (side, side).withCentre (area.getCentre());
        drawScope (g, square);
        drawCorrelation (g, meter);
    }

    void mouseDown (const juce::MouseEvent&) override
    {
        autoScale = !autoScale;
        repaint();
    }

private:
    static constexpr int kPoints = 1536;
    static constexpr float kMeterHeight = 54.0f;
    static constexpr float kPeakRelease = 0.97f;   // per update (~30 Hz): the auto scale relaxes over a few seconds
    static constexpr float kMinRadius = 1.0e-3f;   // -60 dBFS: don't blow noise up to full size
    static constexpr float kFullScaleRadius = 1.41421356f; // full-scale mono

    void drawScope (juce::Graphics& g, juce::Rectangle<float> sq) const
    {
        const auto c = sq.getCentre();
        const float radius = sq.getWidth() * 0.5f;

        g.setColour (juce::Colours::black.withAlpha (0.55f)); // the round bezel the tube sits in
        g.fillEllipse (sq.expanded (5.0f));
        g.setGradientFill (juce::ColourGradient (colours::bezelLit, sq.getX(), sq.getY(), colours::bezelShade, sq.getRight(), sq.getBottom(), false));
        g.fillEllipse (sq.expanded (4.0f));
        g.setGradientFill (juce::ColourGradient (colours::glass.interpolatedWith (colours::accent, 0.07f), c.x, c.y, colours::glass.darker (0.4f), sq.getX(), sq.getY(), true));
        g.fillEllipse (sq);
        g.setColour (colours::glassGrid.brighter (0.25f));
        g.drawEllipse (sq, 1.0f);
        g.drawEllipse (sq.reduced (radius * 0.5f), 1.0f); // half scale
        g.drawLine (c.x - radius, c.y, c.x + radius, c.y, 1.0f);
        g.drawLine (c.x, c.y - radius, c.x, c.y + radius, 1.0f);
        const float d = radius * 0.7071f;
        g.drawLine (c.x - d, c.y - d, c.x + d, c.y + d, 1.0f);
        g.drawLine (c.x - d, c.y + d, c.x + d, c.y - d, 1.0f);

        g.setColour (colours::dim);
        g.setFont (ui::labelFont (10.0f));
        const auto tag = [&] (const char* text, float x, float y) {
            g.drawText (text, juce::Rectangle<float> (14.0f, 12.0f).withCentre ({ x, y }).toNearestInt(), juce::Justification::centred);
        };
        tag ("M", c.x, sq.getY() - 11.0f);
        tag ("L", c.x - d - 8.0f, c.y - d - 8.0f);
        tag ("R", c.x + d + 8.0f, c.y - d - 8.0f);
        tag ("S", sq.getRight() + 11.0f, c.y);
        g.setFont (ui::lcdFont (10.0f));
        g.setColour (colours::amber.withAlpha (0.85f));
        g.drawText (autoScale ? "AUTO" : "0 dBFS", juce::Rectangle<int> (juce::roundToInt (sq.getX()), juce::roundToInt (sq.getBottom()) - 13,
                                                                     juce::roundToInt (sq.getWidth()), 12),
                    juce::Justification::centred);

        if (haveData && numPoints > 0)
            drawTrace (g, c, radius);

        juce::Path tube;
        tube.addEllipse (sq);
        g.saveState();
        g.reduceClipRegion (tube);
        g.setGradientFill (juce::ColourGradient (juce::Colours::black.withAlpha (0.0f), c.x, c.y, juce::Colours::black.withAlpha (0.4f), sq.getX(), sq.getY(), true));
        g.fillEllipse (sq);
        g.setGradientFill (juce::ColourGradient (juce::Colours::white.withAlpha (0.06f), sq.getX(), sq.getY(), juce::Colours::white.withAlpha (0.0f), c.x, c.y, false));
        g.fillEllipse (sq);
        g.restoreState();
    }

    // The beam: plot units -> pixels, with auto-scale putting the recent peak at the circle's edge and the fixed scale putting
    // full-scale mono there. Older samples are fainter, and every dot has a glow round it, like a phosphor trace.
    void drawTrace (juce::Graphics& g, juce::Point<float> c, float radius) const
    {
        const float reference = autoScale ? std::max (peakRadius, kMinRadius) : kFullScaleRadius;
        const float k = radius * 0.94f / reference;
        constexpr int kBuckets = 4;
        for (int pass = 0; pass < 2; ++pass)
        {
            const float size = pass == 0 ? 4.2f : 1.6f;
            for (int bucket = 0; bucket < kBuckets; ++bucket)
            {
                const float age = static_cast<float> (bucket) / (kBuckets - 1);
                g.setColour (colours::accent.withAlpha (pass == 0 ? 0.025f + 0.045f * age : 0.14f + 0.66f * age));
                const int from = numPoints * bucket / kBuckets, to = numPoints * (bucket + 1) / kBuckets;
                for (int i = from; i < to; ++i)
                {
                    const auto& p = points[static_cast<size_t> (i)];
                    const float px = juce::jlimit (-radius, radius, p.x * k), py = juce::jlimit (-radius, radius, p.y * k);
                    g.fillRect (c.x + px - size * 0.5f, c.y - py - size * 0.5f, size, size);
                }
            }
        }
    }

    // -1 ... +1 on a swing-needle panel meter: right = mono-compatible, left = phase problems. The pivot sits far below the glass,
    // so only the top of the arc shows.
    void drawCorrelation (juce::Graphics& g, juce::Rectangle<float> area) const
    {
        constexpr float kWarnAnti = 0.0f, kBadAnti = 0.3f; // anti-correlation (-correlation) beyond which the verdict worsens
        constexpr float kSweep = 0.62f;                    // radians either side of centre
        const auto dial = area.reduced (2.0f, 0.0f);
        ui::hw::drawGlass (g, dial, 4.0f, colours::amber);

        const float radius = dial.getWidth() * 0.62f;
        const juce::Point<float> pivot (dial.getCentreX(), dial.getY() + 12.0f + radius);
        const auto at = [&] (float value, float r) { return pivot + juce::Point<float> (std::sin (value * kSweep), -std::cos (value * kSweep)) * r; };

        g.saveState();
        g.reduceClipRegion (dial.toNearestInt());
        const auto zone = [&] (float from, float to, themis::Level level) {
            juce::Path arc;
            arc.addCentredArc (pivot.x, pivot.y, radius - 3.0f, radius - 3.0f, 0.0f, from * kSweep, to * kSweep, true);
            g.setColour (colours::forLevel (level).withAlpha (0.55f));
            g.strokePath (arc, juce::PathStrokeType (3.0f));
        };
        zone (-1.0f, -kBadAnti, themis::Level::bad);
        zone (-kBadAnti, kWarnAnti, themis::Level::warn);
        zone (kWarnAnti, 1.0f, themis::Level::good);

        g.setFont (ui::lcdFont (9.5f));
        for (int i = -4; i <= 4; ++i)
        {
            const float v = static_cast<float> (i) * 0.25f;
            const bool major = i % 2 == 0;
            g.setColour (colours::dim.withAlpha (major ? 0.9f : 0.5f));
            g.drawLine ({ at (v, radius - 7.0f), at (v, radius - (major ? 12.0f : 10.0f)) }, 1.0f);
            if (major)
            {
                const auto p = at (v, radius - 20.0f);
                g.drawText (i == 0 ? "0" : i == 4 ? "+1" : i == 2 ? ".5" : i == -4 ? "-1" : "-.5",
                            juce::Rectangle<float> (24.0f, 11.0f).withCentre (p).toNearestInt(), juce::Justification::centred);
            }
        }
        g.setColour (colours::dim);
        g.setFont (ui::labelFont (8.5f));
        g.drawText ("CORRELATION", dial.withTrimmedTop (dial.getHeight() - 14.0f).toNearestInt(), juce::Justification::centred);

        if (haveData)
        {
            const auto level = themis::translation::classify (-correlation, kWarnAnti, kBadAnti);
            const auto tip = at (juce::jlimit (-1.0f, 1.0f, correlation), radius - 4.0f);
            const auto base = at (juce::jlimit (-1.0f, 1.0f, correlation), radius - 30.0f);
            g.setColour (juce::Colours::black.withAlpha (0.6f)); // the needle's shadow
            g.drawLine ({ base + juce::Point<float> (1.5f, 1.5f), tip + juce::Point<float> (1.5f, 1.5f) }, 2.0f);
            g.setColour (colours::text);
            g.drawLine ({ base, tip }, 1.6f);
            g.setFont (ui::lcdFont (11.0f, juce::Font::bold));
            g.setColour (colours::forLevel (level));
            g.drawText (juce::String (correlation, 2), dial.withTrimmedTop (dial.getHeight() - 14.0f).withTrimmedRight (8.0f).toNearestInt(), juce::Justification::centredRight);
        }
        g.restoreState();
        ui::hw::drawGlassOverlay (g, dial, 4.0f, false);
    }

    ThemisProcessor& proc;
    std::array<juce::Point<float>, kPoints> points {};
    int numPoints = 0;
    uint64_t lastSequence = 0;
    bool haveData = false;
    bool autoScale = true;
    float correlation = 0.0f;
    float peakRadius = 0.0f;
};
