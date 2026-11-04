// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 The Pantheon authors

#pragma once

#include "PluginProcessor.h"
#include "ThemisUi.h"

#include <array>

// Solo / Mute for the stereo image, the channels and the four energy ranges: the only controls that change what you hear.
// With nothing pressed the audio passes through untouched. State lives in the processor (not saved with the session);
// this panel only mirrors it, so the master plan's "hear it" rows and the buttons stay in step.
class AuditionPanel : public juce::Component
{
public:
    explicit AuditionPanel (ThemisProcessor& p) : proc (p)
    {
        for (int i = 0; i < themis::kNumSources; ++i)
        {
            const auto source = static_cast<themis::Source> (i);
            setup (solo[static_cast<size_t> (i)], "S", colours::warn, "Solo " + juce::String (themis::sourceName (source)) + ": hear only " + describe (source),
                   [this, source] { toggle (source, true); });
            setup (mute[static_cast<size_t> (i)], "M", colours::bad, "Mute " + juce::String (themis::sourceName (source)) + ": hear everything except " + describe (source),
                   [this, source] { toggle (source, false); });
        }
        clear.setButtonText ("CLEAR");
        clear.setTooltip ("Release every Solo and Mute: the output is the input again, bit for bit.");
        clear.onClick = [this] { proc.setAudition ({}); update(); };
        addAndMakeVisible (clear);
        update();
    }

    // Mirrors the processor's switches (message thread, ~30 Hz).
    void update()
    {
        const auto sw = proc.auditionSwitches();
        const bool wasActive = active;
        active = sw.active();
        for (int i = 0; i < themis::kNumSources; ++i)
        {
            const auto b = themis::Switches::bit (static_cast<themis::Source> (i));
            solo[static_cast<size_t> (i)].setToggleState ((sw.solo & b) != 0, juce::dontSendNotification);
            mute[static_cast<size_t> (i)].setToggleState ((sw.mute & b) != 0, juce::dontSendNotification);
        }
        clear.setEnabled (active);
        if (wasActive != active)
            repaint();
    }

    void paint (juce::Graphics& g) override
    {
        auto area = getLocalBounds().toFloat();
        ui::hw::drawModule (g, area);
        if (active)
        {
            g.setColour (colours::bad.withAlpha (0.20f));
            g.drawRoundedRectangle (area.reduced (0.5f), 6.0f, 4.0f);
            g.setColour (colours::bad.withAlpha (0.85f));
            g.drawRoundedRectangle (area.reduced (0.5f), 6.0f, 1.5f);
        }

        ui::hw::drawLabel (g, "AUDITION", titleBounds, juce::Justification::centredLeft);
        if (active)
        {
            ui::hw::drawLamp (g, { static_cast<float> (alertBounds.getX()) + 3.0f, static_cast<float> (alertBounds.getCentreY()) }, 3.0f, colours::bad);
            const auto textArea = alertBounds.withTrimmedLeft (11);
            const bool fits = juce::GlyphArrangement::getStringWidth (ui::labelFont (9.0f), "OUTPUT ALTERED") <= static_cast<float> (textArea.getWidth());
            ui::hw::drawLabel (g, fits ? "OUTPUT ALTERED" : "ALTERED", textArea, juce::Justification::centredLeft, colours::bad, 9.0f);
        }

        for (const auto& group : groups)
        {
            ui::hw::drawLabel (g, group.name, group.labelBounds.withTrimmedLeft (0), juce::Justification::centredLeft, colours::dim.withAlpha (0.8f), 9.0f);
            g.setColour (juce::Colours::black.withAlpha (0.45f)); // a groove under the group's name
            g.drawHorizontalLine (group.labelBounds.getBottom() - 1, static_cast<float> (group.labelBounds.getX() + 56), static_cast<float> (group.labelBounds.getRight()));
        }
        g.setFont (ui::labelFont (10.5f));
        g.setColour (colours::text);
        for (int i = 0; i < themis::kNumSources; ++i)
            g.drawText (juce::String (themis::sourceName (static_cast<themis::Source> (i))).toUpperCase(), nameBounds[static_cast<size_t> (i)], juce::Justification::centredLeft);
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced (10, 6);
        titleBounds = r.removeFromTop (kTitleHeight);
        clear.setBounds (titleBounds.removeFromRight (46).reduced (0, 1));
        alertBounds = titleBounds.withTrimmedLeft (juce::roundToInt (juce::GlyphArrangement::getStringWidth (ui::labelFont(), "AUDITION")) + 12);

        // Fit the three groups (a label row each, then one row per source) into what is left.
        const int labelRows = 3, sourceRows = themis::kNumSources;
        const int rowH = juce::jlimit (16, 24, (r.getHeight() - labelRows * kLabelHeight) / sourceRows);
        int i = 0;
        for (auto& group : groups)
        {
            group.labelBounds = r.removeFromTop (kLabelHeight);
            for (int n = 0; n < group.count; ++n, ++i)
            {
                auto row = r.removeFromTop (rowH);
                mute[static_cast<size_t> (i)].setBounds (row.removeFromRight (kButtonWidth).reduced (1, 1));
                solo[static_cast<size_t> (i)].setBounds (row.removeFromRight (kButtonWidth).reduced (1, 1));
                nameBounds[static_cast<size_t> (i)] = row;
            }
        }
    }

private:
    static constexpr int kTitleHeight = 18, kLabelHeight = 13, kButtonWidth = 34;

    struct Group
    {
        const char* name;
        int count;
        juce::Rectangle<int> labelBounds {};
    };

    // What each source is, for the tooltips.
    static juce::String describe (themis::Source s)
    {
        if (s == themis::Source::mid)
            return "the centre, (L + R) / 2: what a mono sum keeps";
        if (s == themis::Source::side)
            return "what differs between the channels, (L - R) / 2: what a mono sum loses (silent for a mono signal)";
        if (s == themis::Source::left || s == themis::Source::right)
            return s == themis::Source::left ? "the left channel (played on both sides)" : "the right channel (played on both sides)";
        const auto& range = themis::kEnergyRanges[static_cast<int> (s) - themis::kFirstBandSource];
        return "the " + juce::String (range.name) + " range, " + ui::hzText (range.loHz) + " - " + ui::hzText (range.hiHz);
    }

    void setup (juce::TextButton& b, const juce::String& text, juce::Colour on, const juce::String& tip, std::function<void()> onClick)
    {
        b.setButtonText (text);
        b.setClickingTogglesState (false);
        b.setColour (juce::TextButton::buttonOnColourId, on.darker (0.15f));
        b.setTooltip (tip + ". Changes what you hear only; the measurements always see the input.");
        b.onClick = std::move (onClick);
        addAndMakeVisible (b);
    }

    void toggle (themis::Source source, bool isSolo)
    {
        auto sw = proc.auditionSwitches();
        (isSolo ? sw.solo : sw.mute) ^= themis::Switches::bit (source);
        proc.setAudition (sw);
        update();
    }

    ThemisProcessor& proc;
    std::array<juce::TextButton, themis::kNumSources> solo, mute;
    juce::TextButton clear;
    std::array<juce::Rectangle<int>, themis::kNumSources> nameBounds {};
    juce::Rectangle<int> titleBounds, alertBounds;
    std::array<Group, 3> groups { { { "IMAGE", 2 }, { "CHANNELS", 2 }, { "RANGES", 4 } } };
    bool active = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AuditionPanel)
};
