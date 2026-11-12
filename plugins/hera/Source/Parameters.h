// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 The Pantheon authors

#pragma once

#include "DSP/Engine.h"
#include <juce_audio_processors/juce_audio_processors.h>

// The single source of truth for parameters: ids, ranges, defaults, choice lists, and readParams().
namespace hera
{
namespace id
{
inline const juce::String threshold { "thresh" }, ratio { "ratio" }, attack { "attack" }, release { "release" }, knee { "knee" }, makeup { "makeup" }, mix { "mix" },
                          link { "link" }, hpf { "hpf" }, detector { "detector" }, autoRelease { "auto_rel" }, autoMakeup { "auto_mk" }, external { "key" };
}

inline const juce::StringArray detectorNames { "Peak", "RMS" };

inline juce::AudioProcessorValueTreeState::ParameterLayout createLayout()
{
    using namespace juce;
    using Attr = AudioParameterFloatAttributes;
    std::vector<std::unique_ptr<RangedAudioParameter>> p;
    const EngineParams d;

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
    const auto percent = [] (float v, int) { return String (roundToInt (v)) + " %"; };

    F (id::threshold, "Threshold", { -60.0f, 0.0f, 0.1f }, d.thresholdDb, Attr().withStringFromValueFunction ([] (float v, int) { return String (v, 1) + " dB"; }));
    F (id::ratio, "Ratio", skewed (1.0f, 20.0f, 0.1f, 4.0f), d.ratio, Attr().withStringFromValueFunction ([] (float v, int) { return String (v, 1) + ":1"; }));
    F (id::attack, "Attack", skewed (0.1f, 200.0f, 0.1f, 10.0f), d.attackMs, Attr().withStringFromValueFunction (ms).withValueFromStringFunction (msValue));
    F (id::release, "Release", skewed (10.0f, 2000.0f, 1.0f, 200.0f), d.releaseMs, Attr().withStringFromValueFunction (ms).withValueFromStringFunction (msValue));
    F (id::knee, "Knee", { 0.0f, 24.0f, 0.1f }, d.kneeDb, Attr().withStringFromValueFunction ([] (float v, int) { return String (v, 1) + " dB"; }));
    F (id::makeup, "Makeup", { 0.0f, kMaxMakeupDb, 0.1f }, d.makeupDb, Attr().withStringFromValueFunction (db));
    F (id::mix, "Mix", { 0.0f, 100.0f, 1.0f }, d.mix * 100.0f, Attr().withStringFromValueFunction (percent));
    F (id::link, "Link", { 0.0f, 100.0f, 1.0f }, d.link * 100.0f, Attr().withStringFromValueFunction (percent));
    F (id::hpf, "Sidechain High-pass", skewed (20.0f, 500.0f, 1.0f, 100.0f), d.hpfHz,
       Attr().withStringFromValueFunction ([] (float v, int) { return v <= 20.5f ? String ("Off") : String (roundToInt (v)) + " Hz"; }));
    p.push_back (std::make_unique<AudioParameterChoice> (ParameterID { id::detector, 1 }, "Detector", detectorNames, (int) d.detector));
    p.push_back (std::make_unique<AudioParameterBool> (ParameterID { id::autoRelease, 1 }, "Auto Release", d.autoRelease));
    p.push_back (std::make_unique<AudioParameterBool> (ParameterID { id::autoMakeup, 1 }, "Auto Makeup", d.autoMakeup));
    p.push_back (std::make_unique<AudioParameterBool> (ParameterID { id::external, 1 }, "Sidechain Key", d.external));
    return { p.begin(), p.end() };
}

// Reads the current values into the plain struct the engine consumes. Called per block on the audio thread.
inline EngineParams readParams (juce::AudioProcessorValueTreeState& s)
{
    const auto v = [&] (juce::StringRef pid) { return s.getRawParameterValue (pid)->load(); };
    EngineParams e;
    e.thresholdDb = v (id::threshold);
    e.ratio = v (id::ratio);
    e.attackMs = v (id::attack);
    e.releaseMs = v (id::release);
    e.kneeDb = v (id::knee);
    e.makeupDb = v (id::makeup);
    e.mix = v (id::mix) / 100.0f;
    e.link = v (id::link) / 100.0f;
    e.hpfHz = v (id::hpf);
    e.detector = (Detector) std::clamp ((int) v (id::detector), 0, kNumDetectors - 1);
    e.autoRelease = v (id::autoRelease) > 0.5f;
    e.autoMakeup = v (id::autoMakeup) > 0.5f;
    e.external = v (id::external) > 0.5f;
    return e;
}

// For the editor and the checks: set a parameter from its real value.
inline void setValue (juce::AudioProcessorValueTreeState& s, const juce::String& pid, float value)
{
    auto* p = s.getParameter (pid);
    p->setValueNotifyingHost (p->convertTo0to1 (value));
}
} // namespace hera
