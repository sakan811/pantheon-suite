// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 The Pantheon authors

#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <pantheon/ui/HardwareLookAndFeel.h>

namespace olympus::gui
{
// A push switch whose lamp has the colour it is given (the kit's look-and-feel lights every switch in one colour): amber for the transport
// switches, red for a mute, green for a solo, red for a plugin that is not installed.
class LampButton : public juce::TextButton
{
public:
    using juce::TextButton::TextButton;
    juce::Colour lamp = colours::amber;

    void paintButton (juce::Graphics& g, bool highlighted, bool down) override
    {
        ui::hw::drawPushButton (g, getLocalBounds().toFloat(), getToggleState(), lamp, down, highlighted, isEnabled());
        g.setFont (ui::labelFont (juce::jmin (10.5f, (float) getHeight() * 0.5f)));
        g.setColour (getToggleState() && lamp.getPerceivedBrightness() > 0.5f ? colours::lampInk : colours::text);
        g.drawText (getButtonText().toUpperCase(), getLocalBounds().translated (0, down ? 1 : 0), juce::Justification::centred);
    }
};
} // namespace olympus::gui
