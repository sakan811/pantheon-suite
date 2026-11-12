// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 The Pantheon authors

// Renders the plugin editor to a PNG while feeding it a stereo programme, so the goniometer and the correlation show something.
//
//   CastorSnapshot <out.png>

#include "PluginEditor.h"
#include <pantheon/ui/Scaled.h>
#include <random>

int main (int argc, char** argv)
{
    if (argc < 2)
    {
        std::printf ("usage: CastorSnapshot <out.png>\n");
        return 2;
    }
    juce::ScopedJuceInitialiser_GUI gui;
    CastorProcessor proc;
    proc.setPlayConfigDetails (2, 2, 48000.0, 512);
    proc.prepareToPlay (48000.0, 512);

    using namespace castor;
    const auto set = [&] (const juce::String& id, float value) { setValue (proc.apvts, id, value); };
    set (id::width (0), 0.0f);
    set (id::width (1), 40.0f);
    set (id::width (3), 140.0f);
    set (id::width (4), 160.0f);
    set (id::width (5), 180.0f);
    set (id::monoBelow, 120.0f);

    std::unique_ptr<juce::AudioProcessorEditor> editor (proc.createEditor());
    editor->setVisible (true);
    auto& designed = ui::innerOf<CastorEditor> (*editor);

    // A bass and a kick in the middle, a wide pad (independent noise in each channel) and a lead panned a little to the left.
    std::mt19937 gen (7);
    std::uniform_real_distribution<float> white (-1.0f, 1.0f);
    juce::AudioBuffer<float> buffer (2, 512);
    juce::MidiBuffer midi;
    float padL = 0.0f, padR = 0.0f;
    for (int block = 0; block < 240; ++block)
    {
        for (int i = 0; i < 512; ++i)
        {
            const int n = block * 512 + i;
            const float t = (float) n / 48000.0f;
            const float bass = 0.3f * std::sin (6.2831853f * 55.0f * t) + 0.12f * std::sin (6.2831853f * 110.0f * t);
            padL += 0.08f * (white (gen) - padL);
            padR += 0.08f * (white (gen) - padR);
            const float lead = 0.12f * std::sin (6.2831853f * (440.0f + 30.0f * std::sin (2.0f * t)) * t);
            buffer.setSample (0, i, bass + 0.45f * padL + 0.9f * lead);
            buffer.setSample (1, i, bass + 0.45f * padR + 0.4f * lead);
        }
        proc.processBlock (buffer, midi);
        if (block % 4 == 0)
        {
            designed.tickDisplay();
            juce::MessageManager::getInstance()->runDispatchLoopUntil (20);
        }
    }

    const auto image = designed.createComponentSnapshot (designed.getLocalBounds(), true, 1.5f);
    const juce::File out = juce::File::getCurrentWorkingDirectory().getChildFile (argv[1]);
    out.deleteFile();
    juce::FileOutputStream stream (out);
    juce::PNGImageFormat png;
    const bool ok = stream.openedOk() && png.writeImageToStream (image, stream);
    std::printf ("%s %s\n", ok ? "wrote" : "could not write", out.getFullPathName().toRawUTF8());
    return ok ? 0 : 1;
}
