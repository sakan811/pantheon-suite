// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 The Pantheon authors

// Renders the plugin editor to a PNG while feeding it a signal, so the level lamp is lit and the curve and the harmonics show a setting worth looking at.
//
//   DionysusSnapshot <out.png> [diode] [clip]
//   DionysusSnapshot --frames <dir> [diode] [clip]

#include "PluginEditor.h"
#include <pantheon/ui/Scaled.h>
#include <string_view>

int main (int argc, char** argv)
{
    if (argc < 2 || (std::string_view (argv[1]) == "--frames" && argc < 3))
    {
        std::printf ("usage: DionysusSnapshot <out.png> [diode] [clip]\n"
                     "       DionysusSnapshot --frames <dir> [diode] [clip]\n");
        return 2;
    }
    const bool framesMode = std::string_view (argv[1]) == "--frames";
    juce::ScopedJuceInitialiser_GUI gui;
    DionysusProcessor proc;
    proc.setPlayConfigDetails (2, 2, 48000.0, 512);
    proc.prepareToPlay (48000.0, 512);

    const auto set = [&] (const juce::String& id, float value) { dionysus::setValue (proc.apvts, id, value); };
    set (dionysus::id::drive, 16.0f);
    set (dionysus::id::bias, 25.0f);
    set (dionysus::id::tone, 20.0f);
    set (dionysus::id::oversampling, 2.0f);
    set (dionysus::id::mix, 85.0f);
    for (int i = framesMode ? 3 : 2; i < argc; ++i)
    {
        const juce::String arg (argv[i]);
        if (arg == "diode") set (dionysus::id::shape, (float) (int) dionysus::Shape::diode);
        if (arg == "clip") set (dionysus::id::shape, (float) (int) dionysus::Shape::clip);
    }
    std::unique_ptr<juce::AudioProcessorEditor> editor (proc.createEditor());
    editor->setVisible (true);
    juce::AudioBuffer<float> buffer (2, 512);
    juce::MidiBuffer midi;
    for (int block = 0; block < 40; ++block)
    {
        for (int i = 0; i < 512; ++i)
            for (int c = 0; c < 2; ++c)
                buffer.setSample (c, i, 0.25f * std::sin (6.2831853f * 220.0f * (float) (block * 512 + i) / 48000.0f));
        proc.processBlock (buffer, midi);
        juce::MessageManager::getInstance()->runDispatchLoopUntil (30);
    }
    auto& designed = ui::innerOf<DionysusEditor> (*editor); // the editor at the size it is drawn, not as the window scales it
    juce::PNGImageFormat png;

    if (framesMode)
    {
        const juce::String dirArg (argv[2]);
        const juce::File outDir = juce::File::isAbsolutePath (dirArg)
                                    ? juce::File (dirArg)
                                    : juce::File::getCurrentWorkingDirectory().getChildFile (dirArg);
        outDir.createDirectory();
        for (int frame = 0; frame < 30; ++frame)
        {
            const float phase = 6.2831853f * (float) frame / 30.0f;
            const float swell = 0.5f - 0.5f * std::cos (phase);
            set (dionysus::id::drive, 8.0f + 16.0f * swell);
            set (dionysus::id::bias, 35.0f * std::sin (phase));
            const float amp = 0.12f + 0.55f * swell;
            for (int sub = 0; sub < 4; ++sub)
            {
                const int block = 40 + frame * 4 + sub;
                for (int i = 0; i < 512; ++i)
                    for (int c = 0; c < 2; ++c)
                        buffer.setSample (c, i, amp * std::sin (6.2831853f * 220.0f * (float) (block * 512 + i) / 48000.0f));
                proc.processBlock (buffer, midi);
            }
            juce::MessageManager::getInstance()->runDispatchLoopUntil (34);
            const auto image = designed.createComponentSnapshot (designed.getLocalBounds(), true, 1.5f);
            const juce::File out = outDir.getChildFile (juce::String::formatted ("frame_%03d.png", frame));
            out.deleteFile();
            juce::FileOutputStream stream (out);
            if (! (stream.openedOk() && png.writeImageToStream (image, stream)))
            {
                std::printf ("could not write %s\n", out.getFullPathName().toRawUTF8());
                return 1;
            }
        }
        std::printf ("wrote 30 frames to %s\n", outDir.getFullPathName().toRawUTF8());
        return 0;
    }

    const auto image = designed.createComponentSnapshot (designed.getLocalBounds(), true, 1.5f);
    const juce::File out = juce::File::getCurrentWorkingDirectory().getChildFile (argv[1]);
    out.deleteFile();
    juce::FileOutputStream stream (out);
    const bool ok = stream.openedOk() && png.writeImageToStream (image, stream);
    std::printf ("%s %s\n", ok ? "wrote" : "could not write", out.getFullPathName().toRawUTF8());
    return ok ? 0 : 1;
}
