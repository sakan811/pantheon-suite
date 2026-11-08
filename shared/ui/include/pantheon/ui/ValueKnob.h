// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 The Pantheon authors

#pragma once

#include "Hardware.h"

#include <functional>

namespace ui
{
// The knob of Controls.h for a value that is not a plugin parameter: the setting of whatever is selected in an editor, say. The
// same caption, the same window of glass with the value in it, the same typing into the read-out; but it holds its own value and
// tells its owner through callbacks. Wording of the value (units, note names) is the owner's: `format` turns a value into text and
// `parse` turns typed text back into one (return a NaN to refuse it).
//
// `onChange` fires on every change the user makes; `onGestureStart` and `onGestureEnd` bracket a drag (or a typed entry), which is
// when an owner with an undo history wants to take its snapshot. A knob can show "mixed" for a selection whose values differ: the
// first thing the user does to it then sets all of them.
class ValueKnob : public juce::Component, public juce::SettableTooltipClient
{
public:
    enum class Layout { beside, below };

    explicit ValueKnob (Layout layoutToUse = Layout::below) : layout (layoutToUse)
    {
        slider.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
        slider.setTextBoxStyle (juce::Slider::NoTextBox, true, 0, 0);
        slider.setRotaryParameters (juce::MathConstants<float>::pi * 1.25f, juce::MathConstants<float>::pi * 2.75f, true);
        slider.setMouseDragSensitivity (140);
        slider.onValueChange = [this]
        {
            if (! quiet)
            {
                mixed = false;
                if (onChange)
                    onChange (slider.getValue());
            }
            repaint();
        };
        slider.onDragStart = [this] { if (onGestureStart) onGestureStart(); };
        slider.onDragEnd = [this] { if (onGestureEnd) onGestureEnd(); };
        addAndMakeVisible (slider);

        entry.setMultiLine (false);
        entry.setReturnKeyStartsNewLine (false);
        entry.setSelectAllWhenFocused (true);
        entry.setJustification (juce::Justification::centred);
        entry.setFont (lcdFont (12.0f));
        entry.setColour (juce::TextEditor::backgroundColourId, juce::Colours::black);
        entry.setColour (juce::TextEditor::textColourId, colours::amber);
        entry.setColour (juce::TextEditor::highlightColourId, colours::amber.withAlpha (0.35f));
        entry.setColour (juce::TextEditor::highlightedTextColourId, colours::amber);
        entry.setColour (juce::TextEditor::outlineColourId, colours::amber.withAlpha (0.6f));
        entry.setColour (juce::TextEditor::focusedOutlineColourId, colours::amber);
        entry.setColour (juce::CaretComponent::caretColourId, colours::amber);
        entry.onReturnKey = [this] { finishTyping (true); };
        entry.onEscapeKey = [this] { finishTyping (false); };
        entry.onFocusLost = [this] { finishTyping (true); };
        addChildComponent (entry);
        setTooltip ("Drag, or click the number to type a value. Double-click the knob to reset it.");
    }

    // range, step (0 = continuous), the value a double-click returns to, and the wording of a value.
    void setup (const juce::String& captionText, double minimum, double maximum, double step, double resetTo, std::function<juce::String (double)> formatValue,
                std::function<double (const juce::String&)> parseText, const juce::String& tooltip = {})
    {
        caption = captionText;
        format = std::move (formatValue);
        parse = std::move (parseText);
        quiet = true;
        slider.setRange (minimum, maximum, step);
        slider.setDoubleClickReturnValue (true, resetTo);
        quiet = false;
        if (tooltip.isNotEmpty())
            slider.setTooltip (tooltip);
        repaint();
    }

    // Sets the value without telling the owner (the owner is the one changing it).
    void setValue (double v, bool isMixed = false)
    {
        quiet = true;
        slider.setValue (v, juce::dontSendNotification);
        quiet = false;
        mixed = isMixed;
        repaint();
    }
    double getValue() const { return slider.getValue(); }

    void setActive (bool on)
    {
        slider.setEnabled (on);
        setAlpha (on ? 1.0f : 0.4f);
        if (! on)
            finishTyping (false);
    }

    std::function<void (double)> onChange;
    std::function<void()> onGestureStart, onGestureEnd;

    juce::MouseCursor getMouseCursor() override
    {
        return slider.isEnabled() && readout.contains (getMouseXYRelative()) ? juce::MouseCursor::IBeamCursor : juce::MouseCursor::NormalCursor;
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        if (slider.isEnabled() && readout.contains (e.getPosition()))
            beginTyping();
    }

    void beginTyping()
    {
        if (typing || ! slider.isEnabled())
            return;
        typing = true;
        entry.setBounds (readout);
        entry.setText (mixed ? juce::String() : text(), false);
        entry.setVisible (true);
        entry.grabKeyboardFocus();
        entry.selectAll();
    }

    void resized() override
    {
        auto r = getLocalBounds();
        if (layout == Layout::beside)
        {
            slider.setBounds (r.removeFromLeft (r.getHeight()));
            r.removeFromLeft (4);
            captionArea = r.removeFromTop (r.getHeight() * 2 / 5).withTrimmedTop (3);
            readout = r.reduced (0, 2);
        }
        else
        {
            captionArea = r.removeFromTop (16);
            readout = r.removeFromBottom (24).reduced (juce::jmax (0, (r.getWidth() - 84) / 2), 2);
            slider.setBounds (r.reduced (juce::jmax (0, (r.getWidth() - r.getHeight()) / 2), 2));
        }
        entry.setBounds (readout);
    }

    void paint (juce::Graphics& g) override
    {
        const bool beside = layout == Layout::beside;
        hw::drawLabel (g, caption, captionArea, beside ? juce::Justification::bottomLeft : juce::Justification::centred, colours::dim, 9.5f);
        hw::drawGlass (g, readout.toFloat(), 3.0f, colours::amber);
        g.setFont (lcdFont (12.0f));
        g.setColour (mixed ? colours::dim : colours::amber);
        g.drawText (mixed ? juce::String ("mixed") : text(), readout.reduced (6, 0), beside ? juce::Justification::centredLeft : juce::Justification::centred);
        hw::drawGlassOverlay (g, readout.toFloat(), 3.0f, false);
    }

private:
    juce::String text() const { return format ? format (slider.getValue()) : juce::String (slider.getValue(), 2); }

    void finishTyping (bool apply)
    {
        if (! typing)
            return;
        typing = false;
        const auto typed = entry.getText().trim();
        entry.setVisible (false);
        if (apply && typed.isNotEmpty())
        {
            const double v = parse ? parse (typed) : typed.getDoubleValue();
            if (std::isfinite (v))
            {
                if (onGestureStart)
                    onGestureStart();
                slider.setValue (juce::jlimit (slider.getMinimum(), slider.getMaximum(), v), juce::sendNotificationSync);
                if (onGestureEnd)
                    onGestureEnd();
            }
        }
        repaint();
    }

    Layout layout;
    juce::Slider slider;
    juce::TextEditor entry;
    bool typing = false, quiet = false, mixed = false;
    juce::String caption;
    juce::Rectangle<int> captionArea, readout;
    std::function<juce::String (double)> format;
    std::function<double (const juce::String&)> parse;
};
} // namespace ui
