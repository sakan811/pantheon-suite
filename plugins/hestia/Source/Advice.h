// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 The Pantheon authors

#pragma once

#include "DSP/Engine.h"
#include <pantheon/ui/Note.h>
#include <string>
#include <vector>

namespace hestia
{
// What the editor tells the user about the stem and the mix, so that Hestia is not used for what it cannot do and what is heard is not mistaken
// for the result. Plain C++, no JUCE (the notes are shared/ui Note.h); each rule restates a limit under "Limits" in the README, and UiCheck pins it.
struct AdviceInput
{
    EngineParams params;
    int others = 0;               // the other stems of the mix that are playing now
    int sameClaim = 0;            // of them, those with this stem's role and about its importance: neither gives way to the other
    float strongestCutDb = 0.0f;  // the biggest cut in any band now (negative)
    bool delta = false;
    bool sidechain = false;       // a sidechain is connected
    int sentTo = -1;              // COPY TO MIX has just handed its settings to this many other instances (-1: not lately)
};

constexpr float kSameImportance = 0.05f;
constexpr float kHollowCutDb = 9.0f;
constexpr float kFlutterReleaseMs = 60.0f;
constexpr float kFarBack = 0.8f; // from this depth the stem is clearly at the back

inline std::vector<ui::Note> adviceFor (const AdviceInput& in)
{
    std::vector<ui::Note> notes;
    const auto warn = [&] (std::string t) { notes.push_back ({ ui::Severity::warning, std::move (t) }); };
    const auto info = [&] (std::string t) { notes.push_back ({ ui::Severity::info, std::move (t) }); };
    const auto& p = in.params;

    if (in.delta)
        warn ("DELTA is on: you hear only what Hestia changes, not the stem. Switch it off.");
    if (in.sentTo >= 0)
        info (in.sentTo == 0 ? "Copy to mix: no other instance is in this mix yet, so nothing was sent."
                              : "Copied CARVE, REACH, ATTACK and RELEASE to " + std::to_string (in.sentTo) + (in.sentTo == 1 ? " other instance" : " other instances") + " of this mix.");
    if (p.hold)
        info ("HOLD is on: the cut stays as it is, whatever the other stems do. Switch it off to follow them again.");
    if (p.group == 0)
        warn ("MIX is Off: this stem is not linked to any other, so nothing is cut. Pick a mix, A to H.");
    else if (in.others == 0)
        warn ("No other stem is playing in this mix. Put Hestia on the other stems and give them the same MIX.");
    if (p.group != 0 && p.carveDb <= 0.0f && p.distance <= 0.0f)
        warn ("CARVE is 0 dB and DEPTH is 0: Hestia changes nothing. Raise one of them.");
    if (in.sameClaim > 0 && p.carveDb > 0.0f)
        info ("Another stem has this role and about this IMPORTANCE: neither gives way. Raise it on the one that matters.");
    if (p.role == Role::fullRange)
        info ("Role Full Range claims every band a little. Pick the stem's real role so it gives way where it should.");
    if (p.carveDb >= kHollowCutDb && -in.strongestCutDb >= kHollowCutDb - 0.5f)
        warn ("Cutting " + std::to_string ((int) (-in.strongestCutDb + 0.5f)) + " dB in a band: the stem can sound hollow. Lower CARVE, or REACH.");
    if (p.distance >= kFarBack)
        info ("A high DEPTH puts the stem far back: duller, quieter, narrower, with early reflections. Not for a lead.");
    if (in.sidechain && p.scRole == Role::fullRange && p.group != 0)
        info ("A sidechain is connected: set SC ROLE to what it is, or it claims little and nothing is cut for it.");
    if (p.releaseMs < kFlutterReleaseMs && p.group != 0)
        info ("A RELEASE under 60 ms follows the other stems' every note: the cuts can flutter. Try 200 ms or more.");
    if (p.group != 0 && in.others > 0 && p.importance <= 0.0f)
        info ("IMPORTANCE is 0: this stem's claim is halved, so almost every other stem is let in front of it.");

    return ui::ordered (std::move (notes));
}

inline const char* guideTitle() { return "Hestia Space"; }

inline const char* guideText()
{
    return "IT IS FOR making room between stems, in frequency and in depth, while they play:\n"
           "  - Put Hestia on each stem (kick, bass, vocal, guitars, pads...), give each its ROLE, and the same MIX. Each one then gives up the "
           "bands where a stem with a bigger claim is playing, and only while it plays. The owner of a band is never touched there.\n"
           "  - The strip under the spectrum says who owns each band; the amber bars below it are the cut this stem makes now.\n"
           "  - IMPORTANCE moves a stem's claim: raise it on the stem that must not lose room, lower it on the one that may.\n"
           "  - CARVE is the most any band is cut, REACH how much quieter than this stem another can be and still be given room, "
           "ATTACK and RELEASE how fast the cut comes in and goes out. DELTA lets you hear exactly what Hestia changes.\n"
           "  - HOLD freezes the cut where it is now (this stem is still seen by the others): set it while the mix plays as you want it, and the cut stays "
           "through a section where the others change.\n"
           "  - COPY TO MIX hands this stem's CARVE, REACH, ATTACK and RELEASE to every other Hestia in the same mix, so they are set once.\n"
           "  - DEPTH puts the stem back from the front: the air takes the highs, the level falls, the sides narrow and the first reflections "
           "of a room (up to 37 ms) come up. The depth map shows where each stem sits, and a stem at the back also gives way to the ones in front.\n"
           "  - SIDECHAIN: route any track into Hestia's sidechain input (a track with no Hestia on it, from any plugin chain) and set SC ROLE "
           "and SC IMPORTANCE to say what it is: it is then one more stem in the mix. It is only listened to, never heard.\n"
           "  - Where the host tells Hestia the name of its track, the lists and the depth map call each stem by it instead of by its role.\n"
           "\n"
           "IT IS NOT FOR:\n"
           "  - Fixing a single stem: it only cuts, and only because another stem is there. For a fixed cut use an equaliser (Iris).\n"
           "  - Making a room: DEPTH is distance cues and a few early reflections, with no tail. For a space around a sound use a reverb, "
           "and use DEPTH to say how far back the dry sound sits.\n"
           "  - Knowing what is good: the roles are rules of thumb. Two stems of the same role and importance leave each other alone; "
           "you say which one matters.\n"
           "  - Linking across processes: stems with Hestia on them see each other only inside one host process. A host that runs plugins in "
           "separate processes, or a bounce on another machine, does not link them. The sidechain input works wherever the host can route it.\n"
           "  - Adding what is missing: it never lifts a band. Use Hephaestus to add harmonics.\n"
           "\n"
           "GOOD TO KNOW: it adds no delay. The other stems are read as they are, not ahead of time, so a note is cut after it has begun: for "
           "a sound that starts hard (a kick) set the other stems' ATTACK short. An offline bounce can differ from playback in the finest "
           "details, because the stems are not processed in lockstep.";
}
} // namespace hestia
