// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 The Pantheon authors

#pragma once

#include "Parameters.h"
#include <pantheon/ui/Presets.h>

// Starting points for the jobs a stereo imager is most often used for. A preset sets the widths, MID, SIDE and MONO BELOW; what it does not list is back at its
// default. BALANCE and OUTPUT are kept (they are the level of the sound, not its image). Always listen in mono after widening.
namespace castor
{
inline const std::vector<ui::Preset>& presets()
{
    const auto widths = [] (std::initializer_list<float> values)
    {
        std::vector<std::pair<juce::String, float>> v;
        int band = 0;
        for (const float w : values)
            v.push_back ({ id::width (band++), w });
        return v;
    };
    const auto with = [] (std::vector<std::pair<juce::String, float>> a, std::initializer_list<std::pair<juce::String, float>> more)
    {
        a.insert (a.end(), more.begin(), more.end());
        return a;
    };
    static const std::vector<ui::Preset> list
    {
        { "Init", {}, "The starting point: every width at 100%, MID and SIDE at 0 dB, no mono bass: the sound as it is." },
        { "Mono Bass", { { id::monoBelow, 120.0f } },
          "Takes the sides away below 120 Hz: the bass is in the middle, safe for mono playback, club systems and vinyl. Nothing above is touched." },
        { "Club Safe", with (widths ({ 0, 40, 100, 100, 100, 100 }), { { id::monoBelow, 150.0f } }),
          "The sub in mono, the bass nearly mono, the rest as it is: for club systems. It widens nothing." },
        { "Widen Highs", widths ({ 100, 100, 100, 110, 130, 160 }),
          "Opens up the top: the sides grow from the midrange up and the low end stays put. Check in mono: wide highs can thin out." },
        { "Wide Master", with (widths ({ 100, 100, 110, 120, 130, 150 }), { { id::monoBelow, 80.0f } }),
          "A broad, safe widening for a master: mono below 80 Hz and up to 150% in the highs. Past 150% the image can fall apart in mono." },
        { "Narrow Mix", widths ({ 70, 70, 70, 70, 70, 70 }),
          "Pulls the whole image in to 70%: a mix that is too wide or smeared. It cannot separate elements that sit on top of each other." },
        { "Vocal Forward", { { id::mid, 1.5f }, { id::side, -1.5f } },
          "More middle and less sides: a lead vocal or a kick comes forward and the width recedes a little. Levels only; the widths are 100%." },
    };
    return list;
}

// A preset leaves the level of the sound alone.
inline bool keepsParameter (const juce::String& pid) { return pid == id::balance || pid == id::out; }
} // namespace castor
