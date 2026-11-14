// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 The Pantheon authors

#pragma once

#include "Hardware.h"
#include "Presets.h"
#include "Undo.h"

#include <juce_audio_processors/juce_audio_processors.h>

#include <array>
#include <utility>
#include <vector>

namespace ui
{
// A/B compare: two complete settings of a plugin's parameters, one of them live. Choosing the other slot puts the settings as they are into the one you leave and
// loads the other's, so the plugin can be set up two ways and switched between them while listening. The first time a slot is chosen it has nothing in it yet, so it
// starts as a copy of the settings at that moment. A switch is one undo step (see Undo.h). The slots are not part of the session: only which one is active is saved
// (a session opened again has both slots empty, so the first switch starts the other as a copy of what is loaded, and the box shows the slot it was left on). Opt-in: a plugin that has none is drawn as before.
//
//   processor:  ui::AbCompare ab { apvts, keeps };   // after apvts; `keeps` (optional, as for presets) names the parameters left out, e.g. the host's bypass
//               getStateInformation: ab.saveTo (apvts.state) before copyState();   setStateInformation: ab.loadFrom (apvts.state) after replaceState().
//   editor:     ui::AbButtons abButtons { processor.ab };   addAndMakeVisible (abButtons);   abButtons.setBounds (x, y, ui::AbButtons::kWidth, h);
class AbCompare
{
public:
    explicit AbCompare (juce::AudioProcessorValueTreeState& s, Keeps keepsFn = {}) : state (s), keeps (std::move (keepsFn)) {}

    int active() const { return activeSlot; }
    bool hasSettings (int slot) const { return ! slots[(size_t) juce::jlimit (0, 1, slot)].empty(); }

    // Message thread only (it writes parameters the way a gesture on the screen does).
    void select (int slot)
    {
        slot = juce::jlimit (0, 1, slot);
        if (slot == activeSlot)
            return;
        slots[(size_t) activeSlot] = capture();
        if (slots[(size_t) slot].empty())
            slots[(size_t) slot] = slots[(size_t) activeSlot];
        else
        {
            beginUndoStep (state, "A/B");
            write (slots[(size_t) slot]);
        }
        activeSlot = slot;
    }

    void saveTo (juce::ValueTree& tree) const { tree.setProperty ("abSlot", activeSlot, nullptr); }
    void loadFrom (const juce::ValueTree& tree)
    {
        activeSlot = juce::jlimit (0, 1, (int) tree.getProperty ("abSlot", 0));
        for (auto& s : slots)
            s.clear();
    }

private:
    using Settings = std::vector<std::pair<juce::String, float>>; // parameter id, 0..1

    Settings capture() const
    {
        Settings out;
        for (auto* param : state.processor.getParameters())
            if (auto* ranged = dynamic_cast<juce::RangedAudioParameter*> (param))
                if (! (keeps && keeps (ranged->paramID)))
                    out.emplace_back (ranged->paramID, ranged->getValue());
        return out;
    }

    // Each parameter once, to its final value, and only if it differs.
    void write (const Settings& settings)
    {
        for (auto& [pid, value] : settings)
            if (auto* param = state.getParameter (pid))
                if (std::abs (param->getValue() - value) > 1.0e-6f)
                {
                    param->beginChangeGesture();
                    param->setValueNotifyingHost (value);
                    param->endChangeGesture();
                }
    }

    juce::AudioProcessorValueTreeState& state;
    Keeps keeps;
    std::array<Settings, 2> slots;
    int activeSlot = 0;
};

// The A and B buttons: the active one is lit. They follow the processor, so a session that is loaded while the editor is open shows its slot.
class AbButtons : public juce::Component, private juce::Timer
{
public:
    static constexpr int kWidth = 48;

    explicit AbButtons (AbCompare& compare) : ab (compare)
    {
        for (int i = 0; i < 2; ++i)
        {
            auto& b = buttons[(size_t) i];
            b.setButtonText (i == 0 ? "A" : "B");
            b.setTooltip (i == 0 ? "Setting A. Switching keeps what you have set in the one you leave, so the two can be compared while you listen."
                                 : "Setting B. The first time it is a copy of A: change it, then switch back and forth to compare. Not saved with the session.");
            b.setColour (juce::TextButton::buttonOnColourId, colours::amber);
            b.onClick = [this, i]
            {
                ab.select (i);
                sync();
            };
            addAndMakeVisible (b);
        }
        sync();
        startTimerHz (5);
    }

    juce::TextButton& button (int slot) noexcept { return buttons[(size_t) slot]; } // for the plugins' checks

    void resized() override
    {
        auto r = getLocalBounds();
        const int w = (r.getWidth() - 4) / 2;
        buttons[0].setBounds (r.removeFromLeft (w));
        r.removeFromLeft (4);
        buttons[1].setBounds (r);
    }

private:
    void timerCallback() override { sync(); }
    void sync()
    {
        for (int i = 0; i < 2; ++i)
            buttons[(size_t) i].setToggleState (ab.active() == i, juce::dontSendNotification);
    }

    AbCompare& ab;
    std::array<juce::TextButton, 2> buttons;
};
} // namespace ui
