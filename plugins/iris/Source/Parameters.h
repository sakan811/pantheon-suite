// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 The Pantheon authors

#pragma once

#include "DSP/Engine.h"
#include "DSP/ReferenceMatch.h"

#include <pantheon/dsp/BandLink.h>

#include <juce_audio_processors/juce_audio_processors.h>

namespace iris
{
namespace id
{
    inline const juce::String out { "out" };
    inline const juce::String match { "match" };
    inline const juce::String mode { "mode" };       // 0 = Clean, 1 = Sat
    inline const juce::String drive { "drive" };
    inline const juce::String style { "style" };
    inline const juce::String phase { "phase" };     // 0 = Zero Latency, 1 = Linear, 2 = Linear HQ (version 3: it came after the crossover)

    // xo1 ... xo4 were the four crossovers of the first layout (the five regions); xo0, the sub / bass split at the front, came after
    // them, so a session saved before it has the other four exactly where they were.
    inline const std::array<juce::String, numCrossovers> crossover { "xo0", "xo1", "xo2", "xo3", "xo4" };
}

// The parameters of one band. Ids are "b1_on" ... "b8_release": built once and kept, so the audio thread can look
// parameters up without allocating.
struct BandIds
{
    juce::String on, type, place, freq, gain, q, slope, dyn, dynMode, threshold, ratio, range, attack, release;
    juce::String listen; // what the dynamics listen to: the band's own signal or the sidechain (version 4)
};

inline const BandIds& bandIds (int band)
{
    static const auto table = []
    {
        std::array<BandIds, maxBands> t;
        for (int i = 0; i < maxBands; ++i)
        {
            const auto name = [i] (const char* field) { return "b" + juce::String (i + 1) + "_" + field; };
            t[(size_t) i] = { name ("on"), name ("type"), name ("place"), name ("freq"), name ("gain"), name ("q"), name ("slope"),
                              name ("dyn"), name ("dmode"), name ("thresh"), name ("ratio"), name ("range"), name ("attack"), name ("release"),
                              name ("listen") };
        }
        return t;
    }();
    return table[(size_t) std::clamp (band, 0, maxBands - 1)];
}

// The choice lists, written once: the editor reads them from the parameters.
inline const juce::StringArray typeNames { "Bell", "Low Shelf", "High Shelf", "Low Cut", "High Cut", "Notch" };
inline const juce::StringArray placeNames { "Stereo", "Left", "Right", "Mid", "Side" };
inline const juce::StringArray slopeNames { "12 dB/oct", "24 dB/oct", "36 dB/oct", "48 dB/oct" };
inline const juce::StringArray dynModeNames { "Above", "Below" };
// What a dynamic band's detector listens to: the band's own signal, or the other track on the sidechain input.
inline const juce::StringArray listenNames { "Self", "Sidechain" };
inline const juce::StringArray modeNames { "Clean", "Sat" };
inline const juce::StringArray styleNames { "Tape", "Tube", "Console" };
// Zero Latency: minimum-phase bands, no delay. Linear: no phase shift at all, delayed by 2304 samples. Linear HQ: the same at a resolution
// fine enough for the bass, delayed by 9216 samples (DSP/LinearPhase.h).
inline const juce::StringArray phaseNames { "Zero Latency", "Linear", "Linear HQ" };

inline constexpr float minGainDb = -24.0f, maxGainDb = 24.0f;
inline constexpr float defaultDriveDb = 12.0f;

// Where an unused band starts, spread across the spectrum so a new one never lands on top of another.
inline constexpr float defaultFreq (int band) noexcept
{
    constexpr float table[maxBands] = { 60.0f, 150.0f, 400.0f, 1000.0f, 2500.0f, 5000.0f, 9000.0f, 14000.0f };
    return table[band];
}

// Session-wide controls: presets leave these alone.
inline bool isGlobalParameter (const juce::String& paramId)
{
    if (paramId == id::out || paramId == id::match || paramId == id::phase)
        return true;
    return std::any_of (id::crossover.begin(), id::crossover.end(), [&] (const juce::String& x) { return paramId == x; });
}

inline juce::AudioProcessorValueTreeState::ParameterLayout createLayout()
{
    using namespace juce;
    using Float = AudioParameterFloat;
    using Attr = AudioParameterFloatAttributes;
    std::vector<std::unique_ptr<RangedAudioParameter>> p;

    const auto hzText = [] (float v, int) { return v < 1000.0f ? String (roundToInt (v)) + " Hz" : String (v / 1000.0f, 2) + " kHz"; };
    const auto hzValue = [] (const String& s)
    {
        const auto t = s.trim();
        const float v = t.getFloatValue();
        return t.containsIgnoreCase ("k") ? v * 1000.0f : v;
    };
    const auto freq = [] (float centre)
    {
        NormalisableRange<float> r (20.0f, 20000.0f, 0.1f);
        r.setSkewForCentre (centre);
        return r;
    };
    const auto skewed = [] (float lo, float hi, float step, float centre)
    {
        NormalisableRange<float> r (lo, hi, step);
        r.setSkewForCentre (centre);
        return r;
    };
    const auto signedDb = [] (float v, int) { return (v > 0.04f ? "+" : "") + String (v, 1) + " dB"; };
    const auto plainDb = [] (float v, int) { return String (v, 1) + " dB"; };
    const auto msText = [] (float v, int) { return v < 10.0f ? String (v, 1) + " ms" : (v < 1000.0f ? String (roundToInt (v)) + " ms" : String (v / 1000.0f, 2) + " s"); };
    const auto msValue = [] (const String& s)
    {
        const auto t = s.trim();
        const float v = t.getFloatValue();
        return t.containsIgnoreCase ("ms") || ! t.containsIgnoreCase ("s") ? v : v * 1000.0f;
    };

    // Every parameter carries version 1 (the first layout), except what came later: the sub / bass crossover, version 2; the phase mode, 3;
    // what a dynamic band listens to, 4.
    const auto F = [&] (const String& pid, const String& name, NormalisableRange<float> r, float def, Attr a, int version = 1)
    {
        p.push_back (std::make_unique<Float> (ParameterID { pid, version }, name, r, def, a));
    };
    const auto Choice = [&] (const String& pid, const String& name, StringArray items, int def, int version = 1)
    {
        p.push_back (std::make_unique<AudioParameterChoice> (ParameterID { pid, version }, name, items, def));
    };
    const auto Bool = [&] (const String& pid, const String& name, bool def)
    {
        p.push_back (std::make_unique<AudioParameterBool> (ParameterID { pid, 1 }, name, def));
    };

    // Global
    F (id::out, "Output Gain", { -24.0f, 24.0f, 0.1f }, 0.0f, Attr().withStringFromValueFunction (signedDb));
    Bool (id::match, "Gain Match", false);
    Choice (id::mode, "Mode", modeNames, 0);
    F (id::drive, "Drive", { 0.0f, 30.0f, 0.1f }, defaultDriveDb, Attr().withStringFromValueFunction (plainDb));
    Choice (id::style, "Saturation Style", styleNames, 0);
    Choice (id::phase, "Phase", phaseNames, 0, 3);
    for (int k = 0; k < numCrossovers; ++k)
        F (id::crossover[(size_t) k], "Region Split " + String (k + 1), freq (800.0f), (float) defaultCrossoverHz[(size_t) k],
           Attr().withStringFromValueFunction (hzText).withValueFromStringFunction (hzValue), k == 0 ? 2 : 1);

    // Bands
    for (int i = 0; i < maxBands; ++i)
    {
        const auto& ids = bandIds (i);
        const auto label = [i] (const char* what) { return "Band " + String (i + 1) + " " + what; };

        Bool (ids.on, label ("On"), false);
        Choice (ids.type, label ("Type"), typeNames, 0);
        Choice (ids.place, label ("Placement"), placeNames, 0);
        F (ids.freq, label ("Frequency"), freq (1000.0f), defaultFreq (i), Attr().withStringFromValueFunction (hzText).withValueFromStringFunction (hzValue));
        F (ids.gain, label ("Gain"), { minGainDb, maxGainDb, 0.1f }, 0.0f, Attr().withStringFromValueFunction (signedDb));
        F (ids.q, label ("Q"), skewed (0.1f, 18.0f, 0.01f, 1.0f), 1.0f, Attr().withStringFromValueFunction ([] (float v, int) { return String (v, 2); }));
        Choice (ids.slope, label ("Slope"), slopeNames, 1);
        Bool (ids.dyn, label ("Dynamic"), false);
        Choice (ids.dynMode, label ("Dynamic Mode"), dynModeNames, 0);
        F (ids.threshold, label ("Threshold"), { -60.0f, 0.0f, 0.1f }, -24.0f, Attr().withStringFromValueFunction (plainDb));
        F (ids.ratio, label ("Ratio"), skewed (1.0f, 20.0f, 0.1f, 4.0f), 4.0f, Attr().withStringFromValueFunction ([] (float v, int) { return String (v, 1) + ":1"; }));
        F (ids.range, label ("Range"), { -24.0f, 24.0f, 0.1f }, -6.0f, Attr().withStringFromValueFunction (signedDb));
        F (ids.attack, label ("Attack"), skewed (0.1f, 200.0f, 0.1f, 10.0f), 10.0f, Attr().withStringFromValueFunction (msText).withValueFromStringFunction (msValue));
        F (ids.release, label ("Release"), skewed (5.0f, 2000.0f, 1.0f, 120.0f), 120.0f, Attr().withStringFromValueFunction (msText).withValueFromStringFunction (msValue));
    }

    // After every band of the first layout, so the parameters a host already knows keep their places in its list.
    for (int i = 0; i < maxBands; ++i)
        Choice (bandIds (i).listen, "Band " + String (i + 1) + " Listens To", listenNames, 0, 4);

    return { p.begin(), p.end() };
}

// Reads the current parameter values into the plain struct the engine consumes.
inline EngineParams readParams (juce::AudioProcessorValueTreeState& s)
{
    const auto v = [&] (juce::StringRef pid) { return s.getRawParameterValue (pid)->load(); };

    EngineParams e;
    e.outGainDb = v (id::out);
    e.match = v (id::match) > 0.5f;
    e.sat = (int) v (id::mode) == 1;
    e.driveDb = v (id::drive);
    e.style = (SatStyle) std::clamp ((int) v (id::style), 0, 2);
    e.phase = (PhaseMode) std::clamp ((int) v (id::phase), 0, 2);
    for (int k = 0; k < numCrossovers; ++k)
        e.crossover[(size_t) k] = v (id::crossover[(size_t) k]);

    for (int i = 0; i < maxBands; ++i)
    {
        const auto& ids = bandIds (i);
        auto& b = e.bands[(size_t) i];
        b.on = v (ids.on) > 0.5f;
        b.type = (BandType) std::clamp ((int) v (ids.type), 0, 5);
        b.place = (Placement) std::clamp ((int) v (ids.place), 0, 4);
        b.freq = v (ids.freq);
        b.gainDb = v (ids.gain);
        b.q = v (ids.q);
        b.slope = (int) v (ids.slope);
        b.dynamic = v (ids.dyn) > 0.5f;
        b.external = (int) v (ids.listen) == 1;
        b.dyn.mode = (DynMode) std::clamp ((int) v (ids.dynMode), 0, 1);
        b.dyn.thresholdDb = v (ids.threshold);
        b.dyn.ratio = v (ids.ratio);
        b.dyn.rangeDb = v (ids.range);
        b.dyn.attackMs = v (ids.attack);
        b.dyn.releaseMs = v (ids.release);
    }
    return e;
}

// ---- presets ----------------------------------------------------------------------------------
using PresetValues = std::vector<std::pair<juce::String, float>>;

struct Preset
{
    const char* name;
    PresetValues values; // everything not listed falls back to default
};

inline PresetValues bandValues (int band, BandType type, float freqHz, float gainDb, float q, Placement place = Placement::stereo, int slope = 1)
{
    const auto& ids = bandIds (band);
    return { { ids.on, 1 }, { ids.type, (float) (int) type }, { ids.place, (float) (int) place }, { ids.freq, freqHz },
             { ids.gain, gainDb }, { ids.q, q }, { ids.slope, (float) slope } };
}

// Makes a band dynamic: it engages while its own level is above (or below) `thresholdDb`.
inline PresetValues dynamicValues (int band, DynMode mode, float thresholdDb, float ratio, float rangeDb, float attackMs, float releaseMs)
{
    const auto& ids = bandIds (band);
    return { { ids.dyn, 1 }, { ids.dynMode, (float) (int) mode }, { ids.threshold, thresholdDb }, { ids.ratio, ratio },
             { ids.range, rangeDb }, { ids.attack, attackMs }, { ids.release, releaseMs } };
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
    using namespace id;
    using T = BandType;
    using P = Placement;
    static const std::vector<Preset> list
    {
        { "Init", {} },
        { "Master Air",       join ({ bandValues (0, T::lowCut, 28, 0, 1, P::stereo, 1), bandValues (1, T::highShelf, 11000, 1.8f, 0.7f),
                                      bandValues (2, T::bell, 300, -0.8f, 0.9f) }) },
        { "Mono Bass, Wide Air", join ({ bandValues (0, T::lowCut, 160, 0, 1, P::side, 1), bandValues (1, T::highShelf, 7000, 2.5f, 0.7f, P::side),
                                         bandValues (2, T::bell, 2800, -1.0f, 1.2f, P::mid) }) },
        { "Mud Duck",         join ({ bandValues (0, T::bell, 260, 0, 1.2f),
                                      dynamicValues (0, DynMode::above, -24, 3, -4, 15, 180) }) },
        { "De-Esser",         join ({ bandValues (0, T::bell, 7200, 0, 3.0f),
                                      dynamicValues (0, DynMode::above, -30, 5, -7, 1, 70) }) },
        { "Open the Top",     join ({ bandValues (0, T::highShelf, 6000, 0, 0.7f),
                                      dynamicValues (0, DynMode::below, -36, 3, 4, 10, 200) }) },
        { "Tilt",             join ({ bandValues (0, T::lowShelf, 200, 1.2f, 0.6f), bandValues (1, T::highShelf, 3000, -1.2f, 0.6f) }) },
        { "Warm Tube Lift",   join ({ { { mode, 1 }, { style, 1 }, { drive, 10 } },
                                      bandValues (0, T::lowShelf, 90, 3.0f, 0.7f), bandValues (1, T::bell, 2500, 2.0f, 0.8f) }) },
        { "Console Presence", join ({ { { mode, 1 }, { style, 2 }, { drive, 14 } },
                                      bandValues (0, T::bell, 3200, 3.5f, 0.7f), bandValues (1, T::highShelf, 10000, 2.0f, 0.7f) }) },
    };
    return list;
}

// Sets a parameter from the UI/host thread the way a user gesture would, so hosts record automation.
inline void setParameter (juce::RangedAudioParameter& param, float normalised)
{
    param.beginChangeGesture();
    param.setValueNotifyingHost (normalised);
    param.endChangeGesture();
}

// Sets a parameter to a real (not normalised) value.
inline void setParameterValue (juce::AudioProcessorValueTreeState& s, const juce::String& pid, float value)
{
    if (auto* param = s.getParameter (pid))
        setParameter (*param, param->convertTo0to1 (value));
}

inline bool isBandOn (juce::AudioProcessorValueTreeState& s, int band)
{
    return s.getRawParameterValue (bandIds (band).on)->load() > 0.5f;
}

inline BandType bandType (juce::AudioProcessorValueTreeState& s, int band)
{
    return (BandType) std::clamp ((int) s.getRawParameterValue (bandIds (band).type)->load(), 0, 5);
}

inline Placement bandPlacement (juce::AudioProcessorValueTreeState& s, int band)
{
    return (Placement) std::clamp ((int) s.getRawParameterValue (bandIds (band).place)->load(), 0, 4);
}

// A band's settings as the engine sees them.
inline BandParams bandParamsOf (juce::AudioProcessorValueTreeState& s, int band)
{
    return readParams (s).bands[(size_t) band];
}

// Puts a new band into the first free slot (or `preferredSlot`, if that one is free) with every other control of the slot
// back at its default. Returns the slot, or -1 when all are in use.
inline int addBand (juce::AudioProcessorValueTreeState& s, BandType type, double freqHz, double gainDb = 0.0, int preferredSlot = -1)
{
    int slot = -1;
    if (preferredSlot >= 0 && preferredSlot < maxBands && ! isBandOn (s, preferredSlot))
        slot = preferredSlot;
    for (int i = 0; slot < 0 && i < maxBands; ++i)
        if (! isBandOn (s, i))
            slot = i;
    if (slot < 0)
        return -1;

    const auto& ids = bandIds (slot);
    for (auto* pid : { &ids.place, &ids.q, &ids.slope, &ids.dyn, &ids.dynMode, &ids.threshold, &ids.ratio, &ids.range, &ids.attack, &ids.release, &ids.listen })
        if (auto* param = s.getParameter (*pid))
            setParameter (*param, param->getDefaultValue());

    setParameterValue (s, ids.type, (float) (int) type);
    setParameterValue (s, ids.freq, (float) std::clamp (freqHz, minHz, maxHz));
    setParameterValue (s, ids.gain, hasGain (type) ? (float) std::clamp (gainDb, (double) minGainDb, (double) maxGainDb) : 0.0f);
    setParameterValue (s, ids.on, 1.0f);
    return slot;
}

inline void removeBand (juce::AudioProcessorValueTreeState& s, int band) { setParameterValue (s, bandIds (band).on, 0.0f); }

// Bands handed over as text by another plugin of the repository (pdsp::link, e.g. Themis' COPY), each into the next free slot.
// A harmonic-fill band is not an equaliser band and is left out. Returns the slots used; `skipped` counts what did not fit.
inline std::vector<int> pasteBands (juce::AudioProcessorValueTreeState& s, const juce::String& text, int& skipped)
{
    std::vector<int> slots;
    skipped = 0;
    std::vector<pdsp::LinkBand> bands;
    if (! pdsp::link::decode (text.toStdString(), bands))
        return slots;
    using K = pdsp::LinkBand::Kind;
    for (const auto& b : bands)
    {
        if (b.kind == K::fill)
        {
            ++skipped;
            continue;
        }
        const auto type = b.kind == K::lowShelf ? BandType::lowShelf : b.kind == K::highShelf ? BandType::highShelf
                        : b.kind == K::lowCut ? BandType::lowCut : b.kind == K::highCut ? BandType::highCut : BandType::bell;
        const int slot = addBand (s, type, b.freq, b.gainDb);
        if (slot < 0)
        {
            ++skipped;
            continue;
        }
        const auto& ids = bandIds (slot);
        setParameterValue (s, ids.place, (float) (int) b.place); // the same order: stereo, left, right, mid, side
        if (isCut (type))
            setParameterValue (s, ids.slope, (float) std::clamp ((int) std::lround (b.slopeDbPerOct / 12.0) - 1, 0, 3));
        else
            setParameterValue (s, ids.q, (float) b.q);
        slots.push_back (slot);
    }
    return slots;
}

// The spectrum shape another plugin of the repository handed over with its bands (a "curve" line of pdsp::link, e.g. Themis'
// Copy EQ: its target curve). False, and `out` untouched, when the text holds none.
inline bool pasteReference (const juce::String& text, Shape& out, std::string& name)
{
    std::vector<pdsp::LinkBand> ignored;
    pdsp::LinkCurve curve;
    if (! pdsp::link::decode (text.toStdString(), ignored, &curve) || ! shapeFromCurve (curve, out))
        return false;
    name = curve.name;
    return true;
}

inline int freeBandSlots (juce::AudioProcessorValueTreeState& s)
{
    int n = 0;
    for (int i = 0; i < maxBands; ++i)
        n += isBandOn (s, i) ? 0 : 1;
    return n;
}

// Adds a stereo band for each of a match's bands, in the next free slots. Returns the slots used.
inline std::vector<int> addMatchedBands (juce::AudioProcessorValueTreeState& s, const std::vector<match::Band>& bands)
{
    std::vector<int> slots;
    for (const auto& b : bands)
    {
        const int slot = addBand (s, b.type, b.freqHz, b.gainDb);
        if (slot < 0)
            break;
        setParameterValue (s, bandIds (slot).q, (float) b.q);
        slots.push_back (slot);
    }
    return slots;
}

// What the stereo bands that are on already do, added to a shape (and the shape levelled again), so that matching a track that
// has been equalised already asks only for what is still missing. Bands on one channel, a mid or a side, and dynamics, are not counted.
inline void addExistingCurve (juce::AudioProcessorValueTreeState& s, double sampleRate, Shape& shape)
{
    std::array<double, ProfileGrid::kBands> total {};
    for (int i = 0; i < maxBands; ++i)
    {
        auto b = bandParamsOf (s, i);
        if (! b.on || b.place != Placement::stereo)
            continue;
        for (int k = 0; k < ProfileGrid::kBands; ++k)
            total[(size_t) k] += bandResponseDb (b, 0.0, sampleRate, ProfileGrid::centreHz (k));
    }
    double anchor = 0.0;
    int n = 0;
    for (int k = 0; k < ProfileGrid::kBands; ++k)
    {
        if (shape[(size_t) k] <= kNoLevelDb + 1.0f)
            continue;
        shape[(size_t) k] += (float) total[(size_t) k];
        if (ProfileGrid::inAnchor (k))
        {
            anchor += shape[(size_t) k];
            ++n;
        }
    }
    for (auto& v : shape)
        if (v > kNoLevelDb + 1.0f)
            v -= (float) (anchor / std::max (n, 1));
}

inline void applyPreset (juce::AudioProcessorValueTreeState& s, int index)
{
    const auto& list = presets();
    if (! juce::isPositiveAndBelow (index, (int) list.size()))
        return;

    // Start from defaults so presets don't inherit leftovers from the previous one.
    for (auto* param : s.processor.getParameters())
        if (auto* ranged = dynamic_cast<juce::RangedAudioParameter*> (param))
            if (! isGlobalParameter (ranged->paramID))
                setParameter (*ranged, ranged->getDefaultValue());

    for (auto& [pid, value] : list[(size_t) index].values)
        setParameterValue (s, pid, value);
}
} // namespace iris
