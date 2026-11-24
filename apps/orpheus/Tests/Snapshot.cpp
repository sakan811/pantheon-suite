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

#include <string_view>

int main (int argc, char** argv)
{
    if (argc < 2 || (std::string_view (argv[1]) == "--frames" && argc < 3))
    {
        std::printf ("usage: OrpheusSnapshot <out.png> [corrected] [select=N,M]\n"
                     "       OrpheusSnapshot --frames <dir> [file=audio.wav]\n");
        return 2;
    }
    const bool framesMode = std::string_view (argv[1]) == "--frames";
    juce::ScopedJuceInitialiser_GUI gui;
    orpheus::gui::MainComponent window (false);
    window.setSize (orpheus::gui::MainComponent::kDesignWidth, orpheus::gui::MainComponent::kDesignHeight);

    const auto phrase = test::outOfTunePhrase();
    juce::String audioFile;
    for (int i = framesMode ? 3 : 2; i < argc; ++i)
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

    if (framesMode)
    {
        constexpr int kNumFrames = 30;
        const juce::String dirArg (argv[2]);
        const juce::File outDir = juce::File::isAbsolutePath (dirArg)
                                    ? juce::File (dirArg)
                                    : juce::File::getCurrentWorkingDirectory().getChildFile (dirArg);
        outDir.createDirectory();

        std::vector<float> uncorrected;
        uncorrected.reserve (window.session.notes.size());
        for (const auto& n : window.session.notes)
            uncorrected.push_back (n.target);

        window.session.correct (1.0f);
        if (window.session.notes.size() > 5)
            window.session.notes[5].target = window.session.notes[5].centre + 0.5f * (window.session.notes[5].target - window.session.notes[5].centre);

        std::vector<float> corrected;
        corrected.reserve (window.session.notes.size());
        for (const auto& n : window.session.notes)
            corrected.push_back (n.target);

        bool ok = true;
        juce::PNGImageFormat png;
        for (int f = 0; f < kNumFrames; ++f)
        {
            const float amount = f < 6 ? 0.0f : (f < 20 ? (float) (f - 5) / 14.0f : 1.0f);
            for (size_t i = 0; i < window.session.notes.size(); ++i)
                window.session.notes[i].target = uncorrected[i] + amount * (corrected[i] - uncorrected[i]);

            window.session.changedAll();
            if (! window.session.notes.empty())
                window.roll.select ({ (size_t) (f / 5) % window.session.notes.size() });
            window.refreshScalePanel();
            window.refreshInspector();

            const double t = 0.5 + 5.3 * ((double) f / (double) (kNumFrames - 1));
            window.playback.setNextReadPosition ((juce::int64) (t * rate));
            juce::MessageManager::getInstance()->runDispatchLoopUntil (40);

            const auto image = window.createComponentSnapshot (window.getLocalBounds(), true, 1.5f);
            auto out = outDir.getChildFile (juce::String::formatted ("frame_%03d.png", f));
            out.deleteFile();
            juce::FileOutputStream stream (out);
            ok = stream.openedOk() && png.writeImageToStream (image, stream) && ok;
        }
        std::printf ("wrote %d frames to %s\n", kNumFrames, argv[2]);
        return ok ? 0 : 1;
    }

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
