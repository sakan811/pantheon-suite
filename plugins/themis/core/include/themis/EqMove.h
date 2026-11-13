// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 The Pantheon authors

#pragma once

#include <vector>

namespace themis
{
// A change Themis suggests that an equaliser or a harmonic generator can make as it stands: what kind of band, where, how much
// and on which part of the stereo image. The plugin layer hands these to the other plugins of the repository as text
// (pdsp::link in shared/dsp); here they are plain data, so the advice that produces them is tested without a UI.
struct EqMove
{
    enum class Kind { bell, lowShelf, highShelf, lowCut, highCut, harmonicFill };
    enum class Place { stereo, mid, side };

    Kind kind = Kind::bell;
    Place place = Place::stereo;
    float freqHz = 1000.0f, gainDb = 0.0f, q = 0.7071f;
    float loHz = 0.0f, hiHz = 0.0f; // harmonicFill: the range whose harmonics are filled in
};

using EqMoves = std::vector<EqMove>;

// A change Themis suggests that a limiter can make as it stands: where its ceiling should be (an absolute level, dBTP) and/or how far its input
// gain should move (a change in dB, negative to turn it down). Handed to Aegis the same way as the bands (pdsp::link in shared/dsp).
struct LimiterMove
{
    bool setsCeiling = false, changesGain = false;
    float ceilingDb = -1.0f, gainChangeDb = 0.0f;

    bool active() const { return setsCeiling || changesGain; }
};
} // namespace themis
