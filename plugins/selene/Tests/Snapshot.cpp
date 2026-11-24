// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 The Pantheon authors

// Renders the Selene Poly editor to a PNG while playing a City Pop chord so the filter response, oscilloscope, and voice readout are active.
//
//   SeleneSnapshot <out.png>
//   SeleneSnapshot --frames <dir>

#include "PluginEditor.h"
#include <pantheon/ui/Scaled.h>
#include <cmath>
#include <string_view>

int main (int argc, char** argv)
{
    if (argc < 2 || (std::string_view (argv[1]) == "--frames" && argc < 3))
    {
        std::printf ("usage: SeleneSnapshot <out.png> | --frames <dir>\n");
        return 2;
    }
    const bool framesMode = argc >= 3 && std::string_view (argv[1]) == "--frames";
    const auto resolvePath = [] (const char* p)
    {
        const juce::String s (p);
        return juce::File::isAbsolutePath (s) ? juce::File (s) : juce::File::getCurrentWorkingDirectory().getChildFile (s);
    };

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

    juce::PNGImageFormat png;
    if (framesMode)
    {
        const juce::File framesDir = resolvePath (argv[2]);
        framesDir.createDirectory();
        bool framesOk = true;
        int frameIdx = 0;

        for (int f = 0; f < 30; ++f)
        {
            if (f > 0)
            {
                if (f == 3)
                    midi.addEvent (juce::MidiMessage::noteOn (1, 72, 0.85f), 0);
                else if (f == 6)
                {
                    midi.addEvent (juce::MidiMessage::noteOff (1, 72), 0);
                    midi.addEvent (juce::MidiMessage::noteOn (1, 76, 0.90f), 0);
                }
                else if (f == 9)
                {
                    for (const int note : { 53, 57, 60, 64, 67, 76 })
                        midi.addEvent (juce::MidiMessage::noteOff (1, note), 0);
                }
                else if (f == 12)
                {
                    for (const int note : { 55, 59, 62, 65, 69 })
                        midi.addEvent (juce::MidiMessage::noteOn (1, note, 0.86f), 0);
                }
                else if (f == 15)
                    midi.addEvent (juce::MidiMessage::noteOn (1, 74, 0.84f), 0);
                else if (f == 18)
                {
                    midi.addEvent (juce::MidiMessage::noteOff (1, 74), 0);
                    midi.addEvent (juce::MidiMessage::noteOn (1, 71, 0.82f), 0);
                }
                else if (f == 20)
                {
                    for (const int note : { 55, 59, 62, 65, 69, 71 })
                        midi.addEvent (juce::MidiMessage::noteOff (1, note), 0);
                }
                else if (f == 22)
                {
                    for (const int note : { 48, 55, 59, 62, 64 })
                        midi.addEvent (juce::MidiMessage::noteOn (1, note, 0.90f), 0);
                }
                else if (f == 26)
                {
                    midi.addEvent (juce::MidiMessage::noteOn (1, 67, 0.85f), 0);
                    midi.addEvent (juce::MidiMessage::noteOn (1, 71, 0.85f), 0);
                }
                const int ccVal = juce::jlimit (0, 127, (int) std::lround (63.5 + 63.5 * std::sin (6.2831853 * (double) f / 29.0)));
                midi.addEvent (juce::MidiMessage::controllerEvent (1, 1, ccVal), 0);

                for (int b = 0; b < 8; ++b)
                {
                    buffer.clear();
                    proc.processBlock (buffer, midi);
                    midi.clear();
                    if (b % 4 == 0)
                        juce::MessageManager::getInstance()->runDispatchLoopUntil (20);
                }
                designed.tickDisplay();
                juce::MessageManager::getInstance()->runDispatchLoopUntil (20);
            }

            const auto frame = designed.createComponentSnapshot (designed.getLocalBounds(), true, 1.5f);
            const auto outFile = framesDir.getChildFile (juce::String::formatted ("frame_%03d.png", frameIdx++));
            outFile.deleteFile();
            juce::FileOutputStream stream (outFile);
            framesOk = framesOk && stream.openedOk() && png.writeImageToStream (frame, stream);
        }
        std::printf ("%s %d frames to %s\n", framesOk ? "wrote" : "could not write", frameIdx, framesDir.getFullPathName().toRawUTF8());
        return framesOk && frameIdx == 30 ? 0 : 1;
    }

    const auto image = designed.createComponentSnapshot (designed.getLocalBounds(), true, 1.5f);
    const juce::File out = resolvePath (argv[1]);
    out.getParentDirectory().createDirectory();
    out.deleteFile();
    juce::FileOutputStream stream (out);
    const bool ok = stream.openedOk() && png.writeImageToStream (image, stream);
    std::printf ("%s %s\n", ok ? "wrote" : "could not write", out.getFullPathName().toRawUTF8());
    return ok ? 0 : 1;
}
