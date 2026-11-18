// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 The Pantheon authors

#pragma once

#include "Loudness.h"
#include <array>
#include <atomic>
#include <pantheon/dsp/TruePeak.h>
#include <vector>

// The whole gain-staging tool: no GUI, no JUCE. A trim, a polarity, the listening modes (mono, left, right, side, swap), the loudness and true-peak meters, and a reference
// track to compare with, level-matched. Everything is sized in prepare(); process() allocates and locks nothing.
namespace hermes
{
enum class Listen { stereo, mono, left, right, side, swap };
inline constexpr int kNumListen = 6;

inline constexpr float kMaxTrimDb = 24.0f;
inline constexpr double kMatchLimitDb = 24.0;
inline constexpr double kCrossfadeMs = 20.0;
inline constexpr int kTick = 64;
// What AUTO TRIM brings the integrated loudness to, by the choice of its parameter (the index): -14 for streaming (what the streaming services play at),
// -16 for a stereo podcast and -19 for a mono one (the podcast figures), -23 for broadcast (EBU R128). The README has the sources.
inline constexpr std::array<double, 4> kAutoTrimTargets { -14.0, -16.0, -19.0, -23.0 };
// The true-peak ceiling of AUTO TRIM's peak option: -1 dBTP, what the streaming and podcast specs and EBU R128 ask.
inline constexpr double kAutoTrimPeakDb = -1.0;

struct EngineParams
{
    float trimDb = 0.0f;
    bool polarity = false;      // both channels inverted
    Listen listen = Listen::stereo;
    bool reference = false;     // hear the reference instead of the sound (B)
    bool levelMatch = true;     // bring the reference to the loudness of the sound
    float referenceTrimDb = 0.0f;
    bool resetOnPlay = false;   // read by the processor, not the engine: clear the meters when the host starts playing
    int autoTarget = 0;         // read by AUTO TRIM only (an index of kAutoTrimTargets)
    bool autoPeak = false;      // read by AUTO TRIM only: stop at the true-peak ceiling if that comes first
};

// A track to compare with: decoded and brought to the sample rate of the session by whoever loads it (the editor's thread), its loudness measured by the same meter.
struct Reference
{
    std::vector<float> left, right;
    double integratedLufs = -200.0;
    int length() const noexcept { return (int) left.size(); }
};

// The loudness of a whole track, for the reference (the same meter the sound is read with).
inline double measureIntegrated (const std::vector<float>& l, const std::vector<float>& r, double fs)
{
    LoudnessMeter meter;
    meter.prepare (fs);
    for (size_t i = 0; i < l.size(); ++i)
        meter.push (l[i], r[i]);
    return meter.integrated();
}

// The trim that brings the integrated loudness (read after the current trim) to the target: `trimDb + target - integrated`, within the trim's range.
// Nothing measured yet (under the absolute gate): the trim as it is.
// The trim AUTO TRIM sets. The loudness gain is `target - integrated` (both read after the current trim). With the peak option it is the smaller of that and the
// headroom to the ceiling, `ceiling - truePeak`, so a quiet sound with a high crest stops at the ceiling and says so (`peakLimited`). Nothing read yet: the trim as it is.
struct AutoTrim
{
    float trimDb;
    bool peakLimited;
};
inline AutoTrim autoTrim (float trimDb, double integratedLufs, double targetLufs, double truePeakDb, bool peakOption, double ceilingDb = kAutoTrimPeakDb) noexcept
{
    if (! (integratedLufs > kAbsoluteGateLufs))
        return { trimDb, false };
    double gain = targetLufs - integratedLufs;
    bool peakLimited = false;
    if (peakOption && truePeakDb > -150.0 && ceilingDb - truePeakDb < gain)
    {
        gain = ceilingDb - truePeakDb;
        peakLimited = true;
    }
    return { (float) std::clamp ((double) trimDb + gain, (double) -kMaxTrimDb, (double) kMaxTrimDb), peakLimited };
}

inline float autoTrimDb (float trimDb, double integratedLufs, double targetLufs = kAutoTrimTargets[0]) noexcept
{
    return autoTrim (trimDb, integratedLufs, targetLufs, -200.0, false).trimDb;
}

class Engine
{
public:
    void prepare (double sampleRate)
    {
        fs = sampleRate;
        smooth = 1.0 - std::exp (-1.0 / (0.005 * fs));
        fade = 1.0 - std::exp (-1.0 / (0.001 * kCrossfadeMs * fs));
        meter.prepare (fs);
        reset();
    }

    void reset() noexcept
    {
        meter.reset();
        for (auto& r : peakReader)
            r.reset();
        started = false;
        trimNow = 1.0;
        listenMix = 0.0;
        refGain = 1.0;
        position = 0;
        tickCount = 0;
        refGainT = refGain = 1.0;
        matchShown = 0.0;
        truePeakMax = samplePeakMax = 0.0;
    }

    // From the message thread: the reference to compare with (null: none). The previous one is not freed here: the caller keeps it until the audio thread has moved on (see the processor).
    void setReference (const Reference* r) noexcept { reference.store (r, std::memory_order_release); restart.store (true, std::memory_order_relaxed); }
    void restartReference() noexcept { restart.store (true, std::memory_order_relaxed); }
    // From the message thread: play on from this fraction (0 to 1) of the reference. The audio thread takes it, like the restart.
    void seekReference (float fraction) noexcept { seekTo.store (std::clamp (fraction, 0.0f, 1.0f), std::memory_order_relaxed); }
    void resetMeters() noexcept { clear.store (true, std::memory_order_relaxed); }

    // In place. Everything that is not a number is silence.
    void process (float* left, float* right, int n, const EngineParams& p) noexcept
    {
        if (clear.exchange (false, std::memory_order_relaxed))
        {
            meter.reset();
            for (auto& r : peakReader)
                r.reset();
            truePeakMax = samplePeakMax = 0.0;
        }
        if (restart.exchange (false, std::memory_order_relaxed))
            position = 0;
        const Reference* ref = reference.load (std::memory_order_acquire);
        const int refLength = ref != nullptr ? ref->length() : 0;
        const float seek = seekTo.exchange (-1.0f, std::memory_order_relaxed);
        if (seek >= 0.0f && refLength > 0)
            position = std::min ((int) ((double) seek * refLength), refLength - 1);
        // The reference may have been swapped for a shorter one after `restart` was taken: the position is only ever read inside the reference that was just loaded.
        if (position >= refLength)
            position = 0;
        // std::clamp lets a NaN through, and a NaN gain would stay in the output for good: a non-finite TRIM is 0 dB.
        const float trimDb = std::isfinite (p.trimDb) ? p.trimDb : 0.0f;
        const double trimT = std::pow (10.0, std::clamp (trimDb, -kMaxTrimDb, kMaxTrimDb) / 20.0) * (p.polarity ? -1.0 : 1.0);
        if (! started)
        {
            trimNow = trimT;
            started = true;
        }
        const bool wantRef = p.reference && refLength > 0;
        for (int i = 0; i < n; ++i)
        {
            // The match is read every kTick samples, counted from the start, so the output does not depend on the host's block size.
            if (tickCount == 0)
            {
                double matchDb = 0.0;
                if (p.levelMatch && refLength > 0)
                {
                    const double program = meter.integrated();
                    if (program > kAbsoluteGateLufs + 1.0 && ref->integratedLufs > kAbsoluteGateLufs)
                        matchDb = std::clamp (program - ref->integratedLufs, -kMatchLimitDb, kMatchLimitDb);
                }
                refGainT = std::pow (10.0, (matchDb + (double) p.referenceTrimDb) / 20.0);
                matchShown = matchDb;
            }
            tickCount = (tickCount + 1) % kTick;
            trimNow += (trimT - trimNow) * smooth;
            const double l = (std::isfinite (left[i]) ? (double) left[i] : 0.0) * trimNow, r = (std::isfinite (right[i]) ? (double) right[i] : 0.0) * trimNow;

            // The meters read the sound after the trim, whatever is being listened to.
            meter.push (l, r);
            samplePeakMax = std::fmax (samplePeakMax, std::fmax (std::fabs (l), std::fabs (r)));
            truePeakMax = std::fmax (truePeakMax, std::fmax (peakReader[0].push (l), peakReader[1].push (r)));

            double ol = l, or_ = r;
            switch (p.listen)
            {
                case Listen::stereo: break;
                case Listen::mono: ol = or_ = 0.5 * (l + r); break;
                case Listen::left: or_ = l; break;
                case Listen::right: ol = r; break;
                case Listen::side: ol = or_ = 0.5 * (l - r); break;
                case Listen::swap: std::swap (ol, or_); break;
            }
            // A to B and back: a crossfade of 20 ms, so a switch does not click. The reference loops; it moves on whether it is heard or not, so B is a track that plays on.
            listenMix += ((wantRef ? 1.0 : 0.0) - listenMix) * fade;
            if (std::fabs ((wantRef ? 1.0 : 0.0) - listenMix) < 1e-9)
                listenMix = wantRef ? 1.0 : 0.0;
            refGain += (refGainT - refGain) * smooth;
            if (refLength > 0)
            {
                if (listenMix > 0.0)
                {
                    const double rl = (double) ref->left[(size_t) position] * refGain, rr = (double) ref->right[(size_t) position] * refGain;
                    ol += listenMix * (rl - ol);
                    or_ += listenMix * (rr - or_);
                }
                position = (position + 1) % refLength;
            }
            left[i] = (float) ol;
            right[i] = (float) or_;
        }
        momentary.store ((float) meter.momentary(), std::memory_order_relaxed);
        shortTerm.store ((float) meter.shortTerm(), std::memory_order_relaxed);
        integrated.store ((float) meter.integrated(), std::memory_order_relaxed);
        range.store ((float) meter.range(), std::memory_order_relaxed);
        truePeakDb.store (truePeakMax > 1e-9 ? (float) (20.0 * std::log10 (truePeakMax)) : -200.0f, std::memory_order_relaxed);
        samplePeakDb.store (samplePeakMax > 1e-9 ? (float) (20.0 * std::log10 (samplePeakMax)) : -200.0f, std::memory_order_relaxed);
        referencePosition.store (refLength > 0 ? (float) position / (float) refLength : 0.0f, std::memory_order_relaxed);
        referenceMatchDb.store ((float) matchShown, std::memory_order_relaxed);
    }

    // What the editor reads (LUFS, LU and dB; -200 is "nothing yet").
    std::atomic<float> momentary { -200.0f }, shortTerm { -200.0f }, integrated { -200.0f }, range { 0.0f }, truePeakDb { -200.0f }, samplePeakDb { -200.0f };
    std::atomic<float> referencePosition { 0.0f }, referenceMatchDb { 0.0f };

private:
    double fs = 48000.0, smooth = 0.004, fade = 0.002, trimNow = 1.0, listenMix = 0.0, refGain = 1.0, refGainT = 1.0, matchShown = 0.0, truePeakMax = 0.0, samplePeakMax = 0.0;
    int position = 0, tickCount = 0;
    bool started = false;
    std::atomic<const Reference*> reference { nullptr };
    std::atomic<bool> restart { false }, clear { false };
    std::atomic<float> seekTo { -1.0f };
    LoudnessMeter meter;
    std::array<pdsp::TruePeakReader, 2> peakReader;
};
} // namespace hermes
