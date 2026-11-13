// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 The Pantheon authors

#pragma once

#include "Parameters.h"
#include <pantheon/ui/Presets.h>

// A starting point for each kind of stem: its role and how much it matters, how deep the cut may go and how far back it sits. A preset leaves the mix letter, the
// sidechain's role and importance, and OUTPUT alone (they belong to the session, not to the stem); what it does not list is back at its default. The roles are
// rules of thumb: move IMPORTANCE by ear.
namespace hestia
{
inline const std::vector<ui::Preset>& presets()
{
    // The role is a choice: 0 Kick, 1 Bass, 2 Drums, 3 Hats / Cymbals, 4 Lead Vocal, 5 Backing Vocal, 6 Guitar, 7 Keys, 8 Pad, 9 Lead Synth, 10 Strings, 11 FX, 12 Full Range.
    static const std::vector<ui::Preset> list
    {
        { "Init", {}, "The defaults: the role Hestia opens with, an importance of 50%, a cut of 6 dB, at the front. Pick the role, then the rest." },
        { "Kick", { { id::role, 0.0f }, { id::importance, 70.0f }, { id::carve, 4.0f } },
          "The kick owns the low end: the stems that share it give way to it. Lower IMPORTANCE if the bass should win the sub instead." },
        { "Bass", { { id::role, 1.0f }, { id::importance, 65.0f }, { id::carve, 5.0f } },
          "The bass owns the low-mids under the kick's thump. Two stems with the same role and importance leave each other alone." },
        { "Drum Bus", { { id::role, 2.0f }, { id::importance, 60.0f }, { id::carve, 5.0f } },
          "A drum group: a broad claim on the lows and the attack. Put the kick and the snare on their own roles if you have them apart." },
        { "Lead Vocal", { { id::role, 4.0f }, { id::importance, 80.0f }, { id::carve, 3.0f } },
          "Up front: the other stems make room at the voice's presence, and the voice is hardly cut itself." },
        { "Backing Vocals", { { id::role, 5.0f }, { id::importance, 35.0f }, { id::carve, 9.0f }, { id::distance, 35.0f } },
          "Tucked behind the lead: it gives way to the voice and sits a little back (DEPTH 35%)." },
        { "Guitars", { { id::role, 6.0f }, { id::importance, 45.0f }, { id::carve, 7.0f }, { id::distance, 20.0f } },
          "A rhythm part behind the vocal and the drums: gives way at the mids and sits a little back." },
        { "Lead Synth", { { id::role, 9.0f }, { id::importance, 60.0f }, { id::carve, 5.0f }, { id::distance, 10.0f } },
          "A hook that has to be heard: a claim on the mids, cut little. Raise IMPORTANCE if it fights the vocal." },
        { "Pads and Strings", { { id::role, 8.0f }, { id::importance, 25.0f }, { id::carve, 10.0f }, { id::distance, 55.0f } },
          "Fills the background: gives way to everything and sits well back (DEPTH 55%). For pads, strings and ambience." },
        { "FX and Ambience", { { id::role, 11.0f }, { id::importance, 20.0f }, { id::carve, 12.0f }, { id::reach, 18.0f }, { id::distance, 65.0f } },
          "Risers, sweeps, room sounds: they give way almost everywhere and sit far back. DEPTH is not a reverb: it moves the dry sound back." },
    };
    return list;
}

inline bool keepsParameter (const juce::String& pid) { return pid == id::mix || pid == id::scRole || pid == id::scImportance || pid == id::out; }
} // namespace hestia
