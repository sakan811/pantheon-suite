// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 The Pantheon authors

#pragma once

#include "Parameters.h"
#include <pantheon/ui/Presets.h>

// Listening checks and the reference, ready to switch to. A preset sets LISTEN, REF and LEVEL MATCH; TRIM, POLARITY and REF TRIM are kept (they are the level of your
// sound, not what you listen to). The meters always read the whole sound. LISTEN and REF are saved with the session: choose Normal before you bounce.
namespace hermes
{
inline const std::vector<ui::Preset>& presets()
{
    // LISTEN is a choice: 0 Stereo, 1 Mono, 2 Left, 3 Right, 4 Side, 5 Swap.
    static const std::vector<ui::Preset> list
    {
        { "Normal", {}, "Your sound as it is: LISTEN on Stereo, the reference off, LEVEL MATCH on. Choose it before you bounce." },
        { "Mono Check", { { id::listen, 1.0f } },
          "What a mono speaker, a phone or a club system plays. A lead or a bass that fades here has a phase problem. The meters read the whole sound." },
        { "Side Only", { { id::listen, 4.0f } },
          "Only what differs between the channels: reverb, widening, stereo effects. If it is loud, the mix is wide or partly out of phase." },
        { "Left Only", { { id::listen, 2.0f } }, "The left channel in both ears: what is hard-panned, and whether the two channels are alike." },
        { "Right Only", { { id::listen, 3.0f } }, "The right channel in both ears: what is hard-panned, and whether the two channels are alike." },
        { "Swap Check", { { id::listen, 5.0f } }, "Left and right exchanged: a mix that sounds wrong swapped leans to one side. Nothing else changes." },
        { "Reference A/B", { { id::reference, 1.0f }, { id::levelMatch, 1.0f } },
          "Hears the loaded reference instead of your sound, at your sound's loudness, so you compare the tone and not the level. LOAD a track first." },
        { "Reference As Is", { { id::reference, 1.0f }, { id::levelMatch, 0.0f } }, "Hears the loaded reference at its own level (REF TRIM adds to it). Louder usually sounds better: compare with care." },
    };
    return list;
}

inline bool keepsParameter (const juce::String& pid) { return pid == id::trim || pid == id::polarity || pid == id::referenceTrim; }
} // namespace hermes
