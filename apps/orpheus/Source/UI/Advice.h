// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 The Pantheon authors

#pragma once

#include <pantheon/ui/Note.h>
#include <cmath>
#include <string>
#include <vector>

namespace orpheus::gui
{
// What the window tells the user about the state of the session, so that the editor is not used for what it cannot do and what is heard
// is not mistaken for the result. Plain C++, no JUCE (the notes are shared/ui Note.h); each rule restates a limit under "Limits" in the
// README, and UiCheck pins it.
struct AdviceInput
{
    bool busy = false;             // an analysis is running
    bool hasAudio = false;         // a recording is open and analysed
    bool playingOriginal = false;  // ORIGINAL is lit
    bool hasEdits = false;         // something can be undone
    bool projectSaved = false;     // the project has a file (the edits since are not known to be in it)
    double largestShiftSemitones = 0.0; // the biggest move of any note, absolute
    double shortestNoteMs = 1.0e9;      // the shortest note there is
    int notes = 0;
};

constexpr double kTransparentShiftSemitones = 5.0; // beyond it (to about 7) the character of the voice begins to change
constexpr double kShortNoteMs = 100.0;             // a note this short can be a mis-cut

inline std::vector<ui::Note> adviceFor (const AdviceInput& in)
{
    std::vector<ui::Note> notes;
    const auto warn = [&] (std::string t) { notes.push_back ({ ui::Severity::warning, std::move (t) }); };
    const auto info = [&] (std::string t) { notes.push_back ({ ui::Severity::info, std::move (t) }); };

    if (in.busy)
    {
        info ("Analysing the recording: the notes appear when it is done.");
        return ui::ordered (std::move (notes));
    }
    if (! in.hasAudio)
    {
        info ("Open a recording of one voice (or drop a file here). One note at a time: no chords or stacked vocals.");
        return ui::ordered (std::move (notes));
    }

    if (in.playingOriginal)
        warn ("ORIGINAL is lit: you hear the take as sung, not your edits. Press it again (Tab) to hear the result.");
    if (in.hasEdits && ! in.projectSaved)
        warn ("The edits are not saved. SAVE keeps them as a project; EXPORT writes the corrected WAV.");
    if (in.largestShiftSemitones > kTransparentShiftSemitones)
        warn ("A note is moved " + std::to_string (static_cast<int> (in.largestShiftSemitones + 0.5)) + " semitones: past about 5 the voice changes character. Try FORMANT.");
    if (in.notes > 0 && in.shortestNoteMs < kShortNoteMs)
        info ("A note under 100 ms may be a mis-cut between two others. SPLIT (S) and MERGE (M) fix the cut.");

    return ui::ordered (std::move (notes));
}

inline const char* guideTitle() { return "Orpheus Pitch Editor"; }

inline const char* guideText()
{
    return "IT IS FOR fixing the pitch of one voice, offline, note by note:\n"
           "  - Open a recording, and every note the singer sang is on the piano roll. Drag a note to where it should be, or CORRECT ALL "
           "to pull the notes onto a scale. Play it, loop it, and flip to ORIGINAL (Tab) at the same spot.\n"
           "  - Per note: how much the slow drift and the vibrato are kept, FORMANT, and GAIN. Export a WAV when it is right.\n"
           "\n"
           "IT IS NOT FOR:\n"
           "  - Chords, stacked backing vocals or a mix: it follows one note at a time. Use a solo, dry vocal.\n"
           "  - Timing: a note cannot be moved or stretched in time yet, only in pitch and loudness.\n"
           "  - Big leaps: a shift of a few semitones is transparent; beyond about 5 to 7 the voice changes character (FORMANT can help by ear).\n"
           "  - Breaths and sibilants: they have no pitch and are left as sung.\n"
           "  - Use in a DAW: it is a standalone app. Export a WAV and bring it back.\n"
           "  - Trusting the cut blindly: a very short note, or a fall-off at the end of a phrase, can be read as part of the "
           "neighbour. SPLIT (S) cuts it.\n"
           "\n"
           "BEFORE YOU START: DETECT KEY (and DETECT A4 if the song is not tuned to 440 Hz), then CORRECT ALL for a first pass and fix "
           "the rest by hand. UNDO (Cmd-Z) takes back any change. The strip at the foot of the window warns when a setting does not suit the job.";
}
}
