// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 The Pantheon authors

#pragma once

#include "Roles.h"
#include <atomic>
#include <cstdint>

// How the Hestia instances of one session see each other. Every instance on a stem publishes its input's level in each of the thirty bands, a few
// hundred times a second, into a slot of a table that lives in the plugin's own binary: all the instances a host loads from it into one process share
// that table, so no routing is needed. Plain C++, no JUCE; the time comes in as an argument (milliseconds, any wrapping counter), so a test can run it
// faster or slower than real time.
//
// Lock-free: a slot is claimed once, when an instance is made, with one compare-and-swap; after that the owner only writes and the others only read,
// every number being its own relaxed atomic. A reader may see half of a frame (some bands of this tick, some of the last): it is a level that is
// smoothed over tens of milliseconds, so a half-updated frame is not a different frame in any way that matters. Nothing is ever waited for.
namespace hestia
{
inline constexpr int kMaxStems = 32;   // instances that can see each other; a thirty-third works alone
inline constexpr int kMaxGroups = 8;   // the mixes A to H; group 0 is "off"
inline constexpr std::uint32_t kFreshMs = 500; // a slot not published in this long belongs to an instance that has stopped (the transport, a muted track)
inline constexpr float kFloorDb = -120.0f;

struct Stem
{
    int slot = -1;
    Role role = Role::fullRange;
    float importance = 0.5f;
    float distance = 0.0f;              // 0 at the front, 1 at the back; a stem from the sidechain has none
    std::array<float, kBands> level {}; // dB (the peak of a sine that fills the band), kFloorDb for silence
};

class Bus
{
public:
    Bus() = default;
    Bus (const Bus&) = delete;
    Bus& operator= (const Bus&) = delete;

    // A free slot for a new instance, or -1 when all are taken (the instance then works alone). Not for the audio thread (it is a loop of kMaxStems steps, but
    // it belongs to making an instance).
    int claim() noexcept
    {
        const std::uint32_t me = ++lastOwner;
        for (int i = 0; i < kMaxStems; ++i)
        {
            std::uint32_t expected = 0;
            if (slots[(size_t) i].owner.compare_exchange_strong (expected, me))
            {
                slots[(size_t) i].group.store (0, std::memory_order_release);
                return i;
            }
        }
        return -1;
    }

    void release (int slot) noexcept
    {
        if (slot < 0)
            return;
        slots[(size_t) slot].group.store (0, std::memory_order_release); // first unseen, then free
        slots[(size_t) slot].owner.store (0, std::memory_order_release);
    }

    // The owner of `slot` says what it is and how loud each band of its input is. Group 0 hides it.
    void publish (int slot, int group, Role role, float importance, float distance, const float* levelDb, std::uint32_t nowMs) noexcept
    {
        if (slot < 0)
            return;
        auto& s = slots[(size_t) slot];
        for (int k = 0; k < kBands; ++k)
            s.level[(size_t) k].store (levelDb[k], std::memory_order_relaxed);
        s.role.store ((int) role, std::memory_order_relaxed);
        s.importance.store (importance, std::memory_order_relaxed);
        s.distance.store (distance, std::memory_order_relaxed);
        s.beat.store (nowMs, std::memory_order_relaxed);
        s.group.store ((std::uint32_t) group, std::memory_order_release);
    }

    // Copies the other stems of `group` that are live (published within kFreshMs of `nowMs`) into `out` (room for kMaxStems); returns how many.
    int gather (int self, int group, std::uint32_t nowMs, Stem* out) const noexcept
    {
        int n = 0;
        if (group <= 0)
            return 0;
        for (int i = 0; i < kMaxStems; ++i)
        {
            const auto& s = slots[(size_t) i];
            if (i == self || s.group.load (std::memory_order_acquire) != (std::uint32_t) group)
                continue;
            if ((std::uint32_t) (nowMs - s.beat.load (std::memory_order_relaxed)) >= kFreshMs)
                continue;
            auto& o = out[n++];
            o.slot = i;
            o.role = (Role) std::clamp (s.role.load (std::memory_order_relaxed), 0, kNumRoles - 1);
            o.importance = s.importance.load (std::memory_order_relaxed);
            o.distance = s.distance.load (std::memory_order_relaxed);
            for (int k = 0; k < kBands; ++k)
                o.level[(size_t) k] = s.level[(size_t) k].load (std::memory_order_relaxed);
        }
        return n;
    }

    int liveCount (int group, std::uint32_t nowMs) const noexcept
    {
        Stem scratch[kMaxStems];
        return gather (-1, group, nowMs, scratch);
    }

private:
    struct Slot
    {
        std::atomic<std::uint32_t> owner { 0 }, group { 0 }, beat { 0 };
        std::atomic<int> role { 0 };
        std::atomic<float> importance { 0.5f }, distance { 0.0f };
        std::array<std::atomic<float>, kBands> level;
        Slot() { for (auto& l : level) l.store (kFloorDb, std::memory_order_relaxed); }
    };
    std::array<Slot, kMaxStems> slots;
    std::atomic<std::uint32_t> lastOwner { 0 };
};

// The one table of the plugin's binary: the instances of one host process that came from it all find each other here.
inline Bus& sharedBus()
{
    static Bus bus;
    return bus;
}
} // namespace hestia
