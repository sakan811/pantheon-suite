// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 The Pantheon authors

// Renders the plugin editor to a PNG while feeding it a stereo programme (see TestSignals.h), so the display shows real activity.
//
//   IrisSnapshot <out.png> [preset name] [band=N] [region=MASK] [solo=BAND:band|curve] [delta]
//
//   band=N     select band N (1-based)
//   region=M   solo the frequency regions in bit mask M (1 = sub ... 32 = high)
//   solo=N:k   solo band N, as its region (k = band) or its curve (k = curve)
//   sidechain  feed a second track (a bass line over low-passed noise) to the sidechain input, for the masking view

#include "PluginEditor.h"
#include "PluginProcessor.h"
#include "TestSignals.h"

int main (int argc, char** argv)
{
    if (argc < 2)
    {
        std::printf ("usage: IrisSnapshot <out.png> [preset name] [band=N] [region=MASK] [solo=N:band|curve] [delta]\n");
        return 2;
    }

    juce::ScopedJuceInitialiser_GUI gui;
    IrisProcessor proc;
    proc.setPlayConfigDetails (2, 2, 48000.0, 512);
    proc.prepareToPlay (48000.0, 512);

    if (argc > 2)
        for (int i = 0; i < proc.getNumPrograms(); ++i)
            if (proc.getProgramName (i) == argv[2])
                proc.setCurrentProgram (i);

    bool sidechain = false;
    std::unique_ptr<juce::AudioProcessorEditor> editor (proc.createEditor());
    editor->setVisible (true);

    for (int i = 3; i < argc; ++i)
    {
        const juce::String arg (argv[i]);
        if (arg.startsWith ("band="))
            static_cast<IrisEditor&> (*editor).selectBand (arg.fromFirstOccurrenceOf ("=", false, false).getIntValue() - 1);
        else if (arg.startsWith ("region="))
            proc.engine.monitor.regions = (unsigned) arg.fromFirstOccurrenceOf ("=", false, false).getIntValue();
        else if (arg.startsWith ("solo="))
        {
            proc.engine.monitor.soloBand = arg.fromFirstOccurrenceOf ("=", false, false).upToFirstOccurrenceOf (":", false, false).getIntValue() - 1;
            proc.engine.monitor.soloKind = arg.endsWith ("curve") ? (int) iris::SoloKind::curve : (int) iris::SoloKind::band;
        }
        else if (arg == "delta")
            proc.engine.monitor.delta = true;
        else if (arg == "sidechain")
            sidechain = true;
    }

    test::Programme programme;
    juce::AudioBuffer<float> buffer (2, 512);
    juce::MidiBuffer midi;
    juce::AudioBuffer<float> other (2, 512);
    test::PinkNoise otherNoise (33);
    float lowpassed = 0.0f;
    double otherT = 0.0;
    for (int block = 0; block < 400; ++block)
    {
        programme.fill (buffer);
        if (sidechain)
        {
            // Another track on the sidechain: a bass line (55 and 82.5 Hz) over noise low-passed at about 300 Hz.
            for (int i = 0; i < other.getNumSamples(); ++i, ++otherT)
            {
                const double t = otherT / 48000.0;
                lowpassed += 0.04f * (otherNoise.next() - lowpassed);
                const float x = 1.6f * lowpassed + (float) (0.12 * std::sin (2 * M_PI * 55 * t) + 0.08 * std::sin (2 * M_PI * 82.5 * t));
                other.setSample (0, i, x);
                other.setSample (1, i, x);
            }
            proc.sidechainTap.push (other.getArrayOfReadPointers(), 2, other.getNumSamples());
        }
        proc.processBlock (buffer, midi);
        if (block % 4 == 0)
            juce::MessageManager::getInstance()->runDispatchLoopUntil (34); // about one display tick per 4 blocks, so the averaged views (resonances, masking) fill in
    }
    juce::MessageManager::getInstance()->runDispatchLoopUntil (300);

    const auto image = editor->createComponentSnapshot (editor->getLocalBounds(), true, 1.5f);
    juce::File out (argv[1]);
    out.deleteFile();
    juce::FileOutputStream stream (out);
    juce::PNGImageFormat png;
    const bool ok = stream.openedOk() && png.writeImageToStream (image, stream);
    std::printf ("%s %s\n", ok ? "wrote" : "FAILED to write", out.getFullPathName().toRawUTF8());
    return ok ? 0 : 1;
}
