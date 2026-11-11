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
inline const std::array<juce::String, kCrossovers> split { "xo0_on", "xo1_on", "xo2_on", "xo3_on", "xo4_on" }; // off: the two bands are one
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
        F (id::crossover[(size_t) k], "Split " + String (k + 1) + " Frequency",
           skewed (kMinCrossoverHz, kMaxCrossoverHz, 1.0f, 800.0f), (float) pdsp::bands::crossoverHz (k), Attr().withStringFromValueFunction (hz).withValueFromStringFunction (hzValue));

    for (int k = 0; k < kCrossovers; ++k)
        p.push_back (std::make_unique<AudioParameterBool> (ParameterID { id::split[(size_t) k], 1 }, "Split " + String (k + 1) + " On", true));

    for (int i = 0; i < kBands; ++i)
    {
        const auto& ids = bandIds (i);
        const auto label = [i] (const char* what) { return "Band " + String (i + 1) + " " + what; };
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
    {
        e.crossover[(size_t) k] = v (id::crossover[(size_t) k]);
        e.split[(size_t) k] = v (id::split[(size_t) k]) > 0.5f;
    }
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

// ---- the bands as they are laid out --------------------------------------------------------------
// One to six bands. The editor keeps the splits that are on in the first slots, in order (addSplit / removeSplit), so the
// bands are strips 0 ... numBands - 1 from the bottom of the spectrum up; the engine does not rely on that (bandOwners).
inline constexpr double kSpectrumLowHz = 20.0, kSpectrumHighHz = 20000.0;

struct Layout
{
    int numBands = 1;
    std::array<int, kBands> slot {};          // the strip (parameter set) of each band
    std::array<double, kBands + 1> edgeHz {}; // band i is edgeHz[i] ... edgeHz[i + 1]
    std::array<int, kBands> splitAbove {};    // the crossover slot at the top of each band (-1 for the last)
};

inline Layout layoutOf (juce::AudioProcessorValueTreeState& s)
{
    const auto p = readParams (s);
    const auto xo = orderedCrossovers (p.crossover);
    Layout l;
    l.numBands = 0;
    l.edgeHz[0] = kSpectrumLowHz;
    l.slot[0] = 0;
    for (int k = 0; k < kCrossovers; ++k)
        if (p.split[(size_t) k])
        {
            l.splitAbove[(size_t) l.numBands] = k;
            l.edgeHz[(size_t) ++l.numBands] = xo[(size_t) k];
            l.slot[(size_t) l.numBands] = k + 1;
        }
    l.splitAbove[(size_t) l.numBands] = -1;
    l.edgeHz[(size_t) ++l.numBands] = kSpectrumHighHz;
    return l;
}

inline void setValue (juce::AudioProcessorValueTreeState& s, const juce::String& pid, float value)
{
    if (auto* param = s.getParameter (pid))
    {
        param->beginChangeGesture();
        param->setValueNotifyingHost (param->convertTo0to1 (value));
        param->endChangeGesture();
    }
}

inline void copyBand (juce::AudioProcessorValueTreeState& s, int from, int to)
{
    const auto& a = bandIds (from);
    const auto& b = bandIds (to);
    const juce::String* const pairs[][2] = { { &a.on, &b.on }, { &a.channels, &b.channels }, { &a.threshold, &b.threshold }, { &a.ratio, &b.ratio },
                                             { &a.attack, &b.attack }, { &a.release, &b.release }, { &a.makeup, &b.makeup }, { &a.match, &b.match } };
    for (const auto& pair : pairs)
        setValue (s, *pair[1], s.getParameter (*pair[0])->convertFrom0to1 (s.getParameter (*pair[0])->getValue()));
}

// Writes the splits that are on into the first slots, in order, and parks the rest at the top, off.
inline void writeSplits (juce::AudioProcessorValueTreeState& s, const std::vector<double>& hz)
{
    for (int k = 0; k < kCrossovers; ++k)
    {
        const bool on = k < (int) hz.size();
        setValue (s, id::crossover[(size_t) k], on ? (float) hz[(size_t) k] : kMaxCrossoverHz);
        setValue (s, id::split[(size_t) k], on ? 1.0f : 0.0f);
    }
}

inline std::vector<double> splitsOf (const Layout& l)
{
    std::vector<double> hz;
    for (int i = 1; i < l.numBands; ++i)
        hz.push_back (l.edgeHz[(size_t) i]);
    return hz;
}

// Splits the band at `hz` in two; both halves keep its settings, and the bands above move up a strip. Returns the new upper
// band, or -1 when there are six already or `hz` is too close to an edge (kMinCrossoverRatio) or out of range.
inline int addSplit (juce::AudioProcessorValueTreeState& s, double hz)
{
    const auto l = layoutOf (s);
    if (l.numBands >= kBands || hz < kMinCrossoverHz || hz > kMaxCrossoverHz)
        return -1;
    auto splits = splitsOf (l);
    for (double f : splits)
        if (std::max (f, hz) / std::min (f, hz) < kMinCrossoverRatio)
            return -1;
    const int at = (int) (std::upper_bound (splits.begin(), splits.end(), hz) - splits.begin()); // the band that is split
    // Settings first, from the top down, read from the strips the bands are on now.
    for (int band = l.numBands - 1; band >= at; --band)
        copyBand (s, l.slot[(size_t) band], band + 1);
    for (int band = 0; band < at; ++band)
        if (l.slot[(size_t) band] != band)
            copyBand (s, l.slot[(size_t) band], band);
    splits.insert (splits.begin() + at, hz);
    writeSplits (s, splits);
    return at + 1;
}

// Removes the split at the top of `band`: it and the band above become one, with `band`'s settings.
inline bool removeSplit (juce::AudioProcessorValueTreeState& s, int band)
{
    const auto l = layoutOf (s);
    if (band < 0 || band >= l.numBands - 1)
        return false;
    for (int b = 0; b < l.numBands; ++b)
    {
        const int to = b <= band ? b : b - 1;
        if (b != band + 1 && l.slot[(size_t) b] != to)
            copyBand (s, l.slot[(size_t) b], to);
    }
    auto splits = splitsOf (l);
    splits.erase (splits.begin() + band);
    writeSplits (s, splits);
    return true;
}

// Moves the split at the top of `band`, kept between its neighbours.
inline void moveSplit (juce::AudioProcessorValueTreeState& s, int band, double hz)
{
    const auto l = layoutOf (s);
    if (band < 0 || band >= l.numBands - 1)
        return;
    const double lo = band == 0 ? kMinCrossoverHz : l.edgeHz[(size_t) band] * kMinCrossoverRatio;
    const double hi = band == l.numBands - 2 ? kMaxCrossoverHz : l.edgeHz[(size_t) (band + 2)] / kMinCrossoverRatio;
    if (auto* param = s.getParameter (id::crossover[(size_t) l.splitAbove[(size_t) band]]))
        param->setValueNotifyingHost (param->convertTo0to1 ((float) std::clamp (hz, lo, std::max (lo, hi))));
}
} // namespace atlas
