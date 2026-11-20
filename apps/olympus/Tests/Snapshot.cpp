// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 The Pantheon authors

// Renders the real window to a PNG: four synthetic stems, cut into clips on a timeline with a tempo, three markers, a loop and a plugin, so
// the picture shows what the app is for.
//
//   OlympusSnapshot <out.png> [plugins=<folder>]
//
//   plugins=FOLDER   use the suite's built plugins found in FOLDER (VST3) on the chains instead of the test plugin
//
// A second picture, next to <out.png> as screenshot-record.png: the same session while a take runs on two armed tracks (IN 1 and IN 3+4),
// with a device the picture describes (there is no audio device in a headless run; the takes are written to a folder of the snapshot's own).

#include "TestPlugin.h"
#include "UI/MainComponent.h"

namespace
{
constexpr double kRate = 48000.0;
constexpr double kStemSeconds = 24.0;
constexpr double kTempo = 96.0;   // a bar is 2.5 seconds

// One stem of a song: `kind` 0 a vocal (a slow melody with vibrato), 1 a bass, 2 a drum loop, 3 chords.
juce::File makeStem (const juce::File& folder, const juce::String& name, int kind)
{
    const int length = (int) (kRate * kStemSeconds);
    juce::AudioBuffer<float> b (2, length);
    juce::Random random (kind + 7);
    const double notes[] = { 220.0, 247.0, 277.2, 330.0, 293.7, 247.0, 220.0, 196.0 };
    double phase = 0.0;
    for (int i = 0; i < length; ++i)
    {
        const double t = i / kRate;
        const double beat = std::fmod (t * kTempo / 60.0, 1.0);
        float v = 0.0f;
        const float env = (float) std::exp (-3.0 * beat);
        if (kind == 0)
        {
            const double f = notes[(size_t) (t / 3.0) % 8] * (1.0 + 0.012 * std::sin (2.0 * juce::MathConstants<double>::pi * 5.5 * t));
            phase += f / kRate;
            const float gate = (float) (0.5 + 0.5 * std::sin (2.0 * juce::MathConstants<double>::pi * t / 3.0 - 1.2));
            v = gate * 0.35f * (float) (std::sin (2.0 * juce::MathConstants<double>::pi * phase) + 0.4 * std::sin (4.0 * juce::MathConstants<double>::pi * phase));
        }
        else if (kind == 1)
        {
            phase += 55.0 * (((int) (t / 1.5)) % 2 == 0 ? 1.0 : 1.335) / kRate;
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
            phase += f / kRate;
            v = 0.16f * (float) (std::sin (2.0 * juce::MathConstants<double>::pi * phase) + std::sin (2.0 * juce::MathConstants<double>::pi * phase * 1.26) + std::sin (2.0 * juce::MathConstants<double>::pi * phase * 1.5));
        }
        b.setSample (0, i, v);
        b.setSample (1, i, v * (kind == 3 ? 0.8f : 1.0f));
    }
    auto file = folder.getChildFile (name + ".wav");
    file.deleteFile();
    juce::WavAudioFormat wav;
    std::unique_ptr<juce::OutputStream> stream (file.createOutputStream());
    auto writer = wav.createWriterFor (stream, juce::AudioFormatWriter::Options {}.withSampleRate (kRate).withNumChannels (2).withBitsPerSample (24));
    writer->writeFromAudioSampleBuffer (b, 0, length);
    return file;
}

// A clip of the stem: from `offsetSeconds` in the file, placed at `startSeconds` on the timeline, for `lengthSeconds`.
void clip (olympus::Editor& editor, int track, const juce::File& file, double startSeconds, double offsetSeconds, double lengthSeconds)
{
    juce::String error;
    const auto ref = editor.addClip (track, file, (juce::int64) (startSeconds * kRate), (juce::int64) (offsetSeconds * kRate), (juce::int64) (lengthSeconds * kRate), error);
    if (! ref.valid())
        std::printf ("clip on track %d: %s\n", track, error.toRawUTF8());
}
}

// The window to a PNG file. Returns false when it cannot be written.
bool writePicture (olympus::gui::MainComponent& window, const juce::File& out)
{
    const auto image = window.createComponentSnapshot (window.getLocalBounds(), true, 1.5f);
    out.deleteFile();
    juce::FileOutputStream stream (out);
    juce::PNGImageFormat png;
    const bool ok = stream.openedOk() && png.writeImageToStream (image, stream);
    std::printf ("%s %s\n", ok ? "wrote" : "FAILED to write", out.getFullPathName().toRawUTF8());
    return ok;
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

    // starts empty every time: the window reads monitor.xml from here, and each run adds its own stand-in plugin to the strip, so a kept
    // folder would grow the picture's strip by one button per run
    const auto folder = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("olympus-snapshot");
    folder.deleteRecursively();
    folder.createDirectory();
    olympus::gui::MainComponent::Options options;
    options.audioDevice = false;
    options.scanPlugins = false;
    options.settingsFile = folder.getChildFile ("settings.xml");
    options.recordings = folder.getChildFile ("takes");
    if (pluginFolder.isEmpty())
        options.fillCatalog = [] (olympus::Catalog& catalog) { test::addTestPlugins (catalog); };
    olympus::gui::MainComponent window (options);
    window.setSize (olympus::gui::MainComponent::kDesignWidth, olympus::gui::MainComponent::kDesignHeight);

    juce::StringArray chainNames;
    if (pluginFolder.isNotEmpty())
    {
        olympus::ScanOptions scanOptions;
        scanOptions.includeSystem = false;
        window.session.catalog.scan (juce::FileSearchPath (pluginFolder), scanOptions);
        chainNames = { "Nemesis Hush", "Hestia Space", "Hermes Level" };
    }
    else
    {
        chainNames = { "test gain" };
    }

    // four tracks, each a stem cut into clips on the timeline (seconds at the tempo: a bar is 2.5 s)
    auto& editor = window.editor;
    auto& session = window.session;
    const char* names[] = { "Vocal", "Bass", "Drums", "Chords" };
    juce::File stems[4];
    for (int i = 0; i < 4; ++i)
    {
        stems[i] = makeStem (folder, names[i], i);
        session.addEmptyTrack (names[i]);
    }
    juce::String error;
    editor.setTempo (0.0, kTempo, error);
    clip (editor, 0, stems[0], 0.0, 0.0, 8.0);
    clip (editor, 0, stems[0], 10.0, 8.0, 8.0);
    clip (editor, 0, stems[0], 20.0, 16.0, 6.0);
    clip (editor, 1, stems[1], 0.0, 0.0, 12.0);
    clip (editor, 1, stems[1], 14.0, 12.0, 10.0);
    clip (editor, 2, stems[2], 2.0, 0.0, 6.0);
    clip (editor, 2, stems[2], 10.0, 6.0, 6.0);
    clip (editor, 2, stems[2], 18.0, 12.0, 6.0);
    clip (editor, 3, stems[3], 6.0, 0.0, 14.0);
    editor.setClipFades (3, 0, (juce::int64) (2.0 * kRate), (juce::int64) (2.0 * kRate), error);

    const double gains[] = { -1.5, -3.0, -6.0, -9.0 };
    for (int i = 0; i < 4; ++i) editor.setTrackGain (i, (float) gains[i], error);

    for (int i = 0; i < (int) chainNames.size(); ++i)
        session.addPlugin (0, chainNames[i], -1, error);
    // the headphones' chain, a stand-in for a correction plugin: on the MONITOR strip, playback only (the test plugins only)
    if (pluginFolder.isEmpty())
        window.addMonitorPlugin ("Test Gain");

    editor.addMarker ("Intro", 0);
    editor.addMarker ("Verse", (juce::int64) (10.0 * kRate));
    editor.addMarker ("Chorus", (juce::int64) (20.0 * kRate));
    editor.setLoop ((juce::int64) (5.0 * kRate), (juce::int64) (20.0 * kRate), true);
    session.name = "Demo song";
    editor.clearUndo();
    window.arrange.zoomToFit();
    window.refreshNow();
    window.seek ((juce::int64) (11.0 * kRate));
    window.arrange.playhead = (double) window.position();
    juce::MessageManager::getInstance()->runDispatchLoopUntil (300);
    window.arrange.refresh();

    bool ok = writePicture (window, juce::File (argv[1]));

    // the second picture: two armed tracks, IN 1 and IN 3+4, while a take runs from bar 4 (8 seconds); the armed lanes show the take in red
    olympus::gui::MainComponent::DeviceState device;
    device.running = true;
    device.inputName = "Interface";
    device.outputName = "Headphones";
    device.rate = kRate;
    device.bufferSize = 128;
    device.inputs = 4;
    device.outputs = 2;
    device.inputLatency = 96;
    device.outputLatency = 48;
    device.activeInputs.setRange (0, 4, true);
    window.setDeviceForTest (device);
    session.setTrackArm (0, true, error);
    session.setTrackArm (2, true, error);
    session.setTrackInput (0, 0, -1, error);
    session.setTrackInput (2, 2, 3, error);
    window.seek ((juce::int64) (8.0 * kRate));
    window.toggleRecord();
    {
        // 3.5 seconds of the device: a 440 Hz tone on the inputs, in 128-frame blocks
        const int total = (int) (3.5 * kRate);
        std::vector<std::vector<float>> in (4, std::vector<float> (128, 0.0f));
        std::vector<std::vector<float>> out (2, std::vector<float> (128, 0.0f));
        for (int done = 0; done < total; done += 128)
        {
            for (int i = 0; i < 128; ++i)
            {
                const float v = 0.4f * (float) std::sin (2.0 * juce::MathConstants<double>::pi * 440.0 * (done + i) / kRate);
                in[0][(size_t) i] = v;
                in[2][(size_t) i] = 0.6f * v;
                in[3][(size_t) i] = 0.6f * v;
            }
            const float* inPtr[4] = { in[0].data(), in[1].data(), in[2].data(), in[3].data() };
            float* outPtr[2] = { out[0].data(), out[1].data() };
            window.io.deviceBlock (inPtr, 4, outPtr, 2, 128);
        }
    }
    juce::MessageManager::getInstance()->runDispatchLoopUntil (300);
    window.arrange.refresh();
    ok = writePicture (window, juce::File (argv[1]).getSiblingFile ("screenshot-record.png")) && ok;
    window.toggleRecord();   // the take ends: its takes go to the snapshot's folder, which is removed below

    folder.deleteRecursively();
    return ok ? 0 : 1;
}
