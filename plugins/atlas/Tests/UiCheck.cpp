// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 The Pantheon authors

// Builds the editor on the real processor and checks what a host and a user would meet: the parameters, the name, the strips,
// and that solo and delta are monitoring that ends with the window.

#include "Check.h"
#include "PluginEditor.h"

using namespace atlas;
using test::check;
using test::fmt;

int main()
{
    juce::ScopedJuceInitialiser_GUI gui;
    std::printf ("Atlas UI check\n");
    AtlasProcessor proc;
    proc.setPlayConfigDetails (2, 2, 48000.0, 512);
    proc.prepareToPlay (48000.0, 512);

    check (proc.getName() == juce::String (gui::kProjectWord) + " " + gui::kPluginWord, "the reported name is the nameplate's two words");
    check (proc.getParameters().size() == 3 + kCrossovers + 8 * kBands,
           fmt ("%d parameters: phase, oversampling, output, the %d crossovers and 8 for each of %d bands", proc.getParameters().size(), kCrossovers, kBands));
    check (channelNames.size() == kNumChannelModes && oversamplingNames.size() == kNumOsModes, "the choice lists match the engine's modes");
    for (int k = 0; k < kCrossovers; ++k)
        check (std::fabs (proc.apvts.getRawParameterValue (id::crossover[(size_t) k])->load() - (float) pdsp::bands::crossoverHz (k)) < 0.5f,
               fmt ("crossover %d starts on the shared band edge, %.0f Hz", k + 1, pdsp::bands::crossoverHz (k)));

    auto& monitor = proc.engine.monitor;
    {
        std::unique_ptr<juce::AudioProcessorEditor> holder (proc.createEditor());
        auto& editor = static_cast<AtlasEditor&> (*holder);
        editor.setVisible (true);
        check (editor.getWidth() > 900 && editor.strips[5]->bounds.getRight() <= editor.getWidth() && editor.strips[5]->crossover == nullptr && editor.strips[0]->crossover != nullptr,
               "six strips fit the window; every band but the top one has the knob of its upper edge");
        check (editor.strips[2]->channels.getNumItems() == kNumChannelModes, "a strip's channel selector has the parameter's seven items");

        editor.strips[1]->solo.triggerClick();
        editor.strips[3]->solo.triggerClick();
        juce::MessageManager::getInstance()->runDispatchLoopUntil (100);
        check (monitor.solo.load() == ((1u << 1) | (1u << 3)), "SOLO on two strips solos those two bands");
        check (editor.strips[1]->solo.getToggleState() && ! editor.strips[0]->solo.getToggleState(), "... and their buttons light up");
        editor.strips[1]->solo.triggerClick();
        editor.strips[4]->delta.triggerClick();
        juce::MessageManager::getInstance()->runDispatchLoopUntil (100);
        check (monitor.solo.load() == (1u << 3) && monitor.delta.load() == (1u << 4), "a second click releases a solo; DELTA is per band");
        editor.clearButton.triggerClick();
        juce::MessageManager::getInstance()->runDispatchLoopUntil (100);
        check (monitor.solo.load() == 0u && monitor.delta.load() == 0u, "ALL BANDS ends every solo and delta");

        editor.strips[0]->channels.setSelectedItemIndex ((int) Channels::midSide, juce::sendNotificationSync);
        check (readParams (proc.apvts).bands[0].channels == Channels::midSide, "the channel selector sets the band's parameter");

        editor.strips[2]->solo.triggerClick();
        editor.strips[2]->delta.triggerClick();
        juce::MessageManager::getInstance()->runDispatchLoopUntil (100);
    }
    check (monitor.solo.load() == 0u && monitor.delta.load() == 0u, "closing the editor ends solo and delta");

    // The latency reaches the host from the message thread.
    proc.apvts.getParameter (id::phase)->setValueNotifyingHost (1.0f);
    proc.apvts.getParameter (id::oversampling)->setValueNotifyingHost (1.0f);
    juce::AudioBuffer<float> buffer (2, 512);
    juce::MidiBuffer midi;
    buffer.clear();
    proc.processBlock (buffer, midi);
    juce::MessageManager::getInstance()->runDispatchLoopUntil (100);
    check (proc.getLatencySamples() == proc.engine.latencySamples.load() && proc.getLatencySamples() > LinearSplit::latencyFor (48000.0),
           fmt ("Linear with 4x oversampling: the host is told %d samples", proc.getLatencySamples()));

    // State round trip.
    juce::MemoryBlock state;
    proc.getStateInformation (state);
    AtlasProcessor other;
    other.setStateInformation (state.getData(), (int) state.getSize());
    const auto a = readParams (proc.apvts), b = readParams (other.apvts);
    check (b.phase == PhaseMode::linear && b.oversampling == a.oversampling && b.bands[0].channels == Channels::midSide, "a saved state comes back in another instance");

    return test::finish ("UI CHECK");
}
