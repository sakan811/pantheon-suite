// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 The Pantheon authors

#pragma once

#include "DSP/Engine.h"
#include <juce_audio_processors/juce_audio_processors.h>

// The single source of truth for parameters: ids, ranges, defaults, choice lists, and readParams().
namespace nemesis
{
namespace id
{
inline const juce::String freq { "freq" }, q { "q" }, threshold { "thresh" }, range { "range" }, release { "release" }, mode { "mode" }, detection { "detect" }, listen { "listen" }, lookahead { "lookahead" };
}

inline const juce::StringArray modeNames { "Split-band", "Wideband" };
inline const juce::StringArray detectionNames { "Absolute", "Relative" };
inline const juce::StringArray lookaheadNames { "Off", "0.5 ms", "1 ms", "2 ms", "5 ms" }; // kLookaheadMs, in order

inline juce::AudioProcessorValueTreeState::ParameterLayout createLayout()
{
    using namespace juce;
    using Attr = AudioParameterFloatAttributes;
    std::vector<std::unique_ptr<RangedAudioParameter>> p;
    const EngineParams d;

    const auto hz = [] (float v, int) { return v < 1000.0f ? String (roundToInt (v)) + " Hz" : String (v / 1000.0f, 2) + " kHz"; };
    const auto hzValue = [] (const String& s)
    {
        const auto t = s.trim();
        const float v = t.getFloatValue();
        return t.containsIgnoreCase ("k") ? v * 1000.0f : v;
    };
    const auto ms = [] (float v, int) { return String (roundToInt (v)) + " ms"; };
    NormalisableRange<float> freqRange (2000.0f, 12000.0f, 10.0f), qRange (0.5f, 5.0f, 0.01f);
    freqRange.setSkewForCentre (6000.0f);
    qRange.setSkewForCentre (1.5f);

    p.push_back (std::make_unique<AudioParameterFloat> (ParameterID { id::freq, 1 }, "Frequency", freqRange, d.freqHz, Attr().withStringFromValueFunction (hz).withValueFromStringFunction (hzValue)));
    p.push_back (std::make_unique<AudioParameterFloat> (ParameterID { id::q, 1 }, "Width", qRange, d.q, Attr().withStringFromValueFunction ([] (float v, int) { return "Q " + String (v, 2); })));
    p.push_back (std::make_unique<AudioParameterFloat> (ParameterID { id::threshold, 1 }, "Threshold", NormalisableRange<float> (-60.0f, 0.0f, 0.1f), d.thresholdDb,
                                                         Attr().withStringFromValueFunction ([] (float v, int) { return String (v, 1) + " dB"; })));
    p.push_back (std::make_unique<AudioParameterFloat> (ParameterID { id::range, 1 }, "Range", NormalisableRange<float> (0.0f, 24.0f, 0.1f), d.rangeDb,
                                                         Attr().withStringFromValueFunction ([] (float v, int) { return String (v, 1) + " dB"; })));
    p.push_back (std::make_unique<AudioParameterFloat> (ParameterID { id::release, 1 }, "Release", NormalisableRange<float> (5.0f, 300.0f, 1.0f), d.releaseMs, Attr().withStringFromValueFunction (ms)));
    p.push_back (std::make_unique<AudioParameterChoice> (ParameterID { id::mode, 1 }, "Mode", modeNames, (int) d.mode));
    p.push_back (std::make_unique<AudioParameterChoice> (ParameterID { id::detection, 1 }, "Detection", detectionNames, (int) d.detection));
    p.push_back (std::make_unique<AudioParameterBool> (ParameterID { id::listen, 1 }, "Listen", d.listen));
    // Added after the first release: version 2, so a session saved before it has no value and gets the default (Off, the plug-in as it was).
    p.push_back (std::make_unique<AudioParameterChoice> (ParameterID { id::lookahead, 2 }, "Look-ahead", lookaheadNames, d.lookahead));
    return { p.begin(), p.end() };
}

// Reads the current values into the plain struct the engine consumes. Called per block on the audio thread.
inline EngineParams readParams (juce::AudioProcessorValueTreeState& s)
{
    const auto v = [&] (juce::StringRef pid) { return s.getRawParameterValue (pid)->load(); };
    EngineParams e;
    e.freqHz = v (id::freq);
    e.q = v (id::q);
    e.thresholdDb = v (id::threshold);
    e.rangeDb = v (id::range);
    e.releaseMs = v (id::release);
    e.mode = (Mode) std::clamp ((int) v (id::mode), 0, kNumModes - 1);
    e.detection = (Detection) std::clamp ((int) v (id::detection), 0, kNumDetections - 1);
    e.listen = v (id::listen) > 0.5f;
    e.lookahead = std::clamp ((int) v (id::lookahead), 0, kNumLookaheads - 1);
    return e;
}

// For the editor and the checks: set a parameter from its real value.
inline void setValue (juce::AudioProcessorValueTreeState& s, const juce::String& pid, float value)
{
    auto* p = s.getParameter (pid);
    p->setValueNotifyingHost (p->convertTo0to1 (value));
}
} // namespace nemesis
