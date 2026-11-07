// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 The Pantheon authors

#pragma once

#include "Common.h"
#include "TonalAnalyzer.h"

#include <pantheon/dsp/MusicBands.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <functional>
#include <string>

namespace themis
{
// Solo / mute for listening to what the checks flag: the stereo image (mid, side), the channels (left, right) and the six main
// frequency bands of music (SUB, BASS, LOW-MID, MIDRANGE, UPPER-MID, HIGH: pdsp::bands in shared/dsp MusicBands.h, the same
// bands the other plugins of the suite solo). Audition is the one thing in Themis that changes the output, and only while a
// button is down: with nothing engaged the audio is left untouched, bit for bit.
enum class Source { mid, side, left, right, sub, bass, lowMid, midrange, upperMid, high };
inline constexpr int kNumSources = 10;
inline constexpr int kFirstBandSource = 4; // sub .. high are the audition ranges, in kAuditionRanges order

inline constexpr int kNumAuditionRanges = pdsp::bands::kCount;
namespace detail
{
constexpr bool sameLetters (const char* upper, const char* name)
{
    for (; *upper != 0 || *name != 0; ++upper, ++name)
        if (*upper != (*name >= 'a' && *name <= 'z' ? static_cast<char> (*name - 'a' + 'A') : *name))
            return false;
    return true;
}
constexpr EnergyRange auditionRange (const char* upperName, int band)
{
    return { upperName, pdsp::bands::kBands[static_cast<size_t> (band)].loHz, pdsp::bands::kBands[static_cast<size_t> (band)].hiHz };
}
} // namespace detail
inline constexpr EnergyRange kAuditionRanges[kNumAuditionRanges] = { detail::auditionRange ("SUB", 0),      detail::auditionRange ("BASS", 1),
                                                                     detail::auditionRange ("LOW-MID", 2),  detail::auditionRange ("MIDRANGE", 3),
                                                                     detail::auditionRange ("UPPER-MID", 4), detail::auditionRange ("HIGH", 5) };
static_assert (detail::sameLetters (kAuditionRanges[0].name, pdsp::bands::kBands[0].name) && detail::sameLetters (kAuditionRanges[1].name, pdsp::bands::kBands[1].name)
                   && detail::sameLetters (kAuditionRanges[2].name, pdsp::bands::kBands[2].name) && detail::sameLetters (kAuditionRanges[3].name, pdsp::bands::kBands[3].name)
                   && detail::sameLetters (kAuditionRanges[4].name, pdsp::bands::kBands[4].name) && detail::sameLetters (kAuditionRanges[5].name, pdsp::bands::kBands[5].name),
               "the audition ranges are the shared bands, by name");

// The audition ranges that overlap a frequency range (e.g. one of the readouts' kEnergyRanges), as solo bits: what a check
// about that range plays.
inline uint32_t auditionBitsOver (double loHz, double hiHz)
{
    return static_cast<uint32_t> (pdsp::bands::bandsOverlapping (loHz, hiHz)) << kFirstBandSource;
}

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
    std::array<bool, kNumAuditionRanges> band = allOn();
    bool leftSoloed = false, rightSoloed = false; // a soloed channel is played on both sides, like a solo in a DAW

    bool bandsAll() const
    {
        for (const bool b : band)
            if (!b)
                return false;
        return true;
    }
    bool untouched() const { return mid && side && left && right && bandsAll(); }

    static std::array<bool, kNumAuditionRanges> allOn()
    {
        std::array<bool, kNumAuditionRanges> on {};
        on.fill (true);
        return on;
    }
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
    constexpr const char* names[kFirstBandSource] = { "MID", "SIDE", "LEFT", "RIGHT" };
    const int i = static_cast<int> (s);
    return i < kFirstBandSource ? names[i] : kAuditionRanges[i - kFirstBandSource].name;
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
// The ranges are split by the shared pdsp::BandSplitter (Linkwitz-Riley 4th-order crossovers at the edges of the six
// bands, each lower range run through the all-pass of the crossovers above it), so the ranges sum back to a flat-magnitude
// signal and muting one removes just that range.
class Audition
{
public:
    static constexpr double kRampSeconds = 0.01;

    void prepare (double sampleRate)
    {
        fs = sampleRate;
        rampStep = 1.0f / static_cast<float> (std::max (1.0, kRampSeconds * fs));
        splitter.prepare (fs);
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
    static double crossoverHz (int x) { return pdsp::bands::crossoverHz (x); }

private:
    // Splits one channel into the ranges and adds the audible ones.
    float splitAndMix (int ch, float x)
    {
        std::array<double, kNumAuditionRanges> part;
        splitter.split (ch, static_cast<double> (x), part);
        double sum = 0.0;
        for (size_t k = 0; k < part.size(); ++k)
            sum += static_cast<double> (gain[k]) * part[k];
        return static_cast<float> (sum);
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

    void resetFilters() { splitter.reset(); }

    double fs = 48000.0;
    float rampStep = 0.02f;

    std::atomic<uint32_t> requested { 0 };
    Audible target;
    bool settledAtUnity = true, filtersCold = true;

    float m[4] = { 1.0f, 0.0f, 0.0f, 1.0f }, mTarget[4] = { 1.0f, 0.0f, 0.0f, 1.0f };
    std::array<float, kNumAuditionRanges> gain = ones(), gainTarget = ones();
    float bandMix = 0.0f, bandMixTarget = 0.0f;

    pdsp::BandSplitter splitter;

    static std::array<float, kNumAuditionRanges> ones()
    {
        std::array<float, kNumAuditionRanges> v {};
        v.fill (1.0f);
        return v;
    }
};
} // namespace themis
