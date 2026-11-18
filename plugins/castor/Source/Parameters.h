// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 The Pantheon authors

#pragma once

#include "DSP/Engine.h"
#include <juce_audio_processors/juce_audio_processors.h>
#include <pantheon/dsp/BandLink.h>
#include <pantheon/dsp/MusicBands.h>
#include <pantheon/ui/Undo.h>

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
    // std::clamp lets a NaN through and the engine's smoothing would keep it for good: a value that is not a number reads as the default.
    const auto v = [&] (const juce::String& pid, float fallback) { const float x = s.getRawParameterValue (pid)->load(); return std::isfinite (x) ? x : fallback; };
    const EngineParams d;
    EngineParams e;
    for (int b = 0; b < kBands; ++b)
        e.width[(size_t) b] = v (id::width (b), d.width[(size_t) b] * 100.0f) / 100.0f;
    e.midDb = v (id::mid, d.midDb);
    e.sideDb = v (id::side, d.sideDb);
    e.monoBelowHz = v (id::monoBelow, d.monoBelowHz);
    e.balance = v (id::balance, d.balance * 100.0f) / 100.0f;
    e.outDb = v (id::out, d.outDb);
    e.listen = (Listen) std::clamp ((int) v (id::listen, 0.0f), 0, kNumListen - 1);
    return e;
}

// For the editor and the checks: set a parameter from its real value.
inline void setValue (juce::AudioProcessorValueTreeState& s, const juce::String& pid, float value)
{
    auto* p = s.getParameter (pid);
    p->setValueNotifyingHost (p->convertTo0to1 (value));
}

// The width suggestion Themis hands over as text (pdsp::link's width line, from a width item of its plan): the side width of each band, in percent.
// Every band the line names is set, the others are left as they are; the whole paste is one UNDO step. Nothing changes when the text is not a Pantheon
// list or has no width line, and the result says so. The width line has no amount key (pdsp::link's encodeWidth writes none), so each band's percent is taken as it is.
inline std::string pasteWidth (juce::AudioProcessorValueTreeState& s, const juce::String& text)
{
    pdsp::LinkWidth w;
    if (! pdsp::link::decodeWidth (text.toStdString(), w))
        return "Nothing to paste: copy a width item from Themis' plan first (COPY on it).";
    ui::beginUndoStep (s, "Paste"); // every band of one paste is one UNDO step
    std::string said = "Pasted from Themis:";
    bool first = true;
    for (size_t b = 0; b < pdsp::bands::kCount; ++b)
    {
        if (! w.percent[b].has_value())
            continue;
        const float value = std::clamp ((float) *w.percent[b], 0.0f, kMaxWidth * 100.0f);
        setValue (s, id::width ((int) b), value);
        const auto shown = juce::String (juce::roundToInt (s.getRawParameterValue (id::width ((int) b))->load())).toStdString();
        said += std::string (first ? " " : ", ") + juce::String (pdsp::bands::kBands[b].name).toUpperCase().toStdString() + " " + shown + "%";
        first = false;
    }
    return said + ".";
}
} // namespace castor
