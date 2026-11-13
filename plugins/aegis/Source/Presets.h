// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 The Pantheon authors

#pragma once

#include "Parameters.h"
#include <pantheon/ui/Presets.h>

// Starting points for the jobs a true-peak limiter is most often used for. A preset sets every control: what it does not list is back at its default. GAIN is
// set by ear and by the reduction meter, not by the preset: raise it until the sound is as loud as it should be and no further than the music can take.
namespace aegis
{
inline const std::vector<ui::Preset>& presets()
{
    // LOOK-AHEAD is a choice: 0 = 0.5 ms, 1 = 1 ms, 2 = 2 ms, 3 = 5 ms.
    static const std::vector<ui::Preset> list
    {
        { "Init", {}, "The starting point: no gain, a ceiling of -1 dBTP, a release of 100 ms, 2 ms of look-ahead, true peak on." },
        { "Streaming Master", { { id::gain, 3.0f }, { id::ceiling, -1.0f }, { id::release, 150.0f }, { id::lookahead, 2.0f } },
          "A few dB up to a ceiling of -1 dBTP, which streaming services and lossy codecs ask for. Set GAIN by ear, not by the number." },
        { "Loud Master", { { id::gain, 6.0f }, { id::ceiling, -0.5f }, { id::release, 60.0f }, { id::lookahead, 3.0f } },
          "More level with a short release and 5 ms of look-ahead. Past 6 dB of reduction it sounds squashed, and bass distorts under 40 ms." },
        { "Gentle Peak Catch", { { id::gain, 1.5f }, { id::ceiling, -1.0f }, { id::release, 250.0f }, { id::lookahead, 3.0f } },
          "Only the stray peaks: little gain and a long release, the most transparent setting. For a master that is nearly loud enough." },
        { "Broadcast Safe", { { id::ceiling, -2.0f }, { id::release, 200.0f } },
          "A ceiling of -2 dBTP, which some broadcast specs ask for (check the destination's). No gain: set GAIN to taste." },
        { "Tracking Safety", { { id::ceiling, -3.0f }, { id::lookahead, 0.0f } },
          "A net for a bus or a take: -3 dBTP and the shortest delay (0.5 ms). It stops a surprise peak; it is not for the final master." },
    };
    return list;
}
} // namespace aegis
