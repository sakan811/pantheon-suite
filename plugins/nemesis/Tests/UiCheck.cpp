// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 The Pantheon authors

// Builds the editor on the real processor and checks what a host and a user would meet: the parameters, the name, the controls, the audio path,
// the state, and the notes.

#include "Advice.h"
#include "Check.h"
#include "PluginEditor.h"
#include <pantheon/ui/Scaled.h>

using namespace nemesis;
using test::check;
using test::fmt;

int main()
{
    juce::ScopedJuceInitialiser_GUI gui;
    std::printf ("Nemesis UI check\n");
    NemesisProcessor proc;
    proc.setPlayConfigDetails (2, 2, 48000.0, 512);
    proc.prepareToPlay (48000.0, 512);

    check (proc.getName() == juce::String (gui::kProjectWord) + " " + gui::kPluginWord, "the reported name is the nameplate's two words");
    check (proc.getParameters().size() == 8, fmt ("%d parameters: frequency, width, threshold, range, release, mode, detection and listen", proc.getParameters().size()));
    check (modeNames.size() == kNumModes && detectionNames.size() == kNumDetections, "the choice lists match the engine's");
    const auto d = readParams (proc.apvts);
    check (std::fabs (d.freqHz - 6500.0f) < 5.0f && std::fabs (d.q - 1.5f) < 0.01f && std::fabs (d.thresholdDb + 30.0f) < 0.01f && std::fabs (d.rangeDb - 8.0f) < 0.01f && std::fabs (d.releaseMs - 40.0f) < 0.01f &&
               d.mode == Mode::splitBand && d.detection == Detection::absolute && ! d.listen && proc.getLatencySamples() == 0,
           "a new instance: 6.5 kHz, Q 1.5, -30 dB, 8 dB range, 40 ms, split-band, absolute, no latency");

    // The audio path through the real processor: a loud tone in the band is turned down, a quiet one is not.
    {
        const auto gainOf = [] (float levelDb)
        {
            NemesisProcessor q;
            q.setPlayConfigDetails (2, 2, 48000.0, 512);
            q.prepareToPlay (48000.0, 512);
            setValue (q.apvts, id::range, 24.0f);
            juce::AudioBuffer<float> buffer (2, 512);
            juce::MidiBuffer midi;
            const float a = std::pow (10.0f, levelDb / 20.0f);
            float peak = 0.0f;
            for (int block = 0; block < 200; ++block)
            {
                for (int i = 0; i < 512; ++i)
                    for (int c = 0; c < 2; ++c)
                        buffer.setSample (c, i, a * std::sin (2.0f * 3.14159265f * 6500.0f * (float) (block * 512 + i) / 48000.0f));
                q.processBlock (buffer, midi);
                if (block > 100)
                    peak = std::max (peak, buffer.getMagnitude (0, 512));
            }
            return 20.0f * std::log10 (peak / a);
        };
        const float loud = gainOf (-20.0f), quiet = gainOf (-50.0f);
        check (std::fabs (loud + 10.0f) < 0.5f && std::fabs (quiet) < 0.01f, fmt ("through the processor: a sibilant at -20 dBFS is turned down by %.1f dB (10 by the law), one at -50 dBFS by %.3f dB", -loud, -quiet));
    }

    {
        std::unique_ptr<juce::AudioProcessorEditor> holder (proc.createEditor());
        auto& editor = ui::innerOf<NemesisEditor> (*holder);
        editor.setVisible (true);
        check (editor.mode.getNumItems() == kNumModes && editor.detection.getNumItems() == kNumDetections, "the selectors have the parameters' items");
        check (editor.getWidth() > 700 && editor.release.getRight() < editor.mode.getX() && editor.listen.getBottom() < editor.getHeight(), "the controls fit the window");
        editor.listen.triggerClick();
        juce::MessageManager::getInstance()->runDispatchLoopUntil (100);
        check (readParams (proc.apvts).listen && ui::hasNote (editor.notes(), "LISTEN is on", ui::Severity::warning), "LISTEN sets the parameter, and a note says you hear only the band");
        editor.listen.triggerClick();
    }

    // The state survives a round trip.
    {
        setValue (proc.apvts, id::freq, 8200.0f);
        setValue (proc.apvts, id::detection, 1.0f);
        juce::MemoryBlock saved;
        proc.getStateInformation (saved);
        NemesisProcessor other;
        other.setStateInformation (saved.getData(), (int) saved.getSize());
        check (std::fabs (readParams (other.apvts).freqHz - 8200.0f) < 10.0f && readParams (other.apvts).detection == Detection::relative, "the state of a session comes back as it was saved");
    }

    // The notes.
    {
        AdviceInput in;
        check (adviceFor (in).empty(), "a default instance has nothing to warn about");
        in.params.rangeDb = 0.0f;
        in.params.thresholdDb = 0.0f;
        in.heldDb = 12.0f;
        check (ui::hasNote (adviceFor (in), "RANGE is 0 dB", ui::Severity::warning) && ui::hasNote (adviceFor (in), "THRESH is at the top", ui::Severity::warning) && ui::hasNote (adviceFor (in), "makes a lisp", ui::Severity::warning),
               "RANGE 0, THRESH at the top and 12 dB of reduction are warned about");
        in.params.mode = Mode::wideband;
        in.params.detection = Detection::relative;
        in.params.freqHz = 3000.0f;
        check (ui::hasNote (adviceFor (in), "WIDEBAND turns the whole voice", ui::Severity::info) && ui::hasNote (adviceFor (in), "RELATIVE reads", ui::Severity::info) && ui::hasNote (adviceFor (in), "Under 4.5 kHz", ui::Severity::info),
               "wideband, relative detection and a low frequency are explained");
        bool tooLong = false;
        in.params.listen = true;
        for (const auto& n : adviceFor (in))
            if ((int) n.text.size() > ui::kMaxNoteChars)
            {
                tooLong = true;
                std::printf ("  too long (%d): %s\n", (int) n.text.size(), n.text.c_str());
            }
        check (! tooLong, "every note fits a line of the strip");
    }

    return test::finish ("NemesisUiCheck");
}
