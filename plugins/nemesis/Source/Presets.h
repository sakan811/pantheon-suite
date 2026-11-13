// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 The Pantheon authors

#pragma once

#include "Parameters.h"
#include <pantheon/ui/Presets.h>

// Starting points for the voices and jobs a de-esser is most often used on. A preset sets every control: what it does not list is back at its default. The
// RELATIVE thresholds are how far under the whole sound the band may be, so they hold from a quiet recording to a loud one; still, find the s with LISTEN
// and move FREQ onto it, then set THRESH where the reduction meter moves on the s and not on the rest.
namespace nemesis
{
inline const std::vector<ui::Preset>& presets()
{
    static const std::vector<ui::Preset> list
    {
        { "Init", {}, "The starting point: 6.5 kHz, an absolute threshold of -30 dB, 8 dB of range, no look-ahead." },
        { "Male Vocal", { { id::freq, 5500.0f }, { id::q, 1.5f }, { id::detection, 1.0f }, { id::threshold, -18.0f }, { id::range, 8.0f }, { id::release, 40.0f }, { id::lookahead, 2.0f } },
          "A man's s sits at 4 to 7 kHz. Relative detection, 1 ms of look-ahead; check FREQ with LISTEN. It is no cure for a harsh recording." },
        { "Female Vocal", { { id::freq, 7500.0f }, { id::q, 1.5f }, { id::detection, 1.0f }, { id::threshold, -18.0f }, { id::range, 8.0f }, { id::release, 35.0f }, { id::lookahead, 2.0f } },
          "A woman's s sits at 6 to 10 kHz. Relative detection, 1 ms of look-ahead; check FREQ with LISTEN. It is no cure for a harsh recording." },
        { "Gentle Touch", { { id::freq, 7000.0f }, { id::q, 1.0f }, { id::detection, 1.0f }, { id::threshold, -22.0f }, { id::range, 4.0f }, { id::release, 80.0f }, { id::lookahead, 2.0f } },
          "Wide and light, 4 dB at most: takes the edge off a bright voice or a mix bus without a trace. It will not stop a real sibilance problem." },
        { "Hard Sibilance", { { id::freq, 6500.0f }, { id::q, 2.0f }, { id::threshold, -28.0f }, { id::range, 12.0f }, { id::release, 30.0f }, { id::lookahead, 3.0f } },
          "For a voice with a piercing s: a narrow band, up to 12 dB, 2 ms of look-ahead. Past 10 dB it can lisp: raise THRESH if it does." },
        { "Broadcast Voice", { { id::freq, 6000.0f }, { id::q, 1.2f }, { id::mode, 1.0f }, { id::detection, 1.0f }, { id::threshold, -20.0f }, { id::range, 6.0f }, { id::release, 60.0f }, { id::lookahead, 2.0f } },
          "Wideband: the whole voice is turned down with the s, like a compressor keyed to the s. Even, but it ducks the voice a little." },
        { "Find the S", { { id::freq, 6500.0f }, { id::q, 3.0f }, { id::listen, 1.0f } },
          "LISTEN on with a narrow band: sweep FREQ until the hiss of the s is loudest, then pick another preset. Switch LISTEN off before you bounce." },
    };
    return list;
}
} // namespace nemesis
