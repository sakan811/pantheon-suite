// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 The Pantheon authors

#pragma once

#include "DSP/Engine.h"
#include <juce_audio_processors/juce_audio_processors.h>

// The single source of truth for parameters: ids, ranges, defaults, choice lists, and readParams().
namespace hermes
{
namespace id
{
inline const juce::String trim { "trim" }, polarity { "polarity" }, listen { "listen" }, reference { "ref" }, levelMatch { "match" }, referenceTrim { "ref_trim" }, resetOnPlay { "reset_play" };
}

inline const juce::StringArray listenNames { "Stereo", "Mono", "Left", "Right", "Side", "Swap" };

inline juce::AudioProcessorValueTreeState::ParameterLayout createLayout()
{
    using namespace juce;
    using Attr = AudioParameterFloatAttributes;
    std::vector<std::unique_ptr<RangedAudioParameter>> p;
    const EngineParams d;
    const auto db = [] (float v, int) { return (v > 0.04f ? "+" : "") + String (v, 1) + " dB"; };

    p.push_back (std::make_unique<AudioParameterFloat> (ParameterID { id::trim, 1 }, "Trim", NormalisableRange<float> (-kMaxTrimDb, kMaxTrimDb, 0.1f), d.trimDb, Attr().withStringFromValueFunction (db)));
    p.push_back (std::make_unique<AudioParameterBool> (ParameterID { id::polarity, 1 }, "Polarity", d.polarity));
    p.push_back (std::make_unique<AudioParameterChoice> (ParameterID { id::listen, 1 }, "Listen", listenNames, (int) d.listen));
    p.push_back (std::make_unique<AudioParameterBool> (ParameterID { id::reference, 1 }, "Reference", d.reference));
    p.push_back (std::make_unique<AudioParameterBool> (ParameterID { id::levelMatch, 1 }, "Level Match", d.levelMatch));
    p.push_back (std::make_unique<AudioParameterFloat> (ParameterID { id::referenceTrim, 1 }, "Reference Trim", NormalisableRange<float> (-12.0f, 12.0f, 0.1f), d.referenceTrimDb, Attr().withStringFromValueFunction (db)));
    p.push_back (std::make_unique<AudioParameterBool> (ParameterID { id::resetOnPlay, 2 }, "Auto Reset", d.resetOnPlay));
    return { p.begin(), p.end() };
}

// Reads the current values into the plain struct the engine consumes. Called per block on the audio thread.
inline EngineParams readParams (juce::AudioProcessorValueTreeState& s)
{
    const auto v = [&] (juce::StringRef pid) { return s.getRawParameterValue (pid)->load(); };
    EngineParams e;
    e.trimDb = v (id::trim);
    e.polarity = v (id::polarity) > 0.5f;
    e.listen = (Listen) std::clamp ((int) v (id::listen), 0, kNumListen - 1);
    e.reference = v (id::reference) > 0.5f;
    e.levelMatch = v (id::levelMatch) > 0.5f;
    e.referenceTrimDb = v (id::referenceTrim);
    e.resetOnPlay = v (id::resetOnPlay) > 0.5f;
    return e;
}

// For the editor and the checks: set a parameter from its real value.
inline void setValue (juce::AudioProcessorValueTreeState& s, const juce::String& pid, float value)
{
    auto* p = s.getParameter (pid);
    p->setValueNotifyingHost (p->convertTo0to1 (value));
}
} // namespace hermes
