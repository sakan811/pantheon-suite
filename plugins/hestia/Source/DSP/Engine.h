// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 The Pantheon authors

#pragma once

#include "Analysis.h"
#include "Bus.h"
#include "Carver.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>

// The whole of one stem's signal path: measure the input in thirty bands, publish it, read what the other stems of the mix are doing, decide how much of
// each band this stem should give up, and cut it. Plain C++ (Tests.cpp includes it).
namespace hestia
{
struct EngineParams
{
    Role role = Role::fullRange;
    float importance = 0.5f;   // 0 to 1: moves the role's claim down or up
    int group = 1;             // the mix: 1 to kMaxGroups; 0 is off (the stem is not linked and nothing is cut)
    float depthDb = 6.0f;      // the most any band is cut
    float reachDb = 12.0f;     // how far under this stem another can be and still count as in its way
    float attackMs = 30.0f;    // how fast a cut comes in
    float releaseMs = 250.0f;  // how fast it goes out
    float outDb = 0.0f;
};

// Where a stem's claim stops being enough: another stem's claim has to be this much bigger for it to take the whole depth.
inline constexpr float kDominanceSpan = 0.5f;
// An other stem counts from kPresenceLoDb (not at all) to kPresenceHiDb (fully): under it is noise or silence, which has no claim.
inline constexpr float kPresenceLoDb = -80.0f, kPresenceHiDb = -60.0f;

// How much stem `other` is in the way of a stem with claim `mine` in one band, 0 to 1: it needs a bigger claim, it needs to be there at all, and it needs
// to be within `reachDb` of this stem's level (or louder): a stem far under another is not hiding it.
inline float pressureOn (float mineClaim, float otherClaim, float myLevelDb, float otherLevelDb, float reachDb) noexcept
{
    const float dominance = std::clamp ((otherClaim - mineClaim) / kDominanceSpan, 0.0f, 1.0f);
    if (dominance <= 0.0f)
        return 0.0f;
    const float presence = std::clamp ((otherLevelDb - kPresenceLoDb) / (kPresenceHiDb - kPresenceLoDb), 0.0f, 1.0f);
    const float reach = std::fmax (reachDb, 0.1f);
    const float audible = std::clamp ((otherLevelDb - myLevelDb + reach) / reach, 0.0f, 1.0f);
    return dominance * presence * audible;
}

class Engine
{
public:
    static constexpr int kTick = 64; // everything that changes with time steps this often, counted from the start, so the output is the same for any host block size

    explicit Engine (Bus& busToUse = sharedBus()) : bus (busToUse), slot (busToUse.claim())
    {
        for (auto& v : ownLevel) v.store (kFloorDb);
        for (auto& v : cutShown) v.store (0.0f);
    }
    ~Engine() { bus.release (slot); }
    Engine (const Engine&) = delete;
    Engine& operator= (const Engine&) = delete;

    void prepare (double sampleRate)
    {
        fs = sampleRate;
        bank.prepare (fs);
        carver.prepare (fs);
        cut.fill (0.0f);
        phase = 0;
        gain = gainTarget = 1.0f;
        gainStep = 0.0f;
        deltaMix = 0.0f;
        std::fill (std::begin (ownLevel), std::end (ownLevel), kFloorDb);
    }

    // l and r are replaced by the stem with its space made. `nowMs` is the clock the bus is read by.
    void process (float* l, float* r, int n, const EngineParams& p, std::uint32_t nowMs) noexcept
    {
        int done = 0;
        while (done < n)
        {
            const int m = std::min (n - done, kTick - phase);
            for (int i = 0; i < m; ++i)
            {
                const double dryL = finite (l[done + i]), dryR = finite (r[done + i]);
                bank.push (0.5 * (dryL + dryR));
                double wetL = dryL, wetR = dryR;
                carver.process (wetL, wetR);
                // The monitor: the cut alone is the stem as it comes out minus the stem as it came in. deltaMix is exactly 0 at rest.
                l[done + i] = (float) ((wetL - (double) deltaMix * dryL) * gain);
                r[done + i] = (float) ((wetR - (double) deltaMix * dryR) * gain);
                gain += gainStep;
                deltaMix += deltaStep;
            }
            phase += m;
            done += m;
            if (phase == kTick)
            {
                phase = 0;
                tick (p, nowMs);
            }
        }
    }

    // What the editor reads (atomics, written on the audio thread): this stem's input per band, and the cut now in each band, in dB (negative).
    std::array<std::atomic<float>, kBands> ownLevel, cutShown;
    std::atomic<bool> deltaOn { false }; // monitoring, not a parameter: hear only what the cut takes away
    std::atomic<float> inPeak { 0.0f };

    int slotIndex() const noexcept { return slot; }
    const Carver& carverRef() const noexcept { return carver; }
    Bus& busRef() const noexcept { return bus; }
    // How many other stems the last tick found in this stem's mix.
    int linkedStems() const noexcept { return linked.load (std::memory_order_relaxed); }

private:
    static double finite (float x) noexcept { return std::isfinite (x) ? (double) x : 0.0; }

    void tick (const EngineParams& p, std::uint32_t nowMs) noexcept
    {
        // This stem, as it is now.
        std::array<float, kBands> own {};
        bank.read (own.data());
        for (int k = 0; k < kBands; ++k)
            ownLevel[(size_t) k].store (own[(size_t) k], std::memory_order_relaxed);
        const int group = std::clamp (p.group, 0, kMaxGroups);
        bus.publish (slot, group, p.role, p.importance, own.data(), nowMs);

        // The others, and what each of them takes of each band.
        const int count = slot >= 0 ? bus.gather (slot, group, nowMs, others.data()) : 0;
        linked.store (count, std::memory_order_relaxed);
        const auto mine = claims (p.role, p.importance);
        std::array<float, kBands> target {};
        if (count > 0 && p.depthDb > 0.0f)
        {
            std::array<std::array<float, kBands>, kMaxStems> theirs;
            for (int j = 0; j < count; ++j)
                theirs[(size_t) j] = claims (others[(size_t) j].role, others[(size_t) j].importance);
            for (int k = 0; k < kBands; ++k)
            {
                float free = 1.0f; // the share of the band nobody is in the way of
                for (int j = 0; j < count; ++j)
                    free *= 1.0f - pressureOn (mine[(size_t) k], theirs[(size_t) j][(size_t) k], own[(size_t) k], others[(size_t) j].level[(size_t) k], p.reachDb);
                target[(size_t) k] = -p.depthDb * (1.0f - free);
            }
        }

        // The cut follows its target: in fast (attack), out slowly (release).
        const float tickSeconds = (float) kTick / (float) fs;
        const float attack = 1.0f - std::exp (-tickSeconds / (0.001f * std::fmax (p.attackMs, 0.1f)));
        const float release = 1.0f - std::exp (-tickSeconds / (0.001f * std::fmax (p.releaseMs, 1.0f)));
        for (int k = 0; k < kBands; ++k)
        {
            float& c = cut[(size_t) k];
            c += (target[(size_t) k] - c) * (target[(size_t) k] < c ? attack : release);
            if (std::fabs (c) < 0.0005f && std::fabs (target[(size_t) k]) < 1e-9f)
                c = 0.0f; // at rest it is exactly none
            cutShown[(size_t) k].store (c, std::memory_order_relaxed);
        }
        carver.setTarget (cut.data());

        // Output gain and the monitor ramp over the next tick; what was aimed at has been reached by now, so a value at rest is exact.
        gain = gainTarget;
        gainTarget = std::pow (10.0f, p.outDb / 20.0f);
        gainStep = (gainTarget - gain) / (float) kTick;
        deltaMix = deltaTarget;
        deltaTarget = deltaOn.load (std::memory_order_relaxed) ? 1.0f : 0.0f;
        deltaStep = (deltaTarget - deltaMix) / (float) kTick;
    }

    Bus& bus;
    int slot;
    double fs = 48000.0;
    int phase = 0;
    LevelBank bank;
    Carver carver;
    std::array<float, kBands> cut {};
    std::array<Stem, kMaxStems> others {};
    std::atomic<int> linked { 0 };
    float gain = 1.0f, gainTarget = 1.0f, gainStep = 0.0f;
    float deltaMix = 0.0f, deltaTarget = 0.0f, deltaStep = 0.0f;
};
} // namespace hestia
