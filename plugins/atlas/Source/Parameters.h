// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 The Pantheon authors

#pragma once

#include "DSP/Engine.h"
#include <juce_audio_processors/juce_audio_processors.h>

// The single source of truth for parameters: ids, ranges, defaults, choice lists, and readParams().
namespace atlas
{
namespace id
{
inline const juce::String phase { "phase" }, oversampling { "os" }, out { "out" };
inline const std::array<juce::String, kCrossovers> crossover { "xo0", "xo1", "xo2", "xo3", "xo4" };
}

// Ids are "b1_on" ... "b6_match": built once and kept, so the audio thread never builds a String.
struct BandIds
{
    juce::String on, channels, threshold, ratio, attack, release, makeup, match;
};

inline const BandIds& bandIds (int band)
{
    static const auto table = []
    {
        std::array<BandIds, kBands> t;
        for (int i = 0; i < kBands; ++i)
        {
            const auto name = [i] (const char* what) { return "b" + juce::String (i + 1) + "_" + what; };
            t[(size_t) i] = { name ("on"), name ("ch"), name ("thresh"), name ("ratio"), name ("attack"), name ("release"), name ("makeup"), name ("match") };
        }
        return t;
    }();
    return table[(size_t) band];
}

inline const juce::StringArray channelNames { "Stereo", "L/R", "M/S", "Left", "Right", "Mid", "Side" }; // the order of Channels
inline const juce::StringArray phaseNames { "Zero Latency", "Linear" };
inline const juce::StringArray oversamplingNames { "Off", "2x", "4x" };

inline constexpr float kMinCrossoverHz = 40.0f, kMaxCrossoverHz = 16000.0f; // the linear mode's kernels resolve a split down to here

inline juce::AudioProcessorValueTreeState::ParameterLayout createLayout()
{
    using namespace juce;
    using Attr = AudioParameterFloatAttributes;
    std::vector<std::unique_ptr<RangedAudioParameter>> p;

    const auto skewed = [] (float lo, float hi, float step, float centre)
    {
        NormalisableRange<float> r (lo, hi, step);
        r.setSkewForCentre (centre);
        return r;
    };
    const auto db = [] (float v, int) { return (v > 0.04f ? "+" : "") + String (v, 1) + " dB"; };
    const auto ms = [] (float v, int) { return v < 10.0f ? String (v, 1) + " ms" : (v < 1000.0f ? String (roundToInt (v)) + " ms" : String (v / 1000.0f, 2) + " s"); };
    const auto msValue = [] (const String& s)
    {
        const auto t = s.trim();
        const float v = t.getFloatValue();
        return t.containsIgnoreCase ("ms") || ! t.containsIgnoreCase ("s") ? v : v * 1000.0f;
    };
    const auto hz = [] (float v, int) { return v < 1000.0f ? String (roundToInt (v)) + " Hz" : String (v / 1000.0f, 2) + " kHz"; };
    const auto hzValue = [] (const String& s) { const float v = s.getFloatValue(); return s.containsIgnoreCase ("k") ? v * 1000.0f : v; };
    const auto F = [&] (const String& pid, const String& name, NormalisableRange<float> r, float def, Attr a)
    {
        p.push_back (std::make_unique<AudioParameterFloat> (ParameterID { pid, 1 }, name, r, def, a));
    };

    p.push_back (std::make_unique<AudioParameterChoice> (ParameterID { id::phase, 1 }, "Phase", phaseNames, 0));
    p.push_back (std::make_unique<AudioParameterChoice> (ParameterID { id::oversampling, 1 }, "Oversampling", oversamplingNames, 0));
    F (id::out, "Output", { -24.0f, 24.0f, 0.1f }, 0.0f, Attr().withStringFromValueFunction (db));
    for (int k = 0; k < kCrossovers; ++k)
        F (id::crossover[(size_t) k], String (pdsp::bands::kBands[(size_t) k].name) + " / " + pdsp::bands::kBands[(size_t) (k + 1)].name + " Crossover",
           skewed (kMinCrossoverHz, kMaxCrossoverHz, 1.0f, 800.0f), (float) pdsp::bands::crossoverHz (k), Attr().withStringFromValueFunction (hz).withValueFromStringFunction (hzValue));

    for (int i = 0; i < kBands; ++i)
    {
        const auto& ids = bandIds (i);
        const auto label = [i] (const char* what) { return String (pdsp::bands::kBands[(size_t) i].name) + " " + what; };
        const BandParams d;
        p.push_back (std::make_unique<AudioParameterBool> (ParameterID { ids.on, 1 }, label ("On"), d.on));
        p.push_back (std::make_unique<AudioParameterChoice> (ParameterID { ids.channels, 1 }, label ("Channels"), channelNames, 0));
        F (ids.threshold, label ("Threshold"), { -60.0f, 0.0f, 0.1f }, (float) d.thresholdDb, Attr().withStringFromValueFunction ([] (float v, int) { return String (v, 1) + " dB"; }));
        F (ids.ratio, label ("Ratio"), skewed (1.0f, 20.0f, 0.1f, 4.0f), (float) d.ratio, Attr().withStringFromValueFunction ([] (float v, int) { return String (v, 1) + ":1"; }));
        F (ids.attack, label ("Attack"), skewed (0.1f, 200.0f, 0.1f, 10.0f), (float) d.attackMs, Attr().withStringFromValueFunction (ms).withValueFromStringFunction (msValue));
        F (ids.release, label ("Release"), skewed (5.0f, 2000.0f, 1.0f, 120.0f), (float) d.releaseMs, Attr().withStringFromValueFunction (ms).withValueFromStringFunction (msValue));
        F (ids.makeup, label ("Makeup"), { -12.0f, 24.0f, 0.1f }, 0.0f, Attr().withStringFromValueFunction (db));
        p.push_back (std::make_unique<AudioParameterBool> (ParameterID { ids.match, 1 }, label ("Gain Match"), d.match));
    }
    return { p.begin(), p.end() };
}

// Reads the current values into the plain struct the engine consumes. Called per block on the audio thread.
inline EngineParams readParams (juce::AudioProcessorValueTreeState& s)
{
    const auto v = [&] (juce::StringRef pid) { return s.getRawParameterValue (pid)->load(); };
    EngineParams e;
    e.phase = (PhaseMode) std::clamp ((int) v (id::phase), 0, 1);
    e.oversampling = std::clamp ((int) v (id::oversampling), 0, kNumOsModes - 1);
    e.outGainDb = v (id::out);
    for (int k = 0; k < kCrossovers; ++k)
        e.crossover[(size_t) k] = v (id::crossover[(size_t) k]);
    for (int i = 0; i < kBands; ++i)
    {
        const auto& ids = bandIds (i);
        auto& b = e.bands[(size_t) i];
        b.on = v (ids.on) > 0.5f;
        b.channels = (Channels) std::clamp ((int) v (ids.channels), 0, kNumChannelModes - 1);
        b.thresholdDb = v (ids.threshold);
        b.ratio = v (ids.ratio);
        b.attackMs = v (ids.attack);
        b.releaseMs = v (ids.release);
        b.makeupDb = v (ids.makeup);
        b.match = v (ids.match) > 0.5f;
    }
    return e;
}

// The edges of band `b` as the engine has them (ordered), for the editor's labels.
inline std::pair<double, double> bandRangeHz (juce::AudioProcessorValueTreeState& s, int b)
{
    std::array<double, kCrossovers> xo {};
    for (int k = 0; k < kCrossovers; ++k)
        xo[(size_t) k] = s.getRawParameterValue (id::crossover[(size_t) k])->load();
    xo = orderedCrossovers (xo);
    return { b == 0 ? pdsp::bands::kBands[0].loHz : xo[(size_t) (b - 1)], b == kBands - 1 ? pdsp::bands::kBands[(size_t) (kBands - 1)].hiHz : xo[(size_t) b] };
}
} // namespace atlas
