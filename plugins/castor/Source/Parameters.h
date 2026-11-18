// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 The Pantheon authors

#pragma once

#include "DSP/Engine.h"
#include <juce_audio_processors/juce_audio_processors.h>
#include <pantheon/dsp/BandLink.h>
#include <pantheon/dsp/MusicBands.h>
#include <pantheon/ui/Note.h>
#include <pantheon/ui/Undo.h>
#include <optional>
#include <string>
#include <vector>

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
    NormalisableRange<float> monoRange (kMonoOffHz, kMonoMaxHz, 1.0f);
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

// The low-end cut Themis hands over (the low-end mono COPY): a lowcut band on the side channel (`place=side`), a 24 dB/oct high-pass of the sides.
// Its frequency is MONO BELOW's, clamped to the parameter's range (at or under kMonoOffHz is off). A lowcut on the stereo or on a mid band is not
// MONO BELOW, and the slope is not read: MONO BELOW is always 24 dB/oct.
inline std::optional<float> monoBelowFrom (const std::vector<pdsp::LinkBand>& bands)
{
    for (const auto& b : bands)
        if (b.kind == pdsp::LinkBand::Kind::lowCut && b.place == pdsp::LinkBand::Place::side)
            return std::clamp ((float) b.freq, kMonoOffHz, kMonoMaxHz);
    return std::nullopt;
}

// What a paste sets, for the strip: a percent for each band it names, and MONO BELOW in Hz (or Off), as the knobs show them.
inline std::string monoBelowText (float hz)
{
    return hz <= kMonoOffHz + 0.5f ? std::string ("MONO BELOW Off") : "MONO BELOW " + juce::String (juce::roundToInt (hz)).toStdString() + " Hz";
}

// The paste of a Themis hand-off as text (pdsp::link): the width line (the side width of each band, from a width item) and the side low-cut
// (MONO BELOW, from the low-end mono item). Every band the width line names is set, the others are left as they are; both are one UNDO step.
// Nothing changes when the text is not a Pantheon list or has neither, and the result says so. The width line has no amount key, so each band's
// percent is taken as it is. The strip says what was set and that it can be changed by hand or undone; if that line would not fit the strip, the
// widths are counted instead of named.
inline std::string pasteFromThemis (juce::AudioProcessorValueTreeState& s, const juce::String& text)
{
    const std::string str = text.toStdString();
    constexpr const char* nothing = "Nothing to paste: copy a width item or the low-end mono item from Themis' plan first (COPY on it).";
    std::vector<pdsp::LinkBand> bands;
    if (! pdsp::link::decode (str, bands)) // not a Pantheon band list
        return nothing;
    pdsp::LinkWidth w;
    const bool hasWidth = pdsp::link::decodeWidth (str, w);
    const auto monoHz = monoBelowFrom (bands);
    if (! hasWidth && ! monoHz)
        return nothing;

    ui::beginUndoStep (s, "Paste"); // the widths and MONO BELOW of one paste are one UNDO step
    std::vector<std::string> bandParts;
    for (size_t b = 0; b < pdsp::bands::kCount; ++b)
    {
        if (! w.percent[b].has_value())
            continue;
        const float value = std::clamp ((float) *w.percent[b], 0.0f, kMaxWidth * 100.0f);
        setValue (s, id::width ((int) b), value);
        const auto shown = juce::String (juce::roundToInt (s.getRawParameterValue (id::width ((int) b))->load())).toStdString();
        bandParts.push_back (juce::String (pdsp::bands::kBands[b].name).toUpperCase().toStdString() + " " + shown + "%");
    }
    std::string monoPart;
    if (monoHz)
    {
        setValue (s, id::monoBelow, *monoHz);
        monoPart = monoBelowText (s.getRawParameterValue (id::monoBelow)->load());
    }

    const auto join = [] (std::vector<std::string> parts)
    {
        std::string out;
        for (const auto& p : parts)
            out += (out.empty() ? "" : ", ") + p;
        return out;
    };
    const auto sentence = [] (const std::string& items) { return "Pasted from Themis: " + items + ". Change by hand or UNDO."; };
    std::vector<std::string> all = bandParts;
    if (! monoPart.empty())
        all.push_back (monoPart);
    std::string said = sentence (join (all));
    if ((int) said.size() > ui::kMaxNoteChars && ! bandParts.empty())
    {
        std::vector<std::string> counted { std::to_string (bandParts.size()) + (bandParts.size() == 1 ? " width" : " widths") };
        if (! monoPart.empty())
            counted.push_back (monoPart);
        said = sentence (join (counted));
    }
    return said;
}
} // namespace castor
