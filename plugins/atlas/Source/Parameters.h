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
inline const juce::String phase { "phase" }, oversampling { "os" }, out { "out" }, lookahead { "look" }, mix { "mix" };
inline const std::array<juce::String, kCrossovers> crossover { "xo0", "xo1", "xo2", "xo3", "xo4" };
inline const std::array<juce::String, kCrossovers> split { "xo0_on", "xo1_on", "xo2_on", "xo3_on", "xo4_on" }; // off: the two bands are one
}

// Ids are "b1_on" ... "b6_match": built once and kept, so the audio thread never builds a String.
struct BandIds
{
    juce::String on, channels, threshold, ratio, attack, release, makeup, match;
    juce::String listen; // what its compressor listens to: its own band, or the sidechain's (version 2)
    juce::String direction; // downward (turns down what is loud) or upward (lifts what is quiet) (version 3)
};

inline const BandIds& bandIds (int band)
{
    static const auto table = []
    {
        std::array<BandIds, kBands> t;
        for (int i = 0; i < kBands; ++i)
        {
            const auto name = [i] (const char* what) { return "b" + juce::String (i + 1) + "_" + what; };
            t[(size_t) i] = { name ("on"), name ("ch"), name ("thresh"), name ("ratio"), name ("attack"), name ("release"), name ("makeup"), name ("match"), name ("listen"), name ("dir") };
        }
        return t;
    }();
    return table[(size_t) band];
}

inline const juce::StringArray channelNames { "Stereo", "L/R", "M/S", "Left", "Right", "Mid", "Side" }; // the order of Channels
inline const juce::StringArray phaseNames { "Zero Latency", "Linear" };
inline const juce::StringArray oversamplingNames { "Off", "2x", "4x" };
inline const juce::StringArray listenNames { "Self", "Sidechain" };
inline const juce::StringArray directionNames { "Downward", "Upward" };
// The choices of kLookaheadMs (Engine.h), named from it.
inline juce::StringArray lookaheadNames()
{
    juce::StringArray names;
    for (double ms : kLookaheadMs)
        names.add (ms == 0.0 ? juce::String ("Off") : juce::String (ms, 0) + " ms");
    return names;
}

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
    // Added after the first release, so version 2 and after everything else: the parameters a host already knows keep their places.
    p.push_back (std::make_unique<AudioParameterChoice> (ParameterID { id::lookahead, 2 }, "Look-ahead", lookaheadNames(), 0));
    for (int i = 0; i < kBands; ++i)
        p.push_back (std::make_unique<AudioParameterChoice> (ParameterID { bandIds (i).listen, 2 }, "Band " + String (i + 1) + " Listens To", listenNames, 0));
    for (int i = 0; i < kBands; ++i)
        p.push_back (std::make_unique<AudioParameterChoice> (ParameterID { bandIds (i).direction, 3 }, "Band " + String (i + 1) + " Direction", directionNames, 0));
    // Dry / wet for parallel compression, after everything else (version 4). 100 % is the plugin as it was, so an old session sounds the same.
    p.push_back (std::make_unique<AudioParameterFloat> (ParameterID { id::mix, 4 }, "Mix", NormalisableRange<float> (0.0f, 100.0f, 0.1f), 100.0f,
                                                        Attr().withStringFromValueFunction ([] (float v, int) { return String (v, 0) + " %"; })
                                                              .withValueFromStringFunction ([] (const String& t) { return t.getFloatValue(); })));
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
    e.lookahead = std::clamp ((int) v (id::lookahead), 0, kNumLookahead - 1);
    e.mix = std::clamp ((double) v (id::mix) * 0.01, 0.0, 1.0);
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
        b.sidechain = v (ids.listen) > 0.5f;
        b.upward = v (ids.direction) > 0.5f;
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

// The ids of a band's settings in one list (the order is `BandValues`'s).
inline std::array<const juce::String*, 10> bandIdList (const BandIds& a)
{
    return { &a.on, &a.channels, &a.threshold, &a.ratio, &a.attack, &a.release, &a.makeup, &a.match, &a.listen, &a.direction };
}

using BandValues = std::array<float, 10>;

inline BandValues readBand (juce::AudioProcessorValueTreeState& s, int slot)
{
    BandValues v {};
    const auto ids = bandIdList (bandIds (slot));
    for (size_t i = 0; i < ids.size(); ++i)
    {
        auto* param = s.getParameter (*ids[i]);
        v[i] = param->convertFrom0to1 (param->getValue());
    }
    return v;
}

inline void writeBand (juce::AudioProcessorValueTreeState& s, int slot, const BandValues& v)
{
    const auto ids = bandIdList (bandIds (slot));
    for (size_t i = 0; i < ids.size(); ++i)
        setValue (s, *ids[i], v[i]);
}

inline void copyBand (juce::AudioProcessorValueTreeState& s, int from, int to)
{
    writeBand (s, to, readBand (s, from));
}

// Several copies at once, `from -> to` in the order given, all read before any is written: a copy never sees what an earlier one wrote.
inline void copyBands (juce::AudioProcessorValueTreeState& s, const std::vector<std::pair<int, int>>& moves)
{
    std::array<BandValues, kBands> before;
    for (int i = 0; i < kBands; ++i)
        before[(size_t) i] = readBand (s, i);
    for (const auto& m : moves)
        writeBand (s, m.second, before[(size_t) m.first]);
}

// THRESH, RATIO, ATTACK and RELEASE of one band to every other band there is (not its channels, direction, makeup or on/off: those are per band by nature).
inline void copyDynamicsToAll (juce::AudioProcessorValueTreeState& s, int fromSlot)
{
    const auto l = layoutOf (s);
    const auto& a = bandIds (fromSlot);
    for (int band = 0; band < l.numBands; ++band)
    {
        const int to = l.slot[(size_t) band];
        if (to == fromSlot)
            continue;
        const auto& b = bandIds (to);
        for (const auto& pair : { std::pair<const juce::String*, const juce::String*> { &a.threshold, &b.threshold }, { &a.ratio, &b.ratio }, { &a.attack, &b.attack }, { &a.release, &b.release } })
            if (auto* param = s.getParameter (*pair.first))
                setValue (s, *pair.second, param->convertFrom0to1 (param->getValue()));
    }
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
    // Settings first, read from the strips the bands are on now (a snapshot: a copy must not read a strip an earlier one wrote).
    std::vector<std::pair<int, int>> moves;
    for (int band = l.numBands - 1; band >= at; --band)
        moves.push_back ({ l.slot[(size_t) band], band + 1 });
    for (int band = 0; band < at; ++band)
        if (l.slot[(size_t) band] != band)
            moves.push_back ({ l.slot[(size_t) band], band });
    copyBands (s, moves);
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
    std::vector<std::pair<int, int>> moves;
    for (int b = 0; b < l.numBands; ++b)
    {
        const int to = b <= band ? b : b - 1;
        if (b != band + 1 && l.slot[(size_t) b] != to)
            moves.push_back ({ l.slot[(size_t) b], to });
    }
    copyBands (s, moves);
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

// ---- presets -------------------------------------------------------------------------------------
// A preset is the bands' settings; everything not listed goes back to its default, the session-wide controls (phase, oversampling,
// look-ahead, output, mix) are kept. The thresholds are starting points for a master around -14 LUFS: the band's level decides, so set THRESH
// where the meter first moves.
using PresetValues = std::vector<std::pair<juce::String, float>>;

struct Preset
{
    const char* name;
    PresetValues values;
    const char* purpose; // one line: what it is for, and what it is not
};

inline bool isGlobalParameter (const juce::String& pid)
{
    return pid == id::phase || pid == id::oversampling || pid == id::out || pid == id::lookahead || pid == id::mix;
}

inline PresetValues bandValues (int band, double thresholdDb, double ratio, double attackMs, double releaseMs,
                                Channels channels = Channels::stereo, bool match = false, bool sidechain = false, bool upward = false)
{
    const auto& ids = bandIds (band);
    return { { ids.on, 1.0f }, { ids.threshold, (float) thresholdDb }, { ids.ratio, (float) ratio }, { ids.attack, (float) attackMs },
             { ids.release, (float) releaseMs }, { ids.channels, (float) (int) channels }, { ids.match, match ? 1.0f : 0.0f }, { ids.listen, sidechain ? 1.0f : 0.0f },
             { ids.direction, upward ? 1.0f : 0.0f } };
}

inline PresetValues bandsOff (std::initializer_list<int> bands)
{
    PresetValues v;
    for (int b : bands)
        v.push_back ({ bandIds (b).on, 0.0f });
    return v;
}

inline PresetValues join (std::initializer_list<PresetValues> parts)
{
    PresetValues all;
    for (auto& part : parts)
        all.insert (all.end(), part.begin(), part.end());
    return all;
}

inline const std::vector<Preset>& presets()
{
    static constexpr double glue[kBands] = { -24.0, -20.0, -24.0, -26.0, -32.0, -38.0 };
    static const std::vector<Preset> list
    {
        { "Init", {}, "The starting point: six bands with their thresholds at the top, so nothing compresses until you lower THRESH." },
        { "Mastering Glue", join ({ bandValues (0, glue[0], 1.5, 30, 200, Channels::stereo, true), bandValues (1, glue[1], 1.5, 30, 200, Channels::stereo, true),
                                    bandValues (2, glue[2], 1.5, 30, 200, Channels::stereo, true), bandValues (3, glue[3], 1.5, 30, 200, Channels::stereo, true),
                                    bandValues (4, glue[4], 1.5, 30, 200, Channels::stereo, true), bandValues (5, glue[5], 1.5, 30, 200, Channels::stereo, true) }),
          "A light, slow squeeze on every band, gain-matched. Evens a master out; it is not a limiter and will not make it louder." },
        { "Tame the Low End", join ({ bandValues (0, -22, 3, 20, 150), bandValues (1, -18, 3, 20, 150), bandsOff ({ 2, 3, 4, 5 }) }),
          "A boomy or uneven sub and bass: turns them down only when they swell. It does not cut what is always too loud: use an equaliser." },
        { "Steady Low-Mids", join ({ bandValues (2, -26, 2.5, 30, 250), bandsOff ({ 0, 1, 3, 4, 5 }) }),
          "Mud that comes and goes in the low-mids. It follows the swells; a constant excess is an equaliser's job." },
        { "De-Harsh Upper-Mid", join ({ bandValues (4, -30, 3, 5, 80), bandsOff ({ 0, 1, 2, 3, 5 }) }),
          "Harshness that spikes at 2 - 6 kHz (a loud voice or cymbal). Turns the band down only while it spikes." },
        { "De-Ess", join ({ bandValues (5, -36, 4, 1, 40), bandsOff ({ 0, 1, 2, 3, 4 }) }),
          "Sibilance in the top band. Set LOOK-AHEAD to 1 - 2 ms so the first 's' is caught; it works on one band, so it is a broad de-esser." },
        { "Mid/Side Control", join ({ bandValues (0, glue[0], 2, 20, 150, Channels::midSide), bandValues (1, glue[1], 2, 20, 150, Channels::midSide),
                                      bandValues (2, glue[2], 2, 20, 150, Channels::midSide), bandValues (3, glue[3], 2, 20, 150, Channels::midSide),
                                      bandValues (4, glue[4], 2, 20, 150, Channels::midSide), bandValues (5, glue[5], 2, 20, 150, Channels::midSide) }),
          "Each band's centre and sides get their own gain, so a loud centre does not pull the width down. Check mono with DELTA." },
        { "Lift the Low-Mids", join ({ bandValues (2, -24, 2, 20, 300, Channels::stereo, false, false, true), bandsOff ({ 0, 1, 3, 4, 5 }) }),
          "Low-mids that go empty between sections: lifts the band while it is quiet, up to 12 dB. A constant gap is an equaliser's job." },
        { "Lift the Air", join ({ bandValues (5, -34, 2, 20, 300, Channels::stereo, false, false, true), bandsOff ({ 0, 1, 2, 3, 4 }) }),
          "A top that dips in the quiet parts: lifts it while it is quiet, hiss included, so listen for noise." },
        { "Kick Ducks Bass", join ({ bandValues (0, -30, 4, 5, 120, Channels::stereo, false, true), bandValues (1, -30, 4, 5, 120, Channels::stereo, false, true),
                                     bandsOff ({ 2, 3, 4, 5 }) }),
          "Put the kick on the SIDECHAIN input: the sub and bass duck when it hits. Needs a sidechain routed in your host." },
    };
    return list;
}

inline void applyPreset (juce::AudioProcessorValueTreeState& s, int index)
{
    const auto& list = presets();
    if (! juce::isPositiveAndBelow (index, (int) list.size()))
        return;
    // Start from the defaults so a preset does not inherit what the last one left.
    for (auto* param : s.processor.getParameters())
        if (auto* ranged = dynamic_cast<juce::RangedAudioParameter*> (param))
            if (! isGlobalParameter (ranged->paramID))
                setValue (s, ranged->paramID, ranged->convertFrom0to1 (ranged->getDefaultValue()));
    for (auto& [pid, value] : list[(size_t) index].values)
        setValue (s, pid, value);
}
} // namespace atlas
