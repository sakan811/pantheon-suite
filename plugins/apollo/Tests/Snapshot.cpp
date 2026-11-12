// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 The Pantheon authors

// Renders the plugin editor to a PNG with a curve worth looking at: a high-pass, a low shelf cut, a dip, two boosts, a notch at the top and a side-only band.
//
//   ApolloSnapshot <out.png> [linear] [select=BAND]

#include "PluginEditor.h"
#include <pantheon/ui/Scaled.h>

int main (int argc, char** argv)
{
    if (argc < 2)
    {
        std::printf ("usage: ApolloSnapshot <out.png> [linear] [select=BAND]\n");
        return 2;
    }
    juce::ScopedJuceInitialiser_GUI gui;
    ApolloProcessor proc;
    proc.setPlayConfigDetails (2, 2, 48000.0, 512);
    proc.prepareToPlay (48000.0, 512);

    using namespace apollo;
    const auto set = [&] (const juce::String& id, float value) { setValue (proc.apvts, id, value); };
    set (bandIds (0).on, 1.0f);
    set (bandIds (0).slope, 2.0f);
    set (bandIds (1).gain, -4.0f);
    set (bandIds (2).gain, -2.5f);
    set (bandIds (2).freq, 320.0f);
    set (bandIds (3).gain, 3.5f);
    set (bandIds (3).freq, 1800.0f);
    set (bandIds (3).q, 0.8f);
    set (bandIds (4).gain, -5.0f);
    set (bandIds (4).freq, 3400.0f);
    set (bandIds (4).q, 4.0f);
    set (bandIds (5).gain, 2.0f);
    set (bandIds (5).freq, 5200.0f);
    set (bandIds (5).target, 2.0f); // sides only
    set (bandIds (6).gain, 3.0f);
    set (bandIds (7).on, 1.0f);
    set (bandIds (7).freq, 17000.0f);

    std::unique_ptr<juce::AudioProcessorEditor> editor (proc.createEditor());
    editor->setVisible (true);
    auto& designed = ui::innerOf<ApolloEditor> (*editor);
    for (int i = 2; i < argc; ++i)
    {
        const juce::String arg (argv[i]);
        if (arg == "linear")
            set (id::phase, 1.0f);
        else if (arg.startsWith ("select="))
            designed.view.selected = arg.fromFirstOccurrenceOf ("=", false, false).getIntValue() - 1;
    }
    juce::MessageManager::getInstance()->runDispatchLoopUntil (200);
    designed.showTypes();

    const auto image = designed.createComponentSnapshot (designed.getLocalBounds(), true, 1.5f);
    const juce::File out = juce::File::getCurrentWorkingDirectory().getChildFile (argv[1]);
    out.deleteFile();
    juce::FileOutputStream stream (out);
    juce::PNGImageFormat png;
    const bool ok = stream.openedOk() && png.writeImageToStream (image, stream);
    std::printf ("%s %s\n", ok ? "wrote" : "could not write", out.getFullPathName().toRawUTF8());
    return ok ? 0 : 1;
}
