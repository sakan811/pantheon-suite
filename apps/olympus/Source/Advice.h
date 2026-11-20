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
    return "Olympus is a basic DAW for the Pantheon plugins and for any VST3 (on macOS also Audio Unit) plugin the scan finds.\n\n"
           "It is for arranging audio on a timeline: IMPORT AUDIO, or drop files on the window, puts each file on a track at the playhead; drag clips "
           "to move them, edges to trim them, S splits at the playhead. Set the tempo, the grid, markers and a loop. Each track has a chain of "
           "plugins (FX), a fader and a pan. RENDER STEMS writes one WAV per track and the mix.\n\n"
           "It is not for MIDI or instruments, and it does not time-stretch: audio keeps its own speed, and a tempo change moves the grid, not the audio.\n\n"
           "Keys: Space plays and stops, Return rewinds, L loops, M adds a marker, Delete removes the selected clip, Cmd+D duplicates it, "
           "Cmd+Z and Shift+Cmd+Z undo and redo, Cmd+S saves.\n\n"
           "An agent can edit the project through the MCP server (OlympusMcp, see the README); open the saved project here.";
}

// `deviceRate`: the rate of the audio device, 0 when there is none (tests, snapshots).
inline std::vector<ui::Note> adviceFor (const Session& s, double deviceRate = 0.0)
{
    std::vector<ui::Note> notes;
    const auto add = [&notes] (ui::Severity sev, const juce::String& text) { notes.push_back ({ sev, text.toStdString() }); };

    int missingPlugins = 0, anyAudible = 0, clips = 0;
    for (auto& t : s.tracks)
    {
        bool missingAudio = false;
        clips += (int) t->clips.size();
        for (auto& clip : t->clips)
            missingAudio = missingAudio || clip.audio == nullptr;
        if (missingAudio)
            add (ui::Severity::warning, "Track '" + t->name.substring (0, 24) + "': an audio file is missing. The track stays, and its clips play as silence.");
        for (auto& slot : t->chain)
            if (slot->instance == nullptr) ++missingPlugins;
        anyAudible += s.audible (*t) ? 1 : 0;
    }
    if (s.catalog.names().isEmpty())
        add (ui::Severity::warning, "No plugin found. PLUGINS: look in a folder that holds the built .vst3 files, or rescan.");
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
        add (ui::Severity::info, "No audio yet: IMPORT AUDIO, or drop files on the window, puts them on tracks at the playhead.");
    else if (clips == 0)
        add (ui::Severity::info, "No audio yet: the tracks are empty. IMPORT AUDIO puts a file at the playhead, or drop one on a lane.");
    // the strip shows the kit's own "plugin" reminder when there is nothing else: Olympus always has a line of its own
    if (notes.empty())
        add (ui::Severity::info, "Space plays and stops, S splits, M marks, L loops. GUIDE says what Olympus is for.");
    return ui::ordered (notes);
}
}
