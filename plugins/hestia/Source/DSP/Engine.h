// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 The Pantheon authors

#pragma once

#include "Analysis.h"
#include "Bus.h"
#include "Carver.h"
#include "Depth.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>

// The whole of one stem's signal path: measure the input in thirty bands, publish it, read what the other stems of the mix are doing (and what the
// sidechain hears), decide how much of each band this stem should give up, cut it, and put the stem as far back as it was asked to sit. Plain C++
// (Tests.cpp includes it).
namespace hestia
{
struct EngineParams
{
    Role role = Role::fullRange;
    float importance = 0.5f;   // 0 to 1: moves the role's claim down or up
    float distance = 0.0f;     // 0 (front) to 1 (back): how far back the stem sits (Depth.h); it also gives up some of its claim
    int group = 1;             // the mix: 1 to kMaxGroups; 0 is off (the stem is not linked and nothing is cut)
    float carveDb = 6.0f;      // the most any band is cut
    float reachDb = 12.0f;     // how far under this stem another can be and still count as in its way
    float attackMs = 30.0f;    // how fast a cut comes in
    float releaseMs = 250.0f;  // how fast it goes out
    float outDb = 0.0f;
    Role scRole = Role::fullRange; // what the sidechain input is (a track with no Hestia on it), as a role...
    float scImportance = 0.5f;     // ... and how much it matters
};

// Where a stem's claim stops being enough: another stem's claim has to be this much bigger for it to take the whole carve.
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
        for (auto& v : sidechainLevel) v.store (kFloorDb);
    }
    ~Engine() { bus.release (slot); }
    Engine (const Engine&) = delete;
    Engine& operator= (const Engine&) = delete;

    void prepare (double sampleRate)
    {
        fs = sampleRate;
        bank.prepare (fs);
        scBank.prepare (fs);
        carver.prepare (fs);
        reflections.prepare (fs);
        for (auto& a : air)
            a = pdsp::Biquad();
        cut.fill (0.0f);
        phase = 0;
        gain.reset (1.0f);
        widthLoss.reset (0.0f);
        reflect.reset (0.0f);
        deltaMix.reset (0.0f);
        distanceNow = 0.0f;
        shownDistance = -1.0f;
        airOn = false;
        scFed = 0;
        scLive = false;
        std::fill (std::begin (ownLevel), std::end (ownLevel), kFloorDb);
    }

    // A host locate, loop or stop: the memory of the old position (the level bank, the carve filters, the early-reflection line) is cleared; the settings and their smoothing stay.
    void reset() noexcept
    {
        bank.reset();
        scBank.reset();
        carver.reset();
        reflections.reset();
    }

    // l and r are replaced by the stem with its space made. `nowMs` is the clock the bus is read by. scL / scR are the sidechain input (a track that has
    // no Hestia on it), null when there is none; it is only listened to and never heard.
    void process (float* l, float* r, int n, const EngineParams& p, std::uint32_t nowMs, const float* scL = nullptr, const float* scR = nullptr) noexcept
    {
        int done = 0;
        while (done < n)
        {
            const int m = std::min (n - done, kTick - phase);
            for (int i = 0; i < m; ++i)
            {
                const double dryL = finite (l[done + i]), dryR = finite (r[done + i]);
                bank.push (0.5 * (dryL + dryR));
                if (scL != nullptr)
                {
                    const double a = finite (scL[done + i]), b = scR != nullptr ? finite (scR[done + i]) : a;
                    scBank.push (0.5 * (a + b));
                    ++scFed;
                }

                // The carve, then the depth: the air takes the highs, the image narrows, the first reflections of the room come up, the level falls.
                double wetL = dryL, wetR = dryR;
                carver.process (wetL, wetR);
                double reflectedL, reflectedR;
                reflections.process (wetL, wetR, reflectedL, reflectedR);
                if (airOn) // left out at the front, not run at 0 dB: see Carver::setTarget
                {
                    wetL = air[0].process (wetL);
                    wetR = air[1].process (wetR);
                }
                const double side = 0.5 * (wetL - wetR), loss = (double) widthLoss.advance(), room = (double) reflect.advance();
                wetL = wetL - loss * side + room * reflectedL;
                wetR = wetR + loss * side + room * reflectedR;

                // The monitor: everything Hestia changes is the stem as it comes out minus the stem as it came in. deltaMix is exactly 0 at rest.
                const double g = (double) gain.advance(), dm = (double) deltaMix.advance();
                l[done + i] = (float) ((wetL - dm * dryL) * g);
                r[done + i] = (float) ((wetR - dm * dryR) * g);
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

    // What the editor reads (atomics, written on the audio thread): this stem's input per band, the cut now in each band, in dB (negative), and what the
    // sidechain hears.
    std::array<std::atomic<float>, kBands> ownLevel, cutShown, sidechainLevel;
    std::atomic<bool> deltaOn { false };         // monitoring, not a parameter: hear only what Hestia changes
    std::atomic<bool> sidechainActive { false }; // the last tick had a sidechain
    std::atomic<float> inPeak { 0.0f };

    int slotIndex() const noexcept { return slot; }
    const Carver& carverRef() const noexcept { return carver; }
    Bus& busRef() const noexcept { return bus; }
    // How many other stems the last tick found in this stem's mix, the sidechain included.
    int linkedStems() const noexcept { return linked.load (std::memory_order_relaxed); }

private:
    // A value that moves in a straight line over the next tick and is exactly where it was aimed once the tick is over: so a value at rest is exact.
    struct Ramp
    {
        float value = 0.0f, target = 0.0f, step = 0.0f;
        void reset (float v) noexcept { value = target = v; step = 0.0f; }
        void aim (float next) noexcept { value = target; target = next; step = (target - value) / (float) kTick; }
        inline float advance() noexcept { const float r = value; value += step; return r; }
    };

    static double finite (float x) noexcept { return std::isfinite (x) ? (double) x : 0.0; }

    void tick (const EngineParams& p, std::uint32_t nowMs) noexcept
    {
        // This stem, as it is now.
        std::array<float, kBands> own {};
        bank.read (own.data());
        for (int k = 0; k < kBands; ++k)
            ownLevel[(size_t) k].store (own[(size_t) k], std::memory_order_relaxed);
        const int group = std::clamp (p.group, 0, kMaxGroups);
        const float distance = std::clamp (p.distance, 0.0f, 1.0f);
        bus.publish (slot, group, p.role, p.importance, distance, own.data(), nowMs);

        // The others, and the sidechain as one more of them. It starts from clean filters when it comes back.
        int count = slot >= 0 ? bus.gather (slot, group, nowMs, others.data()) : 0;
        const bool sidechain = scFed > 0;
        scFed = 0;
        if (sidechain)
        {
            std::array<float, kBands> heard {};
            scBank.read (heard.data());
            for (int k = 0; k < kBands; ++k)
                sidechainLevel[(size_t) k].store (heard[(size_t) k], std::memory_order_relaxed);
            if (group > 0 && count < kMaxStems)
            {
                auto& o = others[(size_t) count++];
                o = Stem();
                o.role = p.scRole;
                o.importance = p.scImportance;
                o.level = heard;
            }
        }
        else if (scLive)
        {
            scBank.reset();
            for (auto& v : sidechainLevel) v.store (kFloorDb, std::memory_order_relaxed);
        }
        scLive = sidechain;
        sidechainActive.store (sidechain, std::memory_order_relaxed);
        linked.store (count, std::memory_order_relaxed);

        // What each of them takes of each band.
        const auto mine = claims (p.role, p.importance, distance);
        std::array<float, kBands> target {};
        if (count > 0 && p.carveDb > 0.0f)
        {
            std::array<std::array<float, kBands>, kMaxStems> theirs;
            for (int j = 0; j < count; ++j)
                theirs[(size_t) j] = claims (others[(size_t) j].role, others[(size_t) j].importance, others[(size_t) j].distance);
            for (int k = 0; k < kBands; ++k)
            {
                float free = 1.0f; // the share of the band nobody is in the way of
                for (int j = 0; j < count; ++j)
                    free *= 1.0f - pressureOn (mine[(size_t) k], theirs[(size_t) j][(size_t) k], own[(size_t) k], others[(size_t) j].level[(size_t) k], p.reachDb);
                target[(size_t) k] = -p.carveDb * (1.0f - free);
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

        // The depth moves to where it was put over a few ticks (no zipper), and is exact once it is there.
        distanceNow += (distance - distanceNow) * 0.2f;
        if (std::fabs (distance - distanceNow) < 1e-5f)
            distanceNow = distance;
        const auto cues = cuesFor (distanceNow);
        if (std::fabs (distanceNow - shownDistance) > 1e-9f)
        {
            shownDistance = distanceNow;
            airOn = distanceNow > 0.0f;
            for (auto& a : air)
            {
                a = pdsp::Biquad(); // a shelf that is off has no state; one that is wanted again starts from rest
                if (airOn)
                    a.setHighShelf (fs, kAirHz, 0.7071, cues.airDb);
            }
        }

        // Output gain, the level of the distance, the width, the room and the monitor ramp over the next tick; what was aimed at has been reached by now.
        gain.aim (std::pow (10.0f, p.outDb / 20.0f) * cues.levelGain);
        widthLoss.aim (cues.widthLoss);
        reflect.aim (cues.reflections);
        deltaMix.aim (deltaOn.load (std::memory_order_relaxed) ? 1.0f : 0.0f);
    }

    Bus& bus;
    int slot;
    double fs = 48000.0;
    int phase = 0;
    LevelBank bank, scBank;
    Carver carver;
    EarlyReflections reflections;
    std::array<pdsp::Biquad, 2> air;
    std::array<float, kBands> cut {};
    std::array<Stem, kMaxStems> others {};
    std::atomic<int> linked { 0 };
    Ramp gain, widthLoss, reflect, deltaMix;
    float distanceNow = 0.0f, shownDistance = -1.0f;
    int scFed = 0;
    bool scLive = false, airOn = false;
};
} // namespace hestia
