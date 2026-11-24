// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 The Pantheon authors

#pragma once

#include "DSP/Engine.h"
#include <pantheon/ui/Note.h>
#include <string>
#include <vector>

namespace selene
{
// Contextual warnings and tips for Selene Poly, plain C++ (no JUCE). Every note stays within ui::kMaxNoteChars (108).
struct AdviceInput
{
    EngineParams params;
    int activeVoices = 0;
    float outputPeak = 0.0f;
};

inline std::vector<ui::Note> adviceFor (const AdviceInput& in)
{
    std::vector<ui::Note> notes;
    const auto warn = [&] (std::string t) { notes.push_back ({ ui::Severity::warning, std::move (t) }); };
    const auto info = [&] (std::string t) { notes.push_back ({ ui::Severity::info, std::move (t) }); };
    const auto& p = in.params;

    if (in.outputPeak >= 0.99f || p.masterGainDb >= 6.0f)
        warn ("Output is near or over 0 dBFS: lower MASTER GAIN to leave headroom for chords.");
    if (p.resonance >= 0.90f)
        warn ("RESONANCE is near self-oscillation: watch levels on chords and high notes.");
    if ((p.filterMode == FilterMode::lp24 || p.filterMode == FilterMode::lp12) && p.cutoffHz < 120.0f && p.filterEnvAmt < 0.10f && p.keytrack < 0.20f)
        warn ("CUTOFF is very low with little envelope or keytrack: sound may be dark or silent.");

    if (p.voiceMode == VoiceMode::mono)
        info ("MONO mode active: single-voice with last-note priority and legato glide.");
    else if (p.voiceMode == VoiceMode::unison)
        info ("UNISON active: 4 stacked voices per note with stereo spread (up to 4 notes).");
    if (p.chorusMode == ChorusMode::both)
        info ("CHORUS I+II active: dual-rate BBD stereo shimmer.");

    return ui::ordered (std::move (notes));
}

inline const char* guideTitle() { return "Selene Poly"; }

inline const char* guideText()
{
    return "FOR\n"
           "Polyphonic analog-style synthesis for City Pop: warm pads, brass stabs, glassy FM keys, "
           "slap synth bass, and shimmering chorus leads.\n\n"
           "CONTROLS\n"
           "OSC 1 & SUB give the main body (with PWM and -1 octave sub). OSC 2 adds detune, intervals, "
           "or FM modulation onto OSC 1 for electric pianos and bells. The ZDF resonant filter shapes the tone "
           "with bipolar envelope, keytrack, and analog drive. BBD CHORUS (I, II, I+II) and TAPE ECHO finish the stereo image.\n\n"
           "NOT FOR\n"
           "Sample-accurate orchestral realism or wavetable/granular resynthesis; it is a classic subtractive + FM poly synth.";
}
} // namespace selene
