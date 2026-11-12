// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 The Pantheon authors

// Builds the editor on the real processor and checks what a host and a user would meet: the parameters, the name, the strips, the node on the display,
// the latency the host is told, the state, and the notes.

#include "Advice.h"
#include "Check.h"
#include "PluginEditor.h"
#include <pantheon/ui/Scaled.h>

using namespace apollo;
using test::check;
using test::fmt;

int main()
{
    juce::ScopedJuceInitialiser_GUI gui;
    std::printf ("Apollo UI check\n");
    ApolloProcessor proc;
    proc.setPlayConfigDetails (2, 2, 48000.0, 512);
    proc.prepareToPlay (48000.0, 512);

    check (proc.getName() == juce::String (gui::kProjectWord) + " " + gui::kPluginWord, "the reported name is the nameplate's two words");
    check (proc.getParameters().size() == 2 + 7 * kBands, fmt ("%d parameters: phase, output and 7 for each of %d bands", proc.getParameters().size(), kBands));
    check (typeNames.size() == kNumTypes && targetNames.size() == kNumTargets && slopeNames.size() == kNumSlopes && phaseNames.size() == kNumPhases, "the choice lists match the engine's");
    const auto d = readParams (proc.apvts);
    bool defaultsOk = d.phase == Phase::minimum && std::fabs (d.outDb) < 1e-4;
    for (int i = 0; i < kBands; ++i)
    {
        const auto want = defaultBand (i);
        const auto& got = d.bands[(size_t) i];
        defaultsOk = defaultsOk && got.on == want.on && got.type == want.type && std::fabs (got.freqHz - want.freqHz) < want.freqHz * 1e-3 && std::fabs (got.gainDb) < 1e-4;
    }
    check (defaultsOk, "a new instance has the bands of defaultBand(), all at 0 dB, minimum phase, output 0");
    check (std::none_of (d.bands.begin(), d.bands.end(), [] (const BandParams& b) { return ! isIdentity (b); }), "... and none of them changes the sound");

    // The latency the host is told follows the phase mode.
    check (proc.getLatencySamples() == 0, "minimum phase: the host is told 0 samples");
    {
        setValue (proc.apvts, id::phase, 1.0f);
        juce::AudioBuffer<float> buffer (2, 512);
        buffer.clear();
        juce::MidiBuffer midi;
        proc.processBlock (buffer, midi);
        juce::MessageManager::getInstance()->runDispatchLoopUntil (100);
        check (proc.getLatencySamples() == linearSizesFor (48000.0).latency(), fmt ("linear phase: the host is told %d samples", proc.getLatencySamples()));
        setValue (proc.apvts, id::phase, 0.0f);
    }

    {
        std::unique_ptr<juce::AudioProcessorEditor> holder (proc.createEditor());
        auto& editor = ui::innerOf<ApolloEditor> (*holder);
        editor.setVisible (true);
        check (editor.getWidth() > 900 && editor.strips[7]->bounds.getRight() <= editor.getWidth() && editor.strips[0]->type.getNumItems() == kNumTypes, "eight strips fit the window and have the parameters' items");
        check (editor.strips[3]->gain.isVisible() && editor.strips[3]->q.isVisible() && ! editor.strips[3]->slope.isVisible(), "a bell shows GAIN and Q");
        check (! editor.strips[0]->gain.isVisible() && ! editor.strips[0]->q.isVisible() && editor.strips[0]->slope.isVisible(), "a high-pass shows its slope, and no GAIN or Q");

        // A node on the display: dragging it sets the frequency and the gain.
        auto& view = editor.view;
        const auto node = view.nodeOf (3); // band 4: a bell at 1 kHz
        check (view.nodeNear (node) == 3 && std::fabs (view.hzAt (node.x) - 1000.0) < 10.0, "band 4's node is on the display at 1 kHz");
        view.dragTo (3, { view.xOf (2000.0), view.yOf (6.0) });
        const auto b = readParams (proc.apvts).bands[3];
        check (std::fabs (b.freqHz - 2000.0) < 20.0 && std::fabs (b.gainDb - 6.0) < 0.1, fmt ("dragging the node to 2 kHz, +6 dB sets the band (%.0f Hz, %+.1f dB)", b.freqHz, b.gainDb));
        view.dragTo (0, { view.xOf (60.0), view.yOf (9.0) });
        check (std::fabs (readParams (proc.apvts).bands[0].freqHz - 60.0) < 1.0 && std::fabs (readParams (proc.apvts).bands[0].gainDb) < 1e-3f, "dragging a high-pass's node moves its frequency only (it has no gain)");
        view.mouseDoubleClick (juce::MouseEvent (juce::Desktop::getInstance().getMainMouseSource(), view.nodeOf (3), juce::ModifierKeys(), 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, &view, &view,
                                                  juce::Time::getCurrentTime(), view.nodeOf (3), juce::Time::getCurrentTime(), 1, false));
        check (! readParams (proc.apvts).bands[3].on, "a double-click on a node switches its band off");
        setValue (proc.apvts, bandIds (3).on, 1.0f);
        setValue (proc.apvts, bandIds (3).type, (float) (int) Type::highPass);
        editor.showTypes();
        check (editor.strips[3]->slope.isVisible() && ! editor.strips[3]->gain.isVisible(), "a strip changes its controls with its type");
    }

    // The state survives a round trip.
    {
        setValue (proc.apvts, bandIds (2).gain, -7.5f);
        setValue (proc.apvts, bandIds (2).target, 2.0f);
        setValue (proc.apvts, id::out, 3.0f);
        juce::MemoryBlock saved;
        proc.getStateInformation (saved);
        ApolloProcessor other;
        other.setStateInformation (saved.getData(), (int) saved.getSize());
        const auto o = readParams (other.apvts);
        check (std::fabs (o.bands[2].gainDb + 7.5f) < 0.01 && o.bands[2].target == Target::side && std::fabs (o.outDb - 3.0) < 0.01, "the state of a session comes back as it was saved");
    }

    // The audio path through the real processor: a boost comes out.
    {
        ApolloProcessor q;
        q.setPlayConfigDetails (2, 2, 48000.0, 512);
        q.prepareToPlay (48000.0, 512);
        setValue (q.apvts, bandIds (3).gain, 12.0f);
        juce::AudioBuffer<float> buffer (2, 512);
        juce::MidiBuffer midi;
        float peak = 0.0f;
        for (int block = 0; block < 100; ++block)
        {
            for (int i = 0; i < 512; ++i)
                for (int c = 0; c < 2; ++c)
                    buffer.setSample (c, i, 0.1f * std::sin (2.0f * 3.14159265f * 1000.0f * (float) (block * 512 + i) / 48000.0f));
            q.processBlock (buffer, midi);
            if (block > 50)
                peak = std::max (peak, buffer.getMagnitude (0, 512));
        }
        check (std::fabs (20.0f * std::log10 (peak / 0.1f) - 12.0f) < 0.1f, fmt ("a 12 dB bell at 1 kHz lifts a tone through the processor by %.2f dB", 20.0f * std::log10 (peak / 0.1f)));
    }

    // The notes.
    {
        AdviceInput in;
        check (adviceFor (in).size() == 1 && ui::hasNote (adviceFor (in), "No band changes anything", ui::Severity::info), "a default instance says that nothing is changed yet");
        in.params.bands[3] = { true, Type::bell, 1000.0, 10.0, 1.0, 0, Target::stereo };
        check (ui::hasNote (adviceFor (in), "Band 4: a boost of 10 dB", ui::Severity::warning), "a 10 dB boost is flagged");
        in.params.bands[4] = { true, Type::bell, 1500.0, 4.0, 8.0, 0, Target::stereo };
        check (ui::hasNote (adviceFor (in), "add up to 14 dB", ui::Severity::warning) && ui::hasNote (adviceFor (in), "narrow bell", ui::Severity::info), "boosts within an octave add up, and a narrow bell is said to ring");
        in.params.phase = Phase::linear;
        in.params.bands[1] = { true, Type::lowShelf, 40.0, 3.0, 0.7, 0, Target::stereo };
        in.latencyMs = 48.0;
        check (ui::hasNote (adviceFor (in), "linear phase resolves", ui::Severity::info) && ui::hasNote (adviceFor (in), "delays the sound by 48 ms", ui::Severity::info), "linear phase: a low band is called soft, and the delay is said");
        bool tooLong = false;
        for (const auto& n : adviceFor (in))
            if ((int) n.text.size() > ui::kMaxNoteChars)
            {
                tooLong = true;
                std::printf ("  too long (%d): %s\n", (int) n.text.size(), n.text.c_str());
            }
        in.params.bands[0] = { true, Type::highPass, 30.0, 0.0, 0.7, 3, Target::stereo };
        for (const auto& n : adviceFor (in))
            if ((int) n.text.size() > ui::kMaxNoteChars)
            {
                tooLong = true;
                std::printf ("  too long (%d): %s\n", (int) n.text.size(), n.text.c_str());
            }
        check (! tooLong, "every note fits a line of the strip");
    }

    return test::finish ("ApolloUiCheck");
}
