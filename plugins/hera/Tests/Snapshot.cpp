// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 The Pantheon authors

// Renders the plugin editor to a PNG while feeding it a programme, so the displays show real activity.
//
//   HeraSnapshot <out.png>

#include "PluginEditor.h"
#include <pantheon/ui/Scaled.h>
#include <random>

int main (int argc, char** argv)
{
    if (argc < 2)
    {
        std::printf ("usage: HeraSnapshot <out.png>\n");
        return 2;
    }
    juce::ScopedJuceInitialiser_GUI gui;
    HeraProcessor proc;
    proc.setPlayConfigDetails (2, 2, 48000.0, 512);
    proc.prepareToPlay (48000.0, 512);

    const auto set = [&] (const juce::String& id, float value) { hera::setValue (proc.apvts, id, value); };
    set (hera::id::threshold, -22.0f);
    set (hera::id::ratio, 4.0f);
    set (hera::id::attack, 15.0f);
    set (hera::id::knee, 8.0f);
    set (hera::id::hpf, 90.0f);
    set (hera::id::autoMakeup, 1.0f);
    set (hera::id::autoRelease, 1.0f);

    std::unique_ptr<juce::AudioProcessorEditor> editor (proc.createEditor());
    editor->setVisible (true);

    // A beat: a kick on the quarter notes under a bed of noise that swells.
    std::mt19937 gen (7);
    std::uniform_real_distribution<float> white (-1.0f, 1.0f);
    juce::AudioBuffer<float> buffer (2, 512);
    juce::MidiBuffer midi;
    float lowL = 0.0f, lowR = 0.0f;
    for (int block = 0; block < 720; ++block)
    {
        for (int i = 0; i < 512; ++i)
        {
            const int n = block * 512 + i;
            const float beat = std::exp (-(float) (n % 12000) / 2600.0f);
            lowL += 0.3f * (white (gen) - lowL);
            lowR += 0.3f * (white (gen) - lowR);
            const float kick = 0.7f * std::sin (6.2831853f * (45.0f + 50.0f * beat) * (float) n / 48000.0f) * beat;
            const float swell = 0.12f + 0.1f * std::sin (6.2831853f * (float) n / 96000.0f);
            buffer.setSample (0, i, kick + swell * lowL);
            buffer.setSample (1, i, kick + swell * lowR);
        }
        proc.processBlock (buffer, midi);
        if (block % 4 == 0)
            juce::MessageManager::getInstance()->runDispatchLoopUntil (34);
    }

    auto& designed = ui::innerOf<HeraEditor> (*editor); // the editor at the size it is drawn, not as the window scales it
    const auto image = designed.createComponentSnapshot (designed.getLocalBounds(), true, 1.5f);
    const juce::File out = juce::File::getCurrentWorkingDirectory().getChildFile (argv[1]);
    out.deleteFile();
    juce::FileOutputStream stream (out);
    juce::PNGImageFormat png;
    const bool ok = stream.openedOk() && png.writeImageToStream (image, stream);
    std::printf ("%s %s\n", ok ? "wrote" : "could not write", out.getFullPathName().toRawUTF8());
    return ok ? 0 : 1;
}
