// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 The Pantheon authors

// Builds the editor on the real processor and checks what a host and a user would meet: the parameters, the name, the controls, loading a reference file
// (and what happens with a bad one), the audio path, the state, and the notes.

#include "Advice.h"
#include "Check.h"
#include "PluginEditor.h"
#include <pantheon/ui/Scaled.h>

using namespace hermes;
using test::check;
using test::fmt;

namespace
{
// A reference file in a temporary folder: a sine of the given level, written as a WAV.
juce::File makeReference (const juce::String& name, double rate, int channels, float peakDb, double seconds = 3.0)
{
    const auto file = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile (name);
    file.deleteFile();
    juce::AudioBuffer<float> buffer (channels, (int) (rate * seconds));
    const float a = std::pow (10.0f, peakDb / 20.0f);
    for (int c = 0; c < channels; ++c)
        for (int i = 0; i < buffer.getNumSamples(); ++i)
            buffer.setSample (c, i, a * std::sin (2.0f * 3.14159265f * 1000.0f * (float) i / (float) rate));
    juce::WavAudioFormat wav;
    std::unique_ptr<juce::AudioFormatWriter> writer (wav.createWriterFor (new juce::FileOutputStream (file), rate, (unsigned) channels, 24, {}, 0));
    if (writer != nullptr)
        writer->writeFromAudioSampleBuffer (buffer, 0, buffer.getNumSamples());
    return file;
}
}

int main()
{
    juce::ScopedJuceInitialiser_GUI gui;
    std::printf ("Hermes UI check\n");
    HermesProcessor proc;
    proc.setPlayConfigDetails (2, 2, 48000.0, 512);
    proc.prepareToPlay (48000.0, 512);

    check (proc.getName() == juce::String (gui::kProjectWord) + " " + gui::kPluginWord, "the reported name is the nameplate's two words");
    check (proc.getParameters().size() == 6, fmt ("%d parameters: trim, polarity, listen, reference, level match and reference trim", proc.getParameters().size()));
    check (listenNames.size() == kNumListen, "the choice list matches the engine's modes");
    const auto d = readParams (proc.apvts);
    check (std::fabs (d.trimDb) < 1e-4f && ! d.polarity && d.listen == Listen::stereo && ! d.reference && d.levelMatch && proc.getLatencySamples() == 0, "a new instance changes nothing: trim 0, stereo, no reference playing, no latency");

    // Loading a reference.
    const auto wav = makeReference ("hermes_check_reference.wav", 44100.0, 1, -14.0f);
    {
        std::unique_ptr<juce::AudioProcessorEditor> holder (proc.createEditor());
        auto& editor = ui::innerOf<HermesEditor> (*holder);
        editor.setVisible (true);
        check (editor.getWidth() > 800 && editor.levelMatch.getRight() <= editor.getWidth() && editor.levelMatch.getBottom() < editor.getHeight(), "the controls fit the window");
        editor.loadFile (juce::File ("/nonexistent/nothing.wav"));
        check (editor.status.isNotEmpty() && proc.referenceName().isEmpty(), "a file that is not there is refused with a reason, and nothing is loaded");
        editor.loadFile (wav);
        check (editor.status.isEmpty() && proc.referenceName() == wav.getFileName(), "a WAV is loaded");
        check (std::fabs (proc.referenceLufs() + 14.04) < 0.15, fmt ("a mono 44.1 kHz file is brought to the session's 48 kHz, made stereo and measured: %.2f LUFS (a -14 dBFS sine: -14.04)", proc.referenceLufs()));
        check (ui::hasNote (editor.notes(), "LEVEL MATCH brings", ui::Severity::info), "with a reference loaded and LEVEL MATCH on, a note says to let the meter run first");
        editor.reference.triggerClick();
        juce::MessageManager::getInstance()->runDispatchLoopUntil (100);
        check (readParams (proc.apvts).reference && ui::hasNote (editor.notes(), "REF is on: you hear the reference", ui::Severity::warning), "REF sets the parameter, and a note says you hear the reference");
        editor.reference.triggerClick();
        editor.listen.setSelectedItemIndex ((int) Listen::mono, juce::sendNotificationSync);
        check (readParams (proc.apvts).listen == Listen::mono && ui::hasNote (editor.notes(), "LISTEN is not Stereo", ui::Severity::warning), "LISTEN sets the parameter, and a note says it is a check");
        editor.listen.setSelectedItemIndex ((int) Listen::stereo, juce::sendNotificationSync);
    }

    // The state keeps the file, and a processor that is given it loads it.
    {
        setValue (proc.apvts, id::trim, -4.5f);
        juce::MemoryBlock saved;
        proc.getStateInformation (saved);
        HermesProcessor other;
        other.setPlayConfigDetails (2, 2, 48000.0, 512);
        other.prepareToPlay (48000.0, 512);
        other.setStateInformation (saved.getData(), (int) saved.getSize());
        juce::MessageManager::getInstance()->runDispatchLoopUntil (800);
        check (std::fabs (readParams (other.apvts).trimDb + 4.5f) < 0.01f && other.referenceName() == wav.getFileName(), "the state of a session comes back as it was saved, and its reference is loaded again from its path");
    }

    // The audio path through the real processor: the trim, and the reference at the loudness of the sound.
    {
        HermesProcessor q;
        q.setPlayConfigDetails (2, 2, 48000.0, 512);
        q.prepareToPlay (48000.0, 512);
        q.loadReference (wav);
        setValue (q.apvts, id::trim, 6.0f);
        juce::AudioBuffer<float> buffer (2, 512);
        juce::MidiBuffer midi;
        float peak = 0.0f;
        for (int block = 0; block < 600; ++block)
        {
            if (block == 300)
                setValue (q.apvts, id::reference, 1.0f);
            for (int i = 0; i < 512; ++i)
                for (int c = 0; c < 2; ++c)
                    buffer.setSample (c, i, 0.05f * std::sin (2.0f * 3.14159265f * 500.0f * (float) (block * 512 + i) / 48000.0f));
            q.processBlock (buffer, midi);
            if (block == 250)
                peak = buffer.getMagnitude (0, 512);
        }
        check (std::fabs (20.0f * std::log10 (peak / 0.05f) - 6.0f) < 0.05f, fmt ("TRIM +6 dB through the processor: %.2f dB", 20.0f * std::log10 (peak / 0.05f)));
        check (q.engine.integrated.load() > -40.0f && q.engine.integrated.load() < -20.0f && std::fabs (q.engine.referenceMatchDb.load()) > 1.0f,
               fmt ("the meters read the sound (%.1f LUFS integrated) and the reference is matched to it (%+.1f dB)", q.engine.integrated.load(), q.engine.referenceMatchDb.load()));
    }

    // The notes.
    {
        AdviceInput in;
        check (adviceFor (in).empty(), "a default instance has nothing to warn about");
        in.truePeakDb = 0.4;
        in.integratedLufs = -7.5;
        in.rangeLu = 2.0;
        in.params.polarity = true;
        check (ui::hasNote (adviceFor (in), "over -1 dBTP", ui::Severity::warning) && ui::hasNote (adviceFor (in), "Louder than -9 LUFS", ui::Severity::warning) && ui::hasNote (adviceFor (in), "under 3 LU", ui::Severity::info) &&
                   ui::hasNote (adviceFor (in), "POLARITY inverts", ui::Severity::info),
               "a true peak over -1 dBTP, a master over -9 LUFS, a flat range and an inverted polarity are flagged");
        in.integratedLufs = -30.0;
        in.params.reference = true;
        check (ui::hasNote (adviceFor (in), "no reference is loaded", ui::Severity::warning) && ui::hasNote (adviceFor (in), "Under -24 LUFS", ui::Severity::info), "REF with nothing loaded, and a quiet master, are flagged");
        bool tooLong = false;
        in.referenceLoaded = true;
        in.params.listen = Listen::side;
        for (const auto& n : adviceFor (in))
            if ((int) n.text.size() > ui::kMaxNoteChars)
            {
                tooLong = true;
                std::printf ("  too long (%d): %s\n", (int) n.text.size(), n.text.c_str());
            }
        in.integratedLufs = -7.5;
        in.params.levelMatch = true;
        for (const auto& n : adviceFor (in))
            if ((int) n.text.size() > ui::kMaxNoteChars)
            {
                tooLong = true;
                std::printf ("  too long (%d): %s\n", (int) n.text.size(), n.text.c_str());
            }
        check (! tooLong, "every note fits a line of the strip");
    }
    wav.deleteFile();

    return test::finish ("HermesUiCheck");
}
