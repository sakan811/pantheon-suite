// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 The Pantheon authors

#pragma once

#include <pantheon/ui/Note.h>
#include <cmath>
#include <string>
#include <vector>

namespace harmonia
{
// What the editor tells the user about the settings in front of them, so that a pitch corrector is not used for what it cannot do and
// what is heard is not mistaken for the result. Plain C++, no JUCE (the notes are shared/ui Note.h); each rule restates a limit under
// "Limitations" or "Features" in the README, and the selftest pins it.
struct AdviceInput
{
    bool bypass = false, autoKey = false;
    double speedMs = 35.0, formantSemitones = 0.0, tuningHz = 440.0, holdMs = 70.0;
    int allowedNotes = 12;       // notes the voice may land on
    bool voiceRangeFixed = false; // Voice is not Auto
};

constexpr double kInstantSpeedMs = 10.0;  // below this the correction is the robotic effect
constexpr int kFewNotes = 5;              // fewer allowed notes than a pentatonic scale

inline std::vector<ui::Note> adviceFor (const AdviceInput& in)
{
    std::vector<ui::Note> notes;
    const auto warn = [&] (std::string t) { notes.push_back ({ ui::Severity::warning, std::move (t) }); };
    const auto info = [&] (std::string t) { notes.push_back ({ ui::Severity::info, std::move (t) }); };

    if (in.bypass)
        warn ("BYPASS is on: you hear the original vocal, not the corrected one. Switch it off to hear the correction.");
    if (in.allowedNotes == 0)
        warn ("No note is allowed, so nothing can be corrected. Click piano keys, or pick a scale.");
    else if (in.allowedNotes < kFewNotes)
        warn ("Only " + std::to_string (in.allowedNotes) + " notes are allowed: a sung note is pulled to the nearest of them, even a right one.");
    if (std::fabs (in.tuningHz - 440.0) > 0.5)
        warn ("TUNING (A4) is " + std::to_string ((int) std::lround (in.tuningHz)) + " Hz, not 440: right only if the song is tuned that way too.");

    if (in.speedMs < kInstantSpeedMs)
        info ("SPEED is almost instant: that is the robotic effect. 20 - 60 ms is transparent.");
    if (in.holdMs < 0.5)
        info ("NOTE HOLD is 0: the correction switches at once and can flicker to a wrong neighbouring note.");
    if (in.autoKey)
        info ("AUTO KEY needs about 6 s of singing, follows major and minor only, and ignores KEY and SCALE.");
    if (std::fabs (in.formantSemitones) > 0.05)
        info ("FORMANT is moved: the voice will not sound like the singer. Leave it at 0 to keep their own voice.");
    if (in.voiceRangeFixed)
        info ("VOICE narrows the pitch search (to G4, F5 or C6 at most). A voice above it is not tracked. Auto covers all.");

    return ui::ordered (std::move (notes));
}

inline const char* guideTitle() { return "Harmonia Pitch"; }

inline const char* guideText()
{
    return "IT IS FOR tuning one voice at a time:\n"
           "  - A solo lead vocal, or a solo instrument that plays one note at a time, snapped to the notes of a key. Vibrato and "
           "expression stay (NATURAL), and the voice keeps its colour (the grains are re-spaced, not resampled).\n"
           "  - From transparent (Natural) to polished (Pop) to the robotic effect (Hard-tune).\n"
           "  - Following a melody or chord you play: while MIDI notes are held, only their note names are allowed (any octave); release them and the "
           "piano keys are back. The host has to send MIDI to the plug-in (VST3 and the standalone do; most AU hosts do not for an effect).\n"
           "\n"
           "IT IS NOT FOR:\n"
           "  - Chords, backing-vocal stacks or full mixes: it follows one pitch, so it will pull the wrong thing. Put it on the "
           "isolated vocal only.\n"
           "  - Rescuing a wrong note: it moves a note to the nearest allowed one, so block notes the song never uses; a note far off "
           "is pulled to the wrong neighbour.\n"
           "  - Fixing timing, breaths or noise: consonants and breaths pass through untouched.\n"
           "  - Monitoring while recording: the delay is fixed at 46 ms. Record dry and add Harmonia afterwards.\n"
           "\n"
           "BEFORE YOU START: set the key (DETECT KEY, or AUTO KEY for a song that changes key) and block notes the song never "
           "uses on the piano. Choose a style, then lower SPEED if it sounds robotic. Use BYPASS to compare with the original, which "
           "stays aligned. The strip under the controls warns when a setting does not suit its job.";
}
}
