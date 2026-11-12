// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 The Pantheon authors

#pragma once

#include "DSP/Engine.h"
#include <juce_audio_processors/juce_audio_processors.h>

// The single source of truth for parameters: ids, ranges, defaults, choice lists, and readParams().
namespace apollo
{
namespace id
{
inline const juce::String phase { "phase" }, out { "out" };
}

struct BandIds
{
    juce::String on, type, freq, gain, q, slope, target;
};

// b1_on ... b8_target. Stored in sessions and automation: never rename or remove one.
inline const BandIds& bandIds (int band)
{
    static const auto all = [] {
        std::array<BandIds, kBands> a;
        for (int i = 0; i < kBands; ++i)
        {
            const auto p = "b" + juce::String (i + 1) + "_";
            a[(size_t) i] = { p + "on", p + "type", p + "freq", p + "gain", p + "q", p + "slope", p + "target" };
        }
        return a;
    }();
    return all[(size_t) band];
}

inline const juce::StringArray typeNames { "Bell", "Low shelf", "High shelf", "High-pass", "Low-pass", "Notch" };
inline const juce::StringArray targetNames { "Stereo", "Mid", "Side" };
inline const juce::StringArray slopeNames { "12 dB/oct", "24 dB/oct", "36 dB/oct", "48 dB/oct" };
inline const juce::StringArray phaseNames { "Minimum", "Linear" };

// What each band is when a new instance opens: a high-pass and a low-pass at the ends (off), shelves, and four bells, all at 0 dB, so dragging a node does something at once.
inline BandParams defaultBand (int band)
{
    static const BandParams defaults[kBands] = {
        { false, Type::highPass, 30.0, 0.0, 0.707, 1, Target::stereo },    { true, Type::lowShelf, 100.0, 0.0, 0.707, 0, Target::stereo },
        { true, Type::bell, 250.0, 0.0, 1.0, 0, Target::stereo },          { true, Type::bell, 1000.0, 0.0, 1.0, 0, Target::stereo },
        { true, Type::bell, 3000.0, 0.0, 1.0, 0, Target::stereo },         { true, Type::bell, 6000.0, 0.0, 1.0, 0, Target::stereo },
        { true, Type::highShelf, 10000.0, 0.0, 0.707, 0, Target::stereo }, { false, Type::lowPass, 18000.0, 0.0, 0.707, 1, Target::stereo } };
    return defaults[band];
}

inline juce::AudioProcessorValueTreeState::ParameterLayout createLayout()
{
    using namespace juce;
    using Attr = AudioParameterFloatAttributes;
    std::vector<std::unique_ptr<RangedAudioParameter>> p;

    const auto db = [] (float v, int) { return (v > 0.04f ? "+" : "") + String (v, 1) + " dB"; };
    const auto hz = [] (float v, int) { return v < 1000.0f ? String (roundToInt (v)) + " Hz" : String (v / 1000.0f, v < 10000.0f ? 2 : 1) + " kHz"; };
    const auto hzValue = [] (const String& s)
    {
        const auto t = s.trim();
        const float v = t.getFloatValue();
        return t.containsIgnoreCase ("k") ? v * 1000.0f : v;
    };
    NormalisableRange<float> freqRange (20.0f, 20000.0f, 0.1f), qRange (0.1f, 10.0f, 0.01f);
    freqRange.setSkewForCentre (1000.0f);
    qRange.setSkewForCentre (1.0f);

    for (int i = 0; i < kBands; ++i)
    {
        const auto& ids = bandIds (i);
        const auto d = defaultBand (i);
        p.push_back (std::make_unique<AudioParameterBool> (ParameterID { ids.on, 1 }, "Band " + String (i + 1) + " On", d.on));
        p.push_back (std::make_unique<AudioParameterChoice> (ParameterID { ids.type, 1 }, "Band " + String (i + 1) + " Type", typeNames, (int) d.type));
        p.push_back (std::make_unique<AudioParameterFloat> (ParameterID { ids.freq, 1 }, "Band " + String (i + 1) + " Frequency", freqRange, (float) d.freqHz,
                                                             Attr().withStringFromValueFunction (hz).withValueFromStringFunction (hzValue)));
        p.push_back (std::make_unique<AudioParameterFloat> (ParameterID { ids.gain, 1 }, "Band " + String (i + 1) + " Gain", NormalisableRange<float> ((float) -kMaxGainDb, (float) kMaxGainDb, 0.1f),
                                                             (float) d.gainDb, Attr().withStringFromValueFunction (db)));
        p.push_back (std::make_unique<AudioParameterFloat> (ParameterID { ids.q, 1 }, "Band " + String (i + 1) + " Q", qRange, (float) d.q,
                                                             Attr().withStringFromValueFunction ([] (float v, int) { return String (v, 2); })));
        p.push_back (std::make_unique<AudioParameterChoice> (ParameterID { ids.slope, 1 }, "Band " + String (i + 1) + " Slope", slopeNames, d.slope));
        p.push_back (std::make_unique<AudioParameterChoice> (ParameterID { ids.target, 1 }, "Band " + String (i + 1) + " Channels", targetNames, (int) d.target));
    }
    p.push_back (std::make_unique<AudioParameterChoice> (ParameterID { id::phase, 1 }, "Phase", phaseNames, 0));
    p.push_back (std::make_unique<AudioParameterFloat> (ParameterID { id::out, 1 }, "Output", NormalisableRange<float> (-24.0f, 24.0f, 0.1f), 0.0f, Attr().withStringFromValueFunction (db)));
    return { p.begin(), p.end() };
}

// Reads the current values into the plain struct the engine consumes. Called per block on the audio thread.
inline EngineParams readParams (juce::AudioProcessorValueTreeState& s)
{
    const auto v = [&] (const juce::String& pid) { return s.getRawParameterValue (pid)->load(); };
    EngineParams e;
    for (int i = 0; i < kBands; ++i)
    {
        const auto& ids = bandIds (i);
        auto& b = e.bands[(size_t) i];
        b.on = v (ids.on) > 0.5f;
        b.type = (Type) std::clamp ((int) v (ids.type), 0, kNumTypes - 1);
        b.freqHz = v (ids.freq);
        b.gainDb = v (ids.gain);
        b.q = v (ids.q);
        b.slope = std::clamp ((int) v (ids.slope), 0, kNumSlopes - 1);
        b.target = (Target) std::clamp ((int) v (ids.target), 0, kNumTargets - 1);
    }
    e.phase = (Phase) std::clamp ((int) v (id::phase), 0, kNumPhases - 1);
    e.outDb = v (id::out);
    return e;
}

// For the editor and the checks: set a parameter from its real value.
inline void setValue (juce::AudioProcessorValueTreeState& s, const juce::String& pid, float value)
{
    auto* p = s.getParameter (pid);
    p->setValueNotifyingHost (p->convertTo0to1 (value));
}
} // namespace apollo
