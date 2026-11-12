// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 The Pantheon authors

#pragma once

#include "../PluginProcessor.h"
#include "HestiaUi.h"
#include <pantheon/ui/FreqAxis.h>

namespace hestia::gui
{
// The display. Above, the level of this stem (filled, teal) and of each other stem of the mix (a line in its own colour) in the thirty bands; under
// it, a strip that says who owns each band (the colour of the stem with the biggest claim there, brighter the bigger the claim); under that, the
// cut this stem is making in each band, in amber, hanging from 0 dB. It is what the engine reads and decides, not another analysis: the levels are
// the ones the stems publish to each other. Everything here runs on the editor's timer; nothing on the audio thread.
class SpaceView : public juce::Component, public juce::SettableTooltipClient
{
public:
    explicit SpaceView (HestiaProcessor& p) : proc (p)
    {
        setTooltip ("The space of the mix, low to high across.\nTop: this stem (teal) and the other stems of the mix, each in its colour: how loud each is in each third-octave band.\n"
                    "The strip: who owns each band, by the stem with the biggest claim there.\nBottom: the cut this stem makes, in amber: it gives up a band only where a stem with a bigger "
                    "claim is playing.");
    }

    float xForHz (double hz) const noexcept { return plot().getX() + ui::FreqAxis::toNormalised ((float) hz) * plot().getWidth(); }

    // From the editor's timer.
    void tick()
    {
        const auto params = readParams (proc.apvts);
        for (int k = 0; k < kBands; ++k)
        {
            own[(size_t) k] = proc.engine.ownLevel[(size_t) k].load (std::memory_order_relaxed);
            cut[(size_t) k] = proc.engine.cutShown[(size_t) k].load (std::memory_order_relaxed);
        }
        role = params.role;
        importance = params.importance;
        group = params.group;
        depth = params.depthDb;
        numOthers = proc.engine.slotIndex() >= 0 ? proc.engine.busRef().gather (proc.engine.slotIndex(), params.group, (std::uint32_t) juce::Time::getMillisecondCounter(), others.data()) : 0;
        repaint();
    }

    int otherStems() const noexcept { return numOthers; }
    const std::array<Stem, kMaxStems>& stems() const noexcept { return others; }
    float strongestCutDb() const noexcept { return *std::min_element (cut.begin(), cut.end()); }

    void paint (juce::Graphics& g) override
    {
        namespace hw = ui::hw;
        const auto frame = getLocalBounds().toFloat();
        hw::drawGlass (g, frame, 4.0f, colours::accent);
        const auto area = plot();
        const float spectraBottom = area.getY() + area.getHeight() * 0.62f;
        const auto spectra = juce::Rectangle<float> (area.getX(), area.getY(), area.getWidth(), spectraBottom - area.getY());
        const auto strip = juce::Rectangle<float> (area.getX(), spectraBottom + 4.0f, area.getWidth(), 10.0f);
        const auto cutPane = juce::Rectangle<float> (area.getX(), strip.getBottom() + 6.0f, area.getWidth(), area.getBottom() - strip.getBottom() - 6.0f);

        // The graticule: decades and the dB of the spectra.
        g.setFont (juce::FontOptions (9.5f));
        for (double hz : { 50.0, 100.0, 200.0, 500.0, 1000.0, 2000.0, 5000.0, 10000.0 })
        {
            const float x = xForHz (hz);
            g.setColour (colours::glassGrid);
            g.drawVerticalLine ((int) x, area.getY(), area.getBottom());
            if (hz == 100.0 || hz == 1000.0 || hz == 10000.0 || hz == 50.0 || hz == 500.0 || hz == 5000.0)
            {
                g.setColour (colours::dim);
                g.drawText (hz < 1000.0 ? juce::String ((int) hz) : juce::String ((int) (hz / 1000.0)) + "k", juce::Rectangle<float> (x - 20.0f, area.getBottom() + 2.0f, 40.0f, 12.0f), juce::Justification::centred);
            }
        }
        for (float db : { -60.0f, -30.0f, 0.0f })
        {
            const float y = yForDb (db, spectra);
            g.setColour (colours::glassGrid);
            g.drawHorizontalLine ((int) y, area.getX(), area.getRight());
            g.setColour (colours::dim);
            g.drawText (juce::String ((int) db), juce::Rectangle<float> (0.0f, y - 6.0f, area.getX() - 4.0f, 12.0f), juce::Justification::centredRight);
        }

        // The others, then this stem on top.
        for (int j = 0; j < numOthers; ++j)
        {
            const auto colour = stemColour (others[(size_t) j].slot);
            g.setColour (colour.withAlpha (0.85f));
            g.strokePath (levelPath (others[(size_t) j].level, spectra), juce::PathStrokeType (1.4f));
        }
        {
            auto line = levelPath (own, spectra);
            auto fill = line;
            fill.lineTo (xForBand (kBands - 1), spectra.getBottom());
            fill.lineTo (xForBand (0), spectra.getBottom());
            fill.closeSubPath();
            g.setColour (colours::accent.withAlpha (0.20f));
            g.fillPath (fill);
            g.setColour (colours::accent);
            g.strokePath (line, juce::PathStrokeType (1.8f));
        }

        // Who owns each band.
        const auto mine = claims (role, importance);
        std::vector<std::array<float, kBands>> theirs;
        for (int j = 0; j < numOthers; ++j)
            theirs.push_back (claims (others[(size_t) j].role, others[(size_t) j].importance));
        const float bandWidth = area.getWidth() * 0.1f * (float) std::log (10.0) / (float) std::log (1000.0);
        for (int k = 0; k < kBands; ++k)
        {
            float best = mine[(size_t) k];
            auto colour = colours::accent;
            for (int j = 0; j < numOthers; ++j)
                if (theirs[(size_t) j][(size_t) k] > best)
                {
                    best = theirs[(size_t) j][(size_t) k];
                    colour = stemColour (others[(size_t) j].slot);
                }
            g.setColour (colour.withAlpha (juce::jlimit (0.0f, 1.0f, best / 1.5f * 0.95f)));
            g.fillRect (juce::Rectangle<float> (bandWidth - 1.0f, strip.getHeight()).withCentre ({ xForBand (k), strip.getCentreY() }));
        }
        g.setColour (colours::dim);
        g.setFont (juce::FontOptions (8.5f));
        g.drawText ("OWNER", juce::Rectangle<float> (0.0f, strip.getY() - 1.0f, area.getX() - 4.0f, strip.getHeight() + 2.0f), juce::Justification::centredRight);

        // The cut.
        g.setColour (colours::glassGrid);
        g.drawHorizontalLine ((int) cutPane.getY(), area.getX(), area.getRight());
        g.setColour (colours::dim);
        g.drawText ("CUT", juce::Rectangle<float> (0.0f, cutPane.getY() - 6.0f, area.getX() - 4.0f, 12.0f), juce::Justification::centredRight);
        g.drawText (juce::String (-(int) kCutRangeDb), juce::Rectangle<float> (0.0f, cutPane.getBottom() - 12.0f, area.getX() - 4.0f, 12.0f), juce::Justification::centredRight);
        for (int k = 0; k < kBands; ++k)
        {
            const float h = juce::jlimit (0.0f, 1.0f, -cut[(size_t) k] / kCutRangeDb) * cutPane.getHeight();
            if (h < 0.5f)
                continue;
            g.setColour (colours::amber.withAlpha (0.8f));
            g.fillRect (juce::Rectangle<float> (bandWidth - 1.5f, h).withX (xForBand (k) - (bandWidth - 1.5f) * 0.5f).withY (cutPane.getY()));
        }

        // Who is who.
        float x = spectra.getX() + 8.0f;
        const auto legend = [&] (const juce::String& text, juce::Colour colour)
        {
            const float w = juce::GlyphArrangement::getStringWidth (ui::lcdFont (10.5f), text) + 20.0f;
            g.setColour (colour);
            g.fillEllipse (x, spectra.getY() + 7.0f, 7.0f, 7.0f);
            g.setFont (ui::lcdFont (10.5f));
            g.setColour (colours::text);
            g.drawText (text, juce::Rectangle<float> (x + 11.0f, spectra.getY() + 3.0f, w, 14.0f), juce::Justification::centredLeft);
            x += w + 8.0f;
        };
        legend (juce::String (roleName (role)) + " (this)", colours::accent);
        for (int j = 0; j < numOthers; ++j)
            legend (roleName (others[(size_t) j].role), stemColour (others[(size_t) j].slot));
        if (group == 0 || numOthers == 0)
        {
            g.setFont (juce::FontOptions (11.0f));
            g.setColour (colours::dim);
            g.drawText (group == 0 ? "Mix is Off: this stem is not linked." : "Waiting for other stems in this mix: put Hestia on them too.",
                        spectra.withTrimmedTop (spectra.getHeight() * 0.35f).withHeight (20.0f), juce::Justification::centred);
        }
        hw::drawGlassOverlay (g, frame, 4.0f, false);
    }

private:
    juce::Rectangle<float> plot() const noexcept { return getLocalBounds().toFloat().withTrimmedLeft (38.0f).withTrimmedRight (10.0f).withTrimmedTop (8.0f).withTrimmedBottom (18.0f); }
    float xForBand (int k) const noexcept { return xForHz (bandCentreHz (k)); }
    static float yForDb (float db, const juce::Rectangle<float>& r) noexcept
    {
        return r.getBottom() - juce::jlimit (0.0f, 1.0f, (db - kSpectraFloorDb) / (kSpectraCeilingDb - kSpectraFloorDb)) * r.getHeight();
    }
    juce::Path levelPath (const std::array<float, kBands>& level, const juce::Rectangle<float>& r) const
    {
        juce::Path path;
        for (int k = 0; k < kBands; ++k)
        {
            const juce::Point<float> p (xForBand (k), yForDb (level[(size_t) k], r));
            if (k == 0) path.startNewSubPath (p); else path.lineTo (p);
        }
        return path;
    }

    HestiaProcessor& proc;
    std::array<float, kBands> own {}, cut {};
    std::array<Stem, kMaxStems> others {};
    int numOthers = 0, group = 1;
    Role role = Role::fullRange;
    float importance = 0.5f, depth = 6.0f;
};
} // namespace hestia::gui
