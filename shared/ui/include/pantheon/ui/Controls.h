// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 The Pantheon authors

#pragma once

#include "Hardware.h"

#include <juce_audio_processors/juce_audio_processors.h>

#include <memory>

namespace ui
{
using Apvts = juce::AudioProcessorValueTreeState;

// A rotary knob bound to a parameter, with its name and a digital read-out of the value in a window of glass. A choice
// parameter turns it into a detent switch (the tick ring shows the positions). `beside` puts the knob at the left with the name
// and read-out to its right; `below` stacks name, knob and read-out for a tall cell. A knob can be bound later and re-bound to
// another parameter (a panel whose knobs change meaning). Clicking the read-out opens it for typing: Enter applies the value
// (in the parameter's own units, e.g. "250" or "250 Hz"; a detent switch takes the name of a position), Esc or a click
// elsewhere leaves it as it was.
class Knob : public juce::Component, public juce::SettableTooltipClient
{
public:
    enum class Layout { beside, below };

    explicit Knob (Layout layoutToUse = Layout::beside) : layout (layoutToUse)
    {
        slider.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
        slider.setTextBoxStyle (juce::Slider::NoTextBox, true, 0, 0);
        slider.setRotaryParameters (juce::MathConstants<float>::pi * 1.25f, juce::MathConstants<float>::pi * 2.75f, true);
        slider.setMouseDragSensitivity (140);
        addAndMakeVisible (slider);
        slider.onValueChange = [this] { repaint(); };

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
        setTooltip ("Click the number to type a value. Enter applies it, Esc cancels.");
    }

    Knob (Apvts& state, const juce::String& paramId, const juce::String& labelText, const juce::String& tooltip, Layout layoutToUse = Layout::beside)
        : Knob (layoutToUse)
    {
        setCaption (labelText);
        setKnobTooltip (tooltip);
        bind (state, paramId);
    }

    void setCaption (const juce::String& text) { caption = text; repaint(); }
    void setKnobTooltip (const juce::String& text) { slider.setTooltip (text); }

    // (Re-)attaches the knob to a parameter; any earlier binding ends.
    void bind (Apvts& state, const juce::String& paramId)
    {
        finishTyping (false);
        attachment.reset();
        choiceParam = dynamic_cast<juce::AudioParameterChoice*> (state.getParameter (paramId));
        attachment = std::make_unique<Apvts::SliderAttachment> (state, paramId, slider);
        if (auto* p = state.getParameter (paramId))
        {
            slider.setDoubleClickReturnValue (true, slider.getNormalisableRange().convertFrom0to1 (p->getDefaultValue()));
            slider.setTextValueSuffix (p->getLabel().isNotEmpty() ? " " + p->getLabel() : juce::String()); // the unit, where the parameter has one
        }
    }

    void unbind() { finishTyping (false); attachment.reset(); choiceParam = nullptr; }

    // Opens the read-out for typing (what a click on it does).
    void beginTyping()
    {
        if (attachment == nullptr || typing)
            return;
        typing = true;
        entry.setBounds (readout);
        entry.setText (slider.getTextFromValue (slider.getValue()), false);
        entry.setVisible (true);
        entry.grabKeyboardFocus();
        entry.selectAll();
    }

    juce::MouseCursor getMouseCursor() override
    {
        return attachment != nullptr && readout.contains (getMouseXYRelative()) ? juce::MouseCursor::IBeamCursor : juce::MouseCursor::NormalCursor;
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        if (readout.contains (e.getPosition()))
            beginTyping();
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
        g.setColour (colours::amber);
        g.drawText (slider.getTextFromValue (slider.getValue()), readout.reduced (6, 0), beside ? juce::Justification::centredLeft : juce::Justification::centred);
        hw::drawGlassOverlay (g, readout.toFloat(), 3.0f, false);
    }

private:
    // Closes the entry; with `apply`, sets the parameter from the text if it makes sense (anything else leaves the value alone).
    void finishTyping (bool apply)
    {
        if (! typing)
            return;
        typing = false;
        const auto text = entry.getText().trim();
        entry.setVisible (false); // gives up the focus without ending in here again: `typing` is already false
        if (apply && attachment != nullptr && text.isNotEmpty())
        {
            if (choiceParam != nullptr)
            {
                const int index = choiceParam->choices.indexOf (text, true);
                if (index >= 0)
                    slider.setValue (index, juce::sendNotificationSync);
            }
            else if (text.containsAnyOf ("0123456789")) // "abc" is not 0
            {
                slider.setValue (slider.getValueFromText (text), juce::sendNotificationSync);
            }
        }
        repaint();
    }

    Layout layout;
    juce::Slider slider;
    juce::TextEditor entry;
    bool typing = false;
    juce::AudioParameterChoice* choiceParam = nullptr;
    juce::String caption;
    juce::Rectangle<int> captionArea, readout;
    std::unique_ptr<Apvts::SliderAttachment> attachment;
};

// A selector bound to a choice parameter, drawn by HardwareLookAndFeel as a window of glass. The items come from the parameter
// itself, so the list of choices is defined once, where the parameter is.
class Choice : public juce::ComboBox
{
public:
    Choice (Apvts& state, const juce::String& paramId, const juce::String& tooltip = {})
    {
        if (auto* choice = dynamic_cast<juce::AudioParameterChoice*> (state.getParameter (paramId)))
            addItemList (choice->choices, 1);
        setTooltip (tooltip);
        attachment = std::make_unique<Apvts::ComboBoxAttachment> (state, paramId, *this);
    }

private:
    std::unique_ptr<Apvts::ComboBoxAttachment> attachment;
};

// A push switch bound to a bool parameter: it lights up while the parameter is on.
class Toggle : public juce::TextButton
{
public:
    Toggle (Apvts& state, const juce::String& paramId, const juce::String& text, const juce::String& tooltip)
    {
        setButtonText (text);
        setTooltip (tooltip);
        setClickingTogglesState (true);
        attachment = std::make_unique<Apvts::ButtonAttachment> (state, paramId, *this);
    }

private:
    std::unique_ptr<Apvts::ButtonAttachment> attachment;
};
} // namespace ui
