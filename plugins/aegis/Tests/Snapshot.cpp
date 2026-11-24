// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 The Pantheon authors

// Renders the plugin editor to a PNG while feeding it a loud programme, so the reduction display shows real activity.
//
//   AegisSnapshot <out.png> [gain=DB] [ceiling=DB] [tp=0|1]
//   AegisSnapshot --frames <dir> [gain=DB] [ceiling=DB] [tp=0|1]

#include "PluginEditor.h"
#include <pantheon/ui/Scaled.h>
#include <random>
#include <string_view>

int main (int argc, char** argv)
{
    if (argc < 2 || (std::string_view (argv[1]) == "--frames" && argc < 3))
    {
        std::printf ("usage: AegisSnapshot <out.png> [gain=DB] [ceiling=DB] [tp=0|1]\n"
                     "       AegisSnapshot --frames <dir> [gain=DB] [ceiling=DB] [tp=0|1]\n");
        return 2;
    }
    const bool framesMode = std::string_view (argv[1]) == "--frames";
    juce::ScopedJuceInitialiser_GUI gui;
    AegisProcessor proc;
    proc.setPlayConfigDetails (2, 2, 48000.0, 512);
    proc.prepareToPlay (48000.0, 512);

    const auto set = [&] (const juce::String& id, float value) { auto* p = proc.apvts.getParameter (id); p->setValueNotifyingHost (p->convertTo0to1 (value)); };
    set (aegis::id::gain, 8.0f);
    std::unique_ptr<juce::AudioProcessorEditor> editor (proc.createEditor());
    editor->setVisible (true);
    for (int i = framesMode ? 3 : 2; i < argc; ++i)
    {
        const juce::String arg (argv[i]);
        const float value = arg.fromFirstOccurrenceOf ("=", false, false).getFloatValue();
        if (arg.startsWith ("gain="))         set (aegis::id::gain, value);
        else if (arg.startsWith ("ceiling=")) set (aegis::id::ceiling, value);
        else if (arg.startsWith ("tp="))      set (aegis::id::truePeak, value);
    }

    auto& designed = ui::innerOf<AegisEditor> (*editor); // the editor at the size it is drawn, not as the window scales it
    juce::File outDir;
    if (framesMode)
    {
        const juce::String dirArg (argv[2]);
        outDir = juce::File::isAbsolutePath (dirArg) ? juce::File (dirArg)
                                                     : juce::File::getCurrentWorkingDirectory().getChildFile (dirArg);
        outDir.createDirectory();
    }
    juce::PNGImageFormat png;

    // A beat: a kick on the quarter notes under a bed of noise that swells, so the reduction moves in time.
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
            const float beat = std::exp (-(float) (n % 12000) / 2200.0f);
            lowL += 0.35f * (white (gen) - lowL);
            lowR += 0.35f * (white (gen) - lowR);
            const float kick = 0.8f * std::sin (6.2831853f * (45.0f + 60.0f * beat) * (float) n / 48000.0f) * beat;
            const float swell = 0.25f + 0.2f * std::sin (6.2831853f * (float) n / 96000.0f);
            buffer.setSample (0, i, kick + swell * lowL);
            buffer.setSample (1, i, kick + swell * lowR);
        }
        proc.processBlock (buffer, midi);
        if (block % 4 == 0)
            juce::MessageManager::getInstance()->runDispatchLoopUntil (34);

        if (framesMode && block >= 480 && (block - 480) % 8 == 0)
        {
            const int frame = (block - 480) / 8;
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
    }

    if (framesMode)
    {
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
