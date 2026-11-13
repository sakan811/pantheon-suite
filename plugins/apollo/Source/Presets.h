// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 The Pantheon authors

#pragma once

#include "Parameters.h"
#include <pantheon/ui/Presets.h>

// Starting points for the jobs an equaliser is most often used for. A preset sets the bands it needs; the others go back to their defaults (all at 0 dB, so they do
// nothing). PHASE and OUTPUT are kept: they are how the equaliser works, not what it does to this sound. Every number here is a place to begin: move the node until
// it sounds right, and cut before you boost.
namespace apollo
{
inline const std::vector<ui::Preset>& presets()
{
    using Values = std::vector<std::pair<juce::String, float>>;
    // A band: switched on, its frequency, gain and Q; `type` and `slope` are the choices of typeNames and slopeNames, `target` of targetNames (0 Stereo, 1 Mid, 2 Side).
    const auto band = [] (int i, double hz, double db, double q, Target target = Target::stereo)
    {
        const auto& ids = bandIds (i);
        return Values { { ids.on, 1.0f }, { ids.freq, (float) hz }, { ids.gain, (float) db }, { ids.q, (float) q }, { ids.target, (float) (int) target } };
    };
    // A high-pass or a low-pass (slope: 0 = 12, 1 = 24, 2 = 36, 3 = 48 dB per octave).
    const auto pass = [] (int i, Type type, double hz, int slope, Target target = Target::stereo)
    {
        const auto& ids = bandIds (i);
        return Values { { ids.on, 1.0f }, { ids.type, (float) (int) type }, { ids.freq, (float) hz }, { ids.slope, (float) slope }, { ids.target, (float) (int) target } };
    };
    const auto join = [] (std::initializer_list<Values> parts)
    {
        Values all;
        for (const auto& part : parts)
            all.insert (all.end(), part.begin(), part.end());
        return all;
    };
    // The default bands: 1 high-pass, 2 low shelf, 3 to 6 bells (250 Hz, 1 kHz, 3 kHz, 6 kHz), 7 high shelf, 8 low-pass.
    static const std::vector<ui::Preset> list
    {
        { "Init", {}, "The starting point: eight bands at 0 dB, the high-pass and low-pass off. The sound goes through as it came." },
        { "Low Cut", pass (0, Type::highPass, 80.0, 1),
          "A 24 dB/oct high-pass at 80 Hz: rumble, handling noise and sub you do not need. Not for a kick or a bass: move it down for them." },
        { "Vocal Presence", join ({ pass (0, Type::highPass, 90.0, 1), band (2, 250.0, -2.5, 1.2), band (4, 3500.0, 2.5, 0.9), band (6, 10000.0, 2.0, 0.7) }),
          "Clears the mud at 250 Hz, adds presence at 3.5 kHz and a little air. A start for a vocal; it cannot fix a bad recording." },
        { "Mud Cut", band (2, 300.0, -3.0, 1.2),
          "A bell cut at 300 Hz, the boxy zone of vocals, guitars and keys. Sweep FREQ to find where it is worst; cut before you boost." },
        { "Air Lift", band (6, 12000.0, 2.5, 0.7),
          "A gentle high shelf from 12 kHz: air and sheen on a mix or a master. It lifts the noise with the highs." },
        { "Bass Weight", join ({ band (1, 80.0, 3.0, 0.7), band (2, 250.0, -2.0, 1.0) }),
          "A low shelf at 80 Hz and a dip at 250 Hz: more weight, less boom. Check it on small speakers." },
        { "Master Smile", join ({ band (1, 70.0, 1.2, 0.7), band (3, 450.0, -1.0, 0.7), band (6, 9000.0, 1.5, 0.7) }),
          "A very light lift of the lows and the highs with a small dip between: for a finished mix. LINEAR phase keeps the image intact." },
        { "Sides Open Top", join ({ pass (0, Type::highPass, 120.0, 1, Target::side), band (6, 8000.0, 2.0, 0.7, Target::side) }),
          "High-passes the sides at 120 Hz (a mono low end) and lifts their air: a wider top without a wider bass. Check it in mono." },
        { "Telephone", join ({ pass (0, Type::highPass, 500.0, 2), pass (7, Type::lowPass, 3500.0, 2) }),
          "A band-pass from 500 Hz to 3.5 kHz: an old radio or a phone. It is an effect, not a repair." },
    };
    return list;
}

// A preset leaves how the equaliser works alone.
inline bool keepsParameter (const juce::String& pid) { return pid == id::phase || pid == id::out; }
} // namespace apollo
