// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 The Pantheon authors

#pragma once

#include "Common.h"
#include "Loudness.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <vector>

namespace themis
{
// Listening aids that sit with the audition after every measurement: a streaming preview (the master turned down the way a
// normalising service would play it) and a reference player (a track you trust, at the same loudness as the master, to switch
// to and back). Like the audition, they change the output only while switched on, are bit-exact when idle, are never saved,
// and the processor skips them while the host renders offline.

// A reference track, already at the host's sample rate, and what it measures. Built off the audio thread, never changed after.
struct ReferenceClip
{
    std::vector<float> left, right;
    float integratedLufs = LoudnessMeter::kSilence;
    float samplePeakDb = -120.0f;

    int64_t length() const { return static_cast<int64_t> (left.size()); }

    // Measures the clip (BS.1770 integrated loudness and the sample peak). Call once the samples are in.
    void measure (double sampleRate)
    {
        LoudnessMeter m;
        m.prepare (sampleRate);
        m.process (left.data(), right.data(), static_cast<int> (left.size()));
        integratedLufs = m.integrated();
        float peak = 0.0f;
        for (size_t i = 0; i < left.size(); ++i)
            peak = std::max ({ peak, std::abs (left[i]), std::abs (right[i]) });
        samplePeakDb = peak > 0.0f ? 20.0f * std::log10 (peak) : -120.0f;
    }
};

namespace listen
{
// A normalising service turns a master louder than its target down and (most of them) leaves a quieter one alone, so the
// preview only ever lowers the level.
inline float previewGainDb (float masterLufs, float targetLufs)
{
    return masterLufs <= LoudnessMeter::kSilence + 1.0f ? 0.0f : std::min (0.0f, targetLufs - masterLufs);
}

// The reference is played at the loudness the master is heard at (after the preview, when it is on). A reference quieter than
// that is raised only as far as its peak allows (no clipping): `capped` says it could not be matched all the way.
struct ReferenceGain
{
    float db = 0.0f;
    bool capped = false;
};

inline ReferenceGain referenceGainDb (const ReferenceClip& clip, float heardMasterLufs)
{
    if (clip.integratedLufs <= LoudnessMeter::kSilence + 1.0f || heardMasterLufs <= LoudnessMeter::kSilence + 1.0f)
        return {};
    const float want = heardMasterLufs - clip.integratedLufs;
    const float room = -clip.samplePeakDb; // up to 0 dBFS sample peak
    return want > room ? ReferenceGain { room, true } : ReferenceGain { want, false };
}
} // namespace listen

// The audio-thread half. The message thread hands it a clip through `setClip` (the clip must outlive its use: see
// `retiredClipSafe`), switches the reference on or off and sets the two gains; process() ramps between them over 10 ms.
class ListenStage
{
public:
    void prepare (double sampleRate)
    {
        fs = sampleRate;
        rampStep = static_cast<float> (1.0 / (0.010 * sampleRate));
        mix = gain = 1.0f;
        refGain = 1.0f;
        freePosition = 0;
    }

    // ---- message thread ----
    void setClip (const ReferenceClip* c)
    {
        clip.store (c, std::memory_order_release);
        handedOver.fetch_add (1, std::memory_order_acq_rel);
    }
    // True once the audio thread has finished a block that started after the last setClip: the previous clip is no longer read.
    bool retiredClipSafe() const { return seen.load (std::memory_order_acquire) == handedOver.load (std::memory_order_acquire); }
    void setReferenceOn (bool on) { referenceOn.store (on, std::memory_order_relaxed); }
    bool isReferenceOn() const { return referenceOn.load (std::memory_order_relaxed); }
    void setMasterGainDb (float db) { masterGainDb.store (db, std::memory_order_relaxed); }
    void setReferenceGainDb (float db) { referenceGainDb.store (db, std::memory_order_relaxed); }
    bool idle() const { return !referenceOn.load (std::memory_order_relaxed) && std::abs (masterGainDb.load (std::memory_order_relaxed)) < 1.0e-4f; }

    // ---- audio thread ----
    // `hostPosition` is the host's timeline in samples (negative when the host gives none): the reference plays from the same
    // place modulo its length, so it follows the song when you jump around; without a timeline it runs on by itself.
    void process (float* l, float* r, int n, int64_t hostPosition)
    {
        const uint32_t generation = handedOver.load (std::memory_order_acquire);
        const ReferenceClip* c = clip.load (std::memory_order_acquire);
        const bool wantRef = referenceOn.load (std::memory_order_relaxed) && c != nullptr && c->length() > 0;
        const float targetMix = wantRef ? 0.0f : 1.0f; // 1 = master, 0 = reference
        const float targetGain = dbToGain (masterGainDb.load (std::memory_order_relaxed));
        const float targetRefGain = dbToGain (referenceGainDb.load (std::memory_order_relaxed));
        const int64_t pos = hostPosition >= 0 ? hostPosition : freePosition;
        freePosition = pos + n;

        // Exact comparisons on purpose: the ramps land exactly on their targets, and only exact unity may skip the loop.
        const bool atUnity = same (mix, 1.0f) && same (targetMix, 1.0f) && same (gain, 1.0f) && same (targetGain, 1.0f);
        if (!atUnity)
        {
            for (int i = 0; i < n; ++i)
            {
                // Linear ramps that land exactly on their target, so the stage is bit-exact again once it is back at unity.
                mix += std::clamp (targetMix - mix, -rampStep, rampStep);
                gain += std::clamp (targetGain - gain, -rampStep, rampStep);
                refGain += std::clamp (targetRefGain - refGain, -rampStep, rampStep);
                float refL = 0.0f, refR = 0.0f;
                if (mix < 1.0f && c != nullptr && c->length() > 0)
                {
                    const auto at = static_cast<size_t> (((pos + i) % c->length() + c->length()) % c->length());
                    refL = c->left[at] * refGain;
                    refR = c->right[at] * refGain;
                }
                l[i] = mix * gain * l[i] + (1.0f - mix) * refL;
                r[i] = mix * gain * r[i] + (1.0f - mix) * refR;
            }
        }
        seen.store (generation, std::memory_order_release);
    }

private:
    static bool same (float a, float b) { return !(a < b) && !(a > b); }
    static float dbToGain (float db) { return std::abs (db) < 1.0e-4f ? 1.0f : std::pow (10.0f, db / 20.0f); }

    double fs = 48000.0;
    float rampStep = 1.0f / 480.0f, mix = 1.0f, gain = 1.0f, refGain = 1.0f;
    int64_t freePosition = 0;
    std::atomic<const ReferenceClip*> clip { nullptr };
    std::atomic<uint32_t> handedOver { 0 }, seen { 0 };
    std::atomic<bool> referenceOn { false };
    std::atomic<float> masterGainDb { 0.0f }, referenceGainDb { 0.0f };
};
} // namespace themis
