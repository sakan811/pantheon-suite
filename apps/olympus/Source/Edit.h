// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 The Pantheon authors

#pragma once

#include "Session.h"

#include <cmath>
#include <limits>
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

class Editor
{
public:
    explicit Editor (Session& s) : session (s) {}

    // ---- clips ----------------------------------------------------------------------------------------------------------------
    // Adds a clip of a file to a track, at `start`: from `offset` in the file, for `length` samples (0 = to the end of the file). The file
    // is read into the pool first, so the first file sets the session's rate. A clip that does not fit in the file is refused.
    ClipRef addClip (int track, const juce::File& file, juce::int64 start, juce::int64 offset, juce::int64 length, juce::String& error)
    {
        EditState before;
        {
            const juce::ScopedLock sl (session.lock);
            if (! isTrack (track)) { error = "No track " + juce::String (track); return {}; }
            before = session.captureEdit();
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
        const auto before = session.captureEdit();
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
        const auto before = session.captureEdit();
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
        const auto before = session.captureEdit();
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
        const auto before = session.captureEdit();
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
        const auto before = session.captureEdit();
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
        const auto before = session.captureEdit();
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
        const auto before = session.captureEdit();
        auto& clips = session.tracks[(size_t) track]->clips;
        clips.erase (clips.begin() + clip);
        record (before, "Delete clip");
        return true;
    }

    // The clip's gain, clamped to -100..+24 dB.
    bool setClipGain (int track, int clip, float db, juce::String& error)
    {
        const juce::ScopedLock sl (session.lock);
        Clip* c = clipAt (track, clip, error);
        if (c == nullptr) return false;
        const float gain = juce::jlimit (-100.0f, 24.0f, db);
        if (same (gain, c->gainDb)) return true;
        const auto before = session.captureEdit();
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
        const auto before = session.captureEdit();
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
        const auto before = session.captureEdit();
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
        const auto before = session.captureEdit();
        c->name = text;
        record (before, "Rename clip");
        return true;
    }

    // ---- track mix and master -------------------------------------------------------------------------------------------------
    // The fader, clamped to -100..+12 dB.
    bool setTrackGain (int track, float db, juce::String& error)
    {
        const juce::ScopedLock sl (session.lock);
        if (! isTrack (track)) { error = "No track " + juce::String (track); return false; }
        auto& t = *session.tracks[(size_t) track];
        const float gain = juce::jlimit (-100.0f, 12.0f, db);
        if (same (gain, t.gainDb)) return true;
        const auto before = session.captureEdit();
        t.gainDb = gain;
        record (before, "Track gain");
        return true;
    }

    // The balance, clamped to -1..1.
    bool setTrackPan (int track, float pan, juce::String& error)
    {
        const juce::ScopedLock sl (session.lock);
        if (! isTrack (track)) { error = "No track " + juce::String (track); return false; }
        auto& t = *session.tracks[(size_t) track];
        const float value = juce::jlimit (-1.0f, 1.0f, pan);
        if (same (value, t.pan)) return true;
        const auto before = session.captureEdit();
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
        const auto before = session.captureEdit();
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
        const auto before = session.captureEdit();
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
        const auto before = session.captureEdit();
        t.name = text;
        record (before, "Rename track");
        return true;
    }

    // The master gain, clamped to -100..+12 dB.
    void setMasterDb (float db)
    {
        const juce::ScopedLock sl (session.lock);
        const float value = juce::jlimit (-100.0f, 12.0f, db);
        if (same (value, session.masterDb)) return;
        const auto before = session.captureEdit();
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
        const auto before = session.captureEdit();
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
        const auto before = session.captureEdit();
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
        const auto before = session.captureEdit();
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
        const auto before = session.captureEdit();
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
        const auto before = session.captureEdit();
        session.metronomeOn = on;
        record (before, on ? "Metronome on" : "Metronome off");
    }

    // ---- markers --------------------------------------------------------------------------------------------------------------
    // Adds a marker. Returns its index in the sorted list.
    int addMarker (const juce::String& name, juce::int64 at)
    {
        const juce::ScopedLock sl (session.lock);
        const auto before = session.captureEdit();
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
        const auto before = session.captureEdit();
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
        const auto before = session.captureEdit();
        session.markers[(size_t) index].name = text;
        record (before, "Rename marker");
        return true;
    }

    bool removeMarker (int index, juce::String& error)
    {
        const juce::ScopedLock sl (session.lock);
        if (! isMarker (index)) { error = "No marker " + juce::String (index); return false; }
        const auto before = session.captureEdit();
        session.removeMarker (index, error);
        record (before, "Remove marker");
        return true;
    }

    // ---- view settings (not undone; saved with the project) -------------------------------------------------------------------
    void setSnap (bool on)
    {
        const juce::ScopedLock sl (session.lock);
        if (on == session.snapOn) return;
        session.snapOn = on;
        session.dirty = true;
    }

    void setGrid (Grid g)
    {
        const juce::ScopedLock sl (session.lock);
        if (g == session.grid) return;
        session.grid = g;
        session.dirty = true;
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
    bool canUndo() const { return ! undoSteps.empty(); }
    bool canRedo() const { return ! redoSteps.empty(); }

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
        undoSteps.clear();
        redoSteps.clear();
    }

    // The label of the edit that undo would revert ("Split clip"), or "" when there is none.
    juce::String undoLabel() const { return undoSteps.empty() ? juce::String() : undoSteps.back().label; }

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
            }
        }

        ~Gesture()
        {
            const juce::ScopedLock sl (editor.session.lock);
            if (--editor.gestureDepth == 0 && editor.gestureChanged)
                editor.push ({ std::move (editor.gestureBefore), editor.gestureTracks, editor.gestureLabel });
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
    int gestureDepth = 0;
    bool gestureChanged = false;
    EditState gestureBefore;
    int gestureTracks = 0;
    juce::String gestureLabel;

    // Called after a change, with the state from before it. Inside a gesture the step waits for the gesture's end.
    void record (const EditState& before, const juce::String& label)
    {
        session.dirty = true;
        if (gestureDepth > 0)
        {
            gestureChanged = true;
            return;
        }
        push ({ before, (int) session.tracks.size(), label });
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
