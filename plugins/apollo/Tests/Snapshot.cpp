// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 The Pantheon authors

// Renders the plugin editor to a PNG with a curve worth looking at, over a programme for the analyser: a high-pass, a low shelf cut, a dip, two boosts, a notch at the top and a side-only band.
//
//   ApolloSnapshot <out.png> [linear] [match] [delta] [themis] [select=BAND]
//
//   match   GAIN MATCH on (the readout says what it applies)
//   delta   DELTA on
//   themis  no bands of its own: what Themis' Copy EQ hands over (five bands and a target curve) is pasted, as PASTE does it

#include "PluginEditor.h"
#include <pantheon/ui/Scaled.h>

int main (int argc, char** argv)
{
    if (argc < 2)
    {
        std::printf ("usage: ApolloSnapshot <out.png> [linear] [match] [delta] [themis] [select=BAND]\n");
        return 2;
    }
    juce::ScopedJuceInitialiser_GUI gui;
    ApolloProcessor proc;
    proc.setPlayConfigDetails (2, 2, 48000.0, 512);
    proc.prepareToPlay (48000.0, 512);

    using namespace apollo;
    const auto set = [&] (const juce::String& id, float value) { setValue (proc.apvts, id, value); };
    bool themis = false, delta = false;
    for (int i = 2; i < argc; ++i)
    {
        const juce::String arg (argv[i]);
        if (arg == "linear")
            set (id::phase, 1.0f);
        else if (arg == "match")
            set (id::match, 1.0f);
        else if (arg == "delta")
            delta = true;
        else if (arg == "themis")
            themis = true;
    }
    proc.engine.delta = delta; // before the programme, so the analyser's traces are the delta's too
    if (! themis)
    {
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
    }

    // A programme for the analyser behind the curve: a low note with its harmonics over noise that falls with frequency, the same every run.
    {
        juce::AudioBuffer<float> buffer (2, 512);
        juce::MidiBuffer midi;
        unsigned seed = 12345u;
        float lowpass = 0.0f, lowpass2 = 0.0f;
        for (int block = 0; block < 120; ++block)
        {
            for (int i = 0; i < 512; ++i)
            {
                seed = seed * 1664525u + 1013904223u;
                const float white = (float) (seed >> 8) / (float) (1 << 24) * 2.0f - 1.0f;
                lowpass += 0.12f * (white - lowpass);
                lowpass2 += 0.5f * (lowpass - lowpass2);
                const float t = (float) (block * 512 + i) / 48000.0f;
                float tone = 0.0f;
                for (int k = 1; k <= 12; ++k)
                    tone += std::sin (2.0f * 3.14159265f * 82.0f * (float) k * t) / (float) k;
                const float x = 0.045f * tone + 0.9f * lowpass2;
                buffer.setSample (0, i, x);
                buffer.setSample (1, i, 0.9f * x);
            }
            proc.processBlock (buffer, midi);
        }
    }

    std::unique_ptr<juce::AudioProcessorEditor> editor (proc.createEditor());
    editor->setVisible (true);
    auto& designed = ui::innerOf<ApolloEditor> (*editor);
    for (int i = 2; i < argc; ++i)
    {
        const juce::String arg (argv[i]);
        if (arg.startsWith ("select="))
            designed.view.selected = arg.fromFirstOccurrenceOf ("=", false, false).getIntValue() - 1;
    }
    designed.view.tick(); // what the programme above left in the taps
    if (themis)
    {
        // What Themis' Copy EQ puts on the clipboard for a programme with too much low end and too little air: a low shelf, two bells, a high shelf and a low cut, with the
        // target curve it was fitted to (a gentle tilt, 4.5 dB per octave, in the format of shared/dsp BandLink.h).
        using K = pdsp::LinkBand::Kind;
        const auto band = [] (K kind, double hz, double gain, double q, pdsp::LinkBand::Place place = pdsp::LinkBand::Place::stereo)
        {
            pdsp::LinkBand b;
            b.kind = kind;
            b.freq = hz;
            b.gainDb = gain;
            b.q = q;
            b.place = place;
            return b;
        };
        auto cut = band (K::lowCut, 35.0, 0.0, 0.7);
        cut.slopeDbPerOct = 24.0;
        pdsp::LinkCurve curve;
        curve.f0Hz = 20.0;
        curve.perOctave = 6.0;
        curve.name = "Example Target";
        for (int i = 0; i < 60; ++i)
            curve.db.push_back (-4.5 * std::log2 (curve.frequency ((size_t) i) / 1000.0));
        designed.pasteFromClipboard (juce::String (pdsp::link::encode ({ cut, band (K::lowShelf, 110.0, -3.0, 0.7), band (K::bell, 320.0, -2.5, 1.2), band (K::bell, 3400.0, 2.0, 0.9),
                                                                         band (K::highShelf, 9000.0, 3.5, 0.7) }, &curve)));
        designed.view.tick();
    }
    designed.deltaButton.setToggleState (delta, juce::dontSendNotification);
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
