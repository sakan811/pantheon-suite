// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 The Pantheon authors

// Renders the real window to a PNG with a synthetic singer who is out of tune, so the display shows what the app is for.
//
//   OrpheusSnapshot <out.png> [corrected] [select=N,M] [file=audio.wav]
//
//   corrected   pull every note onto C major first (what CORRECT ALL does)
//   select=N,M  select notes by number (1-based)
//   file=PATH   show a real recording instead of the synthetic singer

#include "TestSignals.h"
#include "UI/MainComponent.h"

int main (int argc, char** argv)
{
    if (argc < 2)
    {
        std::printf ("usage: OrpheusSnapshot <out.png> [corrected] [select=N,M]\n");
        return 2;
    }
    juce::ScopedJuceInitialiser_GUI gui;
    orpheus::gui::MainComponent window (false);
    window.setSize (orpheus::gui::MainComponent::kDesignWidth, orpheus::gui::MainComponent::kDesignHeight);

    const auto phrase = test::outOfTunePhrase();
    juce::String audioFile;
    for (int i = 2; i < argc; ++i)
        if (juce::String (argv[i]).startsWith ("file="))
            audioFile = juce::String (argv[i]).fromFirstOccurrenceOf ("=", false, false);
    double rate = 48000.0;
    if (audioFile.isNotEmpty())
    {
        juce::String error;
        if (! window.session.load (juce::File (audioFile), error))
        {
            std::printf ("%s\n", error.toRawUTF8());
            return 1;
        }
        rate = window.session.sampleRate;
    }
    else
    {
        const auto voice = test::sing (phrase, 7.0);
        juce::AudioBuffer<float> audio (1, (int) voice.audio.size());
        std::copy (voice.audio.begin(), voice.audio.end(), audio.getWritePointer (0));
        window.session.adoptAudio (std::move (audio), voice.rate, juce::File ("phrase.wav"));
    }
    window.analyseNow();
    window.session.key.root = 0;
    window.session.key.scale = orpheus::kMajor;

    std::vector<size_t> selection;
    bool corrected = false;
    for (int i = 2; i < argc; ++i)
    {
        const juce::String arg (argv[i]);
        if (arg == "corrected")
            corrected = true;
        else if (arg.startsWith ("select="))
            for (const auto& s : juce::StringArray::fromTokens (arg.fromFirstOccurrenceOf ("=", false, false), ",", ""))
                selection.push_back ((size_t) s.getIntValue() - 1);
    }
    if (corrected)
        window.session.correct (1.0f);
    // one note is left half-corrected, so the ghost of where it was sung shows
    if (corrected && window.session.notes.size() > 5)
        window.session.notes[5].target = window.session.notes[5].centre + 0.5f * (window.session.notes[5].target - window.session.notes[5].centre);
    window.session.changedAll();
    window.roll.select (selection);
    window.refreshScalePanel();
    window.refreshInspector();
    window.playback.setNextReadPosition ((juce::int64) (1.9 * rate));
    juce::MessageManager::getInstance()->runDispatchLoopUntil (300);

    const auto image = window.createComponentSnapshot (window.getLocalBounds(), true, 1.5f);
    juce::File out (argv[1]);
    out.deleteFile();
    juce::FileOutputStream stream (out);
    juce::PNGImageFormat png;
    const bool ok = stream.openedOk() && png.writeImageToStream (image, stream);
    std::printf ("%s %s\n", ok ? "wrote" : "FAILED to write", out.getFullPathName().toRawUTF8());
    return ok ? 0 : 1;
}
