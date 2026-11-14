// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 The Pantheon authors

#pragma once

#include "DSP/Engine.h"
#include <juce_audio_processors/juce_audio_processors.h>

// The single source of truth for parameters: ids, ranges, defaults, choice lists, and readParams().
namespace dionysus
{
namespace id
{
inline const juce::String drive { "drive" }, shape { "shape" }, bias { "bias" }, tone { "tone" }, mix { "mix" }, out { "out" }, autoGain { "autogain" }, oversampling { "os" }, lowCut { "lowcut" };
}

inline const juce::StringArray shapeNames { "Tape", "Tube", "Transformer", "Diode", "Fold", "Clip" };
inline const juce::StringArray oversamplingNames { "Off", "2x", "4x", "8x" };

inline juce::AudioProcessorValueTreeState::ParameterLayout createLayout()
{
    using namespace juce;
    using Attr = AudioParameterFloatAttributes;
    std::vector<std::unique_ptr<RangedAudioParameter>> p;
    const EngineParams d;

    const auto db = [] (float v, int) { return (v > 0.04f ? "+" : "") + String (v, 1) + " dB"; };
    const auto percent = [] (float v, int) { return String (roundToInt (v)) + " %"; };
    const auto signedPercent = [] (float v, int) { return (v > 0.5f ? "+" : "") + String (roundToInt (v)) + " %"; };

    NormalisableRange<float> lowCutRange (20.0f, 500.0f, 1.0f);
    lowCutRange.setSkewForCentre (100.0f);

    p.push_back (std::make_unique<AudioParameterFloat> (ParameterID { id::drive, 1 }, "Drive", NormalisableRange<float> (0.0f, kMaxDriveDb, 0.1f), d.driveDb, Attr().withStringFromValueFunction (db)));
    p.push_back (std::make_unique<AudioParameterChoice> (ParameterID { id::shape, 1 }, "Curve", shapeNames, (int) d.shape));
    p.push_back (std::make_unique<AudioParameterFloat> (ParameterID { id::bias, 1 }, "Bias", NormalisableRange<float> (-100.0f, 100.0f, 1.0f), d.bias * 100.0f, Attr().withStringFromValueFunction (signedPercent)));
    p.push_back (std::make_unique<AudioParameterFloat> (ParameterID { id::tone, 1 }, "Tone", NormalisableRange<float> (-100.0f, 100.0f, 1.0f), d.tone * 100.0f, Attr().withStringFromValueFunction (signedPercent)));
    p.push_back (std::make_unique<AudioParameterFloat> (ParameterID { id::mix, 1 }, "Mix", NormalisableRange<float> (0.0f, 100.0f, 1.0f), d.mix * 100.0f, Attr().withStringFromValueFunction (percent)));
    p.push_back (std::make_unique<AudioParameterFloat> (ParameterID { id::out, 1 }, "Output", NormalisableRange<float> (-24.0f, 12.0f, 0.1f), d.outDb, Attr().withStringFromValueFunction (db)));
    p.push_back (std::make_unique<AudioParameterBool> (ParameterID { id::autoGain, 1 }, "Auto Gain", d.autoGain));
    p.push_back (std::make_unique<AudioParameterChoice> (ParameterID { id::oversampling, 1 }, "Oversampling", oversamplingNames, d.oversampling));
    // Version 2: a session saved before it has no value and gets the default (Off, the plug-in as it was).
    p.push_back (std::make_unique<AudioParameterFloat> (ParameterID { id::lowCut, 2 }, "Low Cut", lowCutRange, d.lowCutHz,
                                                         Attr().withStringFromValueFunction ([] (float v, int) { return v <= 20.5f ? String ("Off") : String (roundToInt (v)) + " Hz"; })));
    return { p.begin(), p.end() };
}

// Reads the current values into the plain struct the engine consumes. Called per block on the audio thread.
inline EngineParams readParams (juce::AudioProcessorValueTreeState& s)
{
    const auto v = [&] (juce::StringRef pid) { return s.getRawParameterValue (pid)->load(); };
    EngineParams e;
    e.driveDb = v (id::drive);
    e.shape = (Shape) std::clamp ((int) v (id::shape), 0, kNumShapes - 1);
    e.bias = v (id::bias) / 100.0f;
    e.tone = v (id::tone) / 100.0f;
    e.mix = v (id::mix) / 100.0f;
    e.outDb = v (id::out);
    e.autoGain = v (id::autoGain) > 0.5f;
    e.oversampling = std::clamp ((int) v (id::oversampling), 0, kNumOversampling - 1);
    e.lowCutHz = v (id::lowCut);
    return e;
}

// For the editor and the checks: set a parameter from its real value.
inline void setValue (juce::AudioProcessorValueTreeState& s, const juce::String& pid, float value)
{
    auto* p = s.getParameter (pid);
    p->setValueNotifyingHost (p->convertTo0to1 (value));
}
} // namespace dionysus
