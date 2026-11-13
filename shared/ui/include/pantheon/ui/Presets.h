// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 The Pantheon authors

#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include <functional>
#include <utility>
#include <vector>

namespace ui
{
// Presets for a plugin whose parameters are all in one AudioProcessorValueTreeState: what a preset is, how it is applied, the part of the processor that
// holds "the current program", and the box that chooses one. The settings, the names and the one-line purposes are the plugin's; this is only the mechanism.
//
//   processor:  ui::Programs programs { apvts, myPresets(), keeps };   // after apvts
//               int getNumPrograms() override { return programs.count(); }   int getCurrentProgram() override { return programs.current(); }
//               void setCurrentProgram (int i) override { programs.set (i); }   const juce::String getProgramName (int i) override { return programs.name (i); }
//               getStateInformation: programs.saveTo (apvts.state) before copyState();   setStateInformation: programs.loadFrom (apvts.state) after replaceState().
//   editor:     ui::PresetBox presetBox { processor, myPresets(), "what is kept" };   call presetBox.sync() from the timer.
struct Preset
{
    const char* name;
    std::vector<std::pair<juce::String, float>> values; // parameter id, real value (not 0..1); everything not listed goes back to its default
    const char* purpose;                                 // one line: what it is for, and what it is not
};

using Keeps = std::function<bool (const juce::String&)>; // true for a parameter a preset leaves alone (the session's own controls)

inline void applyPreset (juce::AudioProcessorValueTreeState& state, const std::vector<Preset>& list, int index, const Keeps& keeps = {})
{
    if (! juce::isPositiveAndBelow (index, (int) list.size()))
        return;
    // From the defaults, so a preset does not inherit what the last one left.
    for (auto* param : state.processor.getParameters())
        if (auto* ranged = dynamic_cast<juce::RangedAudioParameter*> (param))
            if (! (keeps && keeps (ranged->paramID)))
            {
                ranged->beginChangeGesture();
                ranged->setValueNotifyingHost (ranged->getDefaultValue());
                ranged->endChangeGesture();
            }
    for (auto& [pid, value] : list[(size_t) index].values)
        if (auto* param = state.getParameter (pid))
        {
            param->beginChangeGesture();
            param->setValueNotifyingHost (param->convertTo0to1 (value));
            param->endChangeGesture();
        }
}

class Programs
{
public:
    Programs (juce::AudioProcessorValueTreeState& s, const std::vector<Preset>& presetList, Keeps keepsFn = {}) : state (s), list (presetList), keeps (std::move (keepsFn)) {}

    int count() const { return (int) list.size(); }
    int current() const { return currentIndex; }
    juce::String name (int index) const { return juce::isPositiveAndBelow (index, count()) ? juce::String (list[(size_t) index].name) : juce::String(); }
    void set (int index)
    {
        if (! juce::isPositiveAndBelow (index, count()))
            return;
        currentIndex = index;
        applyPreset (state, list, index, keeps);
    }
    // The chosen preset is kept with the session, so the box shows it again when the session is opened (the settings themselves are parameters).
    void saveTo (juce::ValueTree& tree) const { tree.setProperty ("program", currentIndex, nullptr); }
    void loadFrom (const juce::ValueTree& tree) { currentIndex = juce::jlimit (0, std::max (count() - 1, 0), (int) tree.getProperty ("program", 0)); }

private:
    juce::AudioProcessorValueTreeState& state;
    const std::vector<Preset>& list;
    Keeps keeps;
    int currentIndex = 0;
};

// The box that chooses a preset. Its tooltip says what is kept and what the chosen one is for.
class PresetBox : public juce::ComboBox
{
public:
    PresetBox (juce::AudioProcessor& p, const std::vector<Preset>& presetList, juce::String whatIsKept) : processor (p), list (presetList), kept (std::move (whatIsKept))
    {
        for (int i = 0; i < (int) list.size(); ++i)
            addItem (list[(size_t) i].name, i + 1);
        setSelectedId (processor.getCurrentProgram() + 1, juce::dontSendNotification);
        onChange = [this]
        {
            processor.setCurrentProgram (getSelectedId() - 1);
            describe();
        };
        describe();
    }

    // The editor's timer calls this: a host can change the program too.
    void sync()
    {
        if (const int wanted = processor.getCurrentProgram() + 1; wanted != getSelectedId())
        {
            setSelectedId (wanted, juce::dontSendNotification);
            describe();
        }
    }

private:
    void describe()
    {
        juce::String text = "Presets: starting points, not finished settings. " + kept;
        if (const int i = getSelectedId() - 1; juce::isPositiveAndBelow (i, (int) list.size()))
            text << "\n\n" << list[(size_t) i].name << ": " << list[(size_t) i].purpose;
        setTooltip (text);
    }

    juce::AudioProcessor& processor;
    const std::vector<Preset>& list;
    juce::String kept;
};
} // namespace ui
