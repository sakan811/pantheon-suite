// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 The Pantheon authors

#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

namespace aegis
{
// A transparent strip laid over a part of the display that is painted, not a control, so it can carry a tooltip. A click on it goes to the editor
// (the parent) as if the strip were not there: the MAX REDUCTION number clears its hold from the editor's mouseDown. It has to take the click to
// show its tooltip at all (a component that does not intercept clicks is never the one under the mouse), so it hands the click on.
class TipArea : public juce::Component, public juce::SettableTooltipClient
{
public:
    explicit TipArea (const juce::String& text)
    {
        setTooltip (text);
        setInterceptsMouseClicks (true, false);
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        if (auto* parent = getParentComponent())
            parent->mouseDown (e.getEventRelativeTo (parent));
    }
};
} // namespace aegis
