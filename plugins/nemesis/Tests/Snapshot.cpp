// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 The Pantheon authors

// Renders the plugin editor to a PNG while feeding it something like a voice: a buzzing vowel with bursts of hiss, so the reduction display shows real activity.
//
//   NemesisSnapshot <out.png> [listen]

#include "PluginEditor.h"
#include <pantheon/ui/Scaled.h>
#include <random>

int main (int argc, char** argv)
{
    if (argc < 2)
    {
        std::printf ("usage: NemesisSnapshot <out.png> [listen]\n");
        return 2;
    }
    juce::ScopedJuceInitialiser_GUI gui;
    NemesisProcessor proc;
    proc.setPlayConfigDetails (2, 2, 48000.0, 512);
    proc.prepareToPlay (48000.0, 512);

    const auto set = [&] (const juce::String& id, float value) { nemesis::setValue (proc.apvts, id, value); };
    set (nemesis::id::threshold, -30.0f);
    set (nemesis::id::range, 12.0f);
    set (nemesis::id::detection, 0.0f);
    std::unique_ptr<juce::AudioProcessorEditor> editor (proc.createEditor());
    editor->setVisible (true);
    for (int i = 2; i < argc; ++i)
        if (juce::String (argv[i]) == "listen")
            set (nemesis::id::listen, 1.0f);

    // A vowel (a buzz at 140 Hz through a few formants) and every 0.6 s an s: noise through a band at 6.5 kHz, 120 ms long.
    std::mt19937 gen (7);
    std::uniform_real_distribution<float> white (-1.0f, 1.0f);
    juce::AudioBuffer<float> buffer (2, 512);
    juce::MidiBuffer midi;
    float hp1 = 0.0f, hp2 = 0.0f, lp = 0.0f;
    for (int block = 0; block < 720; ++block)
    {
        for (int i = 0; i < 512; ++i)
        {
            const int n = block * 512 + i;
            const float t = (float) n / 48000.0f;
            float vowel = 0.0f;
            for (int h = 1; h <= 12; ++h)
                vowel += std::sin (6.2831853f * 140.0f * (float) h * t) / (float) h;
            const float inEss = (float) (n % 28800) < 5760.0f ? 1.0f : 0.0f;
            const float noise = white (gen);
            hp1 += 0.55f * (noise - hp1);
            hp2 += 0.55f * (hp1 - hp2);
            lp += 0.2f * (noise - lp);
            const float hiss = (hp1 - lp) * 0.9f * inEss;
            const float s = 0.11f * vowel * (1.0f - inEss * 0.8f) + 0.25f * hiss;
            buffer.setSample (0, i, s);
            buffer.setSample (1, i, s);
        }
        proc.processBlock (buffer, midi);
        if (block % 4 == 0)
            juce::MessageManager::getInstance()->runDispatchLoopUntil (34);
    }
    auto& designed = ui::innerOf<NemesisEditor> (*editor); // the editor at the size it is drawn, not as the window scales it
    const auto image = designed.createComponentSnapshot (designed.getLocalBounds(), true, 1.5f);
    const juce::File out = juce::File::getCurrentWorkingDirectory().getChildFile (argv[1]);
    out.deleteFile();
    juce::FileOutputStream stream (out);
    juce::PNGImageFormat png;
    const bool ok = stream.openedOk() && png.writeImageToStream (image, stream);
    std::printf ("%s %s\n", ok ? "wrote" : "could not write", out.getFullPathName().toRawUTF8());
    return ok ? 0 : 1;
}
