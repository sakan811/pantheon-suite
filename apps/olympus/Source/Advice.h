// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 The Pantheon authors

#pragma once

#include "Session.h"

#include <pantheon/ui/Note.h>

// What Olympus says about the session in front of the user (the foot of the window). Rules only, no drawing: the kit draws the notes.
namespace olympus
{
inline const char* guideTitle() { return "Olympus"; }
inline const char* guideText()
{
    return "Olympus puts the Pantheon plugins on stems, so each stem can be edited later.\n\n"
           "Add the stems as audio files (ADD TRACK, or drop them on the window), click + on a track to put a plugin on it, click a plugin to open it, "
           "shift-click to bypass it, right-click for more. SAVE keeps the project; RENDER STEMS writes one WAV per stem with its chain, and the mix.\n\n"
           "An agent can do the placing for you through the MCP server (OlympusMcp, see the README), and you open the saved project here.\n\n"
           "It is not a recorder or a sequencer: there is no timeline editing, no MIDI and no instruments. It plays the stems from the start, "
           "hosts the suite's own plugins only, and writes the stems to render them: arranging and mixing stay in your own DAW.";
}

// `deviceRate`: the rate of the audio device, 0 when there is none (tests, snapshots).
inline std::vector<ui::Note> adviceFor (const Session& s, double deviceRate = 0.0)
{
    std::vector<ui::Note> notes;
    const auto add = [&notes] (ui::Severity sev, const juce::String& text) { notes.push_back ({ sev, text.toStdString() }); };

    int missingPlugins = 0, anyAudible = 0;
    for (auto& t : s.tracks)
    {
        if (t->length() == 0)
            add (ui::Severity::warning, "Track '" + t->name.substring (0, 24) + "': its audio file is missing. The track stays and keeps its chain.");
        for (auto& slot : t->chain)
            if (slot->instance == nullptr) ++missingPlugins;
        anyAudible += s.audible (*t) ? 1 : 0;
    }
    if (s.catalog.names().isEmpty())
        add (ui::Severity::warning, "No plugin of the suite found. PLUGINS: choose the folder that holds the built .vst3 files (or plugins/).");
    if (missingPlugins > 0)
        add (ui::Severity::warning, juce::String (missingPlugins) + " plugin(s) in this project are not installed here: they pass the audio through and keep their settings.");
    if (! s.tracks.empty() && anyAudible == 0)
        add (ui::Severity::warning, "Every track is muted, or others are soloed: nothing would be played or rendered.");
    if (deviceRate > 0.0 && s.sampleRate > 0.0 && std::abs (deviceRate - s.sampleRate) > 1.0)
        add (ui::Severity::warning, "The audio device runs at " + juce::String (juce::roundToInt (deviceRate)) + " Hz, the session at "
                                        + juce::String (juce::roundToInt (s.sampleRate)) + " Hz: playback is at the wrong pitch. Stems are not affected.");
    for (auto& t : s.tracks)
        if (t->latency() > 0)
        {
            add (ui::Severity::info, "'" + t->name.substring (0, 24) + "': its plugins delay it by " + juce::String (t->latency()) + " samples ("
                                         + juce::String (1000.0 * t->latency() / (s.sampleRate > 0.0 ? s.sampleRate : 48000.0), 1) + " ms). Playback and stems are aligned for it.");
            break;
        }
    if (s.tracks.empty())
        add (ui::Severity::info, "Drop the stems (audio files) on the window, or press ADD TRACK.");
    return ui::ordered (notes);
}
}
