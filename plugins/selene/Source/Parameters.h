// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 The Pantheon authors

#pragma once

#include "DSP/Engine.h"
#include <juce_audio_processors/juce_audio_processors.h>

// The single source of truth for Selene Poly parameters: ids, ranges, defaults, choice lists, and readParams().
namespace selene
{
namespace id
{
inline const juce::String
    voiceMode { "voiceMode" }, glide { "glide" }, drift { "drift" },
    osc1Wave { "osc1Wave" }, osc1Pwm { "osc1Pwm" }, subLevel { "subLevel" },
    osc2Wave { "osc2Wave" }, osc2Semi { "osc2Semi" }, osc2Detune { "osc2Detune" },
    osc2Fm { "osc2Fm" }, oscMix { "oscMix" }, noiseLevel { "noiseLevel" },
    filterMode { "filterMode" }, cutoff { "cutoff" }, resonance { "resonance" },
    filterEnvAmt { "filterEnvAmt" }, keytrack { "keytrack" }, drive { "drive" },
    ampAttack { "ampAttack" }, ampDecay { "ampDecay" }, ampSustain { "ampSustain" },
    ampRelease { "ampRelease" }, velAmp { "velAmp" },
    fltAttack { "fltAttack" }, fltDecay { "fltDecay" }, fltSustain { "fltSustain" },
    fltRelease { "fltRelease" }, velFlt { "velFlt" },
    lfoWave { "lfoWave" }, lfoRate { "lfoRate" }, lfoPitch { "lfoPitch" },
    lfoFilter { "lfoFilter" }, lfoPwm { "lfoPwm" },
    chorusMode { "chorusMode" }, echoTime { "echoTime" }, echoFeedback { "echoFeedback" },
    echoMix { "echoMix" }, masterGain { "masterGain" };
}

inline const juce::StringArray voiceModeNames { "Poly", "Mono", "Unison" };
inline const juce::StringArray oscWaveNames { "Saw", "Pulse", "Triangle", "Sine" };
inline const juce::StringArray filterModeNames { "LP 24", "LP 12", "BP 12", "HP 12" };
inline const juce::StringArray lfoWaveNames { "Sine", "Triangle", "Square", "S&H" };
inline const juce::StringArray chorusModeNames { "Off", "Mode I", "Mode II", "I + II" };

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
    const auto ms = [] (float v, int) { return v < 1000.0f ? String (roundToInt (v)) + " ms" : String (v / 1000.0f, 2) + " s"; };
    const auto msValue = [] (const String& s)
    {
        const auto t = s.trim();
        const float v = t.getFloatValue();
        return (t.containsIgnoreCase ("s") && ! t.containsIgnoreCase ("ms")) ? v * 1000.0f : v;
    };
    const auto pct = [] (float v, int) { return String (roundToInt (v * 100.0f)) + "%"; };
    const auto pctValue = [] (const String& s) { return s.trim().getFloatValue() / 100.0f; };
    const auto signedPct = [] (float v, int)
    {
        const int val = roundToInt (v * 100.0f);
        return (val > 0 ? "+" : "") + String (val) + "%";
    };
    const auto cents = [] (float v, int) { return (v > 0.05f ? "+" : "") + String (v, 1) + " ct"; };
    const auto db = [] (float v, int) { return (v > 0.05f ? "+" : "") + String (v, 1) + " dB"; };

    NormalisableRange<float> glideRange (0.0f, 500.0f, 1.0f);
    glideRange.setSkewForCentre (80.0f);

    NormalisableRange<float> cutoffRange (20.0f, 20000.0f, 1.0f);
    cutoffRange.setSkewForCentre (1000.0f);

    NormalisableRange<float> envTimeRange (1.0f, 5000.0f, 1.0f);
    envTimeRange.setSkewForCentre (250.0f);

    NormalisableRange<float> lfoRateRange (0.05f, 20.0f, 0.01f);
    lfoRateRange.setSkewForCentre (2.0f);

    NormalisableRange<float> echoTimeRange (40.0f, 1000.0f, 1.0f);
    echoTimeRange.setSkewForCentre (300.0f);

    // Voice & tuning
    p.push_back (std::make_unique<AudioParameterChoice> (ParameterID { id::voiceMode, 1 }, "Voice Mode", voiceModeNames, (int) d.voiceMode));
    p.push_back (std::make_unique<AudioParameterFloat> (ParameterID { id::glide, 1 }, "Glide", glideRange, d.glideMs, Attr().withStringFromValueFunction (ms).withValueFromStringFunction (msValue)));
    p.push_back (std::make_unique<AudioParameterFloat> (ParameterID { id::drift, 1 }, "Analog Drift", NormalisableRange<float> (0.0f, 1.0f, 0.01f), d.drift, Attr().withStringFromValueFunction (pct).withValueFromStringFunction (pctValue)));

    // Oscillators
    p.push_back (std::make_unique<AudioParameterChoice> (ParameterID { id::osc1Wave, 1 }, "Osc 1 Wave", oscWaveNames, (int) d.osc1Wave));
    p.push_back (std::make_unique<AudioParameterFloat> (ParameterID { id::osc1Pwm, 1 }, "Osc 1 PWM", NormalisableRange<float> (0.05f, 0.95f, 0.01f), d.osc1Pwm, Attr().withStringFromValueFunction (pct).withValueFromStringFunction (pctValue)));
    p.push_back (std::make_unique<AudioParameterFloat> (ParameterID { id::subLevel, 1 }, "Sub Level", NormalisableRange<float> (0.0f, 1.0f, 0.01f), d.subLevel, Attr().withStringFromValueFunction (pct).withValueFromStringFunction (pctValue)));

    p.push_back (std::make_unique<AudioParameterChoice> (ParameterID { id::osc2Wave, 1 }, "Osc 2 Wave", oscWaveNames, (int) d.osc2Wave));
    p.push_back (std::make_unique<AudioParameterInt> (ParameterID { id::osc2Semi, 1 }, "Osc 2 Semi", -24, 24, d.osc2Semi));
    p.push_back (std::make_unique<AudioParameterFloat> (ParameterID { id::osc2Detune, 1 }, "Osc 2 Detune", NormalisableRange<float> (-50.0f, 50.0f, 0.1f), d.osc2DetuneCents, Attr().withStringFromValueFunction (cents)));
    p.push_back (std::make_unique<AudioParameterFloat> (ParameterID { id::osc2Fm, 1 }, "Osc 2 FM", NormalisableRange<float> (0.0f, 1.0f, 0.01f), d.osc2Fm, Attr().withStringFromValueFunction (pct).withValueFromStringFunction (pctValue)));
    p.push_back (std::make_unique<AudioParameterFloat> (ParameterID { id::oscMix, 1 }, "Osc Mix", NormalisableRange<float> (0.0f, 1.0f, 0.01f), d.oscMix, Attr().withStringFromValueFunction (pct).withValueFromStringFunction (pctValue)));
    p.push_back (std::make_unique<AudioParameterFloat> (ParameterID { id::noiseLevel, 1 }, "Noise", NormalisableRange<float> (0.0f, 1.0f, 0.01f), d.noiseLevel, Attr().withStringFromValueFunction (pct).withValueFromStringFunction (pctValue)));

    // Filter
    p.push_back (std::make_unique<AudioParameterChoice> (ParameterID { id::filterMode, 1 }, "Filter Mode", filterModeNames, (int) d.filterMode));
    p.push_back (std::make_unique<AudioParameterFloat> (ParameterID { id::cutoff, 1 }, "Cutoff", cutoffRange, d.cutoffHz, Attr().withStringFromValueFunction (hz).withValueFromStringFunction (hzValue)));
    p.push_back (std::make_unique<AudioParameterFloat> (ParameterID { id::resonance, 1 }, "Resonance", NormalisableRange<float> (0.0f, 0.98f, 0.01f), d.resonance, Attr().withStringFromValueFunction (pct).withValueFromStringFunction (pctValue)));
    p.push_back (std::make_unique<AudioParameterFloat> (ParameterID { id::filterEnvAmt, 1 }, "Filter Env", NormalisableRange<float> (-1.0f, 1.0f, 0.01f), d.filterEnvAmt, Attr().withStringFromValueFunction (signedPct).withValueFromStringFunction (pctValue)));
    p.push_back (std::make_unique<AudioParameterFloat> (ParameterID { id::keytrack, 1 }, "Keytrack", NormalisableRange<float> (0.0f, 1.0f, 0.01f), d.keytrack, Attr().withStringFromValueFunction (pct).withValueFromStringFunction (pctValue)));
    p.push_back (std::make_unique<AudioParameterFloat> (ParameterID { id::drive, 1 }, "Drive", NormalisableRange<float> (0.0f, 1.0f, 0.01f), d.drive, Attr().withStringFromValueFunction (pct).withValueFromStringFunction (pctValue)));

    // Amp ADSR
    p.push_back (std::make_unique<AudioParameterFloat> (ParameterID { id::ampAttack, 1 }, "Amp Attack", envTimeRange, d.ampAttackMs, Attr().withStringFromValueFunction (ms).withValueFromStringFunction (msValue)));
    p.push_back (std::make_unique<AudioParameterFloat> (ParameterID { id::ampDecay, 1 }, "Amp Decay", envTimeRange, d.ampDecayMs, Attr().withStringFromValueFunction (ms).withValueFromStringFunction (msValue)));
    p.push_back (std::make_unique<AudioParameterFloat> (ParameterID { id::ampSustain, 1 }, "Amp Sustain", NormalisableRange<float> (0.0f, 1.0f, 0.01f), d.ampSustain, Attr().withStringFromValueFunction (pct).withValueFromStringFunction (pctValue)));
    p.push_back (std::make_unique<AudioParameterFloat> (ParameterID { id::ampRelease, 1 }, "Amp Release", envTimeRange, d.ampReleaseMs, Attr().withStringFromValueFunction (ms).withValueFromStringFunction (msValue)));
    p.push_back (std::make_unique<AudioParameterFloat> (ParameterID { id::velAmp, 1 }, "Vel -> Amp", NormalisableRange<float> (0.0f, 1.0f, 0.01f), d.velAmp, Attr().withStringFromValueFunction (pct).withValueFromStringFunction (pctValue)));

    // Filter ADSR
    p.push_back (std::make_unique<AudioParameterFloat> (ParameterID { id::fltAttack, 1 }, "Filter Attack", envTimeRange, d.fltAttackMs, Attr().withStringFromValueFunction (ms).withValueFromStringFunction (msValue)));
    p.push_back (std::make_unique<AudioParameterFloat> (ParameterID { id::fltDecay, 1 }, "Filter Decay", envTimeRange, d.fltDecayMs, Attr().withStringFromValueFunction (ms).withValueFromStringFunction (msValue)));
    p.push_back (std::make_unique<AudioParameterFloat> (ParameterID { id::fltSustain, 1 }, "Filter Sustain", NormalisableRange<float> (0.0f, 1.0f, 0.01f), d.fltSustain, Attr().withStringFromValueFunction (pct).withValueFromStringFunction (pctValue)));
    p.push_back (std::make_unique<AudioParameterFloat> (ParameterID { id::fltRelease, 1 }, "Filter Release", envTimeRange, d.fltReleaseMs, Attr().withStringFromValueFunction (ms).withValueFromStringFunction (msValue)));
    p.push_back (std::make_unique<AudioParameterFloat> (ParameterID { id::velFlt, 1 }, "Vel -> Filter", NormalisableRange<float> (0.0f, 1.0f, 0.01f), d.velFlt, Attr().withStringFromValueFunction (pct).withValueFromStringFunction (pctValue)));

    // LFO
    p.push_back (std::make_unique<AudioParameterChoice> (ParameterID { id::lfoWave, 1 }, "LFO Wave", lfoWaveNames, (int) d.lfoWave));
    p.push_back (std::make_unique<AudioParameterFloat> (ParameterID { id::lfoRate, 1 }, "LFO Rate", lfoRateRange, d.lfoRateHz,
                                                         Attr().withStringFromValueFunction ([] (float v, int) { return String (v, 2) + " Hz"; })));
    p.push_back (std::make_unique<AudioParameterFloat> (ParameterID { id::lfoPitch, 1 }, "LFO -> Pitch", NormalisableRange<float> (0.0f, 100.0f, 0.5f), d.lfoPitchCents, Attr().withStringFromValueFunction (cents)));
    p.push_back (std::make_unique<AudioParameterFloat> (ParameterID { id::lfoFilter, 1 }, "LFO -> Filter", NormalisableRange<float> (0.0f, 1.0f, 0.01f), d.lfoFilter, Attr().withStringFromValueFunction (pct).withValueFromStringFunction (pctValue)));
    p.push_back (std::make_unique<AudioParameterFloat> (ParameterID { id::lfoPwm, 1 }, "LFO -> PWM", NormalisableRange<float> (0.0f, 1.0f, 0.01f), d.lfoPwm, Attr().withStringFromValueFunction (pct).withValueFromStringFunction (pctValue)));

    // Effects & master
    p.push_back (std::make_unique<AudioParameterChoice> (ParameterID { id::chorusMode, 1 }, "Chorus", chorusModeNames, (int) d.chorusMode));
    p.push_back (std::make_unique<AudioParameterFloat> (ParameterID { id::echoTime, 1 }, "Echo Time", echoTimeRange, d.echoTimeMs, Attr().withStringFromValueFunction (ms).withValueFromStringFunction (msValue)));
    p.push_back (std::make_unique<AudioParameterFloat> (ParameterID { id::echoFeedback, 1 }, "Echo Feedback", NormalisableRange<float> (0.0f, 0.85f, 0.01f), d.echoFeedback, Attr().withStringFromValueFunction (pct).withValueFromStringFunction (pctValue)));
    p.push_back (std::make_unique<AudioParameterFloat> (ParameterID { id::echoMix, 1 }, "Echo Mix", NormalisableRange<float> (0.0f, 1.0f, 0.01f), d.echoMix, Attr().withStringFromValueFunction (pct).withValueFromStringFunction (pctValue)));
    p.push_back (std::make_unique<AudioParameterFloat> (ParameterID { id::masterGain, 1 }, "Master Gain", NormalisableRange<float> (-24.0f, 12.0f, 0.1f), d.masterGainDb, Attr().withStringFromValueFunction (db)));

    return { p.begin(), p.end() };
}

// Reads the current values into the plain struct the engine consumes. Called per block on the audio thread.
inline EngineParams readParams (juce::AudioProcessorValueTreeState& s)
{
    const auto v = [&] (juce::StringRef pid) { return s.getRawParameterValue (pid)->load(); };
    EngineParams e;
    e.voiceMode = (VoiceMode) std::clamp ((int) v (id::voiceMode), 0, kNumVoiceModes - 1);
    e.glideMs = v (id::glide);
    e.drift = v (id::drift);

    e.osc1Wave = (OscWave) std::clamp ((int) v (id::osc1Wave), 0, kNumOscWaves - 1);
    e.osc1Pwm = v (id::osc1Pwm);
    e.subLevel = v (id::subLevel);

    e.osc2Wave = (OscWave) std::clamp ((int) v (id::osc2Wave), 0, kNumOscWaves - 1);
    e.osc2Semi = std::clamp ((int) std::lround (v (id::osc2Semi)), -24, 24);
    e.osc2DetuneCents = v (id::osc2Detune);
    e.osc2Fm = v (id::osc2Fm);
    e.oscMix = v (id::oscMix);
    e.noiseLevel = v (id::noiseLevel);

    e.filterMode = (FilterMode) std::clamp ((int) v (id::filterMode), 0, kNumFilterModes - 1);
    e.cutoffHz = v (id::cutoff);
    e.resonance = v (id::resonance);
    e.filterEnvAmt = v (id::filterEnvAmt);
    e.keytrack = v (id::keytrack);
    e.drive = v (id::drive);

    e.ampAttackMs = v (id::ampAttack);
    e.ampDecayMs = v (id::ampDecay);
    e.ampSustain = v (id::ampSustain);
    e.ampReleaseMs = v (id::ampRelease);
    e.velAmp = v (id::velAmp);

    e.fltAttackMs = v (id::fltAttack);
    e.fltDecayMs = v (id::fltDecay);
    e.fltSustain = v (id::fltSustain);
    e.fltReleaseMs = v (id::fltRelease);
    e.velFlt = v (id::velFlt);

    e.lfoWave = (LfoWave) std::clamp ((int) v (id::lfoWave), 0, kNumLfoWaves - 1);
    e.lfoRateHz = v (id::lfoRate);
    e.lfoPitchCents = v (id::lfoPitch);
    e.lfoFilter = v (id::lfoFilter);
    e.lfoPwm = v (id::lfoPwm);

    e.chorusMode = (ChorusMode) std::clamp ((int) v (id::chorusMode), 0, kNumChorusModes - 1);
    e.echoTimeMs = v (id::echoTime);
    e.echoFeedback = v (id::echoFeedback);
    e.echoMix = v (id::echoMix);
    e.masterGainDb = v (id::masterGain);
    return e;
}

// For the editor and the checks: set a parameter from its real value.
inline void setValue (juce::AudioProcessorValueTreeState& s, const juce::String& pid, float value)
{
    auto* p = s.getParameter (pid);
    p->setValueNotifyingHost (p->convertTo0to1 (value));
}
} // namespace selene
