// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 The Pantheon authors

#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include <utility>

namespace ui
{
// Opens a plugin's editor at the size it was drawn (100%), and lets the user drag the corner to scale it. The editor inside is laid out, drawn and tested at its own
// size (the design size it sets in its constructor) and never knows: this window scales it by a transform, as Harmonia's does. The aspect ratio is kept. The scale
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
        const float scale = juce::jlimit (kMinEditorScale, kMaxEditorScale, (float) (double) state.getProperty (scaleProperty, (double) kDefaultEditorScale));
        setResizable (true, true);
        setResizeLimits (juce::roundToInt ((float) designWidth * kMinEditorScale), juce::roundToInt ((float) designHeight * kMinEditorScale),
                         juce::roundToInt ((float) designWidth * kMaxEditorScale), juce::roundToInt ((float) designHeight * kMaxEditorScale));
        getConstrainer()->setFixedAspectRatio ((double) designWidth / (double) designHeight);
        setSize (juce::roundToInt ((float) designWidth * scale), juce::roundToInt ((float) designHeight * scale));
    }

    void resized() override
    {
        const float scale = (float) getWidth() / (float) designWidth;
        inner.setTransform (juce::AffineTransform::scale (scale));
        inner.setBounds (0, 0, designWidth, designHeight);
        if (std::fabs (scale - (float) (double) state.getProperty (scaleProperty, -1.0)) > 0.002f)
            state.setProperty (scaleProperty, (double) scale, nullptr);
    }

private:
    inline static const juce::Identifier scaleProperty { "uiScale" };
    juce::ValueTree state;
    int designWidth = 1, designHeight = 1;

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
