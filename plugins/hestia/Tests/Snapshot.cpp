// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 The Pantheon authors

// Renders the plugin editor to a PNG: a lead vocal in a mix with a kick, a bass and a guitar, each fed a programme so the display shows real activity.
//
//   HestiaSnapshot <out.png> [delta] [alone] [sc]
//
//   delta   DELTA on
//   alone   no other stems: the empty mix
//   sc      a pad on the sidechain input

#include "PluginEditor.h"
#include <random>

namespace
{
constexpr double kFs = 48000.0;
}

int main (int argc, char** argv)
{
    if (argc < 2)
    {
        std::printf ("usage: HestiaSnapshot <out.png> [delta] [alone] [sc]\n");
        return 2;
    }
    juce::ScopedJuceInitialiser_GUI gui;
    bool alone = false, delta = false, sc = false;
    for (int i = 2; i < argc; ++i)
    {
        alone = alone || juce::String (argv[i]) == "alone";
        delta = delta || juce::String (argv[i]) == "delta";
        sc = sc || juce::String (argv[i]) == "sc";
    }

    const hestia::Role roles[] = { hestia::Role::leadVocal, hestia::Role::kick, hestia::Role::bass, hestia::Role::guitar };
    const int stems = alone ? 1 : 4;
    std::vector<std::unique_ptr<HestiaProcessor>> procs;
    for (int s = 0; s < stems; ++s)
    {
        procs.push_back (std::make_unique<HestiaProcessor>());
        auto& p = *procs.back();
        p.setPlayConfigDetails (2, 2, kFs, 512);
        p.prepareToPlay (kFs, 512);
        hestia::setValue (p.apvts, hestia::id::role, (float) (int) roles[s]);
        hestia::setValue (p.apvts, hestia::id::mix, 8.0f); // H: nobody else is in it
    }
    hestia::setValue (procs[0]->apvts, hestia::id::carve, 9.0f);
    const float depths[] = { 12.0f, 18.0f, 40.0f, 75.0f }; // the vocal up front, the guitar at the back
    for (int s = 0; s < stems; ++s)
        hestia::setValue (procs[(size_t) s]->apvts, hestia::id::distance, depths[s]);
    if (sc)
    {
        auto layout = procs[0]->getBusesLayout();
        layout.inputBuses.getReference (1) = juce::AudioChannelSet::stereo();
        procs[0]->setBusesLayout (layout);
        procs[0]->prepareToPlay (kFs, 512);
        hestia::setValue (procs[0]->apvts, hestia::id::scRole, (float) (int) hestia::Role::pad);
    }

    std::unique_ptr<juce::AudioProcessorEditor> editor (procs[0]->createEditor());
    editor->setVisible (true);
    if (delta)
        procs[0]->engine.deltaOn = true;

    // A programme: a voice (harmonics on a moving pitch), a kick every beat, a bass line, a strummed guitar (noise through two poles).
    std::mt19937 gen (7);
    std::uniform_real_distribution<float> white (-1.0f, 1.0f);
    juce::AudioBuffer<float> buffer (2, 512), withSide (4, 512);
    juce::MidiBuffer midi;
    double voicePhase = 0.0, bassPhase = 0.0;
    float g1 = 0.0f, g2 = 0.0f;
    const double kPi = 3.14159265358979323846;
    for (int block = 0; block < 520; ++block)
    {
        for (int s = 0; s < stems; ++s)
        {
            for (int i = 0; i < 512; ++i)
            {
                const long long n = (long long) block * 512 + i;
                const double t = (double) n / kFs;
                float x = 0.0f;
                switch (s)
                {
                    case 0:
                    {
                        const double f0 = 220.0 * std::pow (2.0, 0.25 * std::sin (2.0 * kPi * 0.5 * t));
                        voicePhase += 2.0 * kPi * f0 / kFs;
                        for (int h = 1; h <= 14; ++h)
                            x += (float) (std::sin ((double) h * voicePhase) / (double) h) * (h >= 6 && h <= 12 ? 1.6f : 0.6f);
                        x *= 0.12f;
                        break;
                    }
                    case 1:
                    {
                        const double tb = std::fmod (t, 0.5);
                        x = (float) (0.7 * std::sin (2.0 * kPi * (48.0 + 90.0 * std::exp (-tb * 40.0)) * tb) * std::exp (-tb * 9.0));
                        break;
                    }
                    case 2:
                    {
                        const double f = (std::fmod (t, 1.0) < 0.5 ? 55.0 : 73.4);
                        bassPhase += 2.0 * kPi * f / kFs;
                        x = (float) (0.28 * (std::sin (bassPhase) + 0.45 * std::sin (2.0 * bassPhase) + 0.2 * std::sin (3.0 * bassPhase)));
                        break;
                    }
                    default:
                    {
                        g1 += 0.12f * (white (gen) - g1);
                        g2 += 0.35f * (g1 - g2);
                        x = 0.9f * g2 * (float) (0.4 + 0.6 * std::exp (-std::fmod (t, 0.25) * 6.0));
                        break;
                    }
                }
                buffer.setSample (0, i, x);
                buffer.setSample (1, i, x);
            }
            if (s == 0 && sc)
            {
                // A pad on the sidechain: a few sustained notes' harmonics, mostly the low mids.
                for (int i = 0; i < 512; ++i)
                {
                    const double t = (double) ((long long) block * 512 + i) / kFs;
                    const float pad = (float) (0.08 * (std::sin (2.0 * kPi * 196.0 * t) + std::sin (2.0 * kPi * 246.9 * t) + 0.6 * std::sin (2.0 * kPi * 392.0 * t) + 0.4 * std::sin (2.0 * kPi * 987.8 * t)));
                    for (int c = 0; c < 2; ++c) { withSide.setSample (c, i, buffer.getSample (c, i)); withSide.setSample (2 + c, i, pad); }
                }
                procs[0]->processBlock (withSide, midi);
            }
            else
                procs[(size_t) s]->processBlock (buffer, midi);
        }
        if (block % 4 == 0)
            juce::MessageManager::getInstance()->runDispatchLoopUntil (34);
    }
    juce::MessageManager::getInstance()->runDispatchLoopUntil (100);

    const auto image = editor->createComponentSnapshot (editor->getLocalBounds(), true, 1.5f);
    const juce::File out = juce::File::getCurrentWorkingDirectory().getChildFile (argv[1]);
    out.deleteFile();
    juce::FileOutputStream stream (out);
    juce::PNGImageFormat png;
    const bool ok = stream.openedOk() && png.writeImageToStream (image, stream);
    std::printf ("%s %s\n", ok ? "wrote" : "could not write", out.getFullPathName().toRawUTF8());
    return ok ? 0 : 1;
}
