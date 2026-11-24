// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 The Pantheon authors

// Renders the Selene Poly editor to a PNG while playing a City Pop chord so the filter response, oscilloscope, and voice readout are active.
//
//   SeleneSnapshot <out.png>

#include "PluginEditor.h"
#include <pantheon/ui/Scaled.h>

int main (int argc, char** argv)
{
    if (argc < 2)
    {
        std::printf ("usage: SeleneSnapshot <out.png>\n");
        return 2;
    }

    juce::ScopedJuceInitialiser_GUI gui;
    SeleneProcessor proc;
    proc.setPlayConfigDetails (0, 2, 48000.0, 512);
    proc.prepareToPlay (48000.0, 512);

    // Load "Tokyo Night Pad" (index 1) for lush PWM, Chorus I+II, and filter sweep
    for (int i = 0; i < proc.getNumPrograms(); ++i)
    {
        if (proc.getProgramName (i) == "Tokyo Night Pad")
        {
            proc.setCurrentProgram (i);
            break;
        }
    }

    std::unique_ptr<juce::AudioProcessorEditor> editor (proc.createEditor());
    editor->setVisible (true);

    // Feed an Fmaj9 chord (F3, A3, C4, E4, G4) for ~0.48s so amp/filter envelopes bloom and scope is vivid.
    juce::AudioBuffer<float> buffer (2, 512);
    juce::MidiBuffer midi;
    for (const int note : { 53, 57, 60, 64, 67 })
        midi.addEvent (juce::MidiMessage::noteOn (1, note, 0.88f), 0);

    for (int block = 0; block < 45; ++block)
    {
        proc.processBlock (buffer, midi);
        midi.clear();
        if (block % 4 == 0)
            juce::MessageManager::getInstance()->runDispatchLoopUntil (20);
    }

    auto& designed = ui::innerOf<SeleneEditor> (*editor);
    designed.tickDisplay();
    juce::MessageManager::getInstance()->runDispatchLoopUntil (20);

    const auto image = designed.createComponentSnapshot (designed.getLocalBounds(), true, 1.5f);
    const juce::File out = juce::File::getCurrentWorkingDirectory().getChildFile (argv[1]);
    out.getParentDirectory().createDirectory();
    out.deleteFile();
    juce::FileOutputStream stream (out);
    juce::PNGImageFormat png;
    const bool ok = stream.openedOk() && png.writeImageToStream (image, stream);
    std::printf ("%s %s\n", ok ? "wrote" : "could not write", out.getFullPathName().toRawUTF8());
    return ok ? 0 : 1;
}
