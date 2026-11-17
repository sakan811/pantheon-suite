// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 The Pantheon authors

#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <themis/Common.h>
#include <themis/MasterAdvisor.h>
#include <themis/TargetCurve.h>
#include <themis/TonalAnalyzer.h>
#include <themis/Translation.h>

namespace params
{
// Averaging choices: one table, so the names and the times can't drift apart. 0 s = infinite (cumulative mean).
struct AvgTime
{
    const char* name;
    float seconds;
};
inline constexpr AvgTime avgTimes[] = { { "10 s", 10.0f }, { "30 s", 30.0f }, { "2 min", 120.0f }, { "Infinite", 0.0f } };
inline constexpr int kNumAvgTimes = static_cast<int> (sizeof (avgTimes) / sizeof (avgTimes[0]));
// The plugin opens on "Infinite" (a whole-track measurement, what a master is judged on). The meters' own default window
// (Common.h) is still the "30 s" choice, so that choice must exist and match it.
inline constexpr int kDefaultAvgTime = 3;
static_assert (kDefaultAvgTime >= 0 && kDefaultAvgTime < kNumAvgTimes, "the default averaging choice must exist");
static_assert (avgTimes[1].seconds - themis::kDefaultAveragingSeconds < 1.0e-6f && themis::kDefaultAveragingSeconds - avgTimes[1].seconds < 1.0e-6f,
               "the 30 s choice must match the meters' default window");

inline juce::StringArray avgTimeNames()
{
    juce::StringArray n;
    for (const auto& a : avgTimes)
        n.add (a.name);
    return n;
}

// Target choices: every built-in curve, then "Custom" (learned from references / loaded from file).
inline int customTargetIndex() { return static_cast<int> (themis::TargetCurve::builtIns().size()); }
inline constexpr int kDefaultTarget = 1; // "Modern Pop / Streaming"

inline juce::StringArray targetNames()
{
    juce::StringArray n;
    for (const auto& c : themis::TargetCurve::builtIns())
        n.add (c.name);
    n.add ("Custom");
    jassert (n.size() == customTargetIndex() + 1);
    return n;
}

// Playback-device choices for the translation preview: "Off", then every built-in device profile.
inline constexpr int kDeviceOff = 0;
inline constexpr int kDefaultDevice = kDeviceOff; // no preview until one is chosen (DEVICE LOSS then says so)

inline juce::StringArray deviceNames()
{
    juce::StringArray n;
    n.add ("Off");
    for (const auto& d : themis::DeviceProfile::builtIns())
        n.add (d.name);
    return n;
}

// The device behind a choice index, or nullptr for "Off" / out of range.
inline const themis::DeviceProfile* deviceForChoice (int choice)
{
    const auto& devices = themis::DeviceProfile::builtIns();
    const int idx = choice - (kDeviceOff + 1);
    return idx >= 0 && idx < static_cast<int> (devices.size()) ? &devices[static_cast<size_t> (idx)] : nullptr;
}

// Loudness the master is delivered at: the integrated loudness is judged against this in the master plan. The values are
// themis::delivery::kLufs (core, the one list); the names are the plugin's words. The first entry is what the big streaming
// services normalise to. Append only: the index is saved in sessions.
struct LoudnessTarget
{
    const char* name;      // the choice text is this plus the value: "Streaming  -14 LUFS"
    const char* shortName; // used in the advice text
    float lufs;
};
inline constexpr LoudnessTarget loudnessTargets[] = { { "Streaming", "streaming", themis::delivery::kLufs[0] },
                                                      { "Loud release", "loud release", themis::delivery::kLufs[1] },
                                                      { "Dynamic", "dynamic", themis::delivery::kLufs[2] },
                                                      { "Quiet streaming", "quiet streaming", themis::delivery::kLufs[3] },
                                                      { "Broadcast", "broadcast", themis::delivery::kLufs[4] } };
inline constexpr int kNumLoudnessTargets = static_cast<int> (sizeof (loudnessTargets) / sizeof (loudnessTargets[0]));
static_assert (kNumLoudnessTargets == themis::delivery::kCount, "one name per delivery target in the core list");
inline constexpr int kDefaultLoudnessTarget = 0;

inline juce::StringArray loudnessTargetNames()
{
    juce::StringArray n;
    for (const auto& t : loudnessTargets)
        n.add (juce::String (t.name) + "  " + juce::String (t.lufs, 0) + " LUFS");
    return n;
}

inline constexpr const char* target = "target";
inline constexpr const char* tolerance = "tolerance";
inline constexpr const char* avgTime = "avgTime";
inline constexpr const char* device = "device";
inline constexpr const char* loudnessTarget = "loudnessTarget";
// Added after the first release (truePeakCeiling: parameter version 2; resetOnPlay: version 3, see createLayout): a session saved before them opens with their defaults.
inline constexpr const char* truePeakCeiling = "truePeakCeiling"; // the true-peak ceiling the master is held to, dBTP
inline constexpr const char* resetOnPlay = "resetOnPlay";         // restart the measurement when the host starts playing from the beginning

// The true-peak ceiling: the usual streaming-delivery -1 dBTP unless the user sets another (the limits are in HistoryChecks.h).
inline constexpr float kMinTruePeakCeilingDb = themis::history::clip::kMinCeilingDb, kMaxTruePeakCeilingDb = 0.0f;
inline constexpr float kDefaultTruePeakCeilingDb = themis::history::clip::kTruePeakWarnDb;
// Playing "from the beginning" for resetOnPlay: the host's position is within this many seconds of the start.
inline constexpr double kStartWindowSeconds = 0.05;

juce::AudioProcessorValueTreeState::ParameterLayout createLayout();

// Lock-free reads of the raw parameter values (safe on the audio thread).
class Reader
{
public:
    explicit Reader (juce::AudioProcessorValueTreeState& apvts);

    float value (const juce::String& id) const;
    float averagingSeconds() const;
    float truePeakCeilingDb() const { return truePeakRaw->load(); }
    bool resetsOnPlay() const { return resetOnPlayRaw->load() >= 0.5f; }

private:
    juce::AudioProcessorValueTreeState& state;
    std::atomic<float>* avgTimeRaw = nullptr;
    std::atomic<float>* truePeakRaw = nullptr;
    std::atomic<float>* resetOnPlayRaw = nullptr;
};
} // namespace params
