// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 The Pantheon authors

#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace iris
{
// Other Iris instances in the same host process, found without routing anything: every instance registers itself here, and an editor
// can listen to what another one sends out (what that track sounds like after its own equaliser) to see where the two mask each other.
// Nothing here touches the audio of either instance: a peer only copies its output into a ring, and only while somebody listens.

// One writer (the audio thread), any number of readers (editors, on the message thread). A reader that falls more than half the ring
// behind skips ahead instead of waiting: the display averages over seconds, so a lost stretch costs nothing. Relaxed atomics for the
// samples (a reader may see a torn stretch while the writer overtakes it, which a spectrum does not notice) and a release / acquire
// pair for the write position.
class PeerRing
{
public:
    static constexpr int capacity = 1 << 15;
    static constexpr int maxPush = capacity / 2; // one block longer than this keeps only its tail, so a reader half a ring behind is never overtaken mid-copy

    PeerRing()
    {
        for (auto& ch : data)
            for (auto& s : ch)
                s.store (0.0f, std::memory_order_relaxed);
    }

    // `channels` holds nCh pointers; a mono signal feeds both sides.
    void push (const float* const* channels, int nCh, int n) noexcept
    {
        if (n <= 0 || nCh <= 0)
            return;
        const int skip = std::max (0, n - maxPush);
        const std::uint64_t start = written.load (std::memory_order_relaxed) + (std::uint64_t) skip;
        for (int c = 0; c < 2; ++c)
        {
            const float* src = channels[nCh > 1 ? c : 0];
            for (int i = skip; i < n; ++i)
                data[(size_t) c][(size_t) ((start + (std::uint64_t) (i - skip)) & mask)].store (src[i], std::memory_order_relaxed);
        }
        written.store (start + (std::uint64_t) (n - skip), std::memory_order_release);
    }

    std::uint64_t position() const noexcept { return written.load (std::memory_order_acquire); }

    // Copies what was written since `pos` (at most maxCount samples per channel) and moves `pos` past it.
    int read (std::uint64_t& pos, float* left, float* right, int maxCount) const noexcept
    {
        const std::uint64_t w = written.load (std::memory_order_acquire);
        if (pos > w || w - pos > (std::uint64_t) maxPush)
            pos = w > (std::uint64_t) maxPush / 2 ? w - (std::uint64_t) maxPush / 2 : 0; // a restart, or too far behind: keep the recent stretch
        const int n = (int) std::min<std::uint64_t> (w - pos, (std::uint64_t) maxCount);
        for (int i = 0; i < n; ++i)
        {
            left[i] = data[0][(size_t) ((pos + (std::uint64_t) i) & mask)].load (std::memory_order_relaxed);
            right[i] = data[1][(size_t) ((pos + (std::uint64_t) i) & mask)].load (std::memory_order_relaxed);
        }
        pos += (std::uint64_t) n;
        return n;
    }

private:
    static constexpr std::uint64_t mask = (std::uint64_t) capacity - 1;
    std::array<std::array<std::atomic<float>, capacity>, 2> data;
    std::atomic<std::uint64_t> written { 0 };
};

class Peer;
class PeerListener;

struct PeerInfo
{
    std::string label;     // what the list shows and what a session remembers: the host's track name, with (2), (3) ... for equal ones
    bool playing = false;  // something is coming out of it now
};

// The list of instances. Taken by the message thread (editors) and by constructors and destructors, which hosts may run on any
// thread; the audio thread never takes it.
class PeerRegistry
{
public:
    static PeerRegistry& get()
    {
        static PeerRegistry registry;
        return registry;
    }

    // The other instances, in the order they were loaded.
    std::vector<PeerInfo> others (const Peer* self);

private:
    friend class Peer;
    friend class PeerListener;
    PeerRegistry() = default;

    std::string labelOf (const Peer* peer) const; // lock held
    Peer* find (const std::string& label, const Peer* self) const; // lock held

    std::mutex lock;
    std::vector<Peer*> peers;
    std::vector<PeerListener*> listeners;
};

// One per instance: owned by the processor, which calls publish from its audio thread.
class Peer
{
public:
    Peer()
    {
        auto& r = PeerRegistry::get();
        std::scoped_lock l (r.lock);
        r.peers.push_back (this);
    }

    ~Peer();

    Peer (const Peer&) = delete;
    Peer& operator= (const Peer&) = delete;

    // Audio thread: this instance's output for the block, and how loud it is (the engine's smoothed peak). Copied only while listened to.
    void publish (const float* const* channels, int nCh, int n, float peak) noexcept
    {
        level.store (peak, std::memory_order_relaxed);
        if (listening.load (std::memory_order_relaxed) > 0)
            ring->push (channels, nCh, n);
    }

    // Message thread: the host's name for the track this instance sits on (empty when it does not say).
    void setTrackName (const std::string& name)
    {
        std::scoped_lock l (PeerRegistry::get().lock);
        trackName = name;
    }

private:
    friend class PeerRegistry;
    friend class PeerListener;
    static constexpr float kPlayingLevel = 1.0e-4f; // -80 dB

    std::unique_ptr<PeerRing> ring = std::make_unique<PeerRing>(); // 256 KB: on the heap, so an instance on a stack (a test's) stays small
    std::atomic<float> level { 0.0f };
    std::atomic<int> listening { 0 };
    std::string trackName; // under the registry's lock
};

// An editor's view of one other instance, chosen by its label. Message thread only.
class PeerListener
{
public:
    explicit PeerListener (Peer& owner) : self (&owner)
    {
        auto& r = PeerRegistry::get();
        std::scoped_lock l (r.lock);
        r.listeners.push_back (this);
    }

    ~PeerListener()
    {
        auto& r = PeerRegistry::get();
        std::scoped_lock l (r.lock);
        release();
        r.listeners.erase (std::remove (r.listeners.begin(), r.listeners.end(), this), r.listeners.end());
    }

    PeerListener (const PeerListener&) = delete;
    PeerListener& operator= (const PeerListener&) = delete;

    // "" listens to nobody.
    void select (const std::string& label)
    {
        auto& r = PeerRegistry::get();
        std::scoped_lock l (r.lock);
        wanted = label;
        release();
    }

    const std::string& selected() const noexcept { return wanted; }

    // Moves what the chosen instance has sent out since the last call into left and right; 0 when nothing is chosen, it is not
    // loaded (yet), or it is silent. `found` says whether it is loaded.
    int pull (float* left, float* right, int maxCount, bool& found, bool& playing)
    {
        found = playing = false;
        auto& r = PeerRegistry::get();
        std::scoped_lock l (r.lock);
        if (wanted.empty())
            return 0;
        Peer* target = r.find (wanted, self);
        if (target != current)
        {
            release();
            current = target;
            if (current != nullptr)
            {
                current->listening.fetch_add (1, std::memory_order_relaxed);
                pos = current->ring->position();
            }
        }
        if (current == nullptr)
            return 0;
        found = true;
        playing = current->level.load (std::memory_order_relaxed) > Peer::kPlayingLevel;
        return current->ring->read (pos, left, right, maxCount);
    }

private:
    friend class Peer;

    void release() // lock held
    {
        if (current != nullptr)
            current->listening.fetch_sub (1, std::memory_order_relaxed);
        current = nullptr;
    }

    void forget (Peer* gone) noexcept // lock held, from Peer's destructor: it is going away, so no count to give back
    {
        if (current == gone)
            current = nullptr;
    }

    Peer* self;
    std::string wanted;
    Peer* current = nullptr;
    std::uint64_t pos = 0;
};

inline Peer::~Peer()
{
    auto& r = PeerRegistry::get();
    std::scoped_lock l (r.lock);
    r.peers.erase (std::remove (r.peers.begin(), r.peers.end(), this), r.peers.end());
    for (auto* listener : r.listeners)
        listener->forget (this);
}

inline std::string PeerRegistry::labelOf (const Peer* peer) const
{
    const std::string base = peer->trackName.empty() ? "Iris" : peer->trackName;
    int nth = 0;
    for (const auto* p : peers)
    {
        if ((p->trackName.empty() ? "Iris" : p->trackName) == base)
            ++nth;
        if (p == peer)
            break;
    }
    return nth > 1 ? base + " (" + std::to_string (nth) + ")" : base;
}

inline Peer* PeerRegistry::find (const std::string& label, const Peer* self) const
{
    for (auto* p : peers)
        if (p != self && labelOf (p) == label)
            return p;
    return nullptr;
}

inline std::vector<PeerInfo> PeerRegistry::others (const Peer* self)
{
    std::scoped_lock l (lock);
    std::vector<PeerInfo> out;
    for (auto* p : peers)
        if (p != self)
            out.push_back ({ labelOf (p), p->level.load (std::memory_order_relaxed) > Peer::kPlayingLevel });
    return out;
}
} // namespace iris
