// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 The Pantheon authors

#pragma once

#include <string>
#include <vector>

namespace ui
{
// One line of advice a plugin gives about the settings in front of the user (a limit it measured, a switch that makes what they
// hear something other than the result). Plain C++, no JUCE: a plugin's rules (its Advice.h) produce these and its tests read
// them; Advice.h in this folder draws them.
enum class Severity { warning, info };

struct Note
{
    Severity severity;
    std::string text; // one line of a panel: at most kMaxNoteChars
};

// What one line of a panel holds at the size the kit draws notes (every plugin's tests hold its notes to it).
constexpr int kMaxNoteChars = 108;

// Warnings first, then information, each group in the order they were added.
inline std::vector<Note> ordered (std::vector<Note> notes)
{
    std::vector<Note> out;
    for (auto severity : { Severity::warning, Severity::info })
        for (auto& n : notes)
            if (n.severity == severity)
                out.push_back (n);
    return out;
}

// True when any note of `severity` contains `word` (for tests).
inline bool hasNote (const std::vector<Note>& notes, const char* word, Severity severity)
{
    for (auto& n : notes)
        if (n.severity == severity && n.text.find (word) != std::string::npos)
            return true;
    return false;
}
}
