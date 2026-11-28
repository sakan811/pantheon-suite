// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 The Pantheon authors

#pragma once

#include "Session.h"
#include "Tempo.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

// Standard MIDI Files, read and written with JUCE's juce::MidiFile. Notes are kept in quarter-note beats (MidiNote), the unit the
// session's clips use, so a file's ticks are converted here and nowhere else. The file is written as type 1 at 960 ticks per quarter
// note: the tempo track (track 0) holds every tempo point and the time signature, and each exported track follows it.
namespace olympus
{
// One track of a file that has notes: its beats are from the file's start.
struct ImportedMidiTrack
{
    juce::String name;
    std::vector<MidiNote> notes;
};

// One track to write: the notes are at absolute beats (a clip's start beat is already added).
struct ExportMidiTrack
{
    juce::String name;
    std::vector<MidiNote> notes;
};

inline constexpr int kMidiTicksPerQuarter = 960;
inline constexpr double kMidiMinNoteBeats = 1.0 / 16.0; // a note with no note-off is at least this long

inline const char* kSmpteRefusal = "This MIDI file counts time in SMPTE frames; only files in beats (PPQ) can be read.";

// Reads the file. A track with no notes is skipped. `firstBpm`, `num` and `den` come from the first tempo and time-signature
// events (0 when the file has none). Returns false, with the reason in `error`, for a missing file, a file that is not MIDI, and
// an SMPTE file.
inline bool readMidiFile (const juce::File& file, std::vector<ImportedMidiTrack>& out, double& firstBpm, int& num, int& den, juce::String& error)
{
    out.clear();
    firstBpm = 0.0;
    num = 0;
    den = 0;

    if (! file.existsAsFile())
    {
        error = "There is no MIDI file at " + file.getFullPathName() + ".";
        return false;
    }
    juce::FileInputStream in (file);
    if (! in.openedOk())
    {
        error = "The MIDI file " + file.getFullPathName() + " cannot be opened.";
        return false;
    }

    // createMatchingNoteOffs = false: a note with no note-off is ours to end (see below), not JUCE's
    juce::MidiFile midi;
    if (! midi.readFrom (in, false))
    {
        error = file.getFileName() + " is not a Standard MIDI File.";
        return false;
    }

    const short timeFormat = midi.getTimeFormat();
    if (timeFormat < 0)
    {
        error = kSmpteRefusal;
        return false;
    }
    if (timeFormat == 0)
    {
        error = file.getFileName() + " has no time division.";
        return false;
    }
    const double ticksPerQuarter = (double) timeFormat;

    // the first tempo and time signature, by tick (ties go to the earlier track)
    double tempoTick = std::numeric_limits<double>::infinity();
    double signatureTick = std::numeric_limits<double>::infinity();

    for (int t = 0; t < midi.getNumTracks(); ++t)
    {
        juce::MidiMessageSequence seq (*midi.getTrack (t));
        seq.updateMatchedPairs();

        for (int i = 0; i < seq.getNumEvents(); ++i)
        {
            const auto* holder = seq.getEventPointer (i);
            const auto& msg = holder->message;
            if (msg.isTempoMetaEvent() && msg.getTimeStamp() < tempoTick && msg.getTempoSecondsPerQuarterNote() > 0.0)
            {
                tempoTick = msg.getTimeStamp();
                // rounded to 1/1000 bpm: a tempo of 90 is stored as microseconds, and that does not come back exactly
                firstBpm = std::round (60.0 / msg.getTempoSecondsPerQuarterNote() * 1000.0) / 1000.0;
            }
            if (msg.isTimeSignatureMetaEvent() && msg.getTimeStamp() < signatureTick)
            {
                int n = 0, d = 0;
                msg.getTimeSignatureInfo (n, d);
                if (n > 0 && d > 0)
                {
                    signatureTick = msg.getTimeStamp();
                    num = n;
                    den = d;
                }
            }
        }

        std::vector<MidiNote> notes;
        juce::String trackName;
        double lastTick = 0.0;

        for (int i = 0; i < seq.getNumEvents(); ++i)
        {
            const auto* holder = seq.getEventPointer (i);
            const auto& msg = holder->message;
            lastTick = std::max (lastTick, msg.getTimeStamp());
            if (msg.isTrackNameEvent() && trackName.isEmpty())
                trackName = msg.getTextFromTextMetaEvent().trim();
        }

        for (int i = 0; i < seq.getNumEvents(); ++i)
        {
            const auto* holder = seq.getEventPointer (i);
            const auto& msg = holder->message;
            if (! msg.isNoteOn())
                continue;

            MidiNote note;
            note.startBeat = msg.getTimeStamp() / ticksPerQuarter;
            note.pitch = msg.getNoteNumber();
            note.velocity = msg.getVelocity();
            note.channel = msg.getChannel();

            if (holder->noteOffObject != nullptr)
            {
                // a zero-length pair still gets one tick, so the note is not silently dropped on playback
                const double offTick = holder->noteOffObject->message.getTimeStamp();
                note.durationBeats = std::max (offTick - msg.getTimeStamp(), 1.0) / ticksPerQuarter;
            }
            else
            {
                // no note-off: the note runs to the last event of its track
                const double endBeat = std::max (lastTick / ticksPerQuarter, note.startBeat);
                note.durationBeats = std::max (endBeat - note.startBeat, kMidiMinNoteBeats);
            }
            notes.push_back (note);
        }

        if (notes.empty())
            continue;

        std::stable_sort (notes.begin(), notes.end(), [] (const MidiNote& a, const MidiNote& b)
        {
            return a.startBeat < b.startBeat || (a.startBeat == b.startBeat && a.pitch < b.pitch);
        });

        ImportedMidiTrack imported;
        imported.name = trackName.isNotEmpty() ? trackName : "MIDI " + juce::String ((int) out.size() + 1);
        imported.notes = std::move (notes);
        out.push_back (std::move (imported));
    }

    if (firstBpm < 0.0 || ! std::isfinite (firstBpm))
        firstBpm = 0.0;
    return true;
}

// Writes a type 1 file: track 0 holds the tempo map and the time signature, then one track per entry of `tracks`. Note-offs go
// before note-ons at the same tick. The file is written beside the target and then moved over it, so a failed write leaves
// the old file as it was.
inline bool writeMidiFile (const juce::File& target, const std::vector<ExportMidiTrack>& tracks, const TempoMap& map, juce::String& error)
{
    juce::MidiFile midi;
    midi.setTicksPerQuarterNote (kMidiTicksPerQuarter);
    const auto tick = [] (double beats) { return (juce::int64) std::llround (std::max (0.0, beats) * kMidiTicksPerQuarter); };

    juce::MidiMessageSequence tempoTrack;
    for (const auto& p : map.points)
    {
        const double bpm = std::max (1.0, p.bpm);
        tempoTrack.addEvent (juce::MidiMessage::tempoMetaEvent ((int) std::llround (60000000.0 / bpm)), (double) tick (p.beat));
    }
    tempoTrack.addEvent (juce::MidiMessage::timeSignatureMetaEvent (map.numerator, map.denominator), 0.0);
    tempoTrack.sort();
    midi.addTrack (tempoTrack);

    for (const auto& t : tracks)
    {
        struct Event { juce::int64 tick; bool on; juce::MidiMessage msg; };
        std::vector<Event> events;
        events.push_back ({ 0, true, juce::MidiMessage::textMetaEvent (0x03, t.name.isNotEmpty() ? t.name : juce::String ("MIDI")) });
        for (const auto& n : t.notes)
        {
            if (! std::isfinite (n.startBeat) || ! std::isfinite (n.durationBeats) || n.durationBeats <= 0.0)
                continue;
            const int channel = juce::jlimit (1, 16, n.channel);
            const int pitch = juce::jlimit (0, 127, n.pitch);
            const auto on = tick (n.startBeat);
            const auto off = std::max (on + 1, tick (n.startBeat + n.durationBeats));
            const auto velocity = (juce::uint8) juce::jlimit (1, 127, n.velocity);
            events.push_back ({ on, true, juce::MidiMessage::noteOn (channel, pitch, velocity) });
            events.push_back ({ off, false, juce::MidiMessage::noteOff (channel, pitch) });
        }
        // at one tick: note-offs first, so a repeated pitch ends before it starts again
        std::stable_sort (events.begin(), events.end(), [] (const Event& a, const Event& b)
        {
            if (a.tick != b.tick) return a.tick < b.tick;
            return ! a.on && b.on;
        });

        juce::MidiMessageSequence seq;
        for (const auto& e : events)
            seq.addEvent (e.msg, (double) e.tick);
        midi.addTrack (seq);
    }

    const auto folder = target.getParentDirectory();
    if (! folder.isDirectory() && ! folder.createDirectory())
    {
        error = "The folder " + folder.getFullPathName() + " cannot be made.";
        return false;
    }

    juce::TemporaryFile temp (target);
    {
        juce::FileOutputStream out (temp.getFile());
        if (! out.openedOk())
        {
            error = "Cannot write " + target.getFullPathName() + ".";
            return false;
        }
        const bool written = midi.writeTo (out);
        out.flush();
        if (! written || out.getStatus().failed())
        {
            error = "Writing " + target.getFullPathName() + " failed.";
            return false;
        }
    }
    if (! temp.overwriteTargetFileWithTemporary())
    {
        error = "Cannot replace " + target.getFullPathName() + ".";
        return false;
    }
    return true;
}
} // namespace olympus
