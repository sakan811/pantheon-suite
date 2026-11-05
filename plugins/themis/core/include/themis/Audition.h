// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 The Pantheon authors

#pragma once

#include "Biquad.h"
#include "Common.h"
#include "TonalAnalyzer.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <functional>
#include <string>

namespace themis
{
// Solo / mute for listening to what the checks flag: the stereo image (mid, side), the channels (left, right) and five
// ranges: the energy ranges of the readouts (LOW .. HIGH) with LOW split in two at kSubBassHz, so the sub and the bass can
// be heard apart (they are what small speakers lose and what a bass line carries). Audition is the one thing in Themis
// that changes the output, and only while a button is down: with nothing engaged the audio is left untouched, bit for bit.
enum class Source { mid, side, left, right, sub, bass, lowMid, highMid, high };
inline constexpr int kNumSources = 9;
inline constexpr int kFirstBandSource = 4; // sub .. high are the audition ranges, in kAuditionRanges order

// The boundary between SUB and BASS, inside the readouts' LOW range. Iris' lowest crossover is at the same frequency.
inline constexpr double kSubBassHz = 60.0;
inline constexpr int kNumAuditionRanges = kNumEnergyRanges + 1;
inline constexpr EnergyRange kAuditionRanges[kNumAuditionRanges] = { { "SUB", kEnergyRanges[0].loHz, kSubBassHz },
                                                                     { "BASS", kSubBassHz, kEnergyRanges[0].hiHz },
                                                                     kEnergyRanges[1], kEnergyRanges[2], kEnergyRanges[3] };

// Which sources are soloed / muted, as bit masks (bit = static_cast<int> (Source)). Plain data so the message thread can
// publish it in one atomic and the audio thread read it.
struct Switches
{
    uint32_t solo = 0, mute = 0;

    static constexpr uint32_t bit (Source s) { return 1u << static_cast<int> (s); }
    bool active() const { return (solo | mute) != 0; }
    bool operator== (const Switches& o) const { return solo == o.solo && mute == o.mute; }
    bool operator!= (const Switches& o) const { return !(*this == o); }

    uint32_t pack() const { return (solo & 0xffffu) | ((mute & 0xffffu) << 16); }
    static Switches unpack (uint32_t v) { return { v & 0xffffu, v >> 16 }; }
};

// What the switches mean: within each group (stereo image, channels, ranges) a solo silences whatever is not soloed, a
// mute silences that source, and a source that is both soloed and muted is silent. Groups combine: soloing LOW and SIDE
// plays the low range of the side signal.
struct Audible
{
    bool mid = true, side = true, left = true, right = true;
    std::array<bool, kNumAuditionRanges> band { true, true, true, true, true };
    bool leftSoloed = false, rightSoloed = false; // a soloed channel is played on both sides, like a solo in a DAW

    bool bandsAll() const
    {
        for (const bool b : band)
            if (!b)
                return false;
        return true;
    }
    bool untouched() const { return mid && side && left && right && bandsAll(); }
};

inline Audible resolve (const Switches& sw)
{
    const auto audible = [&] (uint32_t group, Source s) {
        const uint32_t b = Switches::bit (s);
        return (sw.solo & group) != 0 ? ((sw.solo & b) != 0 && (sw.mute & b) == 0) : (sw.mute & b) == 0;
    };
    constexpr uint32_t kImage = (1u << static_cast<int> (Source::mid)) | (1u << static_cast<int> (Source::side));
    constexpr uint32_t kChannels = (1u << static_cast<int> (Source::left)) | (1u << static_cast<int> (Source::right));
    constexpr uint32_t kBands = ((1u << kNumAuditionRanges) - 1u) << kFirstBandSource;

    Audible a;
    a.mid = audible (kImage, Source::mid);
    a.side = audible (kImage, Source::side);
    a.left = audible (kChannels, Source::left);
    a.right = audible (kChannels, Source::right);
    for (int i = 0; i < kNumAuditionRanges; ++i)
        a.band[static_cast<size_t> (i)] = audible (kBands, static_cast<Source> (kFirstBandSource + i));
    a.leftSoloed = (sw.solo & Switches::bit (Source::left)) != 0;
    a.rightSoloed = (sw.solo & Switches::bit (Source::right)) != 0;
    return a;
}

inline const char* sourceName (Source s)
{
    constexpr const char* names[kNumSources] = { "MID", "SIDE", "LEFT", "RIGHT", "SUB", "BASS", "LOW-MID", "HIGH-MID", "HIGH" };
    return names[static_cast<int> (s)];
}
static_assert (kNumSources == kFirstBandSource + kNumAuditionRanges, "one source per audition range");

// "SIDE + LOW" for the soloed sources of `sw` (empty when none are soloed).
inline std::string soloText (const Switches& sw)
{
    std::string text;
    for (int i = 0; i < kNumSources; ++i)
        if ((sw.solo & (1u << i)) != 0)
            text += (text.empty() ? "" : " + ") + std::string (sourceName (static_cast<Source> (i)));
    return text;
}

// The audition stage. Read-modify-write on the stereo buffer, audio thread only; the switches arrive through an
// atomic. Every change fades over kRampSeconds so pressing a button does not click.
//
// Signal flow: ranges (crossovers) -> channel mix (L / R) -> stereo image (M / S). All three are linear, so their order
// only matters for the copy that a soloed channel makes onto the other side.
//
// The ranges are split with Linkwitz-Riley 4th-order crossovers at the edges of kAuditionRanges. Each lower range is run
// through the all-pass of the crossovers above it, so the five ranges sum back to a flat-magnitude signal and muting one
// removes just that range.
class Audition
{
public:
    static constexpr double kRampSeconds = 0.01;

    void prepare (double sampleRate)
    {
        fs = sampleRate;
        rampStep = 1.0f / static_cast<float> (std::max (1.0, kRampSeconds * fs));
        for (int c = 0; c < 2; ++c)
        {
            for (int x = 0; x < kNumCrossovers; ++x)
                xo[c][x].design (fs, crossoverHz (x));
            for (int k = 0, a = 0; k < kNumCrossovers; ++k)
                for (int j = k + 1; j < kNumCrossovers; ++j, ++a)
                    ap[c][a].design (fs, crossoverHz (j));
        }
        resetFilters();
        filtersCold = true;
        snapToTarget();
    }

    // Message thread: what to audition. Applied by the next process().
    void setSwitches (const Switches& s) { requested.store (s.pack(), std::memory_order_relaxed); }
    Switches switches() const { return Switches::unpack (requested.load (std::memory_order_relaxed)); }

    // Audio thread.
    void process (float* l, float* r, int n)
    {
        setTarget (resolve (switches()));
        if (settledAtUnity && target.untouched())
            return; // nothing engaged: leave the buffer alone

        for (int i = 0; i < n; ++i)
        {
            stepRamps();
            float inL = l[i], inR = r[i];
            if (bandMix <= 0.0f && !filtersCold)
            {
                resetFilters(); // not running: the next engagement starts from silence in the filters, not from old audio
                filtersCold = true;
            }
            if (bandMix > 0.0f)
            {
                filtersCold = false;
                const float wetL = splitAndMix (0, inL), wetR = splitAndMix (1, inR);
                inL += bandMix * (wetL - inL); // bandMix fades the split in and out so engaging it does not click
                inR += bandMix * (wetR - inR);
            }
            l[i] = m[0] * inL + m[1] * inR;
            r[i] = m[2] * inL + m[3] * inR;
        }
        settledAtUnity = target.untouched() && rampsDone();
    }

    // Crossover between range x and x + 1.
    static double crossoverHz (int x) { return kAuditionRanges[x].hiHz; }

private:
    static constexpr int kNumCrossovers = kNumAuditionRanges - 1;
    // Range k has to pass the all-pass of every crossover above it (k + 1 ... last): one per pair.
    static constexpr int kNumAllpasses = kNumCrossovers * (kNumCrossovers - 1) / 2;

    // One Linkwitz-Riley 4th-order low / high pair (two Butterworth biquads each).
    struct Crossover
    {
        Biquad lowA, lowB, highA, highB;
        void design (double sampleRate, double hz)
        {
            const auto lp = BiquadCoeffs::lowPass (sampleRate, hz, 0.7071), hp = BiquadCoeffs::highPass (sampleRate, hz, 0.7071);
            lowA.setCoeffs (lp);
            lowB.setCoeffs (lp);
            highA.setCoeffs (hp);
            highB.setCoeffs (hp);
        }
        float low (float x) { return lowB.process (lowA.process (x)); }
        float high (float x) { return highB.process (highA.process (x)); }
        float allpass (float x) { return low (x) + high (x); }
        void reset()
        {
            lowA.reset();
            lowB.reset();
            highA.reset();
            highB.reset();
        }
    };

    // Splits one channel into the ranges and adds the audible ones.
    float splitAndMix (int ch, float x)
    {
        auto& c = xo[ch];
        // Each crossover splits what is left above the one before it; the all-pass copies keep the lower ranges in phase
        // with the upper ones.
        float part[kNumAuditionRanges];
        float rest = x;
        for (int k = 0; k < kNumCrossovers; ++k)
        {
            part[k] = c[k].low (rest);
            rest = c[k].high (rest);
        }
        part[kNumCrossovers] = rest;
        float sum = gain[kNumCrossovers] * rest;
        for (int k = 0, a = 0; k < kNumCrossovers; ++k)
        {
            for (int j = k + 1; j < kNumCrossovers; ++j, ++a)
                part[k] = ap[ch][a].allpass (part[k]);
            sum += gain[static_cast<size_t> (k)] * part[k];
        }
        return sum;
    }

    void setTarget (const Audible& a)
    {
        target = a;
        // Channel mix (rows: output, columns: input), then the stereo image on top of it: out = S * C * in.
        const float cl = a.left ? 1.0f : 0.0f, cr = a.right ? 1.0f : 0.0f;
        float c[4] = { cl, 0.0f, 0.0f, cr };
        if (a.leftSoloed != a.rightSoloed) // exactly one channel soloed: it plays on both sides
        {
            const bool fromLeft = a.leftSoloed;
            c[0] = c[2] = fromLeft ? cl : 0.0f;
            c[1] = c[3] = fromLeft ? 0.0f : cr;
        }
        const float gm = a.mid ? 1.0f : 0.0f, gs = a.side ? 1.0f : 0.0f;
        const float s[4] = { 0.5f * (gm + gs), 0.5f * (gm - gs), 0.5f * (gm - gs), 0.5f * (gm + gs) };
        mTarget[0] = s[0] * c[0] + s[1] * c[2];
        mTarget[1] = s[0] * c[1] + s[1] * c[3];
        mTarget[2] = s[2] * c[0] + s[3] * c[2];
        mTarget[3] = s[2] * c[1] + s[3] * c[3];
        for (int i = 0; i < kNumAuditionRanges; ++i)
            gainTarget[static_cast<size_t> (i)] = a.band[static_cast<size_t> (i)] ? 1.0f : 0.0f;
        bandMixTarget = a.bandsAll() ? 0.0f : 1.0f;
    }

    void stepRamps()
    {
        for (int i = 0; i < 4; ++i)
            m[i] = approach (m[i], mTarget[i]);
        for (int i = 0; i < kNumAuditionRanges; ++i)
            gain[static_cast<size_t> (i)] = approach (gain[static_cast<size_t> (i)], gainTarget[static_cast<size_t> (i)]);
        bandMix = approach (bandMix, bandMixTarget);
    }

    float approach (float v, float t) const { return v < t ? std::min (t, v + rampStep) : std::max (t, v - rampStep); }

    // approach() lands exactly on its target, so a finished ramp compares equal.
    bool rampsDone() const
    {
        const std::equal_to<float> same;
        for (int i = 0; i < 4; ++i)
            if (!same (m[i], mTarget[i]))
                return false;
        for (int i = 0; i < kNumAuditionRanges; ++i)
            if (!same (gain[static_cast<size_t> (i)], gainTarget[static_cast<size_t> (i)]))
                return false;
        return same (bandMix, bandMixTarget);
    }

    void snapToTarget()
    {
        setTarget (Audible {});
        for (int i = 0; i < 4; ++i)
            m[i] = mTarget[i];
        for (int i = 0; i < kNumAuditionRanges; ++i)
            gain[static_cast<size_t> (i)] = gainTarget[static_cast<size_t> (i)];
        bandMix = bandMixTarget;
        settledAtUnity = true;
    }

    void resetFilters()
    {
        for (int c = 0; c < 2; ++c)
        {
            for (auto& x : xo[c])
                x.reset();
            for (auto& a : ap[c])
                a.reset();
        }
    }

    double fs = 48000.0;
    float rampStep = 0.02f;

    std::atomic<uint32_t> requested { 0 };
    Audible target;
    bool settledAtUnity = true, filtersCold = true;

    float m[4] = { 1.0f, 0.0f, 0.0f, 1.0f }, mTarget[4] = { 1.0f, 0.0f, 0.0f, 1.0f };
    std::array<float, kNumAuditionRanges> gain { 1.0f, 1.0f, 1.0f, 1.0f, 1.0f }, gainTarget { 1.0f, 1.0f, 1.0f, 1.0f, 1.0f };
    float bandMix = 0.0f, bandMixTarget = 0.0f;

    Crossover xo[2][kNumCrossovers];
    Crossover ap[2][kNumAllpasses];
};
} // namespace themis
