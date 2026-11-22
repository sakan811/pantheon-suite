// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 The Pantheon authors

#pragma once

#include "Session.h"

#include <cmath>
#include <limits>
#include <optional>
#include <utility>
#include <vector>

// The one place where the timeline is edited and undone: clips, track mix, master, tempo, time signature, loop and markers. The window
// and the MCP server call these; none of them touches the session's tracks any other way.
//
// Every editing function takes the session's lock, checks its arguments (a refusal changes nothing and says why in `error`), and when it
// does change something it marks the session dirty and pushes ONE undo step: the state from before the change, the number of tracks and
// a label. A Gesture groups many changes (a fader drag) into one step; a gesture that changes nothing leaves no step. Snap and the grid
// are view settings: they are saved with the project and dirty it, but are not undone.
//
// Tracks are not undone: adding or removing one clears both stacks (clearUndo), and undo/redo refuse a state from another track count.
// Positions are samples at the session's rate; indices are from 0.
namespace olympus
{
struct ClipRef
{
    int track = -1;
    int clip = -1;
    bool valid() const { return track >= 0 && clip >= 0; }
};

struct MidiClipRef
{
    int track = -1;
    int clip = -1;
    bool valid() const { return track >= 0 && clip >= 0; }
};

class Editor
{
public:
    explicit Editor (Session& s) : session (s) {}

    // ---- clips ----------------------------------------------------------------------------------------------------------------
    // Adds a clip of a file to a track, at `start`: from `offset` in the file, for `length` samples (0 = to the end of the file). The file
    // is read into the pool first, so the first file sets the session's rate. A clip that does not fit in the file is refused.
    ClipRef addClip (int track, const juce::File& file, juce::int64 start, juce::int64 offset, juce::int64 length, juce::String& error)
    {
        std::optional<EditState> before;
        {
            const juce::ScopedLock sl (session.lock);
            if (! isTrack (track)) { error = "No track " + juce::String (track); return {}; }
            before = captureIfNeeded();
        }
        // The file is decoded by the session with the lock free (the audio callback would be silent for the whole decode otherwise), and the
        // clip is checked and inserted under it in one step: a refusal leaves the session as it was, and nothing is recorded.
        const int index = session.addClip (track, file, juce::jmax ((juce::int64) 0, start), error, offset, length);
        if (index < 0) return {};
        const juce::ScopedLock sl (session.lock);
        record (before, "Add clip");
        return { track, index };
    }

    // Cuts a clip in two at `at` (strictly inside it). The left piece keeps the index, the right one is inserted after it and returned.
    // The left piece loses its fade out, the right one its fade in.
    ClipRef splitClip (int track, int clip, juce::int64 at, juce::String& error)
    {
        const juce::ScopedLock sl (session.lock);
        Clip* c = clipAt (track, clip, error);
        if (c == nullptr) return {};
        if (at <= c->start || at >= c->end()) { error = "The split point must be inside the clip (after " + juce::String (c->start) + ", before " + juce::String (c->end()) + ")."; return {}; }
        auto before = captureIfNeeded();
        const juce::int64 cut = at - c->start;
        Clip right = *c;
        right.start = at;
        right.offset = c->offset + cut;
        right.length = c->length - cut;
        right.fadeIn = 0;
        right.fadeOut = juce::jmin (c->fadeOut, right.length);
        c->length = cut;
        c->fadeIn = juce::jmin (c->fadeIn, cut);
        c->fadeOut = 0;
        auto& clips = session.tracks[(size_t) track]->clips;
        clips.insert (clips.begin() + clip + 1, std::move (right));
        record (before, "Split clip");
        return { track, clip + 1 };
    }

    // Moves a clip along its track. The start is not before 0.
    bool moveClip (int track, int clip, juce::int64 newStart, juce::String& error)
    {
        const juce::ScopedLock sl (session.lock);
        Clip* c = clipAt (track, clip, error);
        if (c == nullptr) return false;
        const juce::int64 start = juce::jmax ((juce::int64) 0, newStart);
        if (start == c->start) return true;
        auto before = captureIfNeeded();
        c->start = start;
        record (before, "Move clip");
        return true;
    }

    // Moves a clip to another track; it is appended at the end of that track. On the same track it only moves (and keeps its index).
    ClipRef moveClipToTrack (int track, int clip, int newTrack, juce::int64 newStart, juce::String& error)
    {
        const juce::ScopedLock sl (session.lock);
        if (clipAt (track, clip, error) == nullptr) return {};
        if (! isTrack (newTrack)) { error = "No track " + juce::String (newTrack); return {}; }
        if (newTrack == track)
            return moveClip (track, clip, newStart, error) ? ClipRef { track, clip } : ClipRef {};
        auto before = captureIfNeeded();
        auto& from = session.tracks[(size_t) track]->clips;
        Clip moved = from[(size_t) clip];
        from.erase (from.begin() + clip);
        moved.start = juce::jmax ((juce::int64) 0, newStart);
        auto& to = session.tracks[(size_t) newTrack]->clips;
        to.push_back (std::move (moved));
        record (before, "Move clip");
        return { newTrack, (int) to.size() - 1 };
    }

    // Moves the left edge of a clip. The audio stays where it is on the timeline: the start, the offset and the length change together.
    // Clamped so the offset stays >= 0, the length >= 1 and the start >= 0; a fade in longer than the new length is shortened.
    bool trimStart (int track, int clip, juce::int64 newStart, juce::String& error)
    {
        const juce::ScopedLock sl (session.lock);
        Clip* c = clipAt (track, clip, error);
        if (c == nullptr) return false;
        const juce::int64 low = juce::jmax ((juce::int64) 0, c->start - c->offset);
        const juce::int64 high = juce::jmax (low, c->end() - 1);
        const juce::int64 start = juce::jmax (low, juce::jmin (high, newStart));
        const juce::int64 shift = start - c->start;
        if (shift == 0) return true;
        auto before = captureIfNeeded();
        const juce::int64 length = c->length - shift;
        c->start = start;
        c->offset += shift;
        c->length = length;
        c->fadeIn = juce::jmin (c->fadeIn, length);
        c->fadeOut = juce::jmin (c->fadeOut, length - c->fadeIn);
        record (before, "Trim start");
        return true;
    }

    // Moves the right edge of a clip. Clamped so the length is >= 1 and the clip does not play past the end of its source (when the audio
    // is there); a fade longer than the new length is shortened.
    bool trimEnd (int track, int clip, juce::int64 newEnd, juce::String& error)
    {
        const juce::ScopedLock sl (session.lock);
        Clip* c = clipAt (track, clip, error);
        if (c == nullptr) return false;
        const juce::int64 low = c->start + 1;
        juce::int64 high = std::numeric_limits<juce::int64>::max();
        if (c->audio != nullptr)
            high = c->start + ((juce::int64) c->audio->getNumSamples() - c->offset);
        const juce::int64 end = juce::jmax (low, juce::jmin (juce::jmax (low, high), newEnd));
        const juce::int64 length = end - c->start;
        if (length == c->length) return true;
        auto before = captureIfNeeded();
        c->length = length;
        c->fadeIn = juce::jmin (c->fadeIn, length);
        c->fadeOut = juce::jmin (c->fadeOut, length - c->fadeIn);
        record (before, "Trim end");
        return true;
    }

    // A copy of the clip placed right after the original ends, on the same track (appended to its clips). Returns the copy.
    ClipRef duplicateClip (int track, int clip, juce::String& error)
    {
        const juce::ScopedLock sl (session.lock);
        Clip* c = clipAt (track, clip, error);
        if (c == nullptr) return {};
        auto before = captureIfNeeded();
        Clip copy = *c;
        copy.start = c->end();
        auto& clips = session.tracks[(size_t) track]->clips;
        clips.push_back (std::move (copy));
        record (before, "Duplicate clip");
        return { track, (int) clips.size() - 1 };
    }

    bool deleteClip (int track, int clip, juce::String& error)
    {
        const juce::ScopedLock sl (session.lock);
        if (clipAt (track, clip, error) == nullptr) return false;
        auto before = captureIfNeeded();
        auto& clips = session.tracks[(size_t) track]->clips;
        clips.erase (clips.begin() + clip);
        record (before, "Delete clip");
        return true;
    }

    // The clip's gain, clamped to -100..+24 dB.
    bool setClipGain (int track, int clip, float db, juce::String& error)
    {
        if (! std::isfinite (db)) { error = "Clip gain must be a finite number."; return false; }
        const juce::ScopedLock sl (session.lock);
        Clip* c = clipAt (track, clip, error);
        if (c == nullptr) return false;
        const float gain = juce::jlimit (-100.0f, 24.0f, db);
        if (same (gain, c->gainDb)) return true;
        auto before = captureIfNeeded();
        c->gainDb = gain;
        record (before, "Clip gain");
        return true;
    }

    // Both fades, in samples. Each is clamped to 0..length, and the fade out to what the fade in leaves (fadeIn + fadeOut <= length).
    bool setClipFades (int track, int clip, juce::int64 fadeIn, juce::int64 fadeOut, juce::String& error)
    {
        const juce::ScopedLock sl (session.lock);
        Clip* c = clipAt (track, clip, error);
        if (c == nullptr) return false;
        const juce::int64 in = juce::jlimit ((juce::int64) 0, c->length, fadeIn);
        const juce::int64 out = juce::jmin (juce::jlimit ((juce::int64) 0, c->length, fadeOut), c->length - in);
        if (in == c->fadeIn && out == c->fadeOut) return true;
        auto before = captureIfNeeded();
        c->fadeIn = in;
        c->fadeOut = out;
        record (before, "Clip fades");
        return true;
    }

    bool setClipMute (int track, int clip, bool mute, juce::String& error)
    {
        const juce::ScopedLock sl (session.lock);
        Clip* c = clipAt (track, clip, error);
        if (c == nullptr) return false;
        if (mute == c->mute) return true;
        auto before = captureIfNeeded();
        c->mute = mute;
        record (before, mute ? "Mute clip" : "Unmute clip");
        return true;
    }

    bool renameClip (int track, int clip, const juce::String& name, juce::String& error)
    {
        const juce::ScopedLock sl (session.lock);
        Clip* c = clipAt (track, clip, error);
        if (c == nullptr) return false;
        const auto text = name.trim();
        if (text.isEmpty()) { error = "The name is empty."; return false; }
        if (text == c->name) return true;
        auto before = captureIfNeeded();
        c->name = text;
        record (before, "Rename clip");
        return true;
    }

    // ---- MIDI clips and notes -------------------------------------------------------------------------------------------------
    MidiClipRef addMidiClip (int track, juce::int64 start, juce::int64 length, const juce::String& name, juce::String& error)
    {
        if (length < 1) { error = "MIDI clip length must be at least 1 sample."; return {}; }
        const juce::ScopedLock sl (session.lock);
        if (! isTrack (track)) { error = "No track " + juce::String (track); return {}; }
        auto before = captureIfNeeded();
        auto& clips = session.tracks[(size_t) track]->midiClips;
        MidiClip mc;
        const auto trimmed = name.trim();
        mc.name = trimmed.isNotEmpty() ? trimmed : "MIDI " + juce::String ((int) clips.size() + 1);
        mc.start = juce::jlimit ((juce::int64) 0, kMaxSafeSample, start);
        mc.length = juce::jlimit ((juce::int64) 1, kMaxSafeSample, length);
        clips.push_back (std::move (mc));
        record (before, "Add MIDI clip");
        return { track, (int) clips.size() - 1 };
    }

    bool deleteMidiClip (int track, int clip, juce::String& error)
    {
        const juce::ScopedLock sl (session.lock);
        if (midiClipAt (track, clip, error) == nullptr) return false;
        auto before = captureIfNeeded();
        auto& clips = session.tracks[(size_t) track]->midiClips;
        clips.erase (clips.begin() + clip);
        record (before, "Delete MIDI clip");
        return true;
    }

    bool moveMidiClip (int track, int clip, juce::int64 newStart, juce::String& error)
    {
        const juce::ScopedLock sl (session.lock);
        MidiClip* mc = midiClipAt (track, clip, error);
        if (mc == nullptr) return false;
        const juce::int64 start = juce::jlimit ((juce::int64) 0, kMaxSafeSample, newStart);
        if (start == mc->start) return true;
        auto before = captureIfNeeded();
        mc->start = start;
        record (before, "Move MIDI clip");
        return true;
    }

    bool resizeMidiClip (int track, int clip, juce::int64 newLength, juce::String& error)
    {
        if (newLength < 1) { error = "MIDI clip length must be at least 1 sample."; return false; }
        const juce::ScopedLock sl (session.lock);
        MidiClip* mc = midiClipAt (track, clip, error);
        if (mc == nullptr) return false;
        const juce::int64 len = juce::jlimit ((juce::int64) 1, kMaxSafeSample, newLength);
        if (len == mc->length) return true;
        auto before = captureIfNeeded();
        mc->length = len;
        record (before, "Resize MIDI clip");
        return true;
    }

    int addMidiNote (int track, int clip, double startBeat, double durationBeats, int pitch, int velocity, int channel, juce::String& error)
    {
        if (! std::isfinite (startBeat) || ! std::isfinite (durationBeats))
        {
            error = "Note start and duration must be finite numbers.";
            return -1;
        }
        if (startBeat < 0.0 || startBeat > 1e7) { error = "Note start beat must be between 0 and 1e7."; return -1; }
        if (durationBeats <= 0.0 || durationBeats > 1e6) { error = "Note duration must be between 0 and 1e6 beats."; return -1; }
        if (pitch < 0 || pitch > 127) { error = "Note pitch must be between 0 and 127."; return -1; }
        if (velocity < 1 || velocity > 127) { error = "Note velocity must be between 1 and 127."; return -1; }
        if (channel < 1 || channel > 16) { error = "MIDI channel must be between 1 and 16."; return -1; }

        const juce::ScopedLock sl (session.lock);
        MidiClip* mc = midiClipAt (track, clip, error);
        if (mc == nullptr) return -1;
        auto before = captureIfNeeded();
        MidiNote note;
        note.startBeat = startBeat;
        note.durationBeats = durationBeats;
        note.pitch = pitch;
        note.velocity = velocity;
        note.channel = channel;
        mc->notes.push_back (note);
        record (before, "Add MIDI note");
        return (int) mc->notes.size() - 1;
    }

    int addMidiNote (int track, int clip, double startBeat, double durationBeats, int pitch, int velocity, juce::String& error)
    {
        return addMidiNote (track, clip, startBeat, durationBeats, pitch, velocity, 1, error);
    }

    bool deleteMidiNote (int track, int clip, int note, juce::String& error)
    {
        const juce::ScopedLock sl (session.lock);
        MidiClip* mc = midiClipAt (track, clip, error);
        if (mc == nullptr) return false;
        if (! juce::isPositiveAndBelow (note, (int) mc->notes.size()))
        {
            error = "No note " + juce::String (note) + " in MIDI clip " + juce::String (clip);
            return false;
        }
        auto before = captureIfNeeded();
        mc->notes.erase (mc->notes.begin() + note);
        record (before, "Delete MIDI note");
        return true;
    }

    bool moveMidiNote (int track, int clip, int note, double newStartBeat, int newPitch, juce::String& error)
    {
        if (! std::isfinite (newStartBeat)) { error = "Note start beat must be a finite number."; return false; }
        const juce::ScopedLock sl (session.lock);
        MidiNote* n = midiNoteAt (track, clip, note, error);
        if (n == nullptr) return false;
        const double startBeat = juce::jlimit (0.0, 1e7, newStartBeat);
        const int pitch = juce::jlimit (0, 127, newPitch);
        if (std::abs (startBeat - n->startBeat) < 1e-9 && pitch == n->pitch) return true;
        auto before = captureIfNeeded();
        n->startBeat = startBeat;
        n->pitch = pitch;
        record (before, "Move MIDI note");
        return true;
    }

    bool resizeMidiNote (int track, int clip, int note, double newDurationBeats, juce::String& error)
    {
        if (! std::isfinite (newDurationBeats) || newDurationBeats <= 0.0 || newDurationBeats > 1e6)
        {
            error = "Note duration must be a positive finite number up to 1e6.";
            return false;
        }
        const juce::ScopedLock sl (session.lock);
        MidiNote* n = midiNoteAt (track, clip, note, error);
        if (n == nullptr) return false;
        if (std::abs (newDurationBeats - n->durationBeats) < 1e-9) return true;
        auto before = captureIfNeeded();
        n->durationBeats = newDurationBeats;
        record (before, "Resize MIDI note");
        return true;
    }

    bool setMidiNoteVelocity (int track, int clip, int note, int velocity, juce::String& error)
    {
        const juce::ScopedLock sl (session.lock);
        MidiNote* n = midiNoteAt (track, clip, note, error);
        if (n == nullptr) return false;
        const int vel = juce::jlimit (1, 127, velocity);
        if (vel == n->velocity) return true;
        auto before = captureIfNeeded();
        n->velocity = vel;
        record (before, "MIDI note velocity");
        return true;
    }

    bool quantizeMidiClip (int track, int clip, Grid grid, juce::String& error)
    {
        const juce::ScopedLock sl (session.lock);
        MidiClip* mc = midiClipAt (track, clip, error);
        if (mc == nullptr) return false;
        const double step = session.tempo.gridBeats (grid);
        if (! (step > 0.0)) return true;
        const double clipStartBeats = session.tempo.sampleRate > 0.0
                                        ? session.tempo.samplesToBeats ((double) mc->start)
                                        : 0.0;
        auto snapBeat = [step, clipStartBeats] (double localBeat)
        {
            const double timelineBeat = clipStartBeats + localBeat;
            const double snapped = std::floor (timelineBeat / step + 0.5 + TempoMap::kGridEps) * step;
            return juce::jlimit (0.0, 1e7, snapped - clipStartBeats);
        };
        bool changed = false;
        for (const auto& note : mc->notes)
        {
            const double q = snapBeat (note.startBeat);
            if (std::abs (q - note.startBeat) > 1e-9) { changed = true; break; }
        }
        if (! changed) return true;
        auto before = captureIfNeeded();
        for (auto& note : mc->notes)
            note.startBeat = snapBeat (note.startBeat);
        record (before, "Quantize MIDI clip");
        return true;
    }

    // ---- clipboard ------------------------------------------------------------------------------------------------------------
    // The clipboard holds one clip: its audio (shared, not copied), its place in the file, gain, fades, mute and name. It lives in the
    // Editor only: it is not the system clipboard, it is not saved in the project and it is not an undo step, so it survives undo and redo.
    // It is cleared by NEW and OPEN (clearClipboard), and a paste is refused when the session's rate has changed since the copy.
    bool hasClipboard() const { return clipboard.has_value(); }
    int clipboardTrack() const { return clipboardFrom; }   // the track the copied clip came from; -1 when the clipboard is empty

    // Copies the clip, replacing what the clipboard held. Changes nothing in the session.
    bool copyClip (ClipRef ref, juce::String& error)
    {
        const juce::ScopedLock sl (session.lock);
        Clip* c = clipAt (ref.track, ref.clip, error);
        if (c == nullptr) return false;
        storeClipboard (*c, ref.track);
        return true;
    }

    // Copies the clip and deletes it: ONE undo step ("Cut clip").
    bool cutClip (ClipRef ref, juce::String& error)
    {
        const juce::ScopedLock sl (session.lock);
        Clip* c = clipAt (ref.track, ref.clip, error);
        if (c == nullptr) return false;
        storeClipboard (*c, ref.track);
        auto before = captureIfNeeded();
        auto& clips = session.tracks[(size_t) ref.track]->clips;
        clips.erase (clips.begin() + ref.clip);
        record (before, "Cut clip");
        return true;
    }

    // Adds a copy of the clipboard's clip to `track`, starting at sample `at` (appended to the track's clips): ONE undo step ("Paste
    // clip"). Returns the new clip. Refused, with the reason, when the clipboard is empty, the track is not there, `at` is before 0, or
    // the session's rate is not the one the clip was copied at.
    ClipRef pasteClip (int track, juce::int64 at, juce::String& error)
    {
        const juce::ScopedLock sl (session.lock);
        if (! clipboard) { error = "The clipboard is empty: copy or cut a clip first."; return {}; }
        if (! isTrack (track)) { error = "No track " + juce::String (track); return {}; }
        if (at < 0) { error = "The paste position must be 0 or later."; return {}; }
        if (std::abs (clipboardRate - session.sampleRate) > 0.5) { error = "The clipboard holds audio at another sample rate: copy the clip again."; return {}; }
        auto before = captureIfNeeded();
        Clip copy = *clipboard;
        copy.start = at;
        auto& clips = session.tracks[(size_t) track]->clips;
        clips.push_back (std::move (copy));
        record (before, "Paste clip");
        return { track, (int) clips.size() - 1 };
    }

    void clearClipboard()
    {
        clipboard.reset();
        clipboardFrom = -1;
        clipboardRate = 0.0;
    }

    // ---- track mix and master -------------------------------------------------------------------------------------------------
    // The fader, clamped to -100..+12 dB.
    bool setTrackGain (int track, float db, juce::String& error)
    {
        if (! std::isfinite (db)) { error = "Track gain must be a finite number."; return false; }
        const juce::ScopedLock sl (session.lock);
        if (! isTrack (track)) { error = "No track " + juce::String (track); return false; }
        auto& t = *session.tracks[(size_t) track];
        const float gain = juce::jlimit (-100.0f, 12.0f, db);
        if (same (gain, t.gainDb)) return true;
        auto before = captureIfNeeded();
        t.gainDb = gain;
        record (before, "Track gain");
        return true;
    }

    // The balance, clamped to -1..1.
    bool setTrackPan (int track, float pan, juce::String& error)
    {
        if (! std::isfinite (pan)) { error = "Track pan must be a finite number."; return false; }
        const juce::ScopedLock sl (session.lock);
        if (! isTrack (track)) { error = "No track " + juce::String (track); return false; }
        auto& t = *session.tracks[(size_t) track];
        const float value = juce::jlimit (-1.0f, 1.0f, pan);
        if (same (value, t.pan)) return true;
        auto before = captureIfNeeded();
        t.pan = value;
        record (before, "Track pan");
        return true;
    }

    bool setTrackMute (int track, bool on, juce::String& error)
    {
        const juce::ScopedLock sl (session.lock);
        if (! isTrack (track)) { error = "No track " + juce::String (track); return false; }
        auto& t = *session.tracks[(size_t) track];
        if (on == t.mute) return true;
        auto before = captureIfNeeded();
        t.mute = on;
        record (before, on ? "Mute track" : "Unmute track");
        return true;
    }

    bool setTrackSolo (int track, bool on, juce::String& error)
    {
        const juce::ScopedLock sl (session.lock);
        if (! isTrack (track)) { error = "No track " + juce::String (track); return false; }
        auto& t = *session.tracks[(size_t) track];
        if (on == t.solo) return true;
        auto before = captureIfNeeded();
        t.solo = on;
        record (before, on ? "Solo track" : "Unsolo track");
        return true;
    }

    bool renameTrack (int track, const juce::String& name, juce::String& error)
    {
        const juce::ScopedLock sl (session.lock);
        if (! isTrack (track)) { error = "No track " + juce::String (track); return false; }
        auto& t = *session.tracks[(size_t) track];
        const auto text = name.trim();
        if (text.isEmpty()) { error = "The name is empty."; return false; }
        if (text == t.name) return true;
        auto before = captureIfNeeded();
        t.name = text;
        record (before, "Rename track");
        return true;
    }

    // The master gain, clamped to -100..+12 dB.
    void setMasterDb (float db)
    {
        if (! std::isfinite (db)) return;
        const juce::ScopedLock sl (session.lock);
        const float value = juce::jlimit (-100.0f, 12.0f, db);
        if (same (value, session.masterDb)) return;
        auto before = captureIfNeeded();
        session.masterDb = value;
        record (before, "Master gain");
    }

    // ---- tempo, signature, loop -----------------------------------------------------------------------------------------------
    // Adds or changes the tempo at a beat (TempoMap::setTempo: the bpm is clamped to 20..300, a beat before 0 is 0).
    bool setTempo (double beat, double bpm, juce::String& error)
    {
        const juce::ScopedLock sl (session.lock);
        if (! std::isfinite (beat) || ! std::isfinite (bpm)) { error = "The beat and the tempo must be numbers."; return false; }
        TempoMap probe = session.tempo;
        probe.setTempo (beat, bpm);
        if (sameTempo (probe, session.tempo)) return true;
        auto before = captureIfNeeded();
        session.tempo.setTempo (beat, bpm);
        record (before, "Tempo");
        return true;
    }

    // Removes a tempo point. The first one is the project's start and stays.
    bool removeTempo (int index, juce::String& error)
    {
        const juce::ScopedLock sl (session.lock);
        TempoMap probe = session.tempo;
        if (! probe.removeTempo (index))
        {
            error = index == 0 ? "The first tempo is the start of the project and cannot be removed." : "No tempo " + juce::String (index);
            return false;
        }
        auto before = captureIfNeeded();
        session.tempo.removeTempo (index);
        record (before, "Remove tempo");
        return true;
    }

    // The project's time signature. An invalid denominator is ignored (see TempoMap::setSignature).
    void setSignature (int num, int den)
    {
        const juce::ScopedLock sl (session.lock);
        TempoMap probe = session.tempo;
        probe.setSignature (num, den);
        if (probe.numerator == session.tempo.numerator && probe.denominator == session.tempo.denominator) return;
        auto before = captureIfNeeded();
        session.tempo.setSignature (num, den);
        record (before, "Time signature");
    }

    // The loop, from `start` to `end` (both >= 0). An end that is not after the start leaves the loop off.
    void setLoop (juce::int64 start, juce::int64 end, bool on)
    {
        const juce::ScopedLock sl (session.lock);
        const juce::int64 s = juce::jmax ((juce::int64) 0, start), e = juce::jmax ((juce::int64) 0, end);
        const bool isOn = on && e > s;
        if (s == session.loopStart && e == session.loopEnd && isOn == session.loopOn) return;
        auto before = captureIfNeeded();
        session.loopStart = s;
        session.loopEnd = e;
        session.loopOn = isOn;
        record (before, "Loop");
    }

    // The metronome (a click, not in the stems): an undo step like the loop.
    void setMetronome (bool on)
    {
        const juce::ScopedLock sl (session.lock);
        if (on == session.metronomeOn) return;
        auto before = captureIfNeeded();
        session.metronomeOn = on;
        record (before, on ? "Metronome on" : "Metronome off");
    }

    // ---- markers --------------------------------------------------------------------------------------------------------------
    // Adds a marker. Returns its index in the sorted list.
    int addMarker (const juce::String& name, juce::int64 at)
    {
        const juce::ScopedLock sl (session.lock);
        auto before = captureIfNeeded();
        const int index = session.addMarker (name, at);
        record (before, "Add marker");
        return index;
    }

    // Moves a marker; the list stays sorted and the marker keeps its name.
    bool moveMarker (int index, juce::int64 at, juce::String& error)
    {
        const juce::ScopedLock sl (session.lock);
        if (! isMarker (index)) { error = "No marker " + juce::String (index); return false; }
        const juce::int64 start = juce::jmax ((juce::int64) 0, at);
        if (start == session.markers[(size_t) index].start) return true;
        auto before = captureIfNeeded();
        session.moveMarker (index, start, error);
        record (before, "Move marker");
        return true;
    }

    bool renameMarker (int index, const juce::String& name, juce::String& error)
    {
        const juce::ScopedLock sl (session.lock);
        if (! isMarker (index)) { error = "No marker " + juce::String (index); return false; }
        const auto text = name.trim();
        if (text.isEmpty()) { error = "The name is empty."; return false; }
        if (text == session.markers[(size_t) index].name) return true;
        auto before = captureIfNeeded();
        session.markers[(size_t) index].name = text;
        record (before, "Rename marker");
        return true;
    }

    bool removeMarker (int index, juce::String& error)
    {
        const juce::ScopedLock sl (session.lock);
        if (! isMarker (index)) { error = "No marker " + juce::String (index); return false; }
        auto before = captureIfNeeded();
        session.removeMarker (index, error);
        record (before, "Remove marker");
        return true;
    }

    // ---- automation -----------------------------------------------------------------------------------------------------------
    int addAutomationPoint (int track, const juce::String& targetId, juce::int64 sample, float value, AutoCurveType curve, juce::String& error)
    {
        if (! std::isfinite (value)) { error = "Automation value must be a finite number."; return -1; }
        int slotIdx = -1, paramIdx = -1;
        if (! parseAutoTargetId (targetId, slotIdx, paramIdx, error)) return -1;
        const juce::ScopedLock sl (session.lock);
        if (! isTrack (track)) { error = "No track " + juce::String (track); return -1; }
        auto before = captureIfNeeded();
        auto& t = *session.tracks[(size_t) track];
        auto& lane = findOrCreateLane (t, targetId, slotIdx, paramIdx);
        const juce::int64 clampedSample = juce::jlimit ((juce::int64) 0, kMaxSafeSample, sample);
        const float clampedValue = juce::jlimit (0.0f, 1.0f, value);
        AutoPoint pt { clampedSample, clampedValue, curve };
        auto it = std::upper_bound (lane.points.begin(), lane.points.end(), pt.sample,
                                    [] (juce::int64 s, const AutoPoint& p) { return s < p.sample; });
        const int insertedIndex = (int) (it - lane.points.begin());
        lane.points.insert (it, pt);
        record (before, "Add automation point");
        return insertedIndex;
    }

    bool moveAutomationPoint (int track, const juce::String& targetId, int pointIndex, juce::int64 newSample, float newValue, juce::String& error, int* outNewIndex = nullptr)
    {
        if (! std::isfinite (newValue)) { error = "Automation value must be a finite number."; return false; }
        const juce::ScopedLock sl (session.lock);
        auto* lane = findAutomationLane (track, targetId, error);
        if (lane == nullptr) return false;
        if (! juce::isPositiveAndBelow (pointIndex, (int) lane->points.size()))
        {
            error = "No automation point " + juce::String (pointIndex) + " on lane '" + targetId + "'";
            return false;
        }
        const juce::int64 s = juce::jlimit ((juce::int64) 0, kMaxSafeSample, newSample);
        const float v = juce::jlimit (0.0f, 1.0f, newValue);
        auto& pt = lane->points[(size_t) pointIndex];
        if (pt.sample == s && same (pt.value, v))
        {
            if (outNewIndex != nullptr)
                *outNewIndex = pointIndex;
            return true;
        }
        auto before = captureIfNeeded();
        pt.sample = s;
        pt.value = v;
        struct TaggedPoint { AutoPoint point; bool isMoved; };
        std::vector<TaggedPoint> tagged;
        tagged.reserve (lane->points.size());
        for (int i = 0; i < (int) lane->points.size(); ++i)
            tagged.push_back ({ lane->points[(size_t) i], i == pointIndex });
        std::stable_sort (tagged.begin(), tagged.end(),
                          [] (const TaggedPoint& a, const TaggedPoint& b) { return a.point.sample < b.point.sample; });
        int newIdx = pointIndex;
        for (int i = 0; i < (int) tagged.size(); ++i)
        {
            lane->points[(size_t) i] = tagged[(size_t) i].point;
            if (tagged[(size_t) i].isMoved)
                newIdx = i;
        }
        if (outNewIndex != nullptr)
            *outNewIndex = newIdx;
        record (before, "Move automation point");
        return true;
    }

    bool setAutomationPointCurve (int track, const juce::String& targetId, int pointIndex, AutoCurveType curve, juce::String& error)
    {
        const juce::ScopedLock sl (session.lock);
        auto* lane = findAutomationLane (track, targetId, error);
        if (lane == nullptr) return false;
        if (! juce::isPositiveAndBelow (pointIndex, (int) lane->points.size()))
        {
            error = "No automation point " + juce::String (pointIndex) + " on lane '" + targetId + "'";
            return false;
        }
        auto& pt = lane->points[(size_t) pointIndex];
        if (pt.curve == curve) return true;
        auto before = captureIfNeeded();
        pt.curve = curve;
        record (before, "Automation curve");
        return true;
    }

    bool deleteAutomationPoint (int track, const juce::String& targetId, int pointIndex, juce::String& error)
    {
        const juce::ScopedLock sl (session.lock);
        auto* lane = findAutomationLane (track, targetId, error);
        if (lane == nullptr) return false;
        if (! juce::isPositiveAndBelow (pointIndex, (int) lane->points.size()))
        {
            error = "No automation point " + juce::String (pointIndex) + " on lane '" + targetId + "'";
            return false;
        }
        auto before = captureIfNeeded();
        lane->points.erase (lane->points.begin() + pointIndex);
        record (before, "Delete automation point");
        return true;
    }

    bool setAutomationLaneEnabled (int track, const juce::String& targetId, bool enabled, juce::String& error)
    {
        int slotIdx = -1, paramIdx = -1;
        if (! parseAutoTargetId (targetId, slotIdx, paramIdx, error)) return false;
        const juce::ScopedLock sl (session.lock);
        if (! isTrack (track)) { error = "No track " + juce::String (track); return false; }
        auto& t = *session.tracks[(size_t) track];
        AutomationLane* existing = nullptr;
        for (auto& l : t.automation)
            if (l.targetId == targetId) { existing = &l; break; }
        if (existing != nullptr && existing->enabled == enabled) return true;
        auto before = captureIfNeeded();
        auto& lane = existing != nullptr ? *existing : findOrCreateLane (t, targetId, slotIdx, paramIdx);
        lane.enabled = enabled;
        record (before, enabled ? "Enable automation" : "Disable automation");
        return true;
    }

    bool setTrackShowAutomation (int track, bool show, juce::String& error)
    {
        const juce::ScopedLock sl (session.lock);
        if (! isTrack (track)) { error = "No track " + juce::String (track); return false; }
        auto& t = *session.tracks[(size_t) track];
        if (t.showAutomation == show) return true;
        t.showAutomation = show;
        session.dirty = true;
        session.bumpRevision();
        return true;
    }

    bool setTrackActiveAutoTarget (int track, const juce::String& targetId, juce::String& error)
    {
        int slotIdx = -1, paramIdx = -1;
        if (! parseAutoTargetId (targetId, slotIdx, paramIdx, error)) return false;
        const juce::ScopedLock sl (session.lock);
        if (! isTrack (track)) { error = "No track " + juce::String (track); return false; }
        auto& t = *session.tracks[(size_t) track];
        if (t.activeAutoTarget == targetId) return true;
        t.activeAutoTarget = targetId;
        session.dirty = true;
        session.bumpRevision();
        return true;
    }

    // ---- view settings (not undone; saved with the project) -------------------------------------------------------------------
    void setSnap (bool on)
    {
        const juce::ScopedLock sl (session.lock);
        if (on == session.snapOn) return;
        session.snapOn = on;
        session.dirty = true;
        session.bumpRevision();
    }

    void setGrid (Grid g)
    {
        const juce::ScopedLock sl (session.lock);
        if (g == session.grid) return;
        session.grid = g;
        session.dirty = true;
        session.bumpRevision();
    }

    // The sample a position snaps to: the nearest grid line of the session's grid when snap is on (halfway goes up), else the sample
    // itself. Never before 0.
    juce::int64 snap (juce::int64 sample) const
    {
        const juce::ScopedLock sl (session.lock);
        const juce::int64 at = juce::jmax ((juce::int64) 0, sample);
        if (! session.snapOn)
            return at;
        return juce::jmax ((juce::int64) 0, (juce::int64) std::llround (session.tempo.snapSample ((double) at, session.grid)));
    }

    // ---- undo -----------------------------------------------------------------------------------------------------------------
    bool canUndo() const { return gestureDepth == 0 && ! undoSteps.empty(); }
    bool canRedo() const { return gestureDepth == 0 && ! redoSteps.empty(); }

    // Puts back the state before the last edit (the current one goes to the redo stack). False when there is nothing to undo, or when
    // the track count changed since: then both stacks are cleared.
    bool undo()
    {
        const juce::ScopedLock sl (session.lock);
        if (gestureDepth > 0 || undoSteps.empty()) return false;
        if (undoSteps.back().tracks != (int) session.tracks.size()) { clearUndo(); return false; }
        Step step = std::move (undoSteps.back());
        undoSteps.pop_back();
        redoSteps.push_back ({ session.captureEdit(), step.tracks, step.label });
        trimSteps (redoSteps);
        session.restoreEdit (step.state);
        return true;
    }

    // The mirror of undo: puts back the edit that was undone.
    bool redo()
    {
        const juce::ScopedLock sl (session.lock);
        if (gestureDepth > 0 || redoSteps.empty()) return false;
        if (redoSteps.back().tracks != (int) session.tracks.size()) { clearUndo(); return false; }
        Step step = std::move (redoSteps.back());
        redoSteps.pop_back();
        undoSteps.push_back ({ session.captureEdit(), step.tracks, step.label });
        trimSteps (undoSteps);
        session.restoreEdit (step.state);
        return true;
    }

    void clearUndo()
    {
        const juce::ScopedLock sl (session.lock);
        undoSteps.clear();
        redoSteps.clear();
        if (gestureDepth > 0)
        {
            gestureInvalid = true;
            gestureChanged = false;
        }
    }

    // The label of the edit that undo would revert ("Split clip"), or "" when there is none.
    juce::String undoLabel() const { return undoSteps.empty() ? juce::String() : undoSteps.back().label; }
    // The label of the edit that redo would make again, or "" when there is none.
    juce::String redoLabel() const { return redoSteps.empty() ? juce::String() : redoSteps.back().label; }

    // Groups every edit made while it lives into ONE undo step, labelled `label`. Gestures nest: only the outermost one counts. The step
    // is pushed when the outermost one ends, and only if something in it changed.
    struct Gesture
    {
        Gesture (Editor& e, const juce::String& label) : editor (e)
        {
            const juce::ScopedLock sl (editor.session.lock);
            if (editor.gestureDepth++ == 0)
            {
                editor.gestureBefore = editor.session.captureEdit();
                editor.gestureTracks = (int) editor.session.tracks.size();
                editor.gestureLabel = label;
                editor.gestureChanged = false;
                editor.gestureInvalid = false;
            }
        }

        ~Gesture()
        {
            const juce::ScopedLock sl (editor.session.lock);
            if (--editor.gestureDepth == 0)
            {
                if (editor.gestureChanged && ! editor.gestureInvalid)
                    editor.push ({ std::move (editor.gestureBefore), editor.gestureTracks, editor.gestureLabel });
                editor.gestureInvalid = false;
                editor.gestureChanged = false;
                editor.gestureBefore = {};
            }
        }

        Gesture (const Gesture&) = delete;
        Gesture& operator= (const Gesture&) = delete;

    private:
        Editor& editor;
    };

private:
    static constexpr size_t kMaxSteps = 100;

    struct Step
    {
        EditState state;
        int tracks = 0;
        juce::String label;
    };

    Session& session;
    std::vector<Step> undoSteps, redoSteps;
    std::optional<Clip> clipboard;        // what copyClip and cutClip put there (see the clipboard section)
    int clipboardFrom = -1;
    double clipboardRate = 0.0;           // the session's rate when it was copied: a paste at another rate is refused
    int gestureDepth = 0;
    bool gestureChanged = false;
    bool gestureInvalid = false;
    EditState gestureBefore;
    int gestureTracks = 0;
    juce::String gestureLabel;

    std::optional<EditState> captureIfNeeded() const
    {
        if (gestureDepth > 0)
            return std::nullopt;
        return session.captureEdit();
    }

    // Called after a change, with the state from before it. Inside a gesture the step waits for the gesture's end.
    void record (std::optional<EditState>& before, const juce::String& label)
    {
        session.dirty = true;
        session.publishPlaybackSnapshotLocked();
        session.bumpRevision();
        if (gestureDepth > 0)
        {
            if (! gestureInvalid)
                gestureChanged = true;
            return;
        }
        if (before)
            push ({ std::move (*before), (int) session.tracks.size(), label });
    }

    void push (Step step)
    {
        undoSteps.push_back (std::move (step));
        trimSteps (undoSteps);
        redoSteps.clear();
    }

    static void trimSteps (std::vector<Step>& steps)
    {
        if (steps.size() > kMaxSteps)
            steps.erase (steps.begin(), steps.begin() + (std::ptrdiff_t) (steps.size() - kMaxSteps));
    }

    void storeClipboard (const Clip& clip, int track)
    {
        clipboard = clip;
        clipboardFrom = track;
        clipboardRate = session.sampleRate;
    }

    bool isTrack (int track) const { return juce::isPositiveAndBelow (track, (int) session.tracks.size()); }
    bool isMarker (int index) const { return juce::isPositiveAndBelow (index, (int) session.markers.size()); }

    // The clip, or null with the reason. Valid until the clips of its track change.
    Clip* clipAt (int track, int clip, juce::String& error)
    {
        if (! isTrack (track)) { error = "No track " + juce::String (track); return nullptr; }
        auto& clips = session.tracks[(size_t) track]->clips;
        if (! juce::isPositiveAndBelow (clip, (int) clips.size()))
        {
            error = "No clip " + juce::String (clip) + " on track " + juce::String (track) + " (it has " + juce::String ((int) clips.size()) + ")";
            return nullptr;
        }
        return &clips[(size_t) clip];
    }

    MidiClip* midiClipAt (int track, int clip, juce::String& error)
    {
        if (! isTrack (track)) { error = "No track " + juce::String (track); return nullptr; }
        auto& clips = session.tracks[(size_t) track]->midiClips;
        if (! juce::isPositiveAndBelow (clip, (int) clips.size()))
        {
            error = "No MIDI clip " + juce::String (clip) + " on track " + juce::String (track) + " (it has " + juce::String ((int) clips.size()) + ")";
            return nullptr;
        }
        return &clips[(size_t) clip];
    }

    MidiNote* midiNoteAt (int track, int clip, int note, juce::String& error)
    {
        MidiClip* mc = midiClipAt (track, clip, error);
        if (mc == nullptr) return nullptr;
        if (! juce::isPositiveAndBelow (note, (int) mc->notes.size()))
        {
            error = "No note " + juce::String (note) + " in MIDI clip " + juce::String (clip);
            return nullptr;
        }
        return &mc->notes[(size_t) note];
    }

    static bool parseAutoTargetId (const juce::String& targetId, int& slotIndex, int& paramIndex, juce::String& error)
    {
        if (targetId == "volume" || targetId == "pan")
        {
            slotIndex = -1;
            paramIndex = -1;
            return true;
        }
        if (targetId.startsWith ("plugin:"))
        {
            const auto rest = targetId.substring (7);
            const int colon = rest.indexOfChar (':');
            if (colon > 0 && colon + 1 < rest.length())
            {
                const auto sStr = rest.substring (0, colon);
                const auto pStr = rest.substring (colon + 1);
                if (sStr.containsOnly ("0123456789") && pStr.containsOnly ("0123456789"))
                {
                    slotIndex = sStr.getIntValue();
                    paramIndex = pStr.getIntValue();
                    if (slotIndex >= 0 && paramIndex >= 0)
                        return true;
                }
            }
        }
        error = "Invalid automation target '" + targetId + "'.";
        return false;
    }

    AutomationLane* findAutomationLane (int track, const juce::String& targetId, juce::String& error)
    {
        if (! isTrack (track)) { error = "No track " + juce::String (track); return nullptr; }
        for (auto& lane : session.tracks[(size_t) track]->automation)
            if (lane.targetId == targetId)
                return &lane;
        error = "No automation lane '" + targetId + "' on track " + juce::String (track);
        return nullptr;
    }

    static AutomationLane& findOrCreateLane (Track& t, const juce::String& targetId, int slotIdx, int paramIdx)
    {
        for (auto& lane : t.automation)
            if (lane.targetId == targetId)
                return lane;
        AutomationLane lane;
        lane.targetId = targetId;
        lane.slotIndex = slotIdx;
        lane.paramIndex = paramIdx;
        lane.enabled = true;
        t.automation.push_back (std::move (lane));
        return t.automation.back();
    }

    // Two dB, pan or master values that are the same to a millionth (a value set by the editor is exact anyway).
    static bool same (float a, float b) { return std::abs (a - b) < 1e-6f; }

    static bool sameTempo (const TempoMap& a, const TempoMap& b)
    {
        if (a.points.size() != b.points.size()) return false;
        for (size_t i = 0; i < a.points.size(); ++i)
            if (std::abs (a.points[i].beat - b.points[i].beat) > 1e-9 || std::abs (a.points[i].bpm - b.points[i].bpm) > 1e-9)
                return false;
        return true;
    }
};

using Gesture = Editor::Gesture;
}
