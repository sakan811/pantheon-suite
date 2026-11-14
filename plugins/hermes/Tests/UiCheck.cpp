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
    check (proc.getParameters().size() == 7, fmt ("%d parameters: trim, polarity, listen, reference, level match, reference trim and auto reset", proc.getParameters().size()));
    check (listenNames.size() == kNumListen, "the choice list matches the engine's modes");
    const auto d = readParams (proc.apvts);
    check (std::fabs (d.trimDb) < 1e-4f && ! d.polarity && d.listen == Listen::stereo && ! d.reference && d.levelMatch && ! d.resetOnPlay && proc.getLatencySamples() == 0, "a new instance changes nothing: trim 0, stereo, no reference playing, no latency");

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
        editor.seekTo (editor.barPoint (0.5f));
        {
            juce::AudioBuffer<float> b (2, 64);
            juce::MidiBuffer m;
            b.clear();
            proc.processBlock (b, m);
            check (std::fabs (proc.engine.referencePosition.load() - 0.5f) < 0.02f, fmt ("a click in the middle of the reference's bar seeks to it: %.3f", proc.engine.referencePosition.load()));
        }
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

    // A state with no reference (or whose file is gone) clears the one that is loaded.
    {
        HermesProcessor other;
        other.setPlayConfigDetails (2, 2, 48000.0, 512);
        other.prepareToPlay (48000.0, 512);
        other.loadReference (wav);
        check (other.referenceName().isNotEmpty(), "(a reference is loaded)");
        HermesProcessor bare;
        juce::MemoryBlock none;
        bare.getStateInformation (none);
        other.setStateInformation (none.getData(), (int) none.getSize());
        check (other.referenceName().isEmpty(), "a session saved without a reference opens without one, not with the last session's");
        other.loadReference (wav);
        const auto gone = makeReference ("hermes_check_gone.wav", 48000.0, 2, -20.0, 0.5);
        other.loadReference (gone);
        juce::MemoryBlock withGone;
        other.getStateInformation (withGone);
        gone.deleteFile();
        other.loadReference (wav);
        other.setStateInformation (withGone.getData(), (int) withGone.getSize());
        juce::MessageManager::getInstance()->runDispatchLoopUntil (200);
        check (other.referenceName().isEmpty(), "a session whose reference file has gone opens without a reference");
    }

    // AUTO TRIM sets TRIM from the integrated loudness; AUTO RESET clears the meters when the transport starts.
    {
        HermesProcessor q;
        q.setPlayConfigDetails (2, 2, 48000.0, 512);
        q.prepareToPlay (48000.0, 512);
        std::unique_ptr<juce::AudioProcessorEditor> holder (q.createEditor());
        auto& editor = ui::innerOf<HermesEditor> (*holder);
        editor.setVisible (true);
        juce::AudioBuffer<float> buffer (2, 512);
        juce::MidiBuffer midi;
        struct Head : juce::AudioPlayHead
        {
            bool playing = false;
            juce::Optional<PositionInfo> getPosition() const override { PositionInfo i; i.setIsPlaying (playing); return i; }
        } head;
        q.setPlayHead (&head);
        const auto play = [&] (int blocks)
        {
            for (int b = 0; b < blocks; ++b)
            {
                for (int i = 0; i < 512; ++i)
                    for (int c = 0; c < 2; ++c)
                        buffer.setSample (c, i, 0.05f * std::sin (2.0f * 3.14159265f * 500.0f * (float) (b * 512 + i) / 48000.0f));
                q.processBlock (buffer, midi);
            }
        };
        play (200);
        const float measured = q.engine.integrated.load();
        editor.autoTrimButton.triggerClick();
        juce::MessageManager::getInstance()->runDispatchLoopUntil (100);
        check (std::fabs (readParams (q.apvts).trimDb - (-14.0f - measured)) < 0.06f, fmt ("AUTO TRIM sets TRIM to %.1f dB (-14 target, %.1f LUFS read)", readParams (q.apvts).trimDb, measured));
        check (editor.autoTrimButton.getRight() <= editor.getWidth() && editor.autoReset.getBottom() <= editor.getHeight() && ! editor.autoTrimButton.getBounds().intersects (editor.autoReset.getBounds()) && ! editor.autoTrimButton.getBounds().intersects (editor.resetButton.getBounds()), "AUTO TRIM and AUTO RESET fit and overlap nothing");

        play (5);
        head.playing = false;
        play (1);
        head.playing = true;
        play (1);
        check (q.engine.integrated.load() > -150.0f, "with AUTO RESET off, the transport starting leaves the meters alone");
        setValue (q.apvts, id::resetOnPlay, 1.0f);
        head.playing = false;
        play (1);
        head.playing = true;
        play (1);
        const float after = q.engine.integrated.load();
        play (200);
        check (after < -150.0f, fmt ("with AUTO RESET on, the transport starting clears the integrated loudness (%.1f LUFS right after)", after));
        q.setPlayHead (nullptr);
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

    // Presets: each applies, has a name and a one-line purpose, the chosen one is kept with the session, and the box is in the header.
    {
        HermesProcessor q;
        q.setPlayConfigDetails (2, 2, 48000.0, 512);
        q.prepareToPlay (48000.0, 512);
        bool all = q.getNumPrograms() == (int) presets().size() && q.getNumPrograms() >= 6;
        for (int i = 0; i < q.getNumPrograms(); ++i)
        {
            q.setCurrentProgram (i);
            const auto len = std::strlen (presets()[(size_t) i].purpose);
            all = all && q.getProgramName (i).isNotEmpty() && q.getCurrentProgram() == i && len > 20 && len <= 140;
        }
        check (all, fmt ("all %d presets apply, have a name and a one-line purpose", q.getNumPrograms()));
        const auto indexOf = [&] (const char* name)
        {
            for (int i = 0; i < q.getNumPrograms(); ++i)
                if (q.getProgramName (i) == name)
                    return i;
            check (false, std::string ("a preset called ") + name);
            return 0;
        };
        const auto raw = [&] (const juce::String& pid) { return q.apvts.getRawParameterValue (pid)->load(); };
        setValue (q.apvts, id::trim, 3.0f);
        q.setCurrentProgram (indexOf ("Mono Check"));
        check ((int) raw (id::listen) == 1 && std::fabs (raw (id::trim) - 3.0f) < 0.01f, "Mono Check listens to the mono sum, and TRIM is kept");
        q.setCurrentProgram (indexOf ("Reference A/B"));
        check ((int) raw (id::listen) == 0 && raw (id::reference) > 0.5f && raw (id::levelMatch) > 0.5f, "Reference A/B: the reference on and level matched, and LISTEN back on Stereo");
        q.setCurrentProgram (0);
        check ((int) raw (id::listen) == 0 && raw (id::reference) < 0.5f && raw (id::levelMatch) > 0.5f && std::fabs (raw (id::trim) - 3.0f) < 0.01f, "Normal: Stereo, the reference off, LEVEL MATCH back on (its default), TRIM kept");
        q.setCurrentProgram (indexOf ("Reference As Is"));
        check (raw (id::reference) > 0.5f && raw (id::levelMatch) < 0.5f, "Reference As Is: the reference at its own level (LEVEL MATCH off)");
        const int chosen = indexOf ("Side Only");
        q.setCurrentProgram (chosen);
        check ((int) raw (id::listen) == 4, "Side Only listens to the sides");
        juce::MemoryBlock saved;
        q.getStateInformation (saved);
        HermesProcessor other;
        other.setStateInformation (saved.getData(), (int) saved.getSize());
        check (other.getCurrentProgram() == chosen, "the chosen preset is saved with the session");
        std::unique_ptr<juce::AudioProcessorEditor> holder (q.createEditor());
        auto& editor = ui::innerOf<HermesEditor> (*holder);
        editor.setVisible (true);
        check (editor.presetBox.getNumItems() == q.getNumPrograms() && editor.presetBox.getSelectedId() == chosen + 1, "the editor's preset box lists the presets and shows the current one");
        check (editor.presetBox.getRight() < editor.getWidth() && ! editor.presetBox.getBounds().intersects (editor.guideButton.getBounds()), "the preset box fits the header beside the GUIDE button");
    }

    return test::finish ("HermesUiCheck");
}
