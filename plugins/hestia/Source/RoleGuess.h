// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 The Pantheon authors

#pragma once

#include "DSP/Roles.h"
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// AUTO ROLE: the role a track's name suggests, for a beginner who would rather not choose it by hand. It reads words of the name only, never the
// audio, and it guesses nothing it cannot see: a name with no word from the list gives no role (the editor says so). Plain C++, no JUCE (Tests.cpp includes it).
//
// The name is split into words: letters and digits are words of their own, any other character separates them, and a lower-case letter followed by
// an upper-case one starts a new word (LeadVox is two). Each word the list knows has a role and a priority, and the highest priority wins; between
// equal priorities the word further left wins. The priorities say which word names the sound more exactly: a backing vocal beats a lead vocal,
// a kick beats drums, a bass beats a synth, and a synth beats nothing (Synth Pad is a pad). Words the list does not know are ignored: a track called
// "Kickstart" or "Bassoon" has no role, because its words are not the word.
namespace hestia
{
namespace guess
{
struct Keyword
{
    std::string_view word; // lower case, as the tokeniser makes it
    Role role;
    int priority;          // 5 backing vocal, 4 kick, 3 the solid instruments and the lead vocal, 2 the texture roles, 1 the weakest hints
};

// Abbreviations are the common DAW ones (KD, BD, SD, SN, HH, OHH, KY, KBD, GTR, LVOX, VXB, BGV, SY, DBL is neutral); each is a whole word.
inline constexpr Keyword kKeywords[] = {
    // backing vocals
    { "bv", Role::backVocal, 5 }, { "bvs", Role::backVocal, 5 }, { "bgv", Role::backVocal, 5 }, { "bgvs", Role::backVocal, 5 },
    { "bvox", Role::backVocal, 5 }, { "vxb", Role::backVocal, 5 }, { "harm", Role::backVocal, 5 }, { "harmony", Role::backVocal, 5 },
    { "harmonies", Role::backVocal, 5 }, { "choir", Role::backVocal, 5 },
    // kick
    { "kick", Role::kick, 4 }, { "kicks", Role::kick, 4 }, { "kik", Role::kick, 4 }, { "kck", Role::kick, 4 }, { "kd", Role::kick, 4 },
    { "bd", Role::kick, 4 }, { "bassdrum", Role::kick, 4 }, { "kickdrum", Role::kick, 4 },
    // bass
    { "bass", Role::bass, 3 }, { "basses", Role::bass, 3 }, { "bs", Role::bass, 3 }, { "sub", Role::bass, 3 }, { "subs", Role::bass, 3 },
    { "subbass", Role::bass, 3 }, { "808", Role::bass, 3 }, { "808s", Role::bass, 3 }, { "bassline", Role::bass, 3 },
    // lead vocal
    { "vox", Role::leadVocal, 3 }, { "vocal", Role::leadVocal, 3 }, { "vocals", Role::leadVocal, 3 }, { "voc", Role::leadVocal, 3 },
    { "vx", Role::leadVocal, 3 }, { "lvox", Role::leadVocal, 3 }, { "leadvox", Role::leadVocal, 3 }, { "singer", Role::leadVocal, 3 },
    // guitar
    { "gtr", Role::guitar, 3 }, { "gtrs", Role::guitar, 3 }, { "gt", Role::guitar, 3 }, { "guit", Role::guitar, 3 }, { "guitar", Role::guitar, 3 },
    { "guitars", Role::guitar, 3 }, { "strat", Role::guitar, 3 }, { "tele", Role::guitar, 3 },
    // cymbals and hats
    { "hat", Role::cymbals, 3 }, { "hats", Role::cymbals, 3 }, { "hh", Role::cymbals, 3 }, { "hihat", Role::cymbals, 3 }, { "hihats", Role::cymbals, 3 },
    { "ohh", Role::cymbals, 3 }, { "oh", Role::cymbals, 3 }, { "ohs", Role::cymbals, 3 }, { "overhead", Role::cymbals, 3 },
    { "overheads", Role::cymbals, 3 }, { "cym", Role::cymbals, 3 }, { "cyms", Role::cymbals, 3 }, { "cymbal", Role::cymbals, 3 },
    { "cymbals", Role::cymbals, 3 }, { "crash", Role::cymbals, 3 }, { "crashes", Role::cymbals, 3 }, { "ride", Role::cymbals, 3 },
    { "rides", Role::cymbals, 3 }, { "splash", Role::cymbals, 3 }, { "china", Role::cymbals, 3 },
    // strings
    { "strings", Role::strings, 3 }, { "string", Role::strings, 3 }, { "strs", Role::strings, 3 }, { "str", Role::strings, 3 },
    { "violin", Role::strings, 3 }, { "violins", Role::strings, 3 }, { "vln", Role::strings, 3 }, { "viola", Role::strings, 3 },
    { "cello", Role::strings, 3 }, { "vc", Role::strings, 3 }, { "orch", Role::strings, 3 }, { "orchestra", Role::strings, 3 },
    // keys
    { "keys", Role::keys, 2 }, { "key", Role::keys, 2 }, { "ky", Role::keys, 2 }, { "kb", Role::keys, 2 }, { "kbd", Role::keys, 2 },
    { "keyboard", Role::keys, 2 }, { "keyboards", Role::keys, 2 }, { "piano", Role::keys, 2 }, { "pno", Role::keys, 2 }, { "pn", Role::keys, 2 },
    { "rhodes", Role::keys, 2 }, { "wurli", Role::keys, 2 }, { "wurlitzer", Role::keys, 2 }, { "organ", Role::keys, 2 }, { "org", Role::keys, 2 },
    { "ep", Role::keys, 2 }, { "epiano", Role::keys, 2 }, { "clav", Role::keys, 2 }, { "clavinet", Role::keys, 2 },
    // pad
    { "pad", Role::pad, 2 }, { "pads", Role::pad, 2 }, { "atmo", Role::pad, 2 }, { "atmos", Role::pad, 2 }, { "atmosphere", Role::pad, 2 },
    // drums and percussion
    { "drum", Role::drums, 2 }, { "drums", Role::drums, 2 }, { "drm", Role::drums, 2 }, { "drms", Role::drums, 2 }, { "dr", Role::drums, 2 },
    { "kit", Role::drums, 2 }, { "drumkit", Role::drums, 2 }, { "perc", Role::drums, 2 }, { "percs", Role::drums, 2 },
    { "percussion", Role::drums, 2 }, { "snare", Role::drums, 2 }, { "snares", Role::drums, 2 }, { "sn", Role::drums, 2 }, { "snr", Role::drums, 2 },
    { "sd", Role::drums, 2 }, { "tom", Role::drums, 2 }, { "toms", Role::drums, 2 }, { "clap", Role::drums, 2 }, { "claps", Role::drums, 2 },
    { "shaker", Role::drums, 2 }, { "conga", Role::drums, 2 }, { "congas", Role::drums, 2 }, { "bongo", Role::drums, 2 }, { "bongos", Role::drums, 2 },
    // effects, ambience, returns
    { "fx", Role::fx, 2 }, { "sfx", Role::fx, 2 }, { "riser", Role::fx, 2 }, { "risers", Role::fx, 2 }, { "impact", Role::fx, 2 },
    { "impacts", Role::fx, 2 }, { "foley", Role::fx, 2 }, { "noise", Role::fx, 2 }, { "amb", Role::fx, 2 }, { "ambience", Role::fx, 2 },
    { "ambient", Role::fx, 2 }, { "sweep", Role::fx, 2 }, { "sweeps", Role::fx, 2 }, { "reverb", Role::fx, 2 }, { "rvb", Role::fx, 2 },
    { "verb", Role::fx, 2 }, { "delay", Role::fx, 2 }, { "dly", Role::fx, 2 },
    // lead synth (the weakest word: a synth is a synth unless another word says what it is)
    { "synth", Role::leadSynth, 1 }, { "synths", Role::leadSynth, 1 }, { "syn", Role::leadSynth, 1 }, { "sy", Role::leadSynth, 1 },
    { "arp", Role::leadSynth, 1 }, { "arps", Role::leadSynth, 1 },
    // the whole mix
    { "master", Role::fullRange, 1 }, { "mstr", Role::fullRange, 1 }, { "mix", Role::fullRange, 1 }, { "mixbus", Role::fullRange, 1 },
    { "stereo", Role::fullRange, 1 }, { "full", Role::fullRange, 1 }, { "fullrange", Role::fullRange, 1 },
};

// Words that make a vocal a backing vocal when they stand with one (Back Vox, BG Vocals).
inline bool isBackingModifier (std::string_view w) noexcept
{
    return w == "back" || w == "backing" || w == "background" || w == "bg";
}

// Words that name the drum a bass word is part of (Bass Drum is a kick).
inline bool isDrumWord (std::string_view w) noexcept
{
    return w == "drum" || w == "drums" || w == "dr" || w == "drm";
}

// The name as words: lower case, letters and digits apart, split where a lower-case letter meets an upper-case one.
inline std::vector<std::string> words (std::string_view name)
{
    const auto isLower = [] (char c) { return c >= 'a' && c <= 'z'; };
    const auto isUpper = [] (char c) { return c >= 'A' && c <= 'Z'; };
    const auto isDigit = [] (char c) { return c >= '0' && c <= '9'; };
    std::vector<std::string> out;
    std::string current;
    int kind = 0; // 1 letters, 2 digits, 0 none
    bool lowerBefore = false;
    const auto flush = [&] {
        if (! current.empty())
            out.push_back (current);
        current.clear();
    };
    for (const char c : name)
    {
        const int k = isLower (c) || isUpper (c) ? 1 : isDigit (c) ? 2 : 0;
        if (k == 0)
        {
            flush();
            kind = 0;
            lowerBefore = false;
            continue;
        }
        if (k != kind || (k == 1 && isUpper (c) && lowerBefore))
            flush();
        current += (char) (isUpper (c) ? c - 'A' + 'a' : c);
        kind = k;
        lowerBefore = isLower (c);
    }
    flush();
    return out;
}
} // namespace guess

// The role the name suggests and the priority of the word that gave it, or nothing when no word of it is on the list.
struct Guess
{
    Role role;
    int priority;
};

inline std::optional<Guess> guessFromName (std::string_view name)
{
    const auto w = guess::words (name);
    bool backing = false, vocal = false;
    for (const auto& word : w)
    {
        backing = backing || guess::isBackingModifier (word);
        for (const auto& k : guess::kKeywords)
            if (k.word == word && k.role == Role::leadVocal)
                vocal = true;
    }
    if (backing && vocal)
        return Guess { Role::backVocal, 5 };

    for (size_t i = 0; i + 1 < w.size(); ++i)
        if (w[i] == "bass" && guess::isDrumWord (w[i + 1]))
            return Guess { Role::kick, 4 };

    std::optional<Guess> best;
    for (const auto& word : w)
        for (const auto& k : guess::kKeywords)
            if (k.word == word && (! best || k.priority > best->priority))
                best = Guess { k.role, k.priority }; // strictly greater: between equal words the one further left stays
    return best;
}

inline std::optional<Role> roleFromName (std::string_view name)
{
    const auto g = guessFromName (name);
    if (! g)
        return std::nullopt;
    return g->role;
}

// AUTO ROLE by itself, when the host gives the track name: only when `roleUntouched` (ROLE is still its default and no saved session set it), and
// only for a confident guess. A word of priority 1 (synth, mix, master, full, stereo) is a hint, not a name: it does not decide by itself.
// This is the rule the processor runs once, on the first name the host gives; the editor's button still takes any guess.
inline constexpr int kConfidentPriority = 2;

inline std::optional<Role> autoRoleOnName (std::string_view name, bool roleUntouched)
{
    if (! roleUntouched)
        return std::nullopt;
    const auto g = guessFromName (name);
    if (! g || g->priority < kConfidentPriority)
        return std::nullopt;
    return g->role;
}
} // namespace hestia
