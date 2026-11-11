// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 The Pantheon authors

#pragma once

#include "DSP/Band.h"
#include "DSP/FundamentalEngine.h"
#include <pantheon/ui/Note.h>
#include <string>
#include <vector>

namespace hep
{
// What the editor tells the user about the band it shows and about the session, so that a tool is not used for a job it
// cannot do (the notes are the kit's, shared/ui Note.h): plain C++ with no JUCE in it, so the rules can be tested without a window (test [13]). Every rule here restates
// a limit that the README measured ("Limitations"); a new rule needs its number there too.
// The values the rules read, copied out of the parameters by the editor.
struct AdviceInput
{
    // The band shown (ignored while `bandOn` is false).
    bool bandOn = false;
    BandMode mode = BandMode::saturate;
    double lo = 20.0, hi = 20000.0;
    double driveDb = 0.0;                   // saturate
    bool amountOff = false, rebuildOn = false; // fill: AMOUNT at its minimum, REBUILD above its minimum (isOff in Parameters.h decides)
    double adaptPercent = 100.0;               // fill
    Character character = Character::warm;  // fill

    // The session.
    bool anyFillBandOn = false;
    bool midSide = false, delta = false, gainMatch = false;
    int oversamplingMode = 2;               // 0 = Off
    double mixPercent = 100.0;
    bool soloActive = false;                // a band solo or a main-band solo is on
};

// The top of the Bass band (pdsp::bands): a fill band that ends below it holds bass, where Octave cannot tell partials apart
// and where one note at a time is the usual case.
constexpr double kBassTopHz = 250.0;
// Above this a fill band reaches the mids, where one note at a time is no longer the usual case.
constexpr double kOneNoteTopHz = 400.0;
// Drive from which a saturate band stops being subtle.
constexpr double kHeavyDriveDb = 12.0;

// Warnings first, then information, each group in the order of how likely it is to matter. The editor shows the first few.
inline std::vector<ui::Note> adviceFor (const AdviceInput& in)
{
    std::vector<ui::Note> notes;
    const auto warn = [&] (std::string t) { notes.push_back ({ ui::Severity::warning, std::move (t) }); };
    const auto info = [&] (std::string t) { notes.push_back ({ ui::Severity::info, std::move (t) }); };

    // What makes the output something other than the processed mix comes first: it is the commonest "why does it sound wrong".
    if (in.delta)
        warn ("DELTA is on: you hear only what the plugin adds, not your mix. Switch it off to hear the result.");
    if (in.soloActive)
        warn ("A SOLO is on: the output is a solo, not the mix. Press it again to release.");

    if (in.bandOn && in.mode == BandMode::fill)
    {
        const bool hasAmount = ! in.amountOff;
        if (! hasAmount)
            warn ("AMOUNT is off: this band adds nothing. Turn AMOUNT up.");

        switch (in.character)
        {
            case Character::octave:
                if (in.hi <= kBassTopHz)
                    warn ("OCTAVE cannot tell bass partials apart (about 94 Hz resolution). Use CLEAN or WARM below 250 Hz.");
                info ("OCTAVE adds about 43 ms of latency, reported to the host. Set it before you play or render.");
                break;
            case Character::clean:
                if (in.hi > kOneNoteTopHz)
                    warn ("CLEAN follows one note. Past 400 Hz a band usually holds several, and it adds less. Try OCTAVE.");
                break;
            case Character::warm:
                if (in.lo >= kBassTopHz)
                    info ("WARM mixes every note in the band together, so chords turn dirty. OCTAVE keeps them apart.");
                break;
        }

        if (in.rebuildOn)
            warn ("REBUILD is experimental: on dense material it can make inharmonic bass. Leave it off unless DELTA helps.");
        if (in.adaptPercent < 0.5 && hasAmount)
            info ("ADAPT is 0 %: AMOUNT is added even where the harmonics already exist. Raise it to add only the missing.");
        if (in.midSide)
            info ("Mid/Side: fill bands work on the Mid only, so bass stays centred.");
    }
    else if (in.bandOn)
    {
        if (in.driveDb >= kHeavyDriveDb)
            warn ("Heavy DRIVE: this is distortion, not glue. Compare with GAIN MATCH so louder is not taken for better.");
        if (in.oversamplingMode == 0 && in.driveDb >= kHeavyDriveDb)
            warn ("OVERSAMPLING is off: a hard-driven band can alias (about -30 dB at 18 dB drive, -70 dB oversampled).");
        info ("A saturate band only adds harmonics. It cannot change the level of a region: for that, use an equaliser.");
    }

    if (in.gainMatch)
        info ("GAIN MATCH works against a deliberate boost, bass above all (it matches RMS, which low end dominates).");
    if (in.mixPercent < 99.5)
        info ("MIX is below 100 %: the dry signal is blended in, which is the usual way to ease a band off.");

    return ui::ordered (std::move (notes));
}

// The guide behind the GUIDE button: what Hephaestus is for and, as important, what it is not for.
inline const char* guideTitle() { return "What Hephaestus is for"; }

inline const char* guideText()
{
    return "IT IS FOR adding harmonics:\n"
           "  - Warmth and glue on a mix or a part: a SATURATE band.\n"
           "  - Bass that is too thin or vanishes on small speakers: a HARMONIC FILL band over the bass (CLEAN for one note, WARM otherwise).\n"
           "  - Dull mids and highs: an OCTAVE fill band, or a SATURATE band over the highs.\n"
           "\n"
           "IT IS NOT FOR:\n"
           "  - Fixing the tonal balance. It only adds, it never takes away or lifts what is missing from the spectrum. Use an equaliser (Iris).\n"
           "  - Controlling levels or dynamics, or making a master louder safely. It does not compress or limit, and what it adds sits on top of "
           "the signal, so watch the OUT meter. Use a compressor (Atlas) and a limiter.\n"
           "  - Cleaning up resonances, harshness or noise. It adds energy there, it does not remove it.\n"
           "  - Judging by ear alone: more harmonics sound louder, and louder sounds better. Use DELTA to hear what it adds and GAIN MATCH to compare "
           "at the same loudness.\n"
           "\n"
           "BEFORE YOU START: put it on the master or a bus before the limiter. Pick a preset, or double-click the display where you want a band. "
           "The panel under it names the job of the band you have selected and warns when its settings do not suit it.";
}
}
