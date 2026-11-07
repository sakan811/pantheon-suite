// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 The Pantheon authors

#pragma once

#include "Band.h"

namespace iris
{
// The six regions for soloing are the six main bands of music, split by the shared splitter (shared/dsp MusicBands.h): every
// region goes through the all-pass of each crossover above it, so any combination of regions adds up to the input's own
// magnitude response. Iris moves the crossovers (xo0 ... xo4); the other plugins keep them at the bands' edges.
using RegionSplitter = pdsp::BandSplitter;

inline const char* regionName (int region) noexcept
{
    return pdsp::bands::kBands[(size_t) std::clamp (region, 0, numRegions - 1)].name;
}
} // namespace iris
