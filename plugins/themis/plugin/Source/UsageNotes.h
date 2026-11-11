// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 The Pantheon authors

#pragma once

#include <pantheon/ui/Note.h>
#include <themis/MasterAdvisor.h>
#include <string>
#include <vector>

// What the editor says in its status window when nothing else is being said, so that the plugin is not used for what it cannot do
// (it measures; it does not process) and what is heard is not mistaken for the master. Plain C++, no JUCE (the notes are shared/ui
// Note.h). The master plan is the plugin's advice about the *audio*; these notes are about *how it is being used*. Pinned by
// the smoke test.
namespace usage
{
struct Input
{
    bool signal = false;           // audio is reaching the plugin
    bool auditionActive = false;   // Solo / Mute engaged: the output is altered
    bool comparing = false;        // COMPARE (streaming preview or the reference) is on: the output is altered
    double analysedSeconds = 0.0;  // since the last Reset
};

// One line of the status window holds fewer characters than a panel of the other plugins does.
constexpr int kMaxChars = 84;

inline std::vector<ui::Note> notesFor (const Input& in)
{
    std::vector<ui::Note> notes;
    const auto warn = [&] (std::string t) { notes.push_back ({ ui::Severity::warning, std::move (t) }); };
    const auto info = [&] (std::string t) { notes.push_back ({ ui::Severity::info, std::move (t) }); };

    if (in.auditionActive)
        warn ("AUDITION is engaged: the output is altered. Press CLEAR before you export.");
    if (in.comparing)
        warn ("COMPARE is on: you hear a preview, not your master. CLEAR ends it.");
    if (! in.signal)
        info ("No signal yet. Put Themis last on the master (after the limiter) and play the track.");
    else if (in.analysedSeconds < themis::master::kMinSeconds)
        info ("Measuring: the plan needs " + std::to_string (static_cast<int> (themis::master::kMinSeconds)) + " s of audio. Play the whole track after Reset.");

    return ui::ordered (std::move (notes));
}

inline const char* idleTip() { return "Tip: use 'Learn from tracks...' to build a target from your own reference masters."; }

inline const char* guideTitle() { return "Themis Verdict"; }

inline const char* guideText()
{
    return "IT IS FOR judging a finished master:\n"
           "  - Put it last on the master bus (after the limiter), press Reset and play the whole track. The MASTER PLAN lists what to "
           "fix, worst first, with the amount and the place; hover for why.\n"
           "  - Clipping, true peak, loudness against where it is going (DELIVER FOR), mono fold and phase, the low end's steadiness and "
           "the bass fundamental. HEAR chips play what a check flags, and COMPARE plays the master as a streaming service would.\n"
           "  - The tonal curve against a target (a built-in, or one learned from masters you trust) and how it would translate to a device.\n"
           "\n"
           "IT IS NOT FOR:\n"
           "  - Changing the sound. Themis has no EQ, compressor or limiter and passes the audio untouched. Make the change in "
           "Iris (tone), Atlas (dynamics) or Hephaestus (harmonics), or your limiter, then play it again.\n"
           "  - Telling you what is right. The plan lists objective faults only; a difference from the target curve is taste, and every "
           "limit is a rule of thumb. It is a checklist for your ears, not a replacement for them.\n"
           "  - Certifying a delivery spec. The loudness meter follows BS.1770 but is not a certified meter, and clip detection is a "
           "heuristic on the waveform.\n"
           "  - A short excerpt: a loop of the chorus reads like a chorus. The plan needs 20 s, and a whole track reads best.\n"
           "\n"
           "BEFORE YOU EXPORT: press CLEAR in AUDITION. A host that renders in real time cannot tell, and an engaged audition would end "
           "up in the bounce.";
}
}
