// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 The Pantheon authors

#pragma once

#include "DSP/Engine.h"
#include <juce_audio_processors/juce_audio_processors.h>
#include <pantheon/dsp/BandLink.h>

// The single source of truth for parameters: ids, ranges, defaults, choice lists, and readParams().
namespace aegis
{
namespace id
{
inline const juce::String gain { "gain" }, ceiling { "ceiling" }, release { "release" }, lookahead { "look" }, truePeak { "tp" },
    autoRelease { "autorel" }, // version 2: a session saved before it opens with it off, the old behaviour
    delta { "delta" }, matchLevel { "match" }; // version 2 too: listening aids, off
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
    p.push_back (std::make_unique<AudioParameterBool> (ParameterID { id::autoRelease, 2 }, "Auto Release", d.autoRelease));
    p.push_back (std::make_unique<AudioParameterBool> (ParameterID { id::delta, 2 }, "Delta", d.delta));
    p.push_back (std::make_unique<AudioParameterBool> (ParameterID { id::matchLevel, 2 }, "Match Level", d.matchLevel));
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
    e.autoRelease = v (id::autoRelease) > 0.5f;
    e.delta = v (id::delta) > 0.5f;
    e.matchLevel = v (id::matchLevel) > 0.5f;
    return e;
}

// For the editor and the checks: set a parameter from its real value.
inline void setValue (juce::AudioProcessorValueTreeState& s, const juce::String& pid, float value)
{
    auto* p = s.getParameter (pid);
    p->setValueNotifyingHost (p->convertTo0to1 (value));
}

// A limiter setting another plugin of the repository handed over as text (pdsp::link, Themis' COPY on a plan item): the ceiling to set and how far to
// move GAIN. Returns the line the editor says about it, empty when the text holds no limiter setting (nothing is touched then). A ceiling in dBTP is only
// true with TRUE PEAK on, so a paste that names one switches it on. GAIN stops at its ends (and says so): the plan asked for more than the range has.
inline std::string pasteLimiter (juce::AudioProcessorValueTreeState& s, const juce::String& text)
{
    pdsp::LinkLimiter l;
    if (! pdsp::link::decodeLimiter (text.toStdString(), l))
        return {};
    std::string said = "Pasted from the plan:";
    if (l.hasCeiling)
    {
        if (s.getRawParameterValue (id::truePeak)->load() < 0.5f)
            setValue (s, id::truePeak, 1.0f);
        setValue (s, id::ceiling, std::clamp ((float) l.ceilingDb, -12.0f, 0.0f));
        said += " CEILING " + juce::String (s.getRawParameterValue (id::ceiling)->load(), 1).toStdString() + " dBTP";
    }
    if (l.hasGainChange)
    {
        const float before = s.getRawParameterValue (id::gain)->load(), wanted = before + (float) l.gainChangeDb, now = std::clamp (wanted, 0.0f, kMaxGainDb);
        setValue (s, id::gain, now);
        said += std::string (l.hasCeiling ? "," : "") + " GAIN " + juce::String (before, 1).toStdString() + " to " + juce::String (s.getRawParameterValue (id::gain)->load(), 1).toStdString() + " dB";
        if (std::fabs (wanted - now) > 0.05f)
            said += " (the end of its range)";
    }
    return said + ".";
}
} // namespace aegis
