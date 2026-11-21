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
           "plugins (FX), a fader and a pan. RENDER STEMS writes one WAV per track and the mix; EXPORT writes the final mix (mix.wav) alone, without stems or the monitor chain.\n\n"
           "It records from your audio interface: AUDIO chooses the input and the output device. Arm a track (R) and choose its input (IN); REC "
           "(or the R key) records every armed track into a take, one file per track, kept on disk. When you stop, each take is placed on its track, "
           "earlier by the device's round trip, so it lines up with what you heard.\n\n"
           "It is not for software input monitoring: hear yourself through your interface's direct monitoring or a zero-latency mix. It does not "
           "punch in, loop-record or comp takes, and it is not for MIDI or instruments. It does not time-stretch: audio keeps its own speed, and a "
           "tempo change moves the grid, not the audio.\n\n"
           "Keys: Space plays and stops, Return rewinds, Home and End go to the start and the end, comma and full stop go to the marker before and "
           "after the playhead, L loops, M adds a marker, R records, Delete removes the selected clip, Cmd+D duplicates it, Cmd+C, Cmd+X and Cmd+V "
           "copy, cut and paste it (the paste lands at the playhead), Cmd+Z and Shift+Cmd+Z undo and redo, Cmd+S saves.\n\n"
           "MONITORING: the MONITOR strip holds the plugins the headphones hear, such as a headphone correction plugin (a VST3, or an Audio Unit on "
           "macOS). They run on playback only: they are never in a render, the stems or the mix, and they are saved on this computer, not in the "
           "project. They do not process what you record or the files you write. Use them last, and bypass them (CORRECTION) to check the mix by "
           "ear without correction.\n\n"
           "An agent can edit the project through the MCP server (OlympusMcp, see the README); open the saved project here.";
}

// What the audio device is, for the notes: whether it runs, its rate, its input channels and the round trip of its latency in ms.
struct AdviceDevice
{
    bool running = false;
    double rate = 0.0;
    int inputs = 0;
    double roundTripMs = 0.0;
    juce::String monitorProblem;   // the monitor chain could not be read (its file is not one, or is newer): kept as it is
    bool monitorSkipped = false;   // the last start died loading the monitor chain: it is skipped until PLUGINS loads it
    bool projectSkipped = false;   // the last start died opening the last project: it was not opened again
    int monitorLatencySamples = -1; // the monitor chain's latency as the window last read it (never waits for the lock); -1: ask the session
    bool scanning = false;          // a plugin scan is queued or running: the catalog is not complete yet, so no note says none was found
};

inline juce::String kiloHertz (double rate) { return juce::String (rate / 1000.0, 1) + " kHz"; }

inline std::vector<ui::Note> adviceFor (const Session& s, const AdviceDevice& device = {})
{
    std::vector<ui::Note> notes;
    const auto add = [&notes] (ui::Severity sev, const juce::String& text) { notes.push_back ({ sev, text.toStdString() }); };

    const juce::ScopedTryLock sl (s.lock);
    const bool locked = sl.isLocked() && ! s.renderRunning();

    int missingPlugins = 0, anyAudible = 0, clips = 0, armed = 0;
    if (locked)
    {
        for (auto& t : s.tracks)
        {
            bool missingAudio = false;
            clips += (int) t->clips.size();
            armed += t->armed ? 1 : 0;
            for (auto& clip : t->clips)
                missingAudio = missingAudio || clip.audio == nullptr;
            if (missingAudio)
                add (ui::Severity::warning, "Track '" + t->name.substring (0, 24) + "': an audio file is missing. The track stays, and its clips play as silence.");
            for (auto& slot : t->chain)
                if (slot->instance == nullptr)
                {
                    // installed but refused its saved layout is said as that, not as a plugin missing from this machine
                    if (s.catalog.has (slot->plugin))
                        add (ui::Severity::warning, ("'" + slot->plugin + "': installed, but it cannot run in stereo with the settings this project saved, so it passes the audio through and keeps them.").substring (0, ui::kMaxNoteChars));
                    else
                        ++missingPlugins;
                }
            anyAudible += s.audible (*t) ? 1 : 0;
        }
    }
    // the scan found nothing at all (names() holds the scan's plugins, and the suite-only setting narrows them: then this note is not about the scan)
    if (s.catalog.names().isEmpty() && ! s.catalog.suiteOnly() && ! device.scanning)
        add (ui::Severity::warning, "No plugin found. PLUGINS > Look in a folder... adds a folder with VST3 plugins.");
    if (missingPlugins > 0)
        add (ui::Severity::warning, juce::String (missingPlugins) + " plugin(s) in this project are not installed here: they pass the audio through and keep their settings.");
    if (locked && ! s.tracks.empty() && anyAudible == 0)
        add (ui::Severity::warning, "Every track is muted, or others are soloed: nothing would be played or rendered.");
    if (device.rate > 0.0 && s.sampleRate > 0.0 && std::abs (device.rate - s.sampleRate) > 1.0)
        add (ui::Severity::warning, "The audio device runs at " + juce::String (juce::roundToInt (device.rate)) + " Hz, the session at "
                                        + juce::String (juce::roundToInt (s.sampleRate)) + " Hz: playback is at the wrong pitch. Stems are not affected.");
    // recording: a take is kept at the device's rate, and resampled to the session's when it is placed (Session::loadPooled)
    if (armed > 0 && (! device.running || device.inputs == 0))
        add (ui::Severity::warning, "A track is armed, but no audio input is running: choose the input device in AUDIO.");
    if (armed > 0 && device.rate > 0.0 && s.sampleRate > 0.0 && std::abs (device.rate - s.sampleRate) > 1.0)
        add (ui::Severity::info, "Takes are recorded at " + kiloHertz (device.rate) + " and resampled to the session's " + kiloHertz (s.sampleRate) + " when placed.");
    if (device.running && device.roundTripMs > 30.0)
        add (ui::Severity::warning, "The audio round trip is " + juce::String (juce::roundToInt (device.roundTripMs)) + " ms: lower the buffer size in AUDIO.");
    if (locked)
    {
        for (auto& t : s.tracks)
            if (t->latency() > 0)
            {
                add (ui::Severity::info, "'" + t->name.substring (0, 24) + "': its plugins delay it by " + juce::String (t->latency()) + " samples ("
                                             + juce::String (1000.0 * t->latency() / (s.sampleRate > 0.0 ? s.sampleRate : 48000.0), 1) + " ms). Playback and stems are aligned for it.");
                break;
            }
        // the monitor chain: what the headphones hear, never a render (Session::processMonitor)
        if (! s.monitor.empty())
        {
            int missingMonitor = 0;
            for (auto& slot : s.monitor)
                if (slot->instance == nullptr)
                {
                    if (s.catalog.has (slot->plugin))
                        add (ui::Severity::warning, ("Monitor '" + slot->plugin + "': installed, but it cannot run in stereo with its saved settings; the headphones hear the mix without it.").substring (0, ui::kMaxNoteChars));
                    else
                        ++missingMonitor;
                }
            if (s.monitorBypass)
                add (ui::Severity::warning, "Monitor: headphone correction is bypassed: you are hearing the uncorrected mix.");
            const double rate = device.rate > 0.0 ? device.rate : (s.sampleRate > 0.0 ? s.sampleRate : 48000.0);
            const double monitorMs = 1000.0 * (device.monitorLatencySamples >= 0 ? device.monitorLatencySamples : s.monitorLatency()) / rate;
            if (monitorMs > 30.0)
                add (ui::Severity::warning, "Monitor " + juce::String (juce::roundToInt (monitorMs)) + " ms: a linear-phase correction adds delay; recording is compensated, playback alignment is not.");
            if (missingMonitor > 0)
                add (ui::Severity::warning, juce::String (missingMonitor) + " monitor plugin(s) not installed here: the headphones hear the mix without them.");
        }
    }
    if (device.monitorProblem.isNotEmpty())
        add (ui::Severity::warning, ("Monitor chain not read: " + device.monitorProblem + " Its file is kept.").substring (0, ui::kMaxNoteChars));
    if (device.monitorSkipped)
        add (ui::Severity::warning, "Last start stopped loading the monitor chain: skipped. PLUGINS > Load saved monitor chain tries again.");
    if (device.projectSkipped)
        add (ui::Severity::warning, "Last start stopped opening the last project: not opened again. OPEN opens it.");
    if (locked)
    {
        if (s.tracks.empty())
            add (ui::Severity::info, "No audio yet: IMPORT AUDIO, or drop files on the window, puts them on tracks at the playhead.");
        else if (clips == 0)
            add (ui::Severity::info, "No audio yet: the tracks are empty. IMPORT AUDIO puts a file at the playhead, or drop one on a lane.");
    }
    // the strip shows the kit's own "plugin" reminder when there is nothing else: Olympus always has a line of its own
    if (notes.empty())
        add (ui::Severity::info, "Space plays and stops, S splits, M marks, L loops. GUIDE says what Olympus is for.");
    return ui::ordered (notes);
}
}
