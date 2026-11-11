// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 The Pantheon authors

// Renders the plugin editor to a PNG while feeding it a programme, so the reduction meters show real activity.
//
//   AtlasSnapshot <out.png> [solo=MASK] [delta=MASK] [linear] [os=N] [bands=N] [upward=BAND]
//
//   bands=N   join bands from the top until N are left

#include "PluginEditor.h"
#include <random>

int main (int argc, char** argv)
{
    if (argc < 2)
    {
        std::printf ("usage: AtlasSnapshot <out.png> [solo=MASK] [delta=MASK] [linear] [os=N]\n");
        return 2;
    }
    juce::ScopedJuceInitialiser_GUI gui;
    AtlasProcessor proc;
    proc.setPlayConfigDetails (2, 2, 48000.0, 512);
    proc.prepareToPlay (48000.0, 512);

    // A setting worth looking at: every band working a little, the bass in M/S, the top matched.
    const auto set = [&] (const juce::String& id, float value) { auto* p = proc.apvts.getParameter (id); p->setValueNotifyingHost (p->convertTo0to1 (value)); };
    const float thresholds[] = { -24.0f, -22.0f, -26.0f, -24.0f, -30.0f, -36.0f };
    for (int b = 0; b < atlas::kBands; ++b)
    {
        set (atlas::bandIds (b).threshold, thresholds[b]);
        set (atlas::bandIds (b).ratio, b < 2 ? 4.0f : 2.5f);
    }
    set (atlas::bandIds (1).channels, (float) (int) atlas::Channels::midSide);
    set (atlas::bandIds (5).match, 1.0f);

    std::unique_ptr<juce::AudioProcessorEditor> editor (proc.createEditor());
    editor->setVisible (true);
    for (int i = 2; i < argc; ++i)
    {
        const juce::String arg (argv[i]);
        const int value = arg.fromFirstOccurrenceOf ("=", false, false).getIntValue();
        if (arg.startsWith ("solo="))       proc.engine.monitor.solo = (unsigned) value;
        else if (arg.startsWith ("delta=")) proc.engine.monitor.delta = (unsigned) value;
        else if (arg == "linear")           set (atlas::id::phase, 1.0f);
        else if (arg.startsWith ("os="))    set (atlas::id::oversampling, (float) value);
        else if (arg.startsWith ("upward=")) set (atlas::bandIds (value - 1).direction, 1.0f); // band N (from 1) lifts instead of turning down
        else if (arg.startsWith ("bands="))
            while (atlas::layoutOf (proc.apvts).numBands > std::max (value, 1))
                atlas::removeSplit (proc.apvts, atlas::layoutOf (proc.apvts).numBands - 2);
    }

    // Pink-ish noise with a beat: something in every band, moving.
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
            const float beat = 0.1f + 0.9f * std::exp (-(float) (n % 24000) / 3000.0f);
            lowL += 0.08f * (white (gen) - lowL);
            lowR += 0.08f * (white (gen) - lowR);
            const float kick = 0.6f * std::sin (6.2831853f * 55.0f * (float) n / 48000.0f) * beat;
            const float ring = 0.02f * std::sin (6.2831853f * 3100.0f * (float) n / 48000.0f); // a resonance that stays
            buffer.setSample (0, i, beat * (2.0f * lowL + 0.15f * white (gen)) + kick + ring);
            buffer.setSample (1, i, beat * (2.0f * lowR + 0.15f * white (gen)) + kick + ring);
        }
        proc.processBlock (buffer, midi);
        if (block % 4 == 0)
            juce::MessageManager::getInstance()->runDispatchLoopUntil (34);
    }

    const auto image = editor->createComponentSnapshot (editor->getLocalBounds(), true, 1.5f);
    const juce::File out = juce::File::getCurrentWorkingDirectory().getChildFile (argv[1]);
    out.deleteFile();
    juce::FileOutputStream stream (out);
    juce::PNGImageFormat png;
    const bool ok = stream.openedOk() && png.writeImageToStream (image, stream);
    std::printf ("%s %s\n", ok ? "wrote" : "could not write", out.getFullPathName().toRawUTF8());
    return ok ? 0 : 1;
}
