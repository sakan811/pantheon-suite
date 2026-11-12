// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 The Pantheon authors

// Builds the editor on the real processor and checks what a host and a user would meet: the parameters, the name, the controls, the latency
// the host is told, mono, the state, and the notes.

#include "Advice.h"
#include "Check.h"
#include "PluginEditor.h"
#include <pantheon/ui/Scaled.h>

using namespace aegis;
using test::check;
using test::fmt;

int main()
{
    juce::ScopedJuceInitialiser_GUI gui;
    std::printf ("Aegis UI check\n");
    AegisProcessor proc;
    proc.setPlayConfigDetails (2, 2, 48000.0, 512);
    proc.prepareToPlay (48000.0, 512);

    check (proc.getName() == juce::String (gui::kProjectWord) + " " + gui::kPluginWord, "the reported name is the nameplate's two words");
    check (proc.getParameters().size() == 5, fmt ("%d parameters: gain, ceiling, release, look-ahead and true peak", proc.getParameters().size()));
    check (lookaheadNames().size() == kNumLookahead, "the look-ahead choices match the engine's");
    const auto d = readParams (proc.apvts);
    check (std::fabs (d.gainDb) < 1e-4f && std::fabs (d.ceilingDb + 1.0f) < 1e-4f && std::fabs (d.releaseMs - 100.0f) < 0.01f && d.lookahead == kDefaultLookahead && d.truePeak,
           fmt ("a new instance has GAIN 0, CEILING -1 dBTP, RELEASE 100 ms, 2 ms of look-ahead and TRUE PEAK on (%.3f, %.3f, %.3f, %d, %d)", d.gainDb, d.ceilingDb, d.releaseMs, d.lookahead, (int) d.truePeak));

    // The latency the host is told is the engine's, and follows the look-ahead.
    check (proc.getLatencySamples() == lookaheadSamples (48000.0, kDefaultLookahead) + kDetectorDelay, fmt ("the host is told %d samples of latency", proc.getLatencySamples()));
    {
        setValue (proc.apvts, id::lookahead, 3.0f);
        juce::AudioBuffer<float> buffer (2, 512);
        buffer.clear();
        juce::MidiBuffer midi;
        proc.processBlock (buffer, midi);
        juce::MessageManager::getInstance()->runDispatchLoopUntil (100);
        check (proc.getLatencySamples() == lookaheadSamples (48000.0, 3) + kDetectorDelay, fmt ("after LOOK-AHEAD is set to 5 ms the host is told %d samples", proc.getLatencySamples()));
        setValue (proc.apvts, id::lookahead, (float) kDefaultLookahead);
    }

    // The ceiling holds through the real processor, in stereo and in mono.
    {
        AegisProcessor q;
        q.setPlayConfigDetails (2, 2, 48000.0, 512);
        q.prepareToPlay (48000.0, 512);
        setValue (q.apvts, id::gain, 12.0f);
        juce::AudioBuffer<float> buffer (2, 512);
        juce::MidiBuffer midi;
        float peak = 0.0f;
        for (int block = 0; block < 200; ++block)
        {
            for (int i = 0; i < 512; ++i)
                for (int c = 0; c < 2; ++c)
                    buffer.setSample (c, i, 0.7f * std::sin (0.07f * (float) (block * 512 + i) * (float) (c + 1)));
            q.processBlock (buffer, midi);
            if (block > 20)
                peak = std::max (peak, buffer.getMagnitude (0, 512));
        }
        check (peak <= std::pow (10.0f, -1.0f / 20.0f) + 1e-4f, fmt ("a loud tone through the processor stays under -1 dBFS (%.3f)", 20.0f * std::log10 (peak)));
        check (proc.isBusesLayoutSupported ([&] { auto l = proc.getBusesLayout(); l.getChannelSet (true, 0) = juce::AudioChannelSet::mono(); l.getChannelSet (false, 0) = juce::AudioChannelSet::mono(); return l; }()),
               "a mono layout is supported");
    }

    {
        std::unique_ptr<juce::AudioProcessorEditor> holder (proc.createEditor());
        auto& editor = ui::innerOf<AegisEditor> (*holder);
        editor.setVisible (true);
        check (editor.lookaheadBox.getNumItems() == kNumLookahead, "the look-ahead selector has the parameter's items");
        check (editor.getWidth() > 600 && editor.releaseKnob.getRight() < editor.lookaheadBox.getX() && editor.truePeakButton.getBottom() < editor.getHeight(), "the controls fit the window");
        editor.truePeakButton.triggerClick();
        juce::MessageManager::getInstance()->runDispatchLoopUntil (100);
        check (! readParams (proc.apvts).truePeak, "the TRUE PEAK switch sets the parameter");
        check (ui::hasNote (editor.notes(), "TRUE PEAK is off", ui::Severity::warning), "with TRUE PEAK off, a note says the sound between the samples can go over");
        editor.truePeakButton.triggerClick();
    }

    // The state survives a round trip.
    {
        setValue (proc.apvts, id::ceiling, -3.5f);
        setValue (proc.apvts, id::release, 250.0f);
        juce::MemoryBlock saved;
        proc.getStateInformation (saved);
        AegisProcessor other;
        other.setStateInformation (saved.getData(), (int) saved.getSize());
        check (std::fabs (readParams (other.apvts).ceilingDb + 3.5f) < 0.01f && std::fabs (readParams (other.apvts).releaseMs - 250.0f) < 1.0f, "the state of a session comes back as it was saved");
    }

    // The notes.
    {
        AdviceInput in;
        check (adviceFor (in).size() == 1 && ui::hasNote (adviceFor (in), "Nothing is being limited", ui::Severity::info), "a default instance with nothing to limit says only that");
        in.params.gainDb = 6.0f;
        in.heldDb = 8.0f;
        check (ui::hasNote (adviceFor (in), "Taking off 8 dB", ui::Severity::warning), "8 dB of reduction is called squashing");
        in.params.ceilingDb = -0.1f;
        check (ui::hasNote (adviceFor (in), "ceiling above -0.3", ui::Severity::warning), "a ceiling above -0.3 dBTP warns about codecs");
        in.params.releaseMs = 20.0f;
        check (ui::hasNote (adviceFor (in), "RELEASE under 40", ui::Severity::info), "a fast release with this much reduction can distort bass");
        in.params.gainDb = 0.0f;
        in.heldDb = 0.0f;
        in.params.releaseMs = 100.0f;
        check (ui::hasNote (adviceFor (in), "Nothing is being limited", ui::Severity::info), "nothing limited: it says to raise GAIN");
        in.latencyMs = 2.7;
        bool tooLong = false;
        const auto longest = [&] { for (const auto& n : adviceFor (in)) if ((int) n.text.size() > ui::kMaxNoteChars) { tooLong = true; std::printf ("  too long (%d): %s\n", (int) n.text.size(), n.text.c_str()); } };
        longest();
        in.params.truePeak = false;
        in.heldDb = 12.0f;
        in.params.ceilingDb = 0.0f;
        longest();
        check (! tooLong, "every note fits a line of the strip");
    }

    return test::finish ("AegisUiCheck");
}
