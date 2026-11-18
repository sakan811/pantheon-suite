// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 The Pantheon authors

#pragma once

#include <pantheon/ui/Note.h>
#include <themis/MasterAdvisor.h>
#include <array>
#include <string>
#include <vector>

// What the editor says in its status window when nothing else is being said, so that the plugin is not used for what it cannot do
// (it measures; it does not process) and what is heard is not mistaken for the master. Plain C++, no JUCE (the notes are shared/ui
// Note.h). The master plan is the plugin's advice about the *audio*; these notes are about *how it is being used*. Tested by
// tests/test_main.cpp and pinned by the smoke test.
namespace usage
{
struct Input
{
    bool signal = false;             // audio is reaching the plugin
    bool auditionActive = false;     // Solo / Mute engaged: the output is altered
    bool comparing = false;          // COMPARE (streaming preview or the reference) is on: the output is altered
    double analysedSeconds = 0.0;    // since the last Reset
    bool measurementStarted = false; // Reset was pressed, or auto reset restarted the measurement, since the plugin was loaded
    bool resetOnPlay = true;         // Auto reset is on: a start from the beginning of the host's timeline restarts the measurement
    bool customWithoutCurve = false; // target "Custom" is selected, but no curve was learned or loaded: Flat is shown instead
};

// One line of the status window holds fewer characters than a panel of the other plugins does.
constexpr int kMaxChars = 84;

// The first-run checklist. Every tick comes from what the plugin has seen; the user ticks nothing. The first step cannot be
// seen from inside a plugin (where it sits in the chain), so it is ticked by the same evidence as the second: audio reaching it.
struct Step
{
    std::string label; // the checklist line
    std::string next;  // what the "Next:" line says while this step is the first one not yet ticked
    bool done;
};

inline std::array<Step, 4> checklist (const Input& in)
{
    const std::string seconds = std::to_string (static_cast<int> (themis::master::kMinSeconds));
    return { { { "Put Themis last in the chain", "put Themis last in the chain (after the limiter), then play the track.", in.signal },
               { "Signal seen", "play the track: no signal has reached Themis yet.", in.signal },
               { "Reset pressed / measuring started",
                 in.resetOnPlay ? "play the whole track from its start: Auto reset starts the measurement."
                                : "press Reset, then play the whole track from its start.",
                 in.measurementStarted },
               { seconds + " s of audio measured", "keep playing: the plan needs " + seconds + " s of audio, longer is better.",
                 in.analysedSeconds >= themis::master::kMinSeconds } } };
}

// Index of the first step that is not ticked, or -1 when all four are.
inline int nextStep (const Input& in)
{
    const auto steps = checklist (in);
    for (size_t i = 0; i < steps.size(); ++i)
        if (! steps[i].done)
            return static_cast<int> (i);
    return -1;
}

// The two messages that are warnings about the settings (the same words the target annotation of the tonal view uses).
inline const char* noCurveText() { return "No learned curve yet: showing Flat. Use Learn from tracks..."; }
inline const char* pressResetText() { return "PRESS RESET TO START a clean measurement, then play the whole track."; }

inline std::vector<ui::Note> notesFor (const Input& in)
{
    std::vector<ui::Note> notes;
    const auto warn = [&] (std::string t) { notes.push_back ({ ui::Severity::warning, std::move (t) }); };
    const auto info = [&] (std::string t) { notes.push_back ({ ui::Severity::info, std::move (t) }); };

    if (in.auditionActive)
        warn ("AUDITION is engaged: the output is altered. Press CLEAR before you export.");
    if (in.comparing)
        warn ("COMPARE is on: you hear a preview, not your master. CLEAR ends it.");
    if (in.customWithoutCurve)
        warn (noCurveText());
    if (in.signal && ! in.measurementStarted && ! in.resetOnPlay)
        warn (pressResetText());
    if (const int step = nextStep (in); step >= 0)
        info ("Next: " + checklist (in)[static_cast<size_t> (step)].next);

    return ui::ordered (std::move (notes));
}

inline const char* idleTip() { return "Tip: use 'Learn from tracks...' to build a target from your own reference masters."; }

inline const char* guideTitle() { return "Themis Verdict"; }

// The headroom an encode needs below 0 dBTP, read from the default ceiling (the rule of thumb the clip verdict already uses):
// a lossy codec can clip a master that sits at the ceiling, so the delivery leaves this much room. A rule of thumb, not a measurement.
inline constexpr float kEncodeHeadroomDb = -themis::history::clip::kTruePeakWarnDb;

// The GUIDE text. The glossary at its end is the one place the plugin's jargon is explained; its lines are held to kMaxChars.
inline std::string guideText()
{
    const std::string headroom = themis::format ("%.0f dB", static_cast<double> (kEncodeHeadroomDb));
    return "IT IS FOR judging a finished master:\n"
           "  - Put it last on the master bus (after the limiter), press Reset and play the whole track. The MASTER PLAN lists what to "
           "fix, worst first, with the amount and the place; hover for why.\n"
           "  - Clipping, true peak, loudness against where it is going (DELIVER FOR), mono fold and phase, the low end's steadiness and "
           "the bass fundamental. HEAR chips play what a check flags, and COMPARE plays the master as a streaming service would.\n"
           "  - The tonal curve against a target (a built-in, or one learned from masters you trust) and how it would translate to a device.\n"
           "\n"
           "IT IS NOT FOR:\n"
           "  - Changing the sound. Themis has no EQ, compressor or limiter and passes the audio untouched. Make the change in "
           "Iris or Apollo (tone), Atlas (dynamics) or Hephaestus (harmonics), or your limiter, then play it again.\n"
           "  - Telling you what is right. The plan lists objective faults only; a difference from the target curve is taste, and every "
           "limit is a rule of thumb. It is a checklist for your ears, not a replacement for them.\n"
           "  - Certifying a delivery spec. The loudness meter follows BS.1770 but is not a certified meter, and clip detection is a "
           "heuristic on the waveform.\n"
           "  - Encoding. Themis encodes nothing. A master whose true peak sits at the ceiling can clip when it is encoded to MP3 or AAC, "
           "so leave " + headroom + " or more of room below 0 dBTP before delivery (a rule of thumb).\n"
           "  - A short excerpt: a loop of the chorus reads like a chorus. The plan needs 20 s, and a whole track reads best.\n"
           "\n"
           "BEFORE YOU EXPORT: press CLEAR in AUDITION. A host that renders in real time cannot tell, and an engaged audition would end "
           "up in the bounce.\n"
           "\n"
           "WORDS USED HERE:\n"
           "  LUFS          loudness averaged over the track (BS.1770); what Deliver for uses.\n"
           "  LU            a difference in loudness: 1 LU is 1 dB on the LUFS scale.\n"
           "  dBTP          true peak: the highest level between samples, as an encode sees it.\n"
           "  PLR           peak-to-loudness ratio: true peak minus loudness (headroom left).\n"
           "  LRA           loudness range: how far the quieter and louder sections differ (LU).\n"
           "  Crest factor  peak level over average level. Low is squashed, high is spiky.\n"
           "  Mid / side    mid is shared by both channels; side is what differs between them.\n"
           "  Mono fold     summing left and right to one channel: whatever cancels is lost.\n"
           "  Tolerance     how wide the target band is; 1.00x is normal, lower is stricter.";
}
}
