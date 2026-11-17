// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 The Pantheon authors

#pragma once

#include <array>
#include <cstddef>

namespace themis
{
// The loudness a master can be delivered at, in LUFS: the one list the master plan judges the integrated loudness against
// and the plugin's "Deliver for" choice is built from (plugin/Source/Parameters.h names them). Append only: the choice is
// saved in sessions as an index into this list.
namespace delivery
{
inline constexpr float kLufs[] = { -14.0f, -9.0f, -18.0f, -16.0f, -23.0f };
inline constexpr int kCount = static_cast<int> (sizeof (kLufs) / sizeof (kLufs[0]));
inline constexpr float kStreamingLufs = kLufs[0]; // what the big streaming services normalise to: a louder master is turned down

// Where one master's integrated loudness sits against one target: `offsetDb` = integrated - target, so negative is quieter
// than the target and positive louder (the sign reads the way the sentence does: "2.3 dB quieter than -14").
struct Offset
{
    float targetLufs = 0.0f;
    float offsetDb = 0.0f;
};

// The offset to every delivery target, in the order of kLufs. Pure: the numbers come from the integrated loudness alone.
inline std::array<Offset, static_cast<size_t> (kCount)> offsetsOf (float integratedLufs)
{
    std::array<Offset, static_cast<size_t> (kCount)> out {};
    for (size_t i = 0; i < out.size(); ++i)
    {
        out[i].targetLufs = kLufs[i];
        out[i].offsetDb = integratedLufs - kLufs[i];
    }
    return out;
}
} // namespace delivery
} // namespace themis
