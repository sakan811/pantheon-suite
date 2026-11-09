// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 The Pantheon authors

#pragma once

#include "DSP/Engine.h"
#include <pantheon/dsp/BandLink.h>
#include <juce_audio_processors/juce_audio_processors.h>

namespace hep
{
namespace id
{
    inline constexpr const char* inGain = "inGain";
    inline constexpr const char* outGain = "outGain";
    inline constexpr const char* mix = "mix";
    inline constexpr const char* stereo = "stereo";
    inline constexpr const char* oversampling = "oversampling";
    inline constexpr const char* delta = "delta";
    inline constexpr const char* match = "match";

    // Saturate bands share one character.
    inline constexpr const char* curve = "curve";
    inline constexpr const char* even = "even";
}

// The parameters of one band. Ids are "b1_lo" ... "b6_rebuild": the strings are built once and kept,
// so the audio thread can look parameters up without allocating.
struct BandIds
{
    juce::String on, mode, lo, hi;   // every band
    juce::String drive;              // saturate
    juce::String amount, tone, adapt, rebuild; // fill
    juce::String character;                    // fill: how the harmonics are made (Warm, Clean, Octave)
};

inline const BandIds& bandIds (int band)
{
    static const auto table = []
    {
        std::array<BandIds, maxBands> t;
        for (int i = 0; i < maxBands; ++i)
        {
            const auto name = [i] (const char* field) { return "b" + juce::String (i + 1) + "_" + field; };
            t[(size_t) i] = { name ("on"), name ("mode"), name ("lo"), name ("hi"), name ("drive"),
                              name ("amount"), name ("tone"), name ("adapt"), name ("rebuild"),
                              name ("character") };
        }
        return t;
    }();
    return table[(size_t) std::clamp (band, 0, maxBands - 1)];
}

constexpr float offDb = -60.0f;       // bottom of the Rebuild control; treated as off
constexpr float fillOffDb = -30.0f;   // bottom of a fill band's Amount; treated as off

// True when a dB control sits at its minimum (within one display step), i.e. the feature is off.
inline bool isOff (float db, float floorDb = offDb) noexcept { return db <= floorDb + 0.05f; }

// Linear gain for a dB control, or 0 when it is switched off at the bottom of its range.
inline double gainOrOff (float db, float floorDb = offDb) noexcept
{
    return isOff (db, floorDb) ? 0.0 : (double) juce::Decibels::decibelsToGain (db);
}

// Session-wide controls: presets leave these alone.
inline bool isGlobalParameter (const juce::String& paramId)
{
    using namespace id;
    for (auto* global : { inGain, outGain, mix, stereo, oversampling, delta, match })
        if (paramId == global)
            return true;
    return false;
}

// What a band looks like before anyone touches it: band 1 starts as a gentle full-range saturator
// (the plugin does something useful on first load), the rest are free slots.
struct BandDefaults { bool on; float lo, hi; };
inline constexpr BandDefaults bandDefaults (int band) noexcept
{
    return band == 0 ? BandDefaults { true, 20.0f, 20000.0f } : BandDefaults { false, 100.0f, 1000.0f };
}
inline constexpr float defaultDriveDb = 4.0f, defaultAmountDb = -9.0f;

inline juce::AudioProcessorValueTreeState::ParameterLayout createLayout()
{
    using namespace juce;
    using Float = AudioParameterFloat;
    using Attr = AudioParameterFloatAttributes;
    std::vector<std::unique_ptr<RangedAudioParameter>> p;

    const auto dbText = [] (float floorDb) { return [floorDb] (float v, int) { return isOff (v, floorDb) ? String ("Off") : String (v, 1) + " dB"; }; };
    const auto dbValue = [] (float floorDb) { return [floorDb] (const String& s) { return s.trim().startsWithIgnoreCase ("off") ? floorDb : s.getFloatValue(); }; };
    const auto hzText = [] (float v, int) { return v < 1000.0f ? String (roundToInt (v)) + " Hz" : String (v / 1000.0f, 2) + " kHz"; };
    const auto hzValue = [] (const String& s)
    {
        const auto t = s.trim();
        const float v = t.getFloatValue();
        return t.containsIgnoreCase ("k") ? v * 1000.0f : v;
    };
    const auto pctText = [] (float v, int) { return String (roundToInt (v)) + " %"; };

    const auto freq = [] (float lo, float hi, float centre)
    {
        NormalisableRange<float> r (lo, hi, 0.1f);
        r.setSkewForCentre (centre);
        return r;
    };
    const auto dbAttr = [&] (float floorDb) { return Attr().withStringFromValueFunction (dbText (floorDb)).withValueFromStringFunction (dbValue (floorDb)); };
    const auto hzAttr = [&] { return Attr().withStringFromValueFunction (hzText).withValueFromStringFunction (hzValue); };
    const auto pctAttr = [&] { return Attr().withStringFromValueFunction (pctText); };
    const auto gainAttr = [] { return Attr().withStringFromValueFunction ([] (float v, int) { return String (v, 1) + " dB"; }); };

    // Parameters added after the first release carry a higher version, as hosts expect.
    const auto F = [&] (const String& pid, const String& name, NormalisableRange<float> r, float def, Attr a, int version = 1)
    {
        p.push_back (std::make_unique<Float> (ParameterID { pid, version }, name, r, def, a));
    };
    const auto Choice = [&] (const String& pid, const String& name, StringArray items, int def, int version = 1)
    {
        p.push_back (std::make_unique<AudioParameterChoice> (ParameterID { pid, version }, name, items, def));
    };
    const auto Bool = [&] (const String& pid, const String& name, bool def, int version = 1)
    {
        p.push_back (std::make_unique<AudioParameterBool> (ParameterID { pid, version }, name, def));
    };

    // Global
    F (id::inGain,  "Input Gain",  { -12.0f, 12.0f, 0.1f }, 0.0f, gainAttr());
    F (id::outGain, "Output Gain", { -12.0f, 12.0f, 0.1f }, 0.0f, gainAttr());
    F (id::mix,     "Mix",         { 0.0f, 100.0f, 1.0f }, 100.0f, pctAttr());
    Choice (id::stereo, "Stereo Mode", { "Stereo (L/R)", "Mid/Side" }, 0);
    Choice (id::oversampling, "Oversampling", { "Off", "2x", "4x", "8x" }, 2);
    Bool (id::delta, "Delta", false);
    Bool (id::match, "Gain Match", false);

    // Character shared by every saturate band
    Choice (id::curve, "Curve", { "Tanh", "Atan", "Soft" }, 0);
    F (id::even, "Even Harmonics", { 0.0f, 100.0f, 1.0f }, 25.0f, pctAttr());

    // Bands
    constexpr int bandVersion = 2;
    for (int i = 0; i < maxBands; ++i)
    {
        const auto& ids = bandIds (i);
        const auto d = bandDefaults (i);
        const auto label = [i] (const char* what) { return "Band " + String (i + 1) + " " + what; };

        Bool (ids.on, label ("On"), d.on, bandVersion);
        Choice (ids.mode, label ("Mode"), { "Saturate", "Harmonic Fill" }, 0, bandVersion);
        F (ids.lo, label ("Low"),  freq (20.0f, 20000.0f, 1000.0f), d.lo, hzAttr(), bandVersion);
        F (ids.hi, label ("High"), freq (20.0f, 20000.0f, 1000.0f), d.hi, hzAttr(), bandVersion);
        F (ids.drive,   label ("Drive"),   { 0.0f, 24.0f, 0.1f }, i == 0 ? 3.0f : defaultDriveDb, gainAttr(), bandVersion);
        F (ids.amount,  label ("Amount"),  { fillOffDb, 6.0f, 0.1f }, defaultAmountDb, dbAttr (fillOffDb), bandVersion);
        F (ids.tone,    label ("Tone"),    { 0.0f, 100.0f, 1.0f }, 50.0f, pctAttr(), bandVersion);
        F (ids.adapt,   label ("Adapt"),   { 0.0f, 100.0f, 1.0f }, 100.0f, pctAttr(), bandVersion);
        F (ids.rebuild, label ("Rebuild"), { offDb, 6.0f, 0.1f }, offDb, dbAttr (offDb), bandVersion);
        Choice (ids.character, label ("Character"), { "Warm", "Clean", "Octave" }, 0, 3); // added after the first release, so version 3; Warm = how every earlier session sounds
    }

    return { p.begin(), p.end() };
}

// Reads the current parameter values into the plain struct the engine consumes.
inline EngineParams readParams (juce::AudioProcessorValueTreeState& s)
{
    const auto v = [&] (const char* pid) { return s.getRawParameterValue (pid)->load(); };
    const auto vb = [&] (const juce::String& pid) { return s.getRawParameterValue (pid)->load(); };
    const auto gain = [] (float db) { return (double) juce::Decibels::decibelsToGain (db); };

    EngineParams e;
    e.inGainDb = v (id::inGain);
    e.outGainDb = v (id::outGain);
    e.mix = v (id::mix) * 0.01f;
    e.midSide = (int) v (id::stereo) == 1;
    e.osMode = (int) v (id::oversampling);
    e.delta = v (id::delta) > 0.5f;
    e.match = v (id::match) > 0.5f;
    e.curve = (int) v (id::curve);
    e.bias = 0.8 * v (id::even) * 0.01;

    for (int i = 0; i < maxBands; ++i)
    {
        const auto& ids = bandIds (i);
        auto& b = e.bands[(size_t) i];
        b.on = vb (ids.on) > 0.5f;
        b.mode = (BandMode) (int) vb (ids.mode);
        b.lo = vb (ids.lo);
        b.hi = vb (ids.hi);
        b.drive = gain (vb (ids.drive));

        // Amount is the harmonics' total energy relative to the band's (the tone weights carry
        // unit energy), which is also the level the adaptive fill aims to keep above the band.
        auto& f = b.fill;
        f.extGain = gainOrOff (vb (ids.amount), fillOffDb);
        harmonicsForTone (vb (ids.tone) * 0.01, f.harmonic);
        f.target = f.extGain * f.extGain;
        f.adapt = vb (ids.adapt) * 0.01;
        f.ceiling = 10.0 * effectiveRange (BandMode::fill, b.lo, b.hi).hi;
        f.resGain = gainOrOff (vb (ids.rebuild));
        f.character = (Character) std::clamp ((int) vb (ids.character), 0, 2);
        f.fill = 0.8;
    }
    return e;
}

// Sessions saved before bands existed had three fixed saturator bands and one bass engine. They
// become three saturate bands and one fill band, so an old project still sounds like itself.
inline void migrateLegacyState (juce::ValueTree& state)
{
    const auto find = [&] (const char* pid) { return state.getChildWithProperty ("id", pid); };
    if (! find ("driveLow").isValid() || find (bandIds (0).on.toRawUTF8()).isValid())
        return;

    const auto old = [&] (const char* pid, float fallback)
    {
        const auto node = find (pid);
        return node.isValid() ? (float) node.getProperty ("value") : fallback;
    };
    const auto set = [&] (const juce::String& pid, float value)
    {
        juce::ValueTree node ("PARAM");
        node.setProperty ("id", pid, nullptr);
        node.setProperty ("value", value, nullptr);
        state.appendChild (node, nullptr);
    };
    const auto saturate = [&] (int band, bool on, float lo, float hi, float driveDb)
    {
        const auto& ids = bandIds (band);
        set (ids.on, on ? 1.0f : 0.0f); set (ids.mode, 0.0f);
        set (ids.lo, lo); set (ids.hi, hi); set (ids.drive, driveDb);
    };

    const bool satOn = old ("satOn", 1.0f) > 0.5f;
    const float xoverLow = old ("xoverLow", 150.0f), xoverHigh = old ("xoverHigh", 3500.0f);
    saturate (0, satOn, 20.0f, xoverLow, old ("driveLow", 3.0f));
    saturate (1, satOn, xoverLow, xoverHigh, old ("driveMid", 3.0f));
    saturate (2, satOn, xoverHigh, 20000.0f, old ("driveHigh", 3.0f));

    // The old H2..H5 weights were relative to the fundamental; Amount is their total energy.
    double energy = 0.0;
    const char* weights[] = { "h2", "h3", "h4", "h5" };
    const float weightDefaults[] = { -6.0f, -9.0f, -15.0f, -22.0f };
    for (int k = 0; k < 4; ++k)
    {
        const float w = old (weights[k], weightDefaults[k]);
        energy += isOff (w, -48.0f) ? 0.0 : std::pow (10.0, w / 10.0);
    }
    const float extDb = old ("extAmount", offDb), resDb = old ("resAmount", offDb);
    const bool ext = ! isOff (extDb), rest = ! isOff (resDb);

    const auto& fill = bandIds (3);
    set (fill.on, ext || rest ? 1.0f : 0.0f);
    set (fill.mode, 1.0f);
    set (fill.lo, old ("fundLo", 40.0f));
    set (fill.hi, old ("fundHi", 120.0f));
    set (fill.amount, ext ? std::clamp (extDb + (float) (10.0 * std::log10 (energy + 1.0e-9)), fillOffDb + 0.1f, 6.0f)
                          : fillOffDb);
    set (fill.tone, 50.0f);
    set (fill.adapt, 0.0f); // the old engine was open-loop; keep it that way
    set (fill.rebuild, resDb);
}

using PresetValues = std::vector<std::pair<juce::String, float>>;

struct Preset
{
    const char* name;
    PresetValues values; // everything not listed falls back to default
};

inline PresetValues saturateBand (int band, float lo, float hi, float driveDb)
{
    const auto& ids = bandIds (band);
    return { { ids.on, 1 }, { ids.mode, 0 }, { ids.lo, lo }, { ids.hi, hi }, { ids.drive, driveDb } };
}

inline PresetValues fillBand (int band, float lo, float hi, float amountDb, float tone, float adapt, float rebuildDb = offDb, Character character = Character::warm)
{
    const auto& ids = bandIds (band);
    return { { ids.on, 1 }, { ids.mode, 1 }, { ids.lo, lo }, { ids.hi, hi }, { ids.amount, amountDb },
             { ids.tone, tone }, { ids.adapt, adapt }, { ids.rebuild, rebuildDb }, { ids.character, (float) (int) character } };
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
    static const std::vector<Preset> list
    {
        { "Init", {} },
        { "Master Glue",         join ({ { { curve, 0 }, { even, 20 } }, saturateBand (0, 20, 20000, 3) }) },
        { "Warm Tube",           join ({ { { curve, 0 }, { even, 65 } }, saturateBand (0, 20, 20000, 5) }) },
        { "Tape Body",           join ({ { { curve, 1 }, { even, 10 } },
                                         saturateBand (0, 40, 400, 7), saturateBand (1, 400, 3500, 4), saturateBand (2, 3500, 20000, 2),
                                         fillBand (3, 40, 100, -12, 40, 100) }) },
        { "Air Exciter",         join ({ { { curve, 2 }, { even, 35 } },
                                         saturateBand (0, 150, 3500, 2), saturateBand (1, 3500, 20000, 10) }) },
        { "Missing Fundamental", join ({ { { curve, 0 }, { even, 20 } },
                                         saturateBand (0, 20, 150, 1), saturateBand (1, 150, 3500, 2), saturateBand (2, 3500, 20000, 1),
                                         fillBand (3, 35, 110, -6, 50, 100, -9) }) },
        { "Small-Speaker Bass",  join ({ { { curve, 0 }, { even, 25 } },
                                         saturateBand (0, 20, 20000, 2),
                                         fillBand (1, 50, 160, 0, 60, 100) }) },
        { "808 Sub Fill",        join ({ { { curve, 0 }, { even, 25 } },
                                         saturateBand (0, 30, 150, 4),
                                         fillBand (1, 30, 90, -8, 15, 100) }) },
        { "Clean Sub Fill",      join ({ { { curve, 0 }, { even, 25 } },
                                         fillBand (0, 30, 90, -8, 15, 100, offDb, Character::clean) }) },
        { "Octave Air",          join ({ { { curve, 0 }, { even, 25 } },
                                         fillBand (0, 600, 2000, -12, 35, 100, offDb, Character::octave) }) },
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

inline BandMode bandMode (juce::AudioProcessorValueTreeState& s, int band)
{
    return (BandMode) (int) s.getRawParameterValue (bandIds (band).mode)->load();
}

// The range a band really covers (see effectiveRange), from its parameters.
inline Range bandRange (juce::AudioProcessorValueTreeState& s, int band)
{
    const auto& ids = bandIds (band);
    return effectiveRange (bandMode (s, band), s.getRawParameterValue (ids.lo)->load(), s.getRawParameterValue (ids.hi)->load());
}

// Puts a new band into the first free slot, centred on `centreHz` with a sensible width, and every
// other control of the slot back at its default. Uses `preferredSlot` if that one is free, else the
// first free slot. Returns the slot, or -1 when all are in use.
inline int addBand (juce::AudioProcessorValueTreeState& s, BandMode mode, double centreHz, int preferredSlot = -1)
{
    int slot = -1;
    if (preferredSlot >= 0 && preferredSlot < maxBands && ! isBandOn (s, preferredSlot))
        slot = preferredSlot;
    for (int i = 0; slot < 0 && i < maxBands; ++i)
        if (! isBandOn (s, i))
            slot = i;

    if (slot >= 0)
    {
        const int i = slot;
        const auto& ids = bandIds (i);
        for (auto* pid : { &ids.drive, &ids.amount, &ids.tone, &ids.adapt, &ids.rebuild, &ids.character })
            if (auto* param = s.getParameter (*pid))
                setParameter (*param, param->getDefaultValue());

        const double width = mode == BandMode::fill ? 1.5 : 1.8;
        const auto range = effectiveRange (mode, std::clamp (centreHz / width, 20.0, 20000.0),
                                           std::clamp (centreHz * width, 20.0, 20000.0));
        setParameterValue (s, ids.mode, (float) (int) mode);
        setParameterValue (s, ids.lo, (float) range.lo);
        setParameterValue (s, ids.hi, (float) range.hi);
        setParameterValue (s, ids.on, 1.0f);
    }
    return slot;
}

// Fill bands handed over as text by another plugin of the repository (pdsp::link, e.g. Themis' COPY on a bass item), each into
// the next free slot with its range (held to a fill band's limits by effectiveRange). Equaliser bands are not Hephaestus' and
// are left out. Returns the slots used; `skipped` counts what did not fit or was not a fill band.
inline std::vector<int> pasteBands (juce::AudioProcessorValueTreeState& s, const juce::String& text, int& skipped)
{
    std::vector<int> slots;
    skipped = 0;
    std::vector<pdsp::LinkBand> bands;
    if (! pdsp::link::decode (text.toStdString(), bands))
        return slots;
    for (const auto& b : bands)
    {
        const int slot = b.kind == pdsp::LinkBand::Kind::fill ? addBand (s, BandMode::fill, std::sqrt (b.loHz * b.hiHz)) : -1;
        if (slot < 0)
        {
            ++skipped;
            continue;
        }
        const auto range = effectiveRange (BandMode::fill, b.loHz, b.hiHz);
        setParameterValue (s, bandIds (slot).lo, (float) range.lo);
        setParameterValue (s, bandIds (slot).hi, (float) range.hi);
        slots.push_back (slot);
    }
    return slots;
}

// Switching to fill can shrink the range (see effectiveRange): write what the band really covers back.
inline void setBandMode (juce::AudioProcessorValueTreeState& s, int band, BandMode mode)
{
    const auto& ids = bandIds (band);
    setParameterValue (s, ids.mode, (float) (int) mode);
    const auto r = bandRange (s, band);
    setParameterValue (s, ids.lo, (float) r.lo);
    setParameterValue (s, ids.hi, (float) r.hi);
}

inline void removeBand (juce::AudioProcessorValueTreeState& s, int band) { setParameterValue (s, bandIds (band).on, 0.0f); }

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
}
