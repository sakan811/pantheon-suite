// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 The Pantheon authors

#pragma once

#include "DSP/Engine.h"
#include <juce_audio_processors/juce_audio_processors.h>

// The single source of truth for parameters: ids, ranges, defaults, choice lists, and readParams().
namespace aegis
{
namespace id
{
inline const juce::String gain { "gain" }, ceiling { "ceiling" }, release { "release" }, lookahead { "look" }, truePeak { "tp" };
}

inline juce::StringArray lookaheadNames()
{
    juce::StringArray names;
    for (double ms : kLookaheadMs)
        names.add (juce::String (ms, 1) + " ms");
    return names;
}

inline juce::AudioProcessorValueTreeState::ParameterLayout createLayout()
{
    using namespace juce;
    using Attr = AudioParameterFloatAttributes;
    std::vector<std::unique_ptr<RangedAudioParameter>> p;
    const EngineParams d;

    const auto db = [] (float v, int) { return (v > 0.04f ? "+" : "") + String (v, 1) + " dB"; };
    const auto ms = [] (float v, int) { return v < 100.0f ? String (v, 1) + " ms" : (v < 1000.0f ? String (roundToInt (v)) + " ms" : String (v / 1000.0f, 2) + " s"); };
    const auto msValue = [] (const String& s)
    {
        const auto t = s.trim();
        const float v = t.getFloatValue();
        return t.containsIgnoreCase ("ms") || ! t.containsIgnoreCase ("s") ? v : v * 1000.0f;
    };
    NormalisableRange<float> releaseRange (10.0f, 1000.0f, 1.0f);
    releaseRange.setSkewForCentre (100.0f);

    p.push_back (std::make_unique<AudioParameterFloat> (ParameterID { id::gain, 1 }, "Gain", NormalisableRange<float> (0.0f, kMaxGainDb, 0.1f), d.gainDb, Attr().withStringFromValueFunction (db)));
    p.push_back (std::make_unique<AudioParameterFloat> (ParameterID { id::ceiling, 1 }, "Ceiling", NormalisableRange<float> (-12.0f, 0.0f, 0.1f), d.ceilingDb,
                                                         Attr().withStringFromValueFunction ([] (float v, int) { return String (v, 1) + " dBTP"; })));
    p.push_back (std::make_unique<AudioParameterFloat> (ParameterID { id::release, 1 }, "Release", releaseRange, d.releaseMs, Attr().withStringFromValueFunction (ms).withValueFromStringFunction (msValue)));
    p.push_back (std::make_unique<AudioParameterChoice> (ParameterID { id::lookahead, 1 }, "Look-ahead", lookaheadNames(), d.lookahead));
    p.push_back (std::make_unique<AudioParameterBool> (ParameterID { id::truePeak, 1 }, "True Peak", d.truePeak));
    return { p.begin(), p.end() };
}

// Reads the current values into the plain struct the engine consumes. Called per block on the audio thread.
inline EngineParams readParams (juce::AudioProcessorValueTreeState& s)
{
    const auto v = [&] (juce::StringRef pid) { return s.getRawParameterValue (pid)->load(); };
    EngineParams e;
    e.gainDb = v (id::gain);
    e.ceilingDb = v (id::ceiling);
    e.releaseMs = v (id::release);
    e.lookahead = std::clamp ((int) v (id::lookahead), 0, kNumLookahead - 1);
    e.truePeak = v (id::truePeak) > 0.5f;
    return e;
}

// For the editor and the checks: set a parameter from its real value.
inline void setValue (juce::AudioProcessorValueTreeState& s, const juce::String& pid, float value)
{
    auto* p = s.getParameter (pid);
    p->setValueNotifyingHost (p->convertTo0to1 (value));
}
} // namespace aegis
