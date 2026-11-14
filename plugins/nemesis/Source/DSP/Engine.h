// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 The Pantheon authors

#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <pantheon/dsp/Biquad.h>
#include <vector>

// The whole de-esser: no GUI, no JUCE. Nothing is allocated in process().
namespace nemesis
{
enum class Mode { splitBand, wideband };
inline constexpr int kNumModes = 2;
enum class Detection { absolute, relative };
inline constexpr int kNumDetections = 2;

inline constexpr double kAttackMs = 0.3;       // fixed: a sibilant is over in tens of milliseconds
inline constexpr double kEnvelopeFallMs = 2.0; // the detector's peak hold between the cycles of the band
inline constexpr double kBodyMs = 30.0;        // the broadband level RELATIVE detection compares with
inline constexpr double kKneeDb = 6.0;
inline constexpr double kRestDb = 1e-3;        // a reduction under this, with nothing to reduce, is let go to exactly 0
inline constexpr double kFloorDb = -120.0;

// LOOK-AHEAD: the detector hears the sound as it arrives and the sound is heard this long after, so the first of a loud s is already turned down.
// The choice is an index into this list (Off first), so a session keeps its meaning if a value is ever added at the end.
inline constexpr std::array<float, 5> kLookaheadMs { 0.0f, 0.5f, 1.0f, 2.0f, 5.0f };
inline constexpr int kNumLookaheads = (int) kLookaheadMs.size();

// FIND (auto-find of FREQ): for kFindMs of the sound that arrives, a bank of narrow band-passes from kFindLoHz to kFindHiHz (in equal ratios) keeps the energy of the mono sum of each;
// the energy per hertz (a band-pass of constant Q is wider higher up) is smoothed over its neighbours and the strongest place, refined between the bins, is the answer.
inline constexpr int kFindBins = 17;
inline constexpr double kFindLoHz = 4000.0, kFindHiHz = 10000.0, kFindQ = 10.0, kFindMs = 1500.0;
inline constexpr double kFindMinEnergy = 1e-10; // mean square of the sound under which there is nothing to find (-100 dBFS)

struct EngineParams
{
    float freqHz = 6500.0f;     // the centre of the sibilance
    float q = 1.5f;             // the width of what is listened to (and, in SPLIT-BAND, of what is turned down)
    float thresholdDb = -30.0f; // absolute: dBFS of the band; relative: dB of the band under the whole sound
    float rangeDb = 8.0f;       // the most it takes off
    float releaseMs = 40.0f;
    Mode mode = Mode::splitBand;
    Detection detection = Detection::absolute;
    bool listen = false;        // hear the band the detector hears
    bool delta = false;         // hear what is turned down (the sound minus the output); it takes the place of LISTEN while it is on
    int lookahead = 0;          // an index into kLookaheadMs: the sound is delayed by it, the detector is not
};

// The reduction for a level `over` the threshold: it holds the band at the threshold (infinite ratio), softly over a knee, up to the range.
inline double reductionDb (double levelDb, double thresholdDb, double rangeDb) noexcept
{
    const double over = levelDb - thresholdDb;
    double r;
    if (2.0 * over < -kKneeDb)
        r = 0.0;
    else if (2.0 * std::fabs (over) <= kKneeDb)
        r = (over + 0.5 * kKneeDb) * (over + 0.5 * kKneeDb) / (2.0 * kKneeDb);
    else
        r = over;
    return std::clamp (r, 0.0, std::fmax (rangeDb, 0.0));
}

class Engine
{
public:
    void prepare (double sampleRate, int lookaheadIndex = 0)
    {
        fs = sampleRate;
        ringSize = (int) std::ceil (0.001 * (double) kLookaheadMs.back() * fs) + 2;
        for (auto& c : ch)
        {
            c.ringSound.assign ((size_t) ringSize, 0.0);
            c.ringBand.assign ((size_t) ringSize, 0.0);
        }
        delaySamples = delayOf (lookaheadIndex);
        latencySamples = delaySamples;
        attackCoef = coefOf (kAttackMs);
        envFall = std::exp (-1.0 / (0.001 * kEnvelopeFallMs * fs));
        bodyCoef = coefOf (kBodyMs);
        for (int k = 0; k < kFindBins; ++k)
            findBank[(size_t) k].setBandpass (fs, std::fmin (findHz (k), 0.45 * fs), kFindQ);
        reset();
    }

    void reset() noexcept
    {
        for (auto& c : ch)
        {
            auto sound = std::move (c.ringSound), band = std::move (c.ringBand);
            c = Channel();
            c.ringSound = std::move (sound);
            c.ringBand = std::move (band);
            std::fill (c.ringSound.begin(), c.ringSound.end(), 0.0);
            std::fill (c.ringBand.begin(), c.ringBand.end(), 0.0);
        }
        writePos = 0;
        gr = 0.0;
        designedFor = { -1.0, -1.0 };
        started = false;
        findActive = false;
        finding.store (false, std::memory_order_relaxed);
    }

    // In place. Everything that is not a number is silence.
    void process (float* left, float* right, int n, const EngineParams& p) noexcept
    {
        const double f = std::clamp ((double) p.freqHz, 1000.0, std::fmin (16000.0, 0.45 * fs)), q = std::clamp ((double) p.q, 0.3, 8.0);
        if (designedFor[0] != f || designedFor[1] != q)
        {
            designedFor = { f, q };
            for (auto& c : ch)
                c.band.setBandpass (fs, f, q); // the coefficients move, the states stay
        }
        if (const int wanted = delayOf (p.lookahead); wanted != delaySamples)
        {
            // A new look-ahead restarts the delay (a short gap), as every plugin of the repository does with a setting that moves its latency.
            delaySamples = wanted;
            latencySamples = wanted;
            for (auto& c : ch)
            {
                std::fill (c.ringSound.begin(), c.ringSound.end(), 0.0);
                std::fill (c.ringBand.begin(), c.ringBand.end(), 0.0);
            }
        }
        const double releaseCoef = coefOf (p.releaseMs);
        if (findRequest.exchange (false, std::memory_order_relaxed))
        {
            for (int k = 0; k < kFindBins; ++k)
            {
                findBank[(size_t) k].reset();
                findEnergy[(size_t) k] = 0.0;
            }
            findSamples = 0;
            findTotal = 0.0;
            findLength = std::max (1, (int) (0.001 * kFindMs * fs));
            findActive = true;
            finding.store (true, std::memory_order_relaxed);
        }
        float deepest = 0.0f;

        for (int i = 0; i < n; ++i)
        {
            const double x[2] = { finite (left[i]), finite (right[i]) };
            if (findActive)
                feedFind (0.5 * (x[0] + x[1]));
            double band[2], level = 0.0, body = 0.0;
            for (int c = 0; c < 2; ++c)
            {
                auto& s = ch[(size_t) c];
                band[c] = s.band.process (x[c]);
                s.env = std::fmax (std::fabs (band[c]), s.env * envFall);
                s.body += bodyCoef * (x[c] * x[c] - s.body);
                level = std::fmax (level, s.env);
                body = std::fmax (body, s.body);
            }
            // One gain for both channels, so the image does not move.
            double levelDb = level > 1e-6 ? 20.0 * std::log10 (level) : kFloorDb;
            if (p.detection == Detection::relative && level > 1e-6) // in silence there is nothing to be relative to: stay at the floor, so a stretch of silence does not leave the reduction at full range
                levelDb -= 10.0 * std::log10 (std::fmax (body, 1e-12) * 2.0); // the whole sound, read as the peak of a sine
            const double wanted = reductionDb (levelDb, p.thresholdDb, p.rangeDb);
            gr += (wanted - gr) * (wanted > gr ? attackCoef : releaseCoef);
            if (gr < kRestDb && wanted == 0.0)
                gr = 0.0; // at rest exactly: a gain of 1 is exactly 1
            const double gain = gr == 0.0 ? 1.0 : std::exp (-gr * 0.11512925464970229);
            deepest = std::max (deepest, (float) gr);

            // What is heard is the delayed sound and its delayed band (the same as the input when there is no look-ahead); the gain is the one just found.
            const int readPos = (writePos + ringSize - delaySamples) % ringSize;
            for (int c = 0; c < 2; ++c)
            {
                auto& s = ch[(size_t) c];
                s.ringSound[(size_t) writePos] = x[c];
                s.ringBand[(size_t) writePos] = band[c];
                const double heard = s.ringSound[(size_t) readPos], heardBand = s.ringBand[(size_t) readPos];
                double y;
                if (p.delta)
                    y = p.mode == Mode::wideband ? heard * (1.0 - gain) : (1.0 - gain) * heardBand; // what the other two branches take off: nothing at all when the gain is 1
                else if (p.listen)
                    y = heardBand;
                else if (p.mode == Mode::wideband)
                    y = heard * gain;
                else
                    y = heard - (1.0 - gain) * heardBand; // the band turned down, the rest left alone (and exactly the input when the gain is 1)
                (c == 0 ? left : right)[i] = (float) y;
            }
            writePos = (writePos + 1) % ringSize;
        }
        if (deepest > reductionMeter.load (std::memory_order_relaxed))
            reductionMeter.store (deepest, std::memory_order_relaxed);
    }

    std::atomic<float> reductionMeter { 0.0f }; // the deepest reduction since the editor last asked, in dB
    std::atomic<int> latencySamples { 0 };      // the look-ahead in samples: the processor tells the host

    // From the message thread: listen for kFindMs of the sound and find the strongest place between kFindLoHz and kFindHiHz. `findCount` goes up when an answer is ready; `foundHz` is it (0: the sound was too quiet).
    void startFind() noexcept { findRequest.store (true, std::memory_order_relaxed); }
    static double findHz (int bin) noexcept { return kFindLoHz * std::pow (kFindHiHz / kFindLoHz, (double) bin / (double) (kFindBins - 1)); }
    std::atomic<bool> finding { false };
    std::atomic<unsigned> findCount { 0 };
    std::atomic<float> foundHz { 0.0f };

private:
    struct Channel
    {
        pdsp::Biquad band;
        double env = 0.0, body = 0.0;
        std::vector<double> ringSound, ringBand; // the last look-ahead of the sound and of its band, for the delay
    };

    void feedFind (double mono) noexcept
    {
        findTotal += mono * mono;
        for (int k = 0; k < kFindBins; ++k)
        {
            const double v = findBank[(size_t) k].process (mono);
            findEnergy[(size_t) k] += v * v;
        }
        if (++findSamples < findLength)
            return;
        findActive = false;
        double density[kFindBins], smooth[kFindBins];
        for (int k = 0; k < kFindBins; ++k)
            density[k] = findEnergy[(size_t) k] / findHz (k);
        for (int k = 0; k < kFindBins; ++k)
            smooth[k] = 0.25 * (density[std::max (k - 1, 0)] + 2.0 * density[k] + density[std::min (k + 1, kFindBins - 1)]);
        int best = 0;
        for (int k = 1; k < kFindBins; ++k)
            if (smooth[k] > smooth[best])
                best = k;
        double at = (double) best;
        if (best > 0 && best < kFindBins - 1)
        {
            const double a = smooth[best - 1], b = smooth[best], c = smooth[best + 1], denom = a - 2.0 * b + c;
            if (denom < 0.0)
                at += std::clamp (0.5 * (a - c) / denom, -0.5, 0.5);
        }
        const bool loud = findTotal / (double) findLength > kFindMinEnergy;
        foundHz.store (loud ? (float) (kFindLoHz * std::pow (kFindHiHz / kFindLoHz, at / (double) (kFindBins - 1))) : 0.0f, std::memory_order_relaxed);
        finding.store (false, std::memory_order_relaxed);
        findCount.fetch_add (1, std::memory_order_release);
    }

    static inline double finite (float v) noexcept { return std::isfinite (v) ? (double) v : 0.0; }
    double coefOf (double ms) const noexcept { return 1.0 - std::exp (-1.0 / std::fmax (0.001 * ms * fs, 1.0)); }

    int delayOf (int index) const noexcept
    {
        return std::clamp ((int) std::lround (0.001 * (double) kLookaheadMs[(size_t) std::clamp (index, 0, kNumLookaheads - 1)] * fs), 0, ringSize - 2);
    }

    int ringSize = 2, writePos = 0, delaySamples = 0;
    double fs = 48000.0, attackCoef = 0.05, envFall = 0.97, bodyCoef = 0.001, gr = 0.0;
    std::array<double, 2> designedFor { -1.0, -1.0 };
    bool started = false;
    std::array<Channel, 2> ch;
    std::array<pdsp::Biquad, kFindBins> findBank;
    std::array<double, kFindBins> findEnergy {};
    double findTotal = 0.0;
    int findSamples = 0, findLength = 1;
    bool findActive = false;
    std::atomic<bool> findRequest { false };
};
} // namespace nemesis
