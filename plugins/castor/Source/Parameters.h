// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 The Pantheon authors

#pragma once

#include "DSP/Engine.h"
#include <juce_audio_processors/juce_audio_processors.h>

// The single source of truth for parameters: ids, ranges, defaults, and readParams().
namespace castor
{
namespace id
{
inline const juce::String mid { "mid" }, side { "side" }, monoBelow { "mono" }, balance { "balance" }, out { "out" },
    listen { "listen" }; // version 2: a check on what is heard, Stereo by default
// width1 ... width6: the sides of each of the six bands of music (pdsp::bands), in percent
inline const juce::String& width (int band)
{
    static const juce::String ids[] { "width1", "width2", "width3", "width4", "width5", "width6" }; // made once: the audio thread reads these every block and must not allocate
    return ids[(size_t) band];
}
}

inline const juce::StringArray listenNames { "Stereo", "Mono", "Side" }; // the order of Listen

inline juce::AudioProcessorValueTreeState::ParameterLayout createLayout()
{
    using namespace juce;
    using Attr = AudioParameterFloatAttributes;
    std::vector<std::unique_ptr<RangedAudioParameter>> p;
    const EngineParams d;

    const auto db = [] (float v, int) { return (v > 0.04f ? "+" : "") + String (v, 1) + " dB"; };
    const auto percent = [] (float v, int) { return String (roundToInt (v)) + " %"; };
    for (int b = 0; b < kBands; ++b)
        p.push_back (std::make_unique<AudioParameterFloat> (ParameterID { id::width (b), 1 }, String (pdsp::bands::kBands[(size_t) b].title) + " Width",
                                                             NormalisableRange<float> (0.0f, kMaxWidth * 100.0f, 1.0f), d.width[(size_t) b] * 100.0f, Attr().withStringFromValueFunction (percent)));
    p.push_back (std::make_unique<AudioParameterFloat> (ParameterID { id::mid, 1 }, "Mid", NormalisableRange<float> (-12.0f, 12.0f, 0.1f), d.midDb, Attr().withStringFromValueFunction (db)));
    p.push_back (std::make_unique<AudioParameterFloat> (ParameterID { id::side, 1 }, "Side", NormalisableRange<float> (-12.0f, 12.0f, 0.1f), d.sideDb, Attr().withStringFromValueFunction (db)));
    NormalisableRange<float> monoRange (kMonoOffHz, 500.0f, 1.0f);
    monoRange.setSkewForCentre (120.0f);
    p.push_back (std::make_unique<AudioParameterFloat> (ParameterID { id::monoBelow, 1 }, "Mono Below", monoRange, d.monoBelowHz,
                                                         Attr().withStringFromValueFunction ([] (float v, int) { return v <= kMonoOffHz + 0.5f ? String ("Off") : String (roundToInt (v)) + " Hz"; })));
    p.push_back (std::make_unique<AudioParameterFloat> (ParameterID { id::balance, 1 }, "Balance", NormalisableRange<float> (-100.0f, 100.0f, 1.0f), d.balance * 100.0f,
                                                         Attr().withStringFromValueFunction ([] (float v, int) { return v == 0.0f ? String ("C") : (v < 0.0f ? "L " : "R ") + String (roundToInt (std::fabs (v))); })));
    p.push_back (std::make_unique<AudioParameterFloat> (ParameterID { id::out, 1 }, "Output", NormalisableRange<float> (-12.0f, 12.0f, 0.1f), d.outDb, Attr().withStringFromValueFunction (db)));
    p.push_back (std::make_unique<AudioParameterChoice> (ParameterID { id::listen, 2 }, "Listen", listenNames, (int) d.listen));
    return { p.begin(), p.end() };
}

// Reads the current values into the plain struct the engine consumes. Called per block on the audio thread.
inline EngineParams readParams (juce::AudioProcessorValueTreeState& s)
{
    const auto v = [&] (const juce::String& pid) { return s.getRawParameterValue (pid)->load(); };
    EngineParams e;
    for (int b = 0; b < kBands; ++b)
        e.width[(size_t) b] = v (id::width (b)) / 100.0f;
    e.midDb = v (id::mid);
    e.sideDb = v (id::side);
    e.monoBelowHz = v (id::monoBelow);
    e.balance = v (id::balance) / 100.0f;
    e.outDb = v (id::out);
    e.listen = (Listen) std::clamp ((int) v (id::listen), 0, kNumListen - 1);
    return e;
}

// For the editor and the checks: set a parameter from its real value.
inline void setValue (juce::AudioProcessorValueTreeState& s, const juce::String& pid, float value)
{
    auto* p = s.getParameter (pid);
    p->setValueNotifyingHost (p->convertTo0to1 (value));
}
} // namespace castor
