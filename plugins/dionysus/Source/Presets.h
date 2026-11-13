// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 The Pantheon authors

#pragma once

#include "Parameters.h"
#include <pantheon/ui/Presets.h>

// Starting points for the sounds a saturator is most often used for. A preset sets every control, oversampling included (a hard curve needs it); what it does
// not list is back at its default. AUTO GAIN is on by default, so a preset can be judged at the level it started at: turn it off to hear the drive's loudness too.
namespace dionysus
{
inline const std::vector<ui::Preset>& presets()
{
    // The curve is a choice: 0 Tape, 1 Tube, 2 Transformer, 3 Diode, 4 Fold, 5 Clip. Oversampling: 0 Off, 1 2x, 2 4x, 3 8x.
    static const std::vector<ui::Preset> list
    {
        { "Init", {}, "The starting point: 12 dB of drive into the tape curve, no bias, no tilt, 100% mix, AUTO GAIN on, 2x oversampling." },
        { "Tape Warmth", { { id::drive, 8.0f }, { id::shape, 0.0f }, { id::tone, -8.0f }, { id::oversampling, 1.0f } },
          "Gentle odd harmonics and a little top rolled off: warmth on a bus or a mix. Judge it with AUTO GAIN on, at the same level." },
        { "Tube Glow", { { id::drive, 10.0f }, { id::shape, 1.0f }, { id::bias, 20.0f }, { id::oversampling, 1.0f } },
          "A soft onset and some even harmonics from the bias: a rounder, thicker vocal or synth. More drive gets harsher, not warmer." },
        { "Transformer Thick", { { id::drive, 12.0f }, { id::shape, 2.0f }, { id::tone, -5.0f }, { id::oversampling, 2.0f } },
          "A cubic curve with a strong 3rd harmonic: weight for drums or a bass. At 12 dB it is dense; lower DRIVE for a lighter touch." },
        { "Parallel Grit", { { id::drive, 22.0f }, { id::shape, 3.0f }, { id::mix, 30.0f }, { id::oversampling, 2.0f } },
          "Hard, uneven saturation blended under the dry sound: bite that keeps the transients. Set MIX by ear; DRIVE is only the edge." },
        { "Bass Harmonics", { { id::drive, 14.0f }, { id::shape, 1.0f }, { id::tone, -25.0f }, { id::mix, 45.0f }, { id::oversampling, 1.0f } },
          "Harmonics on a bass or an 808 so it can be heard on small speakers. The tilt keeps the added highs down." },
        { "Fold Fuzz", { { id::drive, 24.0f }, { id::shape, 4.0f }, { id::mix, 60.0f }, { id::oversampling, 3.0f } },
          "Wave-folding for a synth or a guitar: peaks turn back on themselves. It is distortion, not warmth; 8x oversampling holds the aliasing down." },
        { "Hard Clip Edge", { { id::drive, 10.0f }, { id::shape, 5.0f }, { id::oversampling, 2.0f } },
          "Peaks cut flat, as a clipper does: edge and level on drums. It is not a limiter (Aegis limits) and it aliases without oversampling." },
    };
    return list;
}
} // namespace dionysus
