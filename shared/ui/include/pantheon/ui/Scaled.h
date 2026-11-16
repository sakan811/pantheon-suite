// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 The Pantheon authors

#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include "Resize.h"

#include <memory>
#include <utility>

namespace ui
{
// Opens a plugin's editor at the size it was drawn (100%), and lets the user drag the corner to scale it. The editor inside is laid out, drawn and tested at its own
// size (the design size it sets in its constructor) and never knows: this window scales it by a transform. The design size has the kit's ratio (`ui::kEditorAspect`,
// 16:10, see Resize.h; a debug build asserts it) and the window keeps it, from any of its four corners (`ui::ResizeGrips`) or from the host's edges. The scale
// is kept with the session (a property of the processor's state, `uiScale`), so the next time the editor opens it is as it was left.
//
//   juce::AudioProcessorEditor* MyProcessor::createEditor() { return new ui::ScaledEditor<MyEditor> (*this, apvts.state, *this); }
//
// The extra arguments are handed to the editor's constructor. Tests reach the editor inside with `ui::innerOf<MyEditor> (*processor.createEditor())`.
inline constexpr float kDefaultEditorScale = 1.0f, kMinEditorScale = 0.5f, kMaxEditorScale = 1.5f;

template <class Editor>
class ScaledEditor : public juce::AudioProcessorEditor
{
public:
    template <class... Args>
    ScaledEditor (juce::AudioProcessor& processor, juce::ValueTree stateToKeepScaleIn, Args&&... args)
        : juce::AudioProcessorEditor (&processor), state (std::move (stateToKeepScaleIn)), inner (std::forward<Args> (args)...)
    {
        addAndMakeVisible (inner);
        designWidth = inner.getWidth();
        designHeight = inner.getHeight();
        jassert (hasEditorAspect (designWidth, designHeight)); // the editor's design size must be 16:10: kWidth a multiple of 8 and kHeight = ui::editorHeightFor (kWidth)
        const float scale = juce::jlimit (kMinEditorScale, kMaxEditorScale, (float) (double) state.getProperty (scaleProperty, (double) kDefaultEditorScale));
        grips = std::make_unique<ResizeGrips> (*this, juce::roundToInt ((float) designWidth * kMinEditorScale), juce::roundToInt ((float) designWidth * kDefaultEditorScale),
                                               juce::roundToInt ((float) designWidth * kMaxEditorScale));
        addAndMakeVisible (*grips); // after the editor inside, so it is on top
        setSize (juce::roundToInt ((float) designWidth * scale), ResizeGrips::heightFor (juce::roundToInt ((float) designWidth * scale)));
    }

    void resized() override
    {
        const float scale = (float) getWidth() / (float) designWidth;
        inner.setTransform (juce::AffineTransform::scale (scale));
        inner.setBounds (0, 0, designWidth, designHeight);
        if (grips != nullptr)
            grips->setBounds (getLocalBounds());
        if (std::fabs (scale - (float) (double) state.getProperty (scaleProperty, -1.0)) > 0.002f)
            state.setProperty (scaleProperty, (double) scale, nullptr);
    }

private:
    inline static const juce::Identifier scaleProperty { "uiScale" };
    juce::ValueTree state;
    int designWidth = 1, designHeight = 1;
    std::unique_ptr<ResizeGrips> grips;

public:
    Editor inner; // declared last: it is made after the rest of the window
};

// The editor inside a ScaledEditor.
template <class Editor>
Editor& innerOf (juce::AudioProcessorEditor& scaled)
{
    return static_cast<ScaledEditor<Editor>&> (scaled).inner;
}
} // namespace ui
