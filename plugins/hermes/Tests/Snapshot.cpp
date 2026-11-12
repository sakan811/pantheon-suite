// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 The Pantheon authors

// Renders the plugin editor to a PNG while feeding it a programme (loud and quiet passages), with a reference loaded, so the readings and the graph show real activity.
//
//   HermesSnapshot <out.png>

#include "PluginEditor.h"
#include <pantheon/ui/Scaled.h>
#include <random>

int main (int argc, char** argv)
{
    if (argc < 2)
    {
        std::printf ("usage: HermesSnapshot <out.png>\n");
        return 2;
    }
    juce::ScopedJuceInitialiser_GUI gui;
    HermesProcessor proc;
    proc.setPlayConfigDetails (2, 2, 48000.0, 512);
    proc.prepareToPlay (48000.0, 512);

    // A reference: a few seconds of a chord, written to a file in a temporary folder and loaded like any other.
    const auto file = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("hermes_snapshot_reference.wav");
    {
        juce::AudioBuffer<float> chord (2, 48000 * 6);
        for (int i = 0; i < chord.getNumSamples(); ++i)
        {
            const float t = (float) i / 48000.0f;
            const float s = 0.2f * (std::sin (6.2831853f * 110.0f * t) + 0.7f * std::sin (6.2831853f * 220.0f * t) + 0.5f * std::sin (6.2831853f * 330.0f * t) + 0.3f * std::sin (6.2831853f * 1320.0f * t));
            chord.setSample (0, i, s);
            chord.setSample (1, i, s * 0.9f);
        }
        file.deleteFile();
        juce::WavAudioFormat wav;
        std::unique_ptr<juce::AudioFormatWriter> writer (wav.createWriterFor (new juce::FileOutputStream (file), 48000.0, 2, 24, {}, 0));
        if (writer != nullptr)
            writer->writeFromAudioSampleBuffer (chord, 0, chord.getNumSamples());
    }
    std::unique_ptr<juce::AudioProcessorEditor> editor (proc.createEditor());
    editor->setVisible (true);
    auto& designed = ui::innerOf<HermesEditor> (*editor); // the editor at the size it is drawn, not as the window scales it
    designed.loadFile (file);
    hermes::setValue (proc.apvts, hermes::id::trim, 2.5f);

    // A programme that changes: a verse, a chorus that is louder and a bridge that is quiet, over 80 s of processed time (the readings run at the rate the processor is fed, not in real time).
    std::mt19937 gen (7);
    std::uniform_real_distribution<float> white (-1.0f, 1.0f);
    juce::AudioBuffer<float> buffer (2, 512);
    juce::MidiBuffer midi;
    float lowL = 0.0f, lowR = 0.0f;
    for (int block = 0; block < 48000 * 70 / 512; ++block)
    {
        for (int i = 0; i < 512; ++i)
        {
            const int n = block * 512 + i;
            const float seconds = (float) n / 48000.0f;
            const float section = seconds < 20.0f ? 0.35f : seconds < 45.0f ? 0.8f : 0.18f;
            const float beat = 0.45f + 0.55f * std::exp (-(float) (n % 24000) / 5000.0f);
            lowL += 0.1f * (white (gen) - lowL);
            lowR += 0.1f * (white (gen) - lowR);
            const float bass = 0.35f * std::sin (6.2831853f * 55.0f * (float) n / 48000.0f) * beat;
            buffer.setSample (0, i, section * (bass + 1.4f * lowL * beat));
            buffer.setSample (1, i, section * (bass + 1.4f * lowR * beat));
        }
        proc.processBlock (buffer, midi);
        if (block % 9 == 0)
            designed.tickDisplay(); // a reading every 100 ms of the programme, as the timer takes them in real time
    }
    juce::MessageManager::getInstance()->runDispatchLoopUntil (300);

    const auto image = designed.createComponentSnapshot (designed.getLocalBounds(), true, 1.5f);
    file.deleteFile();
    const juce::File out = juce::File::getCurrentWorkingDirectory().getChildFile (argv[1]);
    out.deleteFile();
    juce::FileOutputStream stream (out);
    juce::PNGImageFormat png;
    const bool ok = stream.openedOk() && png.writeImageToStream (image, stream);
    std::printf ("%s %s\n", ok ? "wrote" : "could not write", out.getFullPathName().toRawUTF8());
    return ok ? 0 : 1;
}
