// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 The Pantheon authors

// Renders the real window to a PNG over four synthetic stems, so the picture shows what the app is for.
//
//   OlympusSnapshot <out.png> [plugins=<folder>]
//
//   plugins=FOLDER   use the suite's built plugins found in FOLDER (VST3) instead of stand-ins that carry the same names

#include "TestPlugin.h"
#include "UI/MainComponent.h"

namespace
{
// One stem of a song: `kind` 0 a vocal (a slow melody with vibrato), 1 a bass, 2 a drum loop, 3 chords.
juce::File makeStem (const juce::File& folder, const juce::String& name, int kind)
{
    const double rate = 48000.0;
    const int length = (int) (rate * 24.0);
    juce::AudioBuffer<float> b (2, length);
    juce::Random random (kind + 7);
    const double notes[] = { 220.0, 247.0, 277.2, 330.0, 293.7, 247.0, 220.0, 196.0 };
    double phase = 0.0;
    for (int i = 0; i < length; ++i)
    {
        const double t = i / rate;
        const double beat = std::fmod (t * 2.0, 1.0); // 120 bpm
        float v = 0.0f;
        const float env = (float) std::exp (-3.0 * beat);
        if (kind == 0)
        {
            const double f = notes[(size_t) (t / 3.0) % 8] * (1.0 + 0.012 * std::sin (2.0 * juce::MathConstants<double>::pi * 5.5 * t));
            phase += f / rate;
            const float gate = (float) (0.5 + 0.5 * std::sin (2.0 * juce::MathConstants<double>::pi * t / 3.0 - 1.2));
            v = gate * 0.35f * (float) (std::sin (2.0 * juce::MathConstants<double>::pi * phase) + 0.4 * std::sin (4.0 * juce::MathConstants<double>::pi * phase));
        }
        else if (kind == 1)
        {
            phase += 55.0 * (((int) (t / 1.5)) % 2 == 0 ? 1.0 : 1.335) / rate;
            v = 0.55f * (float) std::sin (2.0 * juce::MathConstants<double>::pi * phase) * (0.6f + 0.4f * env);
        }
        else if (kind == 2)
        {
            const float kick = (beat < 0.25) ? (float) std::sin (2.0 * juce::MathConstants<double>::pi * (60.0 + 80.0 * std::exp (-30.0 * beat)) * beat) * env : 0.0f;
            const float hat = std::fmod (t * 4.0, 1.0) < 0.06 ? (random.nextFloat() - 0.5f) * 0.35f : 0.0f;
            v = 0.8f * kick + hat;
        }
        else
        {
            const double f = 130.8 * (1.0 + 0.5 * (((int) (t / 6.0)) % 2));
            phase += f / rate;
            v = 0.16f * (float) (std::sin (2.0 * juce::MathConstants<double>::pi * phase) + std::sin (2.0 * juce::MathConstants<double>::pi * phase * 1.26) + std::sin (2.0 * juce::MathConstants<double>::pi * phase * 1.5));
        }
        b.setSample (0, i, v);
        b.setSample (1, i, v * (kind == 3 ? 0.8f : 1.0f));
    }
    auto file = folder.getChildFile (name + ".wav");
    file.deleteFile();
    juce::WavAudioFormat wav;
    std::unique_ptr<juce::OutputStream> stream (file.createOutputStream());
    auto writer = wav.createWriterFor (stream, juce::AudioFormatWriter::Options {}.withSampleRate (rate).withNumChannels (2).withBitsPerSample (24));
    writer->writeFromAudioSampleBuffer (b, 0, length);
    return file;
}
}

int main (int argc, char** argv)
{
    if (argc < 2)
    {
        std::printf ("usage: OlympusSnapshot <out.png> [plugins=<folder>]\n");
        return 2;
    }
    juce::ScopedJuceInitialiser_GUI gui;
    juce::String pluginFolder;
    for (int i = 2; i < argc; ++i)
        if (juce::String (argv[i]).startsWith ("plugins="))
            pluginFolder = juce::String (argv[i]).fromFirstOccurrenceOf ("=", false, false);

    const auto folder = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("olympus-snapshot");
    folder.createDirectory();
    olympus::gui::MainComponent::Options options;
    options.audioDevice = false;
    options.scanPlugins = false;
    options.settingsFile = folder.getChildFile ("settings.xml");
    olympus::gui::MainComponent window (options);
    window.setSize (olympus::gui::MainComponent::kDesignWidth, olympus::gui::MainComponent::kDesignHeight);

    if (pluginFolder.isNotEmpty())
    {
        olympus::ScanOptions scanOptions;
        scanOptions.includeSystem = false;
        window.session.catalog.scan (juce::FileSearchPath (pluginFolder), scanOptions);
    }
    else
    {
        for (auto* n : { "Hermes Level", "Aegis Ceiling", "Hestia Space", "Nemesis Hush", "Dionysus Warmth", "Hera Bond", "Iris Prism", "Apollo Curve" })
            test::addStandIn (window.session.catalog, n, juce::String (n) == "Aegis Ceiling" ? 96 : 0);
    }

    juce::String error;
    const char* names[] = { "Vocal", "Bass", "Drums", "Chords" };
    for (int i = 0; i < 4; ++i)
        window.session.addTrack (makeStem (folder, names[i], i), names[i], error);
    const std::vector<std::vector<const char*>> chains = { { "Nemesis Hush", "Hestia Space", "Dionysus Warmth", "Hermes Level", "Aegis Ceiling" }, { "Hera Bond", "Hestia Space", "Hermes Level" },
                                                           { "Hestia Space", "Hermes Level" }, { "Iris Prism", "Hestia Space" } };
    for (int i = 0; i < 4; ++i)
    {
        juce::StringArray chain;
        for (auto* c : chains[(size_t) i]) chain.add (c);
        if (! window.session.setChain (i, chain, error))
            std::printf ("chain %d: %s\n", i, error.toRawUTF8());
    }
    window.session.tracks[0]->gainDb = -1.5f;
    window.session.tracks[1]->gainDb = -3.0f;
    window.session.tracks[2]->gainDb = -6.0f;
    window.session.tracks[3]->gainDb = -9.0f;
    window.session.tracks[3]->mute = true;
    window.session.name = "Demo song";
    window.rebuildNow();
    window.seek ((juce::int64) (9.5 * window.session.sampleRate));
    juce::MessageManager::getInstance()->runDispatchLoopUntil (300);

    const auto image = window.createComponentSnapshot (window.getLocalBounds(), true, 1.5f);
    juce::File out (argv[1]);
    out.deleteFile();
    juce::FileOutputStream stream (out);
    juce::PNGImageFormat png;
    const bool ok = stream.openedOk() && png.writeImageToStream (image, stream);
    std::printf ("%s %s\n", ok ? "wrote" : "FAILED to write", out.getFullPathName().toRawUTF8());
    folder.deleteRecursively();
    return ok ? 0 : 1;
}
