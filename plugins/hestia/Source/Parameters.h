// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 The Pantheon authors

#pragma once

#include "DSP/Engine.h"
#include <juce_audio_processors/juce_audio_processors.h>

// The single source of truth for parameters: ids, ranges, defaults, choice lists, and readParams().
namespace hestia
{
namespace id
{
inline const juce::String role { "role" }, mix { "mix" }, importance { "importance" }, depth { "depth" }, reach { "reach" },
                          attack { "attack" }, release { "release" }, out { "out" };
}

inline const juce::StringArray mixNames { "Off", "A", "B", "C", "D", "E", "F", "G", "H" }; // index = the group of the bus; 0 is off
inline constexpr int kDefaultMix = 1;
inline constexpr float kMaxDepthDb = 18.0f;

inline juce::StringArray roleNames()
{
    juce::StringArray names;
    for (int r = 0; r < kNumRoles; ++r)
        names.add (roleName ((Role) r));
    return names;
}

inline juce::AudioProcessorValueTreeState::ParameterLayout createLayout()
{
    using namespace juce;
    using Attr = AudioParameterFloatAttributes;
    std::vector<std::unique_ptr<RangedAudioParameter>> p;

    const auto db = [] (float v, int) { return (v > 0.04f ? "+" : "") + String (v, 1) + " dB"; };
    const auto ms = [] (float v, int) { return v < 10.0f ? String (v, 1) + " ms" : (v < 1000.0f ? String (roundToInt (v)) + " ms" : String (v / 1000.0f, 2) + " s"); };
    const auto msValue = [] (const String& s)
    {
        const auto t = s.trim();
        const float v = t.getFloatValue();
        return t.containsIgnoreCase ("ms") || ! t.containsIgnoreCase ("s") ? v : v * 1000.0f;
    };
    const auto skewed = [] (float lo, float hi, float step, float centre)
    {
        NormalisableRange<float> r (lo, hi, step);
        r.setSkewForCentre (centre);
        return r;
    };
    const auto F = [&] (const String& pid, const String& name, NormalisableRange<float> r, float def, Attr a)
    {
        p.push_back (std::make_unique<AudioParameterFloat> (ParameterID { pid, 1 }, name, r, def, a));
    };
    const EngineParams d;

    p.push_back (std::make_unique<AudioParameterChoice> (ParameterID { id::role, 1 }, "Role", roleNames(), (int) d.role));
    p.push_back (std::make_unique<AudioParameterChoice> (ParameterID { id::mix, 1 }, "Mix", mixNames, kDefaultMix));
    F (id::importance, "Importance", { 0.0f, 100.0f, 1.0f }, d.importance * 100.0f, Attr().withStringFromValueFunction ([] (float v, int) { return String (roundToInt (v)) + " %"; }));
    F (id::depth, "Depth", { 0.0f, kMaxDepthDb, 0.1f }, d.depthDb, Attr().withStringFromValueFunction ([] (float v, int) { return String (v, 1) + " dB"; }));
    F (id::reach, "Reach", { 0.0f, 36.0f, 0.5f }, d.reachDb, Attr().withStringFromValueFunction ([] (float v, int) { return String (v, 1) + " dB"; }));
    F (id::attack, "Attack", skewed (1.0f, 500.0f, 1.0f, 40.0f), d.attackMs, Attr().withStringFromValueFunction (ms).withValueFromStringFunction (msValue));
    F (id::release, "Release", skewed (20.0f, 3000.0f, 1.0f, 300.0f), d.releaseMs, Attr().withStringFromValueFunction (ms).withValueFromStringFunction (msValue));
    F (id::out, "Output", { -24.0f, 24.0f, 0.1f }, d.outDb, Attr().withStringFromValueFunction (db));
    return { p.begin(), p.end() };
}

// Reads the current values into the plain struct the engine consumes. Called per block on the audio thread.
inline EngineParams readParams (juce::AudioProcessorValueTreeState& s)
{
    const auto v = [&] (juce::StringRef pid) { return s.getRawParameterValue (pid)->load(); };
    EngineParams e;
    e.role = (Role) std::clamp ((int) v (id::role), 0, kNumRoles - 1);
    e.group = std::clamp ((int) v (id::mix), 0, kMaxGroups);
    e.importance = v (id::importance) / 100.0f;
    e.depthDb = v (id::depth);
    e.reachDb = v (id::reach);
    e.attackMs = v (id::attack);
    e.releaseMs = v (id::release);
    e.outDb = v (id::out);
    return e;
}

// For the editor and the checks: set a parameter from its real value.
inline void setValue (juce::AudioProcessorValueTreeState& s, const juce::String& pid, float value)
{
    auto* p = s.getParameter (pid);
    p->setValueNotifyingHost (p->convertTo0to1 (value));
}
} // namespace hestia
