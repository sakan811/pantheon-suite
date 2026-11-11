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
    check (proc.getParameters().size() == 3 + 2 * kCrossovers + 8 * kBands,
           fmt ("%d parameters: phase, oversampling, output, the %d splits (frequency and on) and 8 for each of %d bands", proc.getParameters().size(), kCrossovers, kBands));
    check (channelNames.size() == kNumChannelModes && oversamplingNames.size() == kNumOsModes, "the choice lists match the engine's modes");
    for (int k = 0; k < kCrossovers; ++k)
        check (std::fabs (proc.apvts.getRawParameterValue (id::crossover[(size_t) k])->load() - (float) pdsp::bands::crossoverHz (k)) < 0.5f,
               fmt ("crossover %d starts on the shared band edge, %.0f Hz", k + 1, pdsp::bands::crossoverHz (k)));

    // The bands: one to six, made by splitting and joining. The settings travel with the bands.
    {
        AtlasProcessor q;
        auto& s = q.apvts;
        check (layoutOf (s).numBands == 6 && addSplit (s, 1000.0) == -1, "a new instance has the six bands of music, and a seventh is refused");
        setValue (s, bandIds (1).threshold, -21.0f);
        setValue (s, bandIds (4).threshold, -33.0f);
        check (removeSplit (s, 1) && layoutOf (s).numBands == 5 && std::fabs (layoutOf (s).edgeHz[2] - 500.0) < 1.0
                   && std::fabs (readParams (s).bands[1].thresholdDb + 21.0) < 0.01 && std::fabs (readParams (s).bands[3].thresholdDb + 33.0) < 0.01,
               "joining bands 2 and 3: five bands, band 2 now reaches 500 Hz with its own settings, and the bands above move down a strip with theirs");
        check (addSplit (s, 510.0) == -1 && addSplit (s, 30.0) == -1, "a split too close to an edge, or under 40 Hz, is refused");
        const int made = addSplit (s, 120.0);
        const auto l = layoutOf (s);
        check (made == 2 && l.numBands == 6 && std::fabs (l.edgeHz[2] - 120.0) < 1.0 && std::fabs (readParams (s).bands[2].thresholdDb + 21.0) < 0.01
                   && std::fabs (readParams (s).bands[4].thresholdDb + 33.0) < 0.01 && bandOwners (readParams (s).split) == std::array<int, kBands> { 0, 1, 2, 3, 4, 5 },
               "splitting band 2 at 120 Hz: both halves have its settings, the bands above move up, six bands again");
        for (int k = 0; k < 5; ++k) removeSplit (s, 0);
        check (layoutOf (s).numBands == 1 && ! removeSplit (s, 0) && bandOwners (readParams (s).split) == std::array<int, kBands> { 0, 0, 0, 0, 0, 0 },
               "every split removed: one band over the whole spectrum");
        moveSplit (s, addSplit (s, 300.0) - 1, 5.0);
        check (std::fabs (layoutOf (s).edgeHz[1] - kMinCrossoverHz) < 1.0, "an edge dragged off the bottom stops at 40 Hz");
    }

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

        // The display: a double-click splits, a double-click on an edge joins, a drag moves an edge.
        auto& view = editor.view;
        const auto clickAt = [&] (double hz, int clicks)
        {
            const juce::Point<float> at (view.xForHz (hz), (float) view.getHeight() * 0.5f);
            return juce::MouseEvent (juce::Desktop::getInstance().getMainMouseSource(), at, juce::ModifierKeys::leftButtonModifier, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f,
                                     &view, &view, juce::Time::getCurrentTime(), at, juce::Time::getCurrentTime(), clicks, false);
        };
        view.mouseDoubleClick (clickAt (2000.0, 2));
        check (layoutOf (proc.apvts).numBands == 5 && ! editor.strips[5]->on.isVisible() && editor.strips[4]->on.isVisible(),
               "a double-click on the 2 kHz edge joins the two bands: five strips are shown");
        view.mouseDoubleClick (clickAt (3000.0, 2));
        check (layoutOf (proc.apvts).numBands == 6 && std::fabs (layoutOf (proc.apvts).edgeHz[4] - 3000.0) < 40.0 && editor.strips[5]->on.isVisible(),
               "a double-click at 3 kHz splits the band there: six strips again");
        view.mouseDown (clickAt (layoutOf (proc.apvts).edgeHz[4], 1));
        view.mouseDrag (clickAt (2500.0, 1));
        view.mouseUp (clickAt (2500.0, 1));
        check (std::fabs (layoutOf (proc.apvts).edgeHz[4] - 2500.0) < 40.0, "dragging the edge moves the split");

        // A region that leaps, made a band from its marker: the splits are added and the band listens for its threshold.
        removeSplit (proc.apvts, 0);
        removeSplit (proc.apvts, 0);
        pdsp::IssueFinder::Swing swing { 70.0, 160.0, 14.0f, -20.0f };
        view.makeBand (swing);
        const auto made = layoutOf (proc.apvts);
        check (made.numBands == 6 && std::fabs (made.edgeHz[1] - 70.0) < 1.0 && std::fabs (made.edgeHz[2] - 160.0) < 1.0 && readParams (proc.apvts).bands[1].on,
               "a JUMPS region of 70 to 160 Hz becomes a band with those edges, switched on");
        juce::AudioBuffer<float> audio (2, 512);
        juce::MidiBuffer noMidi;
        for (int block = 0; block < 260; ++block)
        {
            for (int i = 0; i < 512; ++i)
            {
                const float v = 0.25f * std::sin (6.2831853f * 100.0f * (float) (block * 512 + i) / 48000.0f);
                audio.setSample (0, i, v);
                audio.setSample (1, i, v);
            }
            proc.processBlock (audio, noMidi);
            if (block % 3 == 0)
                juce::MessageManager::getInstance()->runDispatchLoopUntil (34);
        }
        const float learned = (float) readParams (proc.apvts).bands[1].thresholdDb;
        const float read = proc.engine.levelMeter[1].load(); // the tone is about 3 dB down inside a 70 to 160 Hz band
        check (read < -13.0f && read > -17.0f && std::fabs (learned - (read - 7.0f)) < 0.5f,
               fmt ("... and after two seconds of a 100 Hz tone at -12 dB (%.1f dB in the band) its threshold is half the jump under that: %.1f dB", read, learned));

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
