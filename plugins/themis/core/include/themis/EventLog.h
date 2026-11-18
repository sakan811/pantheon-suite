// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 The Pantheon authors

#pragma once

#include "FrameHistory.h"
#include "HistoryChecks.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace themis
{
// The event log: the notable moments since the reset, each with its position in the measured audio. Pure C++ (no JUCE, no
// threads): the plugin feeds it readings of the meters from its analysis thread, never the audio thread, and the check report
// reads it. Positions are seconds of audio measured since the reset (the loudness meter's count, not a host timecode); each is
// as exact as the reading that caught it. The plugin reads the meters about every 15 ms, so a position is good to that.
namespace events
{
inline constexpr size_t kCapacity = 64;              // entries kept; the rest are only counted (EventLog::overflowCount)
inline constexpr float kPeakStepDb = 0.5f;           // a new true-peak maximum above the ceiling is logged once it has risen this much
inline constexpr float kExcursionMultiple = 2.0f;    // a loudness excursion starts this many loudness-zone widths above the target
inline constexpr double kExcursionHoldSeconds = 3.0; // and must stay above that level this long before it is logged

enum class Kind { clip, truePeak, loudness };

struct Event
{
    double seconds = 0.0;               // position in the measured audio, from the reset
    Kind kind = Kind::clip;
    float valueDb = 0.0f;               // dBTP for clip and truePeak, LUFS for loudness (the short-term level at the start)
};

// What the meters say at one reading (the same numbers the plan and the readouts show).
struct Reading
{
    double seconds = 0.0;               // audio measured since the reset
    bool clipped = false;               // a flat-topped clip or a sample at 0 dBFS has been seen since the reset
    float truePeakDb = kHistoryFloorDb; // the largest true peak since the reset, dBTP
    float ceilingDb = 0.0f;             // the user's true-peak ceiling
    bool haveLoudness = false;
    float shortTermLufs = kHistoryFloorDb;
    float targetLufs = 0.0f;            // the Deliver for target
};

// The short-term level a loudness excursion starts above: the delivery target plus the loudness zone's width, times the multiple.
inline float excursionLevelLufs (float targetLufs) { return targetLufs + history::kLoudnessZoneLu * kExcursionMultiple; }

class EventLog
{
public:
    // Adds an entry in arrival order. Past kCapacity the entry is counted instead, so the first entries are the ones kept.
    void add (const Event& e)
    {
        if (entries_.size() < kCapacity)
            entries_.push_back (e);
        else
            ++overflow_;
    }

    const std::vector<Event>& entries() const { return entries_; }
    uint64_t overflowCount() const { return overflow_; }

    // Forgets everything, including the rules' memory (a reset starts a new log).
    void clear()
    {
        entries_.clear();
        overflow_ = 0;
        clipLogged_ = false;
        peakLogged_ = false;
        loggedPeakDb_ = 0.0f;
        inExcursion_ = false;
        excursionLogged_ = false;
        excursionStart_ = 0.0;
        excursionDb_ = 0.0f;
    }

    // The rules that decide what is logged, applied to one reading. Readings must come in the order they were taken; a reading
    // that repeats the last one logs nothing new.
    //   * CLIP: the first reading with a flat top or a sample at 0 dBFS.
    //   * PEAK: while the true peak is above the ceiling, a reading that is at least kPeakStepDb above the last logged peak
    //     (the first one above the ceiling is always logged).
    //   * LOUD: the short-term loudness above excursionLevelLufs() for at least kExcursionHoldSeconds; the entry is the
    //     start of that stretch, and the excursion is logged once. Dropping under the level ends it.
    // An excursion is logged when its hold has passed, so an event that lands inside those few seconds is logged before it.
    void observe (const Reading& r)
    {
        if (r.clipped && !clipLogged_)
        {
            clipLogged_ = true;
            add ({ r.seconds, Kind::clip, r.truePeakDb });
        }

        if (r.truePeakDb > r.ceilingDb && (!peakLogged_ || r.truePeakDb >= loggedPeakDb_ + kPeakStepDb))
        {
            peakLogged_ = true;
            loggedPeakDb_ = r.truePeakDb;
            add ({ r.seconds, Kind::truePeak, r.truePeakDb });
        }

        const bool above = r.haveLoudness && r.shortTermLufs > excursionLevelLufs (r.targetLufs);
        if (!above)
        {
            inExcursion_ = false;
            excursionLogged_ = false;
            return;
        }
        if (!inExcursion_)
        {
            inExcursion_ = true;
            excursionStart_ = r.seconds;
            excursionDb_ = r.shortTermLufs;
        }
        if (!excursionLogged_ && r.seconds - excursionStart_ >= kExcursionHoldSeconds)
        {
            excursionLogged_ = true;
            add ({ excursionStart_, Kind::loudness, excursionDb_ });
        }
    }

private:
    std::vector<Event> entries_;
    uint64_t overflow_ = 0;
    bool clipLogged_ = false;
    bool peakLogged_ = false;
    float loggedPeakDb_ = 0.0f;
    bool inExcursion_ = false;
    bool excursionLogged_ = false;
    double excursionStart_ = 0.0;
    float excursionDb_ = 0.0f;
};
} // namespace events
} // namespace themis
