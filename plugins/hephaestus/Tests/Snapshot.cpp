// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 The Pantheon authors

// Renders the plugin editor to a PNG while feeding it programme material (see TestSignals.h), so the
// spectrum display shows real activity.
//
//   HephaestusSnapshot <out.png> [preset name] [missing | sub] [band number to show]
//
//   missing  pink noise over a 55 Hz series without its fundamental (default)
//   sub      a sub-bass note that comes and goes, with nothing above it

#include "PluginEditor.h"
#include <pantheon/ui/Scaled.h>
#include "PluginProcessor.h"
#include "TestSignals.h"

int main (int argc, char** argv)
{
    if (argc < 2)
    {
        std::printf ("usage: HephaestusSnapshot <out.png> [preset name] [missing | sub] [band number]\n");
        return 2;
    }

    juce::ScopedJuceInitialiser_GUI gui;
    HephaestusProcessor proc;
    proc.setPlayConfigDetails (2, 2, 48000.0, 512);
    proc.prepareToPlay (48000.0, 512);

    if (argc > 2)
        for (int i = 0; i < proc.getNumPrograms(); ++i)
            if (proc.getProgramName (i) == argv[2])
                proc.setCurrentProgram (i);

    std::unique_ptr<juce::AudioProcessorEditor> editor (proc.createEditor());
    editor->setVisible (true);
    if (argc > 4)
        ui::innerOf<HephaestusEditor> (*editor).selectBand (juce::String (argv[4]).getIntValue() - 1);

    const bool sub = argc > 3 && juce::String (argv[3]) == "sub";
    test::MissingFundamentalProgramme missing;
    test::SubBassProgramme subBass;
    juce::AudioBuffer<float> buffer (2, 512);
    juce::MidiBuffer midi;

    for (int block = 0; block < (sub ? 440 : 200); ++block) // 440 blocks = 4.7 s: the sub note is on from 4 s to 5 s
    {
        if (sub) subBass.fill (buffer); else missing.fill (buffer);
        proc.processBlock (buffer, midi);
        if (block % 4 == 0)
            juce::MessageManager::getInstance()->runDispatchLoopUntil (3);
    }
    juce::MessageManager::getInstance()->runDispatchLoopUntil (300);

    auto& designed = ui::innerOf<HephaestusEditor> (*editor); // the editor at the size it is drawn, not as the window scales it
    const auto image = designed.createComponentSnapshot (designed.getLocalBounds(), true, 1.5f);
    juce::File out (argv[1]);
    out.deleteFile();
    juce::FileOutputStream stream (out);
    juce::PNGImageFormat png;
    const bool ok = stream.openedOk() && png.writeImageToStream (image, stream);
    std::printf ("%s %s\n", ok ? "wrote" : "FAILED to write", out.getFullPathName().toRawUTF8());
    return ok ? 0 : 1;
}
