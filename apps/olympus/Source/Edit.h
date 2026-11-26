// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 The Pantheon authors

#pragma once

#include "Session.h"

#include <cmath>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

// The one place where the timeline is edited and undone: tracks, plugins, clips, track mix, master, tempo, time signature, loop and markers.
// The window and the MCP server call these; none of them touches the session's tracks any other way.
//
// Every editing function takes the session's lock, checks its arguments (a refusal changes nothing and says why in `error`), and when it
// does change something it marks the session dirty and pushes ONE undo step: the state from before the change (or an XML snapshot for
// structural track/plugin changes) and a label. A Gesture groups many changes (a fader drag) into one step; a gesture that changes nothing
// leaves no step. Snap and the grid are view settings: they are saved with the project and dirty it, but are not undone on their own.
// Positions are samples at the session's rate; indices are from 0.
namespace olympus
{
struct ClipRef
{
    int track = -1;
    int clip = -1;
    bool isMidi = false;
    bool valid() const { return track >= 0 && clip >= 0; }
    bool operator== (const ClipRef& o) const { return track == o.track && clip == o.clip && isMidi == o.isMidi; }
    bool operator!= (const ClipRef& o) const { return ! (*this == o); }
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
    std::function<void()> onBeforeStructureChange;

    // ---- tracks and chains (structural undo) ---------------------------------------------------------------------------------
    int addEmptyTrack (juce::String trackName)
    {
        auto before = captureStructuralXml();
        const int idx = session.addEmptyTrack (std::move (trackName));
        if (idx >= 0)
            recordStructural (std::move (before), "Add track");
        return idx;
    }

    int addMidiTrack (juce::String trackName)
    {
        auto before = captureStructuralXml();
        const int idx = session.addMidiTrack (std::move (trackName));
        if (idx >= 0)
            recordStructural (std::move (before), "Add MIDI track");
        return idx;
    }

    int addBusTrack (const juce::String& busName, juce::String& error)
    {
        auto before = captureStructuralXml();
        const int idx = session.addBusTrack (busName, error);
        if (idx >= 0)
            recordStructural (std::move (before), "Add bus track");
        return idx;
    }

    bool setTrackOutput (int track, int targetBus, juce::String& error)
    {
        {
            const juce::ScopedLock sl (session.lock);
            if (isTrack (track) && session.tracks[(size_t) track]->outputTrack == targetBus)
                return true;
        }
        auto before = captureStructuralXml();
        if (! session.setTrackOutput (track, targetBus, error))
            return false;
        recordStructural (std::move (before), "Set track output");
        return true;
    }

    bool setTrackSend (int track, int targetBus, float levelDb, bool preFader, bool mute, juce::String& error)
    {
        auto before = captureStructuralXml();
        if (! session.setTrackSend (track, targetBus, levelDb, preFader, mute, error))
            return false;
        recordStructural (std::move (before), "Set track send");
        return true;
    }

    bool removeTrackSend (int track, int sendIndex, juce::String& error)
    {
        auto before = captureStructuralXml();
        if (! session.removeTrackSend (track, sendIndex, error))
            return false;
        recordStructural (std::move (before), "Remove track send");
        return true;
    }

    bool setTrackSends (int track, const std::vector<AuxSend>& newSends, juce::String& error)
    {
        auto before = captureStructuralXml();
        if (! session.setTrackSends (track, newSends, error))
            return false;
        recordStructural (std::move (before), "Set track sends");
        return true;
    }

    bool setSlotSidechain (int track, int slot, int sourceTrack, juce::String& error)
    {
        {
            const juce::ScopedLock sl (session.lock);
            if (auto* s = session.slotAt (track, slot, error))
            {
                if (s->sidechainTrack == sourceTrack && ! session.tracks[(size_t) track]->frozen)
                    return true;
            }
        }
        auto before = captureStructuralXml();
        if (! session.setSlotSidechain (track, slot, sourceTrack, error))
            return false;
        recordStructural (std::move (before), "Set slot sidechain");
        return true;
    }

    bool addTrack (const juce::File& file, juce::String trackName, juce::String& error)
    {
        auto before = captureStructuralXml();
        if (! session.addTrack (file, std::move (trackName), error))
            return false;
        recordStructural (std::move (before), "Add track");
        return true;
    }

    bool removeTrack (int index, juce::String& error)
    {
        auto before = captureStructuralXml();
        if (! session.removeTrack (index, error))
            return false;
        recordStructural (std::move (before), "Remove track");
        return true;
    }

    bool freezeTrack (int track, const juce::File& targetWav, juce::String& error)
    {
        auto before = captureStructuralXml();
        if (! session.freezeTrack (track, targetWav, error))
            return false;
        recordStructural (std::move (before), "Freeze track");
        return true;
    }

    bool unfreezeTrack (int track, juce::String& error)
    {
        {
            const juce::ScopedLock sl (session.lock);
            if (! isTrack (track)) { error = "No track " + juce::String (track); return false; }
            if (! session.tracks[(size_t) track]->frozen)
                return true;
        }
        auto before = captureStructuralXml();
        if (! session.unfreezeTrack (track, error))
            return false;
        recordStructural (std::move (before), "Unfreeze track");
        return true;
    }

    bool addPlugin (int track, const juce::String& query, int position, juce::String& error)
    {
        auto before = captureStructuralXml();
        if (! session.addPlugin (track, query, position, error))
            return false;
        recordStructural (std::move (before), "Add plugin");
        return true;
    }

    bool removePlugin (int track, int slot, juce::String& error)
    {
        auto before = captureStructuralXml();
        if (! session.removePlugin (track, slot, error))
            return false;
        recordStructural (std::move (before), "Remove plugin");
        return true;
    }

    bool movePlugin (int track, int from, int to, juce::String& error)
    {
        {
            const juce::ScopedLock sl (session.lock);
            if (! isTrack (track)) { error = "No track " + juce::String (track); return false; }
            const int n = (int) session.tracks[(size_t) track]->chain.size();
            if (! juce::isPositiveAndBelow (from, n) || ! juce::isPositiveAndBelow (to, n))
            {
                error = "No such place in the chain";
                return false;
            }
            if (from == to)
                return true;
        }
        auto before = captureStructuralXml();
        if (! session.movePlugin (track, from, to, error))
            return false;
        recordStructural (std::move (before), "Move plugin");
        return true;
    }

    bool bypassPlugin (int track, int slot, bool bypass, juce::String& error)
    {
        {
            const juce::ScopedLock sl (session.lock);
            auto* s = session.slotAt (track, slot, error);
            if (s == nullptr) return false;
            if (s->bypass == bypass) return true;
        }
        auto before = captureStructuralXml();
        if (! session.setBypass (track, slot, bypass, error))
            return false;
        recordStructural (std::move (before), bypass ? "Bypass plugin" : "Enable plugin");
        return true;
    }

    bool setTrackInstrument (int track, const juce::String& pluginName, juce::String& error)
    {
        {
            const juce::ScopedLock sl (session.lock);
            if (! isTrack (track)) { error = "No track " + juce::String (track); return false; }
            const auto& trk = *session.tracks[(size_t) track];
            if (trk.isBus()) { error = "Cannot set an instrument on a bus track."; return false; }
            if (trk.frozen) { error = "Track '" + trk.name + "' is frozen: unfreeze track to edit plugins."; return false; }
        }
        auto before = captureStructuralXml();
        if (! session.setTrackInstrument (track, pluginName, error, [this] { if (onBeforeStructureChange) onBeforeStructureChange(); }))
            return false;
        recordStructural (std::move (before), "Set instrument");
        return true;
    }

    bool removeTrackInstrument (int track, juce::String& error)
    {
        {
            const juce::ScopedLock sl (session.lock);
            if (! isTrack (track)) { error = "No track " + juce::String (track); return false; }
            const auto& trk = *session.tracks[(size_t) track];
            if (trk.frozen) { error = "Track '" + trk.name + "' is frozen: unfreeze track to edit plugins."; return false; }
            if (trk.instrument == nullptr) { error = "No instrument on track " + juce::String (track); return false; }
        }
        auto before = captureStructuralXml();
        if (onBeforeStructureChange) onBeforeStructureChange();
        if (! session.removeTrackInstrument (track, error))
            return false;
        recordStructural (std::move (before), "Remove instrument");
        return true;
    }

    bool bypassTrackInstrument (int track, bool bypass, juce::String& error)
    {
        {
            const juce::ScopedLock sl (session.lock);
            if (! isTrack (track)) { error = "No track " + juce::String (track); return false; }
            const auto& trk = *session.tracks[(size_t) track];
            if (trk.frozen) { error = "Track '" + trk.name + "' is frozen: unfreeze track to edit plugins."; return false; }
            if (trk.instrument == nullptr) { error = "No instrument on track " + juce::String (track); return false; }
            if (trk.instrument->bypass == bypass) return true;
        }
        auto before = captureStructuralXml();
        if (! session.bypassTrackInstrument (track, bypass, error))
            return false;
        recordStructural (std::move (before), "Bypass instrument");
        return true;
    }

    bool setChain (int track, const juce::StringArray& plugins, juce::String& error)
    {
        auto before = captureStructuralXml();
        if (! session.setChain (track, plugins, error))
            return false;
        recordStructural (std::move (before), "Set chain");
        return true;
    }

    bool copyChain (int from, const std::vector<int>& to, juce::String& error)
    {
        auto before = captureStructuralXml();
        if (! session.copyChain (from, to, error))
            return false;
        recordStructural (std::move (before), "Copy chain");
        return true;
    }

    bool addMasterPlugin (const juce::String& query, int position, juce::String& error)
    {
        auto before = captureStructuralXml();
        if (! session.addMasterPlugin (query, position, error))
            return false;
        recordStructural (std::move (before), "Add master plugin");
        return true;
    }

    bool removeMasterPlugin (int index, juce::String& error)
    {
        auto before = captureStructuralXml();
        if (! session.removeMasterPlugin (index, error))
            return false;
        recordStructural (std::move (before), "Remove master plugin");
        return true;
    }

    bool moveMasterPlugin (int from, int to, juce::String& error)
    {
        {
            const juce::ScopedLock sl (session.lock);
            const int n = (int) session.masterChain.size();
            if (! juce::isPositiveAndBelow (from, n) || ! juce::isPositiveAndBelow (to, n))
            {
                error = "No master plugin " + juce::String (juce::isPositiveAndBelow (from, n) ? to : from);
                return false;
            }
            if (from == to)
                return true;
        }
        auto before = captureStructuralXml();
        if (! session.moveMasterPlugin (from, to, error))
            return false;
        recordStructural (std::move (before), "Move master plugin");
        return true;
    }

    bool bypassMasterPlugin (int index, bool bypass, juce::String& error)
    {
        {
            const juce::ScopedLock sl (session.lock);
            if (! juce::isPositiveAndBelow (index, (int) session.masterChain.size()))
            {
                error = "No master plugin " + juce::String (index);
                return false;
            }
            if (session.masterChain[(size_t) index]->bypass == bypass)
                return true;
        }
        auto before = captureStructuralXml();
        if (! session.bypassMasterPlugin (index, bypass, error))
            return false;
        recordStructural (std::move (before), bypass ? "Bypass master plugin" : "Enable master plugin");
        return true;
    }

    bool setMasterChain (const juce::StringArray& plugins, juce::String& error)
    {
        auto before = captureStructuralXml();
        if (! session.setMasterChain (plugins, error))
            return false;
        recordStructural (std::move (before), "Set master chain");
        return true;
    }

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

    bool relinkMissingAudio (const juce::File& oldFile, const juce::File& newFile, int& relinkedCount, juce::String& error)
    {
        return relinkClipOrMatching (-1, -1, oldFile, newFile, relinkedCount, error);
    }

    bool relinkClipAudio (int track, int clip, const juce::File& newFile, juce::String& error)
    {
        int relinkedCount = 0;
        juce::File oldFile;
        {
            const juce::ScopedLock sl (session.lock);
            Clip* c = clipAt (track, clip, error);
            if (c == nullptr) return false;
            oldFile = c->file;
        }
        return relinkClipOrMatching (track, clip, oldFile, newFile, relinkedCount, error);
    }

    int rescanMissingAudio (const juce::File& searchRoot, juce::String& error)
    {
        const juce::ScopedLock sl (session.lock);
        auto before = captureIfNeeded();
        const int relinked = session.rescanMissingAudioLocked (searchRoot, error);
        if (relinked > 0)
            record (before, "Rescan missing audio");
        return relinked;
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

    bool resizeMidiClipStart (int track, int clip, juce::int64 newStart, juce::String& error, const MidiClip* baseClip = nullptr)
    {
        const juce::ScopedLock sl (session.lock);
        MidiClip* mc = midiClipAt (track, clip, error);
        if (mc == nullptr) return false;
        const juce::int64 baseStart = (baseClip != nullptr) ? baseClip->start : mc->start;
        const juce::int64 baseEnd = (baseClip != nullptr) ? baseClip->end() : mc->end();
        const juce::int64 start = juce::jlimit ((juce::int64) 0, juce::jmax ((juce::int64) 0, baseEnd - 1), newStart);
        if (start == mc->start && baseEnd == mc->end()) return true;
        const double deltaBeats = session.tempo.samplesToBeats ((double) start) - session.tempo.samplesToBeats ((double) baseStart);
        auto before = captureIfNeeded();
        mc->length = baseEnd - start;
        mc->start = start;
        if (baseClip != nullptr && baseClip != mc)
            mc->notes = baseClip->notes;
        for (auto it = mc->notes.begin(); it != mc->notes.end();)
        {
            it->startBeat -= deltaBeats;
            if (it->startBeat + it->durationBeats <= 1e-6)
                it = mc->notes.erase (it);
            else
            {
                if (it->startBeat < 0.0)
                {
                    it->durationBeats += it->startBeat;
                    it->startBeat = 0.0;
                }
                ++it;
            }
        }
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

    bool moveMidiNotes (int track, int clip, const std::vector<int>& notes, double deltaBeats, int deltaPitch, juce::String& error)
    {
        if (! std::isfinite (deltaBeats)) { error = "Delta beats must be a finite number."; return false; }
        const juce::ScopedLock sl (session.lock);
        MidiClip* mc = midiClipAt (track, clip, error);
        if (mc == nullptr) return false;
        const auto targets = resolveMidiNoteTargets (*mc, notes, false);
        bool changed = false;
        for (int idx : targets)
        {
            const auto& n = mc->notes[(size_t) idx];
            const double nextStart = juce::jlimit (0.0, 1e7, n.startBeat + deltaBeats);
            const int nextPitch = juce::jlimit (0, 127, n.pitch + deltaPitch);
            if (std::abs (nextStart - n.startBeat) >= 1e-9 || nextPitch != n.pitch)
            {
                changed = true;
                break;
            }
        }
        if (! changed) return true;
        auto before = captureIfNeeded();
        for (int idx : targets)
        {
            auto& n = mc->notes[(size_t) idx];
            n.startBeat = juce::jlimit (0.0, 1e7, n.startBeat + deltaBeats);
            n.pitch = juce::jlimit (0, 127, n.pitch + deltaPitch);
        }
        record (before, "Move MIDI notes");
        return true;
    }

    bool resizeMidiNotes (int track, int clip, const std::vector<int>& notes, double deltaDurationBeats, juce::String& error)
    {
        if (! std::isfinite (deltaDurationBeats)) { error = "Delta duration must be a finite number."; return false; }
        const juce::ScopedLock sl (session.lock);
        MidiClip* mc = midiClipAt (track, clip, error);
        if (mc == nullptr) return false;
        const auto targets = resolveMidiNoteTargets (*mc, notes, false);
        bool changed = false;
        for (int idx : targets)
        {
            const auto& n = mc->notes[(size_t) idx];
            const double nextDur = juce::jlimit (0.05, 1e6, n.durationBeats + deltaDurationBeats);
            if (std::abs (nextDur - n.durationBeats) >= 1e-9)
            {
                changed = true;
                break;
            }
        }
        if (! changed) return true;
        auto before = captureIfNeeded();
        for (int idx : targets)
        {
            auto& n = mc->notes[(size_t) idx];
            n.durationBeats = juce::jlimit (0.05, 1e6, n.durationBeats + deltaDurationBeats);
        }
        record (before, "Resize MIDI notes");
        return true;
    }

    bool quantizeMidiNotes (int track, int clip, const std::vector<int>& notes, Grid grid, double strength, double swing, juce::String& error)
    {
        if (! std::isfinite (strength)) { error = "Quantize strength must be a finite number."; return false; }
        if (! std::isfinite (swing)) { error = "Quantize swing must be a finite number."; return false; }
        const juce::ScopedLock sl (session.lock);
        MidiClip* mc = midiClipAt (track, clip, error);
        if (mc == nullptr) return false;
        const double s = juce::jlimit (0.0, 1.0, strength);
        const double sw = juce::jlimit (0.0, 1.0, swing);
        const double step = session.tempo.gridBeats (grid);
        if (! (step > 0.0) || s <= 0.0) return true;
        const auto targets = resolveMidiNoteTargets (*mc, notes, true);
        if (targets.empty()) return true;
        const double clipStartBeats = session.tempo.sampleRate > 0.0
                                        ? session.tempo.samplesToBeats ((double) mc->start)
                                        : 0.0;
        auto snapBeat = [step, clipStartBeats, s, sw] (double localBeat)
        {
            const double timelineBeat = clipStartBeats + localBeat;
            const double gridIdx = std::floor (timelineBeat / step + 0.5 + TempoMap::kGridEps);
            double snapped = gridIdx * step;
            if (sw > 0.0 && (std::llround (std::abs (gridIdx)) % 2) == 1)
                snapped += sw * 0.5 * step;
            const double snappedLocal = juce::jlimit (0.0, 1e7, snapped - clipStartBeats);
            return juce::jlimit (0.0, 1e7, localBeat + (snappedLocal - localBeat) * s);
        };
        bool changed = false;
        for (int idx : targets)
        {
            const double q = snapBeat (mc->notes[(size_t) idx].startBeat);
            if (std::abs (q - mc->notes[(size_t) idx].startBeat) > 1e-9) { changed = true; break; }
        }
        if (! changed) return true;
        auto before = captureIfNeeded();
        for (int idx : targets)
            mc->notes[(size_t) idx].startBeat = snapBeat (mc->notes[(size_t) idx].startBeat);
        record (before, notes.empty() ? "Quantize MIDI clip" : "Quantize MIDI notes");
        return true;
    }

    bool quantizeMidiNotes (int track, int clip, const std::vector<int>& notes, Grid grid, double strength, juce::String& error)
    {
        return quantizeMidiNotes (track, clip, notes, grid, strength, 0.0, error);
    }

    bool quantizeMidiClip (int track, int clip, Grid grid, juce::String& error)
    {
        return quantizeMidiNotes (track, clip, {}, grid, 1.0, error);
    }

    std::vector<int> duplicateMidiNotes (int track, int clip, const std::vector<int>& notes, double offsetBeats, juce::String& error)
    {
        if (! std::isfinite (offsetBeats)) { error = "Offset beats must be a finite number."; return {}; }
        const juce::ScopedLock sl (session.lock);
        MidiClip* mc = midiClipAt (track, clip, error);
        if (mc == nullptr) return {};
        const auto targets = resolveMidiNoteTargets (*mc, notes, false);
        if (targets.empty()) return {};
        auto before = captureIfNeeded();
        std::vector<int> created;
        created.reserve (targets.size());
        mc->notes.reserve (mc->notes.size() + targets.size());
        for (int idx : targets)
        {
            MidiNote copy = mc->notes[(size_t) idx];
            copy.startBeat = juce::jlimit (0.0, 1e7, copy.startBeat + offsetBeats);
            created.push_back ((int) mc->notes.size());
            mc->notes.push_back (copy);
        }
        record (before, "Duplicate MIDI notes");
        return created;
    }

    bool transposeMidiNotes (int track, int clip, const std::vector<int>& notes, int semitones, juce::String& error)
    {
        const juce::ScopedLock sl (session.lock);
        MidiClip* mc = midiClipAt (track, clip, error);
        if (mc == nullptr) return false;
        const auto targets = resolveMidiNoteTargets (*mc, notes, true);
        bool changed = false;
        for (int idx : targets)
        {
            const int nextPitch = juce::jlimit (0, 127, mc->notes[(size_t) idx].pitch + semitones);
            if (nextPitch != mc->notes[(size_t) idx].pitch)
            {
                changed = true;
                break;
            }
        }
        if (! changed) return true;
        auto before = captureIfNeeded();
        for (int idx : targets)
            mc->notes[(size_t) idx].pitch = juce::jlimit (0, 127, mc->notes[(size_t) idx].pitch + semitones);
        record (before, "Transpose MIDI notes");
        return true;
    }

    bool legatoMidiNotes (int track, int clip, const std::vector<int>& notes, juce::String& error)
    {
        const juce::ScopedLock sl (session.lock);
        MidiClip* mc = midiClipAt (track, clip, error);
        if (mc == nullptr) return false;
        auto targets = resolveMidiNoteTargets (*mc, notes, true);
        if (targets.size() < 2) return true;

        std::sort (targets.begin(), targets.end(), [mc] (int a, int b)
        {
            const auto& na = mc->notes[(size_t) a];
            const auto& nb = mc->notes[(size_t) b];
            if (na.startBeat < nb.startBeat) return true;
            if (nb.startBeat < na.startBeat) return false;
            if (na.pitch != nb.pitch) return na.pitch < nb.pitch;
            return a < b;
        });

        const size_t m = targets.size();
        std::vector<double> durations (m);
        for (size_t k = 0; k < m; ++k)
            durations[k] = mc->notes[(size_t) targets[k]].durationBeats;

        // First: trim same-pitch overlaps.
        for (size_t i = 0; i < m; ++i)
        {
            const auto& a = mc->notes[(size_t) targets[i]];
            for (size_t j = i + 1; j < m; ++j)
            {
                const auto& b = mc->notes[(size_t) targets[j]];
                if (b.pitch == a.pitch)
                {
                    if (a.startBeat + durations[i] > b.startBeat)
                        durations[i] = juce::jmin (durations[i], juce::jmax (1e-4, b.startBeat - a.startBeat));
                    break;
                }
            }
        }

        // Second: extend each note to the next note starting strictly later unless followed by an earlier same-pitch note.
        for (size_t i = 0; i < m; ++i)
        {
            const auto& ni = mc->notes[(size_t) targets[i]];
            size_t nextIdx = m;
            for (size_t j = i + 1; j < m; ++j)
            {
                if (mc->notes[(size_t) targets[j]].startBeat > ni.startBeat + 1e-6)
                {
                    nextIdx = j;
                    break;
                }
            }
            if (nextIdx == m)
                continue;

            bool earlierSamePitch = false;
            for (size_t k = i + 1; k < nextIdx; ++k)
            {
                if (mc->notes[(size_t) targets[k]].pitch == ni.pitch)
                {
                    earlierSamePitch = true;
                    break;
                }
            }
            if (! earlierSamePitch)
                durations[i] = juce::jlimit (0.05, 1e6, mc->notes[(size_t) targets[nextIdx]].startBeat - ni.startBeat);
        }

        bool changed = false;
        for (size_t k = 0; k < m; ++k)
        {
            if (std::abs (durations[k] - mc->notes[(size_t) targets[k]].durationBeats) >= 1e-9)
            {
                changed = true;
                break;
            }
        }
        if (! changed) return true;

        auto before = captureIfNeeded();
        for (size_t k = 0; k < m; ++k)
            mc->notes[(size_t) targets[k]].durationBeats = durations[k];
        record (before, "Legato MIDI notes");
        return true;
    }

    bool removeOverlapsMidiNotes (int track, int clip, const std::vector<int>& notes, juce::String& error)
    {
        const juce::ScopedLock sl (session.lock);
        MidiClip* mc = midiClipAt (track, clip, error);
        if (mc == nullptr) return false;
        auto targets = resolveMidiNoteTargets (*mc, notes, true);
        if (targets.size() < 2) return true;

        std::sort (targets.begin(), targets.end(), [mc] (int a, int b)
        {
            const auto& na = mc->notes[(size_t) a];
            const auto& nb = mc->notes[(size_t) b];
            if (na.startBeat < nb.startBeat) return true;
            if (nb.startBeat < na.startBeat) return false;
            if (na.pitch != nb.pitch) return na.pitch < nb.pitch;
            return a < b;
        });

        const size_t m = targets.size();
        std::vector<double> durations (m);
        for (size_t k = 0; k < m; ++k)
            durations[k] = mc->notes[(size_t) targets[k]].durationBeats;

        for (size_t i = 0; i < m; ++i)
        {
            const auto& a = mc->notes[(size_t) targets[i]];
            for (size_t j = i + 1; j < m; ++j)
            {
                const auto& b = mc->notes[(size_t) targets[j]];
                if (b.pitch == a.pitch && b.startBeat > a.startBeat + 1e-9)
                {
                    if (a.startBeat + durations[i] > b.startBeat)
                        durations[i] = juce::jmin (durations[i], juce::jmax (1e-4, b.startBeat - a.startBeat));
                    break;
                }
            }
        }

        bool changed = false;
        for (size_t k = 0; k < m; ++k)
        {
            if (std::abs (durations[k] - mc->notes[(size_t) targets[k]].durationBeats) >= 1e-9)
            {
                changed = true;
                break;
            }
        }
        if (! changed) return true;

        auto before = captureIfNeeded();
        for (size_t k = 0; k < m; ++k)
            mc->notes[(size_t) targets[k]].durationBeats = durations[k];
        record (before, "Remove MIDI note overlaps");
        return true;
    }

    bool invertMidiNotes (int track, int clip, const std::vector<int>& notes, juce::String& error)
    {
        const juce::ScopedLock sl (session.lock);
        MidiClip* mc = midiClipAt (track, clip, error);
        if (mc == nullptr) return false;
        const auto targets = resolveMidiNoteTargets (*mc, notes, true);
        if (targets.size() < 2) return true;

        int minPitch = 127;
        int maxPitch = 0;
        for (int idx : targets)
        {
            minPitch = juce::jmin (minPitch, mc->notes[(size_t) idx].pitch);
            maxPitch = juce::jmax (maxPitch, mc->notes[(size_t) idx].pitch);
        }

        bool changed = false;
        for (int idx : targets)
        {
            const int inv = juce::jlimit (0, 127, minPitch + maxPitch - mc->notes[(size_t) idx].pitch);
            if (inv != mc->notes[(size_t) idx].pitch) { changed = true; break; }
        }
        if (! changed) return true;

        auto before = captureIfNeeded();
        for (int idx : targets)
            mc->notes[(size_t) idx].pitch = juce::jlimit (0, 127, minPitch + maxPitch - mc->notes[(size_t) idx].pitch);
        record (before, "Invert MIDI notes");
        return true;
    }

    bool reverseMidiNotes (int track, int clip, const std::vector<int>& notes, juce::String& error)
    {
        const juce::ScopedLock sl (session.lock);
        MidiClip* mc = midiClipAt (track, clip, error);
        if (mc == nullptr) return false;
        const auto targets = resolveMidiNoteTargets (*mc, notes, true);
        if (targets.size() < 2) return true;

        double minStart = std::numeric_limits<double>::max();
        double maxEnd = 0.0;
        for (int idx : targets)
        {
            const auto& n = mc->notes[(size_t) idx];
            minStart = juce::jmin (minStart, n.startBeat);
            maxEnd = juce::jmax (maxEnd, n.startBeat + n.durationBeats);
        }

        bool changed = false;
        for (int idx : targets)
        {
            const auto& n = mc->notes[(size_t) idx];
            const double rev = juce::jlimit (0.0, 1e7, minStart + maxEnd - (n.startBeat + n.durationBeats));
            if (std::abs (rev - n.startBeat) > 1e-9) { changed = true; break; }
        }
        if (! changed) return true;

        auto before = captureIfNeeded();
        for (int idx : targets)
        {
            auto& n = mc->notes[(size_t) idx];
            n.startBeat = juce::jlimit (0.0, 1e7, minStart + maxEnd - (n.startBeat + n.durationBeats));
        }
        record (before, "Reverse MIDI notes");
        return true;
    }

    bool humanizeMidiNotes (int track, int clip, const std::vector<int>& notes, double maxTimingBeats, int maxVelocityDelta, juce::String& error)
    {
        if (! std::isfinite (maxTimingBeats)) { error = "Timing humanize amount must be a finite number."; return false; }
        const juce::ScopedLock sl (session.lock);
        MidiClip* mc = midiClipAt (track, clip, error);
        if (mc == nullptr) return false;
        const double timingRange = juce::jlimit (0.0, 1e6, maxTimingBeats);
        const int velRange = juce::jlimit (0, 127, maxVelocityDelta);
        if (timingRange <= 0.0 && velRange <= 0) return true;
        const auto targets = resolveMidiNoteTargets (*mc, notes, true);
        if (targets.empty()) return true;

        auto splitMix64 = [] (std::uint64_t x) -> std::uint64_t
        {
            x += 0x9e3779b97f4a7c15ULL;
            x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
            x = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;
            return x ^ (x >> 31);
        };

        std::vector<double> nextStarts (targets.size());
        std::vector<int> nextVels (targets.size());
        bool changed = false;
        for (size_t k = 0; k < targets.size(); ++k)
        {
            const int idx = targets[k];
            const auto& n = mc->notes[(size_t) idx];
            const std::uint64_t seed = splitMix64 (static_cast<std::uint64_t> (idx) * 0x9e3779b97f4a7c15ULL
                                                   ^ static_cast<std::uint64_t> (n.pitch) * 0xbf58476d1ce4e5b9ULL
                                                   ^ static_cast<std::uint64_t> (std::llround (n.startBeat * 1e6)));
            const std::uint64_t h1 = splitMix64 (seed);
            const std::uint64_t h2 = splitMix64 (seed ^ 0x517cc1b727220a95ULL);
            const double u1 = static_cast<double> (h1 >> 11) * (1.0 / 9007199254740992.0);
            const double dt = (u1 * 2.0 - 1.0) * timingRange;
            const int dv = velRange > 0 ? static_cast<int> (h2 % static_cast<std::uint64_t> (2 * velRange + 1)) - velRange : 0;

            nextStarts[k] = juce::jlimit (0.0, 1e7, n.startBeat + dt);
            nextVels[k] = juce::jlimit (1, 127, n.velocity + dv);
            if (std::abs (nextStarts[k] - n.startBeat) >= 1e-9 || nextVels[k] != n.velocity)
                changed = true;
        }
        if (! changed) return true;

        auto before = captureIfNeeded();
        for (size_t k = 0; k < targets.size(); ++k)
        {
            auto& n = mc->notes[(size_t) targets[k]];
            n.startBeat = nextStarts[k];
            n.velocity = nextVels[k];
        }
        record (before, "Humanize MIDI notes");
        return true;
    }

    // ---- clipboard ------------------------------------------------------------------------------------------------------------
    // The clipboard holds one or more clips (audio or MIDI) with offsets relative to the earliest clip's start and lowest track
    // index. Audio clips share their buffer (not copied). It lives in the Editor only: it is not the system clipboard, it is not
    // saved in the project and it is not an undo step, so it survives undo and redo. It is cleared by NEW and OPEN (clearClipboard),
    // and a paste is refused when the session's rate has changed since the copy.
    struct ClipboardClip
    {
        bool isMidi = false;
        Clip audio;
        MidiClip midi;
        juce::int64 sampleOffset = 0;
        int trackOffset = 0;
    };

    bool hasClipboard() const { return ! clipboardClips.empty(); }
    int clipboardTrack() const { return clipboardFrom; }   // the lowest track the copied clip(s) came from; -1 when empty
    int clipboardCount() const { return (int) clipboardClips.size(); }

    // Copies one clip, replacing what the clipboard held. Changes nothing in the session.
    bool copyClip (ClipRef ref, juce::String& error)
    {
        return copyClips (std::vector<ClipRef> { ref }, error);
    }

    // Copies multiple clips, preserving their relative sample and track offsets. Changes nothing in the session.
    bool copyClips (const std::vector<ClipRef>& refs, juce::String& error)
    {
        const juce::ScopedLock sl (session.lock);
        std::vector<ClipRef> unique;
        if (! resolveUniqueClipsLocked (refs, unique, error)) return false;
        storeClipboardClipsLocked (unique);
        return true;
    }

    bool copyClips (const std::vector<std::pair<int, int>>& audioClips, const std::vector<std::pair<int, int>>& midiClips, juce::String& error)
    {
        return copyClips (toClipRefs (audioClips, midiClips), error);
    }

    bool copyClips (const std::vector<std::pair<int, int>>& audioClips, juce::String& error)
    {
        return copyClips (toClipRefs (audioClips), error);
    }

    // Copies the clip and deletes it: ONE undo step ("Cut clip").
    bool cutClip (ClipRef ref, juce::String& error)
    {
        return cutClips (std::vector<ClipRef> { ref }, error);
    }

    // Copies multiple clips and deletes all of them in ONE undo step.
    bool cutClips (const std::vector<ClipRef>& refs, juce::String& error)
    {
        const juce::ScopedLock sl (session.lock);
        std::vector<ClipRef> unique;
        if (! resolveUniqueClipsLocked (refs, unique, error)) return false;
        storeClipboardClipsLocked (unique);
        auto before = captureIfNeeded();
        eraseClipsLocked (unique);
        record (before, unique.size() == 1 ? "Cut clip" : "Cut clips");
        return true;
    }

    bool cutClips (const std::vector<std::pair<int, int>>& audioClips, const std::vector<std::pair<int, int>>& midiClips, juce::String& error)
    {
        return cutClips (toClipRefs (audioClips, midiClips), error);
    }

    bool cutClips (const std::vector<std::pair<int, int>>& audioClips, juce::String& error)
    {
        return cutClips (toClipRefs (audioClips), error);
    }

    // Deletes multiple clips in ONE undo step.
    bool deleteClips (const std::vector<ClipRef>& refs, juce::String& error)
    {
        const juce::ScopedLock sl (session.lock);
        std::vector<ClipRef> unique;
        if (! resolveUniqueClipsLocked (refs, unique, error)) return false;
        auto before = captureIfNeeded();
        eraseClipsLocked (unique);
        record (before, unique.size() == 1 ? "Delete clip" : "Delete clips");
        return true;
    }

    bool deleteClips (const std::vector<std::pair<int, int>>& audioClips, const std::vector<std::pair<int, int>>& midiClips, juce::String& error)
    {
        return deleteClips (toClipRefs (audioClips, midiClips), error);
    }

    bool deleteClips (const std::vector<std::pair<int, int>>& audioClips, juce::String& error)
    {
        return deleteClips (toClipRefs (audioClips), error);
    }

    // Adds a copy of the clipboard's clip(s) starting at `track` and sample `at`: ONE undo step ("Paste clip" / "Paste clips").
    // Returns the first pasted clip. Refused, with the reason, when the clipboard is empty, the track is not there, `at` is
    // before 0, or the session's rate is not the one the clip was copied at.
    ClipRef pasteClip (int track, juce::int64 at, juce::String& error)
    {
        const auto pasted = pasteClips (track, at, error);
        return pasted.empty() ? ClipRef {} : pasted.front();
    }

    std::vector<ClipRef> pasteClips (int baseTrack, juce::int64 at, juce::String& error)
    {
        const juce::ScopedLock sl (session.lock);
        if (clipboardClips.empty()) { error = "The clipboard is empty: copy or cut a clip first."; return {}; }
        if (! isTrack (baseTrack)) { error = "No track " + juce::String (baseTrack); return {}; }
        if (at < 0) { error = "The paste position must be 0 or later."; return {}; }
        if (std::abs (clipboardRate - session.sampleRate) > 0.5) { error = "The clipboard holds audio at another sample rate: copy the clip again."; return {}; }
        auto before = captureIfNeeded();
        std::vector<ClipRef> pasted;
        pasted.reserve (clipboardClips.size());
        const int maxTrack = (int) session.tracks.size() - 1;
        for (const auto& cc : clipboardClips)
        {
            const int dstTrack = juce::jlimit (0, maxTrack, baseTrack + cc.trackOffset);
            const juce::int64 dstStart = juce::jlimit ((juce::int64) 0, kMaxSafeSample, at + cc.sampleOffset);
            if (cc.isMidi)
            {
                MidiClip copy = cc.midi;
                copy.start = dstStart;
                auto& clips = session.tracks[(size_t) dstTrack]->midiClips;
                clips.push_back (std::move (copy));
                pasted.push_back ({ dstTrack, (int) clips.size() - 1, true });
            }
            else
            {
                Clip copy = cc.audio;
                copy.start = dstStart;
                auto& clips = session.tracks[(size_t) dstTrack]->clips;
                clips.push_back (std::move (copy));
                pasted.push_back ({ dstTrack, (int) clips.size() - 1, false });
            }
        }
        record (before, clipboardClips.size() == 1 ? "Paste clip" : "Paste clips");
        return pasted;
    }

    void clearClipboard()
    {
        clipboardClips.clear();
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
    // a non-structural step was taken at another track count: then both stacks are cleared.
    bool undo() { return applyStep (undoSteps, redoSteps, true); }

    // The mirror of undo: puts back the edit that was undone.
    bool redo() { return applyStep (redoSteps, undoSteps, false); }

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
                    editor.push ({ std::move (editor.gestureBefore), editor.gestureTracks, editor.gestureLabel, false, {}, {} });
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
        bool structural = false;
        std::shared_ptr<juce::XmlElement> beforeXml, afterXml;
    };

    Session& session;
    std::vector<Step> undoSteps, redoSteps;
    std::vector<ClipboardClip> clipboardClips; // what copy/cut put there (see the clipboard section)
    int clipboardFrom = -1;
    double clipboardRate = 0.0;           // the session's rate when it was copied: a paste at another rate is refused
    int gestureDepth = 0;
    bool gestureChanged = false;
    bool gestureInvalid = false;
    EditState gestureBefore;
    int gestureTracks = 0;
    juce::String gestureLabel;

    bool relinkClipOrMatching (int track, int clip, const juce::File& oldFile, const juce::File& newFile, int& relinkedCount, juce::String& error)
    {
        relinkedCount = 0;
        if (! newFile.existsAsFile())
        {
            error = newFile.getFileName() + " could not be found.";
            return false;
        }
        const juce::ScopedLock sl (session.lock);
        Clip* target = nullptr;
        if (track >= 0)
        {
            target = clipAt (track, clip, error);
            if (target == nullptr)
                return false;
        }
        if (target == nullptr)
        {
            bool hasMatch = false;
            if (oldFile != juce::File())
            {
                for (const auto& t : session.tracks)
                {
                    for (const auto& c : t->clips)
                    {
                        if (c.audio == nullptr && c.file == oldFile)
                        {
                            hasMatch = true;
                            break;
                        }
                    }
                    if (hasMatch)
                        break;
                }
            }
            if (! hasMatch)
            {
                error = "No matching missing audio clip was found.";
                return false;
            }
        }
        auto before = captureIfNeeded();
        auto buf = session.loadPooledLocked (newFile, error);
        if (buf == nullptr)
            return false;
        auto applyRelink = [&] (Clip& c)
        {
            Session::relinkClipLocked (c, newFile, buf);
            ++relinkedCount;
        };
        if (oldFile != juce::File())
        {
            for (auto& t : session.tracks)
                for (auto& c : t->clips)
                    if (c.audio == nullptr && c.file == oldFile)
                        applyRelink (c);
        }
        if (relinkedCount == 0 && target != nullptr)
            applyRelink (*target);
        if (relinkedCount == 0)
        {
            error = "No matching missing audio clip was found.";
            return false;
        }
        session.finishRelinkLocked();
        record (before, "Locate audio");
        return true;
    }

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
            push ({ std::move (*before), (int) session.tracks.size(), label, false, {}, {} });
    }

    std::shared_ptr<juce::XmlElement> captureStructuralXml() const
    {
        const juce::ScopedLock sl (session.lock);
        return session.toXml (session.projectFile);
    }

    void recordStructural (std::shared_ptr<juce::XmlElement> before, const juce::String& label)
    {
        auto after = captureStructuralXml();
        const juce::ScopedLock sl (session.lock);
        if (gestureDepth > 0)
        {
            gestureInvalid = true;
            return;
        }
        Step step;
        step.label = label;
        step.structural = true;
        step.beforeXml = std::move (before);
        step.afterXml = std::move (after);
        push (std::move (step));
    }

    bool applyStep (std::vector<Step>& from, std::vector<Step>& to, bool undoing)
    {
        Step step;
        {
            const juce::ScopedLock sl (session.lock);
            if (gestureDepth > 0 || from.empty()) return false;
            if (! from.back().structural)
            {
                if (from.back().tracks != (int) session.tracks.size()) { clearUndo(); return false; }
                step = std::move (from.back());
                from.pop_back();
                to.push_back ({ session.captureEdit(), step.tracks, step.label, false, {}, {} });
                trimSteps (to);
                session.restoreEdit (step.state);
                return true;
            }
            step = std::move (from.back());
            from.pop_back();
        }
        if (onBeforeStructureChange)
            onBeforeStructureChange();
        auto currentXml = captureStructuralXml();
        const auto& targetXml = undoing ? step.beforeXml : step.afterXml;
        if (targetXml == nullptr || ! session.restoreEditXml (*targetXml))
            return false;
        Step opposite;
        opposite.label = step.label;
        opposite.structural = true;
        opposite.beforeXml = undoing ? std::move (step.beforeXml) : std::move (currentXml);
        opposite.afterXml = undoing ? std::move (currentXml) : std::move (step.afterXml);
        const juce::ScopedLock sl (session.lock);
        to.push_back (std::move (opposite));
        trimSteps (to);
        return true;
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

    static std::vector<ClipRef> toClipRefs (const std::vector<std::pair<int, int>>& audioClips,
                                            const std::vector<std::pair<int, int>>& midiClips = {})
    {
        std::vector<ClipRef> refs;
        refs.reserve (audioClips.size() + midiClips.size());
        for (const auto& p : audioClips) refs.push_back ({ p.first, p.second, false });
        for (const auto& p : midiClips)  refs.push_back ({ p.first, p.second, true });
        return refs;
    }

    bool resolveUniqueClipsLocked (const std::vector<ClipRef>& refs, std::vector<ClipRef>& uniqueOut, juce::String& error)
    {
        uniqueOut.clear();
        for (const auto& r : refs)
        {
            if (r.isMidi)
            {
                if (midiClipAt (r.track, r.clip, error) == nullptr)
                    return false;
            }
            else
            {
                if (clipAt (r.track, r.clip, error) == nullptr)
                    return false;
            }
            if (std::find (uniqueOut.begin(), uniqueOut.end(), r) == uniqueOut.end())
                uniqueOut.push_back (r);
        }
        if (uniqueOut.empty())
        {
            error = "No clips selected.";
            return false;
        }
        return true;
    }

    void storeClipboardClipsLocked (const std::vector<ClipRef>& uniqueRefs)
    {
        juce::int64 minStart = std::numeric_limits<juce::int64>::max();
        int minTrack = std::numeric_limits<int>::max();
        for (const auto& r : uniqueRefs)
        {
            const juce::int64 st = r.isMidi ? session.tracks[(size_t) r.track]->midiClips[(size_t) r.clip].start
                                            : session.tracks[(size_t) r.track]->clips[(size_t) r.clip].start;
            minStart = juce::jmin (minStart, st);
            minTrack = juce::jmin (minTrack, r.track);
        }
        std::vector<ClipboardClip> next;
        next.reserve (uniqueRefs.size());
        for (const auto& r : uniqueRefs)
        {
            ClipboardClip item;
            item.isMidi = r.isMidi;
            item.trackOffset = r.track - minTrack;
            if (r.isMidi)
            {
                item.midi = session.tracks[(size_t) r.track]->midiClips[(size_t) r.clip];
                item.sampleOffset = item.midi.start - minStart;
            }
            else
            {
                item.audio = session.tracks[(size_t) r.track]->clips[(size_t) r.clip];
                item.sampleOffset = item.audio.start - minStart;
            }
            next.push_back (std::move (item));
        }
        clipboardClips = std::move (next);
        clipboardFrom = minTrack;
        clipboardRate = session.sampleRate;
    }

    void eraseClipsLocked (std::vector<ClipRef> uniqueRefs)
    {
        std::sort (uniqueRefs.begin(), uniqueRefs.end(), [] (const ClipRef& a, const ClipRef& b)
        {
            if (a.track != b.track) return a.track > b.track;
            if (a.isMidi != b.isMidi) return a.isMidi > b.isMidi;
            return a.clip > b.clip;
        });
        for (const auto& r : uniqueRefs)
        {
            if (r.isMidi)
            {
                auto& clips = session.tracks[(size_t) r.track]->midiClips;
                clips.erase (clips.begin() + r.clip);
            }
            else
            {
                auto& clips = session.tracks[(size_t) r.track]->clips;
                clips.erase (clips.begin() + r.clip);
            }
        }
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

    static std::vector<int> resolveMidiNoteTargets (const MidiClip& mc, const std::vector<int>& notes, bool allIfEmpty)
    {
        std::vector<int> targets;
        const int count = (int) mc.notes.size();
        if (notes.empty())
        {
            if (! allIfEmpty)
                return targets;
            targets.reserve ((size_t) count);
            for (int i = 0; i < count; ++i)
                targets.push_back (i);
            return targets;
        }
        std::vector<bool> seen ((size_t) count, false);
        targets.reserve (notes.size());
        for (int idx : notes)
        {
            if (juce::isPositiveAndBelow (idx, count) && ! seen[(size_t) idx])
            {
                seen[(size_t) idx] = true;
                targets.push_back (idx);
            }
        }
        return targets;
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
