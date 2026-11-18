// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 The Pantheon authors

#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>

namespace themis
{
inline constexpr int kNumEnergyRanges = 4;
inline constexpr int kAllRanges = kNumEnergyRanges; // the column after the ranges: the whole spectrum
inline constexpr int kNumEnergyColumns = kNumEnergyRanges + 1;

// The ways the stereo signal is looked at in the energy history, with M = (L + R) / 2 and S = (L - R) / 2 as everywhere
// else (L = M + S, R = M - S). A full-scale sine in both channels reads -3 dB in left, right and mid and nothing in
// side; in the left channel only it reads -3 dB in left and -9 dB in mid and side.
enum class Signal { stereo, mid, side, left, right };
inline constexpr int kNumSignals = 5;

// Level that stands for "no signal" in the history (digital silence, or no fundamental found).
inline constexpr float kHistoryFloorDb = -120.0f;
inline bool hasLevel (float db) { return db > kHistoryFloorDb + 1.0f; }

// What one analysed frame contributes to the time graphs. Levels are mean-square levels in dBFS (a full-scale sine
// reads -3 dB), except the peaks, which are dBFS peak values.
struct FrameStats
{
    // Energy of each signal in each energy range and in the whole spectrum (the last column). "stereo" is mid + side,
    // i.e. the mean power of the two channels.
    std::array<std::array<float, kNumEnergyColumns>, kNumSignals> energyDb {};
    float f0Hz = 0.0f; // tracked bass fundamental, 0 when none was found
    float fundamentalDb = kHistoryFloorDb;
    float harmonicsDb = kHistoryFloorDb; // summed energy of the fundamental's harmonics
    float samplePeakDb = kHistoryFloorDb, truePeakDb = kHistoryFloorDb; // highest peaks of the audio since the previous frame
    float peakWindowRmsDb = kHistoryFloorDb; // RMS of both channels over those same samples (the crest factor's level, see CREST)
    float clipEvents = 0.0f, clipSamples = 0.0f, overSamples = 0.0f;    // counts since the previous frame (see ClipMeter)

    float& energy (Signal s, int column) { return energyDb[static_cast<size_t> (s)][static_cast<size_t> (column)]; }
    float energy (Signal s, int column) const { return energyDb[static_cast<size_t> (s)][static_cast<size_t> (column)]; }

    bool hasFundamental() const { return f0Hz > 0.0f; }
    // The fundamental's levels, or the floor when none was found (what a graph of them should plot).
    float fundamentalLevelDb() const { return hasFundamental() ? fundamentalDb : kHistoryFloorDb; }
    float harmonicsLevelDb() const { return hasFundamental() ? harmonicsDb : kHistoryFloorDb; }
    bool clipped() const { return clipEvents > 0.0f || overSamples > 0.0f; }

    static constexpr size_t kFields = kNumSignals * kNumEnergyColumns + 9;

    // Calls f(value) for every field in storage order; the one place that lists them (push and copy both use it).
    template <typename Self, typename F> // Self: FrameStats or const FrameStats
    static void forEachField (Self& s, F&& f)
    {
        for (auto& signal : s.energyDb)
            for (auto& v : signal)
                f (v);
        f (s.f0Hz);
        f (s.fundamentalDb);
        f (s.harmonicsDb);
        f (s.samplePeakDb);
        f (s.truePeakDb);
        f (s.peakWindowRmsDb);
        f (s.clipEvents);
        f (s.clipSamples);
        f (s.overSamples);
    }
};

// The first and the last sound since the reset, counted in samples: a sample is sound when it is at or above kSoundLevel on
// either channel, and anything under it is silence or dither. Kept beside the frame history rather than read back from it,
// because the history window (about 85 s) forgets a track's start long before the track ends. Written by the audio thread
// (clear, scan); the readers are any thread and read atomics. Sample indices run from the same counter as the analyser's.
class SoundEdges
{
public:
    static constexpr float kSoundLevel = 1.0e-4f; // -80 dBFS

    // Audio thread: forget what was heard and count silence from sample `at` on.
    void clear (uint64_t at)
    {
        base.store (at, std::memory_order_relaxed);
        first.store (0, std::memory_order_relaxed);
        last.store (0, std::memory_order_relaxed);
        heardFlag.store (false, std::memory_order_release);
    }

    // Audio thread: `l` and `r` hold samples `at`, `at` + 1, ... (n of them).
    void scan (const float* l, const float* r, int n, uint64_t at)
    {
        bool heard = heardFlag.load (std::memory_order_relaxed);
        uint64_t lastSound = last.load (std::memory_order_relaxed);
        bool any = false;
        for (int i = 0; i < n; ++i)
        {
            if (!(std::abs (l[i]) >= kSoundLevel || std::abs (r[i]) >= kSoundLevel))
                continue;
            const uint64_t at_i = at + static_cast<uint64_t> (i);
            if (!heard)
            {
                first.store (at_i, std::memory_order_relaxed);
                heard = true;
            }
            lastSound = at_i;
            any = true;
        }
        if (any)
        {
            last.store (lastSound, std::memory_order_relaxed);
            heardFlag.store (true, std::memory_order_release);
        }
    }

    // Any thread. Samples of silence before the first sound since the clear, and after the last one up to `now` (the number of
    // samples pushed so far); zero until something has been heard.
    bool heard() const { return heardFlag.load (std::memory_order_acquire); }
    uint64_t leadingSamples() const { return heard() ? first.load (std::memory_order_relaxed) - base.load (std::memory_order_relaxed) : 0; }
    uint64_t trailingSamples (uint64_t now) const
    {
        const uint64_t lastSound = last.load (std::memory_order_relaxed);
        return heard() && now > lastSound ? now - lastSound - 1 : 0;
    }

private:
    std::atomic<uint64_t> base { 0 }, first { 0 }, last { 0 };
    std::atomic<bool> heardFlag { false };
};

// Fixed-size ring of recent FrameStats. One writer (the analysis thread), any number of readers (UI). Every field
// is an atomic, and the writer would have to lap the whole ring (~85 s of frames) to touch an entry a reader is
// copying, so no further locking is needed.
class FrameHistory
{
public:
    static constexpr int kCapacity = 1024;

    // Writer thread.
    void setFramePeriod (double seconds) { period.store (seconds, std::memory_order_relaxed); }
    void clear() { count.store (0, std::memory_order_release); }

    void push (const FrameStats& s)
    {
        const uint64_t n = count.load (std::memory_order_relaxed);
        size_t i = static_cast<size_t> (n % kCapacity) * FrameStats::kFields;
        FrameStats::forEachField (s, [&] (float v) { data[i++].store (v, std::memory_order_relaxed); });
        count.store (n + 1, std::memory_order_release);
    }

    // Any thread.
    double framePeriodSeconds() const { return period.load (std::memory_order_relaxed); }

    // Frames pushed since the last clear(); changes whenever there is something new to draw.
    uint64_t sequence() const { return count.load (std::memory_order_acquire); }

    // Copies the newest frames, oldest first, into out[0..n); returns how many were available.
    int copyRecent (FrameStats* out, int n) const
    {
        const uint64_t total = count.load (std::memory_order_acquire);
        const int have = static_cast<int> (std::min<uint64_t> ({ total, static_cast<uint64_t> (kCapacity), static_cast<uint64_t> (std::max (n, 0)) }));
        for (int i = 0; i < have; ++i)
        {
            size_t j = static_cast<size_t> ((total - static_cast<uint64_t> (have - i)) % kCapacity) * FrameStats::kFields;
            FrameStats::forEachField (out[i], [&] (float& v) { v = data[j++].load (std::memory_order_relaxed); });
        }
        return have;
    }

private:
    std::array<std::atomic<float>, kCapacity * FrameStats::kFields> data {};
    std::atomic<uint64_t> count { 0 };
    std::atomic<double> period { 0.1 };
};
} // namespace themis
