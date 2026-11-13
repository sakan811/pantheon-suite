// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 The Pantheon authors

#pragma once

#include "Parameters.h"
#include <pantheon/ui/Presets.h>

// Starting points for the jobs a compressor is most often used for. A preset sets every control: what it does not list is back at its default. The
// thresholds are for a source around -18 dBFS: set THRESH where the reduction meter first moves.
namespace hera
{
inline const std::vector<ui::Preset>& presets()
{
    static const std::vector<ui::Preset> list
    {
        { "Init", {}, "The starting point: 2:1 with the threshold at -18 dB, RMS detection, no parallel mix, no look-ahead." },
        { "Vocal Level", { { id::threshold, -22.0f }, { id::ratio, 3.0f }, { id::attack, 6.0f }, { id::release, 90.0f }, { id::knee, 9.0f }, { id::hpf, 120.0f }, { id::autoMakeup, 1.0f } },
          "Evens a vocal's level: gentle 3:1, a soft knee, the low end kept out of the detector. For sibilance use a de-esser." },
        { "Drum Punch", { { id::threshold, -16.0f }, { id::ratio, 4.0f }, { id::attack, 30.0f }, { id::release, 70.0f }, { id::knee, 3.0f }, { id::detector, 0.0f }, { id::autoMakeup, 1.0f } },
          "A slow attack lets the hit through and the tail comes up: punch for a kick, a snare or a drum bus. It is not a limiter." },
        { "Bus Glue", { { id::threshold, -18.0f }, { id::ratio, 2.0f }, { id::attack, 30.0f }, { id::autoRelease, 1.0f }, { id::knee, 10.0f }, { id::hpf, 80.0f }, { id::autoMakeup, 1.0f } },
          "A light, slow squeeze on a group so its parts sit together. 1 to 3 dB of reduction is the point; more and it pumps." },
        { "Bass Control", { { id::threshold, -20.0f }, { id::ratio, 4.0f }, { id::attack, 25.0f }, { id::release, 140.0f }, { id::knee, 6.0f }, { id::autoMakeup, 1.0f } },
          "Holds a bass line at one level so it does not jump from note to note. It does not fix a note that is always too loud: use an equaliser." },
        { "Parallel Smash", { { id::threshold, -34.0f }, { id::ratio, 8.0f }, { id::attack, 1.0f }, { id::release, 110.0f }, { id::knee, 0.0f }, { id::mix, 35.0f }, { id::detector, 0.0f }, { id::autoMakeup, 1.0f } },
          "Hard compression blended under the dry sound: the body comes up and the transients stay. Watch the mix, not the meter." },
        { "Peak Catcher", { { id::threshold, -12.0f }, { id::ratio, 10.0f }, { id::attack, 0.5f }, { id::release, 60.0f }, { id::knee, 2.0f }, { id::detector, 0.0f }, { id::lookahead, 2.0f } },
          "Fast, with 2 ms of look-ahead: a peak is turned down before it arrives. It delays the sound (the host compensates); it is not a limiter." },
        { "Sidechain Duck", { { id::threshold, -32.0f }, { id::ratio, 6.0f }, { id::attack, 1.0f }, { id::release, 140.0f }, { id::knee, 0.0f }, { id::detector, 0.0f }, { id::external, 1.0f } },
          "Put the kick on the SIDECHAIN input and this track ducks when it hits. Needs a sidechain routed in the host; with none it listens to itself." },
    };
    return list;
}
} // namespace hera
