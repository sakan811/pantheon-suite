// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 The Pantheon authors

#pragma once

#include "Edit.h"
#include "Session.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <memory>
#include <utility>
#include <vector>

namespace olympus
{
// The automation writer (F4): records a lane's control while the transport plays, for each lane whose mode is not Read.
//
// A lane writes while its mode says so: Touch while its control is held (begin until end), Latch from the first touch of the pass until the
// transport stops (the last value is held), Write from the pass's first tick until it stops. What it writes replaces the lane's curve over the
// span it passes through (a segment). A segment opens with a guard point one sample before its start (the old curve's value, so the curve
// does not jump), and closes with a guard point 50 ms after its last sample (the old curve's value there, so it glides back). Thinning then
// drops the points a straight line already gives. A writing lane is marked overriding, so it is not played: the control's own value sounds.
//
// The pass is one undo step: an Editor::Gesture opened at its first write and closed by stop(). A Write lane becomes Touch at the end of the
// pass, inside that gesture, so a second pass does not overwrite by accident. A structural edit during the pass drops the step (the
// gesture's own rule). Message thread only: the window's timer calls tick(); the faders, the pans and the plugin controls call begin() and
// end(). The plugin listener (ParamTouchWatch) only stores atomics; tick() reads them.
class AutomationWriter
{
public:
    AutomationWriter (Session& s, Editor& e) : session (s), editor (e) {}
    ~AutomationWriter() { stop(); }

    AutomationWriter (const AutomationWriter&) = delete;
    AutomationWriter& operator= (const AutomationWriter&) = delete;

    // A control is held. Touch and Latch lanes start writing at the next tick (the position is only known there). While the transport
    // plays, the lane stops playing its old curve at once, so the plugin's value is not set back for up to a tick.
    void begin (int track, const juce::String& targetId)
    {
        stateOf (track, targetId).touched = true;
        const auto mode = modeOf (track, targetId);
        if (lastPlaying && (mode == AutoMode::Touch || mode == AutoMode::Latch))
        {
            const juce::ScopedTryLock sl (session.lock);
            if (sl.isLocked())
                editor.setAutomationOverriding (track, targetId, true);
        }
    }

    // The control is let go. A Touch segment ends at once when the lock is free; otherwise the next tick ends it.
    void end (int track, const juce::String& targetId)
    {
        auto& st = stateOf (track, targetId);
        st.touched = false;
        const juce::ScopedTryLock sl (session.lock);
        if (sl.isLocked() && st.segOpen && modeOf (track, targetId) == AutoMode::Touch)
            closeSegment (track, targetId, st);
    }

    // The timer's call with the transport's position and state. Does nothing while a render runs, or while the lock is busy: the message
    // thread never waits for it.
    void tick (juce::int64 position, bool playing)
    {
        if (session.renderRunning())
            return;
        const juce::ScopedTryLock sl (session.lock);
        if (! sl.isLocked())
            return;

        // A track was added or removed since the last tick: the lanes' indices now name other tracks. The pass ends here, and no lane is left
        // overriding (a lane that is not played is not written either).
        const bool tracksChanged = trackCount >= 0 && trackCount != (int) session.tracks.size();
        trackCount = (int) session.tracks.size();
        if (tracksChanged)
        {
            entries.clear();
            heldPlugins.clear();
            gesture.reset();
            for (int t = 0; t < (int) session.tracks.size(); ++t)
                for (const auto& lane : session.tracks[(size_t) t]->automation)
                    editor.setAutomationOverriding (t, lane.targetId, false);
            havePos = false;
            return;
        }

        // The plugins' gestures: a control held since an earlier tick, and one begun and let go since the last tick (both happen in one tick).
        std::vector<Hold> begins, ends;
        std::map<const Slot*, Hold> held;
        for (int t = 0; t < (int) session.tracks.size(); ++t)
        {
            auto& track = *session.tracks[(size_t) t];
            for (int s = 0; s < (int) track.chain.size(); ++s)
            {
                auto& slot = *track.chain[(size_t) s];
                auto previous = heldPlugins.find (&slot);
                Hold active = previous != heldPlugins.end() ? previous->second : Hold { -1, {} };
                const int param = slot.touch.touchedParam.exchange (-1);
                if (slot.touch.touching.load())
                {
                    if (param >= 0)
                    {
                        const auto target = pluginTarget (s, param);
                        if (active.track != t || active.target != target)
                        {
                            if (active.track >= 0)
                                ends.push_back (active);
                            begins.push_back ({ t, target });
                            active = { t, target };
                        }
                    }
                }
                else
                {
                    if (active.track >= 0)
                    {
                        ends.push_back (active);
                        active = { -1, {} };
                    }
                    if (param >= 0)
                    {
                        const auto target = pluginTarget (s, param);
                        begins.push_back ({ t, target });
                        ends.push_back ({ t, target });
                    }
                }
                if (active.track >= 0)
                    held[&slot] = active;
            }
        }
        heldPlugins = std::move (held);

        lastPlaying = playing;
        for (const auto& b : begins)
            begin (b.track, b.target);
        if (playing)
            writeStep (position);
        else
            stop();
        for (const auto& e : ends)
            end (e.track, e.target);
    }

    // Ends the pass now: every segment closes, the Write lanes that wrote become Touch, and the pass's undo step is made. The transport's
    // stop calls it; so does the window's destructor.
    void stop()
    {
        const juce::ScopedLock sl (session.lock);
        for (auto& e : entries)
            closeSegment (e.track, e.target, *e.state);
        for (auto& e : entries)
            if (e.state->wrote && modeOf (e.track, e.target) == AutoMode::Write)
            {
                juce::String error;
                editor.setAutomationMode (e.track, e.target, AutoMode::Touch, error); // inside the pass's gesture: one step with it
            }
        for (auto& e : entries)
        {
            e.state->wrote = false;
            e.state->latched = false;
        }
        gesture.reset(); // the pass's one undo step, pushed here unless a structural edit dropped it
        havePos = false;
    }

    // True while the lane's segment is being written (the lane's lamp is lit).
    bool isWriting (int track, const juce::String& targetId) const
    {
        for (const auto& e : entries)
            if (e.track == track && e.target == targetId)
                return e.state->segOpen;
        return false;
    }

private:
    struct Hold
    {
        int track = -1;
        juce::String target;
    };

    // The lane's target of a plugin's parameter, as the session names it ("plugin:<slot>:<param>").
    static juce::String pluginTarget (int slot, int param)
    {
        return "plugin:" + juce::String (slot) + ":" + juce::String (param);
    }

    struct LaneState
    {
        bool touched = false;     // the control is held (begin until end)
        bool latched = false;     // Latch: from the first touch of the pass until the pass stops
        bool segOpen = false;     // a segment is being written: the lane is overriding
        bool wrote = false;       // written in the current pass (a Write lane then becomes Touch)
        juce::int64 last = 0;     // the last sample written: the next tick erases the points after it
        juce::int64 segStart = 0; // the segment's first sample (its start guard, or its first point): thinning starts here
        float tailValue = 0.0f;   // the old curve at the last tick: the tail guard's value
        AutomationLane before;    // the lane as the segment found it: the old curve the guards and the tail read
    };

    struct Entry
    {
        int track = -1;
        juce::String target;
        std::unique_ptr<LaneState> state;
    };

    Session& session;
    Editor& editor;
    std::vector<Entry> entries;                    // one per lane asked about; the states are heap-allocated, so references stay valid
    std::unique_ptr<Editor::Gesture> gesture;      // the pass's undo step: from its first write until stop()
    std::map<const Slot*, Hold> heldPlugins;       // the plugin control each slot last reported as held, carried from tick to tick
    bool havePos = false;
    juce::int64 lastPos = 0;
    int trackCount = -1;                           // the track count at the last tick (-1 before the first); a change ends the pass
    bool lastPlaying = false;                      // the transport's state at the last tick: begin() marks a lane at once only while it plays

    LaneState& stateOf (int track, const juce::String& targetId)
    {
        for (auto& e : entries)
            if (e.track == track && e.target == targetId)
                return *e.state;
        entries.push_back ({ track, targetId, std::make_unique<LaneState>() });
        return *entries.back().state;
    }

    const AutomationLane* laneOf (int track, const juce::String& targetId) const
    {
        if (! juce::isPositiveAndBelow (track, (int) session.tracks.size()))
            return nullptr;
        for (const auto& lane : session.tracks[(size_t) track]->automation)
            if (lane.targetId == targetId)
                return &lane;
        return nullptr;
    }

    AutoMode modeOf (int track, const juce::String& targetId) const
    {
        const auto* lane = laneOf (track, targetId);
        return lane != nullptr ? lane->mode : AutoMode::Read;
    }

    juce::int64 samplesFor (double ms) const
    {
        return (juce::int64) std::llround (ms * 0.001 * (session.sampleRate > 0.0 ? session.sampleRate : 48000.0));
    }

    bool pointWithin (int track, const juce::String& targetId, juce::int64 from, juce::int64 to) const
    {
        const auto* lane = laneOf (track, targetId);
        if (lane == nullptr)
            return false;
        for (const auto& p : lane->points)
            if (p.sample >= from && p.sample <= to)
                return true;
        return false;
    }

    // The control's value, normalised as the lane stores it. False when the control is not there (a plugin that is gone).
    bool controlValue (int track, const juce::String& targetId, float& out) const
    {
        if (! juce::isPositiveAndBelow (track, (int) session.tracks.size()))
            return false;
        const auto& t = *session.tracks[(size_t) track];
        if (targetId == "volume")
        {
            out = dbToAutoNorm (t.gainDb);
            return true;
        }
        if (targetId == "pan")
        {
            out = juce::jlimit (0.0f, 1.0f, (t.pan + 1.0f) * 0.5f);
            return true;
        }
        if (! targetId.startsWith ("plugin:"))
            return false;
        const auto rest = targetId.fromFirstOccurrenceOf ("plugin:", false, false);
        const int colon = rest.indexOfChar (':');
        if (colon <= 0)
            return false;
        const int slot = rest.substring (0, colon).getIntValue();
        const int param = rest.substring (colon + 1).getIntValue();
        if (! juce::isPositiveAndBelow (slot, (int) t.chain.size()))
            return false;
        const auto* instance = t.chain[(size_t) slot]->instance.get();
        if (instance == nullptr)
            return false;
        const auto& params = instance->getParameters();
        if (! juce::isPositiveAndBelow (param, params.size()) || params[param] == nullptr)
            return false;
        out = juce::jlimit (0.0f, 1.0f, params[param]->getValue());
        return true;
    }

    void ensureGesture()
    {
        if (gesture == nullptr)
            gesture = std::make_unique<Editor::Gesture> (editor, "Automation pass");
    }

    // Starts a segment at `pos`: the lane is marked overriding, and a guard point one sample before `pos` holds the old curve's value there
    // (unless a point is within a millisecond of it already).
    void openSegment (int track, const juce::String& targetId, LaneState& st, juce::int64 pos)
    {
        const auto* lane = laneOf (track, targetId);
        if (lane == nullptr)
            return;
        st.before = *lane;
        st.segOpen = true;
        st.wrote = false;
        const bool guard = pos >= 1 && ! pointWithin (track, targetId, pos - samplesFor (1.0), pos);
        if (guard)
        {
            editor.writeAutomationSegment (track, targetId, pos - 1, pos - 1, st.before.evaluateAt (pos));
            st.last = pos - 1;
            st.segStart = pos - 1;
        }
        else
        {
            st.last = pos - 1;
            st.segStart = pos;
        }
        editor.setAutomationOverriding (track, targetId, true);
    }

    // One tick's point: the control's value at `pos`, the segment's points after `last` replaced by it.
    void writePoint (int track, const juce::String& targetId, LaneState& st, juce::int64 pos, float value)
    {
        st.tailValue = st.before.evaluateAt (pos);
        editor.writeAutomationSegment (track, targetId, st.last, pos, value);
        st.last = pos;
        st.wrote = true;
    }

    // Ends a segment: the tail guard 50 ms after its last sample (the old curve there), the thinning of its points, and the lane plays again.
    void closeSegment (int track, const juce::String& targetId, LaneState& st)
    {
        if (! st.segOpen)
        {
            // begin() may have marked the lane before a segment opened: a lane that is not written plays again
            editor.setAutomationOverriding (track, targetId, false);
            return;
        }
        st.segOpen = false;
        if (laneOf (track, targetId) == nullptr)
            return; // the lane went with its track
        const juce::int64 tail = st.last + samplesFor (50.0);
        editor.writeAutomationSegment (track, targetId, tail, tail, st.tailValue);
        editor.thinAutomationRange (track, targetId, st.segStart, tail, 0.002f);
        editor.setAutomationOverriding (track, targetId, false);
    }

    // Writes the lanes that are not Read, for the position of this tick. A position that goes back (a loop's wrap, a seek) ends every segment
    // first, so the next one starts at the new position.
    void writeStep (juce::int64 pos)
    {
        if (havePos && pos < lastPos)
            for (auto& e : entries)
                closeSegment (e.track, e.target, *e.state);
        havePos = true;
        lastPos = pos;

        struct Want { int track; juce::String target; AutoMode mode; };
        std::vector<Want> wants;
        for (int t = 0; t < (int) session.tracks.size(); ++t)
            for (const auto& lane : session.tracks[(size_t) t]->automation)
                if (lane.enabled && lane.mode != AutoMode::Read)
                    wants.push_back ({ t, lane.targetId, lane.mode });

        // a lane that stopped being written (its mode was changed, or it was disabled) ends its segment
        for (auto& e : entries)
        {
            const bool wanted = std::any_of (wants.begin(), wants.end(), [&e] (const Want& w) { return w.track == e.track && w.target == e.target; });
            if (! wanted)
                closeSegment (e.track, e.target, *e.state);
        }

        for (const auto& w : wants)
        {
            auto& st = stateOf (w.track, w.target);
            bool writes = false;
            if (w.mode == AutoMode::Write)
                writes = true;
            else if (w.mode == AutoMode::Touch)
                writes = st.touched;
            else
            {
                if (st.touched)
                    st.latched = true;
                writes = st.latched;
            }
            float value = 0.0f;
            if (! writes || laneOf (w.track, w.target) == nullptr || ! controlValue (w.track, w.target, value))
            {
                closeSegment (w.track, w.target, st);
                continue;
            }
            ensureGesture();
            if (! st.segOpen)
                openSegment (w.track, w.target, st, pos);
            if (st.segOpen)
                writePoint (w.track, w.target, st, pos, value);
        }
    }
};
} // namespace olympus
