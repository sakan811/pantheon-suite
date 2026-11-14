// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 The Pantheon authors

#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <pantheon/dsp/Biquad.h>
#include <vector>

// The whole compressor: no GUI, no JUCE. Nothing is allocated in process().
namespace hera
{
enum class Detector { peak, rms };
inline constexpr int kNumDetectors = 2;

inline constexpr float kMaxMakeupDb = 24.0f;
inline constexpr float kSlowAttackFactor = 20.0f; // the slow follower of AUTO release is this much slower to go down
inline constexpr float kFastAutoReleaseMs = 50.0f, kSlowAutoReleaseMs = 600.0f;
inline constexpr double kRmsMs = 10.0;            // the window of the RMS detector
inline constexpr double kPeakFallMs = 15.0;       // the peak detector holds a peak and lets it fall at this rate, so a low tone does not make the gain ripple
inline constexpr double kFloorDb = -120.0;
inline constexpr double kRestDb = 1e-3;           // a reduction under this, with nothing to reduce, is let go to exactly 0 (-78 dB of the gain: not audible, and the gain is then exactly 1)

// LOOK-AHEAD: the detector reads the sound as it arrives and the sound is heard this long after, so the gain is already down when a peak gets to the output.
// The choice is an index into this list (Off first), so a session keeps its meaning if a value is ever added at the end.
inline constexpr std::array<float, 5> kLookaheadMs { 0.0f, 1.0f, 2.0f, 5.0f, 10.0f };
inline constexpr int kNumLookaheads = (int) kLookaheadMs.size();

struct EngineParams
{
    float thresholdDb = -18.0f;
    float ratio = 2.0f;
    float attackMs = 10.0f, releaseMs = 150.0f;
    float kneeDb = 6.0f;
    float makeupDb = 0.0f;
    float mix = 1.0f;          // 0 dry .. 1 compressed
    float link = 1.0f;         // 0: each channel on its own .. 1: both share the louder
    float hpfHz = 20.0f;       // the detector's high-pass; at 20 or below it is not run
    Detector detector = Detector::rms;
    bool autoRelease = false;  // program-dependent release: RELEASE is ignored
    bool autoMakeup = false;   // makes up what the compression takes from a full-scale signal
    bool external = false;     // listen to the sidechain input
    int lookahead = 0;         // an index into kLookaheadMs: the sound is delayed by it, the detector is not
    bool match = false;        // LEVEL MATCH: the level the compressor and MAKEUP add or take away is taken off again (slowly), so an A/B is not judged by loudness
};

// The gain computer: how many dB the level is turned down, in a soft knee of `kneeDb` around the threshold. Positive.
inline double reductionDb (double levelDb, double thresholdDb, double ratio, double kneeDb) noexcept
{
    const double slope = 1.0 - 1.0 / std::fmax (ratio, 1.0), over = levelDb - thresholdDb;
    if (kneeDb <= 0.01)
        return over > 0.0 ? slope * over : 0.0;
    if (2.0 * over < -kneeDb)
        return 0.0;
    if (2.0 * std::fabs (over) <= kneeDb)
        return slope * (over + 0.5 * kneeDb) * (over + 0.5 * kneeDb) / (2.0 * kneeDb);
    return slope * over;
}

// What AUTO makeup adds: what the transfer curve takes from a signal at 0 dBFS, so full scale in is full scale out.
inline double autoMakeupDb (double thresholdDb, double ratio, double kneeDb) noexcept { return std::fmin (kMaxMakeupDb, reductionDb (0.0, thresholdDb, ratio, kneeDb)); }

class Engine
{
public:
    void prepare (double sampleRate, int lookaheadIndex = 0)
    {
        fs = sampleRate;
        ringSize = (int) std::ceil (0.001 * (double) kLookaheadMs.back() * fs) + 2;
        for (auto& c : ch)
            c.ring.assign ((size_t) ringSize, 0.0);
        delaySamples = delayOf (lookaheadIndex);
        latencySamples = delaySamples;
        rmsCoef = 1.0 - std::exp (-1.0 / (0.001 * kRmsMs * fs));
        matchEnergyK = 1.0 - std::exp (-1.0 / (0.6 * fs));
        matchGainK = 1.0 - std::exp (-1.0 / (0.15 * fs));
        peakFall = std::exp (-1.0 / (0.001 * kPeakFallMs * fs));
        reset();
    }

    void reset() noexcept
    {
        for (auto& c : ch)
        {
            auto ring = std::move (c.ring);
            c = Channel();
            c.ring = std::move (ring);
            std::fill (c.ring.begin(), c.ring.end(), 0.0);
        }
        writePos = 0;
        started = false;
        hpfFor = -1.0;
        matchNow = 1.0;
        msDry = msWet = 1.0e-6;
        matchDb.store (0.0f, std::memory_order_relaxed);
    }

    // The longest delay LOOK-AHEAD can ask for, in samples (what a bypassed plug-in has to delay by, for the largest setting).
    int maxLatencySamples() const noexcept { return ringSize - 2; }

    // In place. `keyL` / `keyR`: the sidechain (null: none); a mono key is given as `keyR == nullptr`.
    void process (float* left, float* right, int n, const EngineParams& p, const float* keyL = nullptr, const float* keyR = nullptr) noexcept
    {
        const bool useKey = p.external && keyL != nullptr;
        if (const int wanted = delayOf (p.lookahead); wanted != delaySamples)
        {
            // A new look-ahead restarts the delay line (a short gap), as every plugin of the repository does with a setting that moves its latency.
            delaySamples = wanted;
            latencySamples = wanted;
            for (auto& c : ch)
                std::fill (c.ring.begin(), c.ring.end(), 0.0);
        }
        if (p.hpfHz != hpfFor)
        {
            hpfFor = p.hpfHz;
            for (auto& c : ch)
                c.hpf.setHighpass (fs, std::clamp ((double) p.hpfHz, 20.0, 0.45 * fs), 0.7071);
        }
        const bool hpfOn = p.hpfHz > 20.5f;
        const double aFast = coefOf (p.attackMs), rFast = coefOf (p.releaseMs);
        const double aSlow = coefOf (p.attackMs * kSlowAttackFactor), rSlow = coefOf (kSlowAutoReleaseMs), rAutoFast = coefOf (kFastAutoReleaseMs);
        const double ratio = std::fmax (p.ratio, 1.0);
        const double makeupTarget = std::pow (10.0, ((double) p.makeupDb + (p.autoMakeup ? autoMakeupDb (p.thresholdDb, ratio, p.kneeDb) : 0.0)) / 20.0);
        const double mixTarget = std::clamp ((double) p.mix, 0.0, 1.0);
        const double smooth = 1.0 - std::exp (-1.0 / (0.005 * fs));
        if (! started)
        {
            makeup = makeupTarget;
            mix = mixTarget;
            started = true;
        }
        const double sqrt2 = std::sqrt (2.0);
        const bool deltaOn = delta.load (std::memory_order_relaxed), keyOn = scListen.load (std::memory_order_relaxed);
        float peakIn = 0.0f, peakOut = 0.0f;
        double deepest = 0.0;

        for (int i = 0; i < n; ++i)
        {
            makeup += (makeupTarget - makeup) * smooth;
            mix += (mixTarget - mix) * smooth;
            const double now[2] = { finite (left[i]), finite (right[i]) };
            // The sound that is heard is the delayed one (the same as the input when there is no look-ahead); the detector reads what is arriving.
            double dry[2];
            for (int c = 0; c < 2; ++c)
            {
                auto& ring = ch[(size_t) c].ring;
                ring[(size_t) writePos] = now[c];
                dry[c] = ring[(size_t) ((writePos + ringSize - delaySamples) % ringSize)];
            }
            writePos = (writePos + 1) % ringSize;
            const double key[2] = { useKey ? finite (keyL[i]) : now[0], useKey ? finite (keyR != nullptr ? keyR[i] : keyL[i]) : now[1] };

            // The detector: the key, high-passed, as a peak or as RMS (read as the peak of a sine).
            double level[2], heard[2];
            for (int c = 0; c < 2; ++c)
            {
                const double k = hpfOn ? ch[(size_t) c].hpf.process (key[c]) : key[c];
                heard[c] = k;
                if (p.detector == Detector::peak)
                {
                    auto& env = ch[(size_t) c].env;
                    env = std::fmax (std::fabs (k), env * peakFall);
                    level[c] = env;
                }
                else
                {
                    ch[(size_t) c].ms += rmsCoef * (k * k - ch[(size_t) c].ms);
                    level[c] = std::sqrt (std::fmax (ch[(size_t) c].ms, 0.0)) * sqrt2;
                }
            }
            const double loud = std::fmax (level[0], level[1]);
            double wet[2];
            for (int c = 0; c < 2; ++c)
            {
                auto& s = ch[(size_t) c];
                const double seen = level[c] + (double) p.link * (loud - level[c]);
                const double levelDb = seen > 1e-6 ? 20.0 * std::log10 (seen) : kFloorDb;
                const double wanted = reductionDb (levelDb, p.thresholdDb, ratio, p.kneeDb);
                double gr;
                if (p.autoRelease)
                {
                    s.fast += (wanted - s.fast) * (wanted > s.fast ? aFast : rAutoFast);
                    s.slow += (wanted - s.slow) * (wanted > s.slow ? aSlow : rSlow);
                    if (s.fast < kRestDb && wanted == 0.0) s.fast = 0.0;
                    if (s.slow < kRestDb && wanted == 0.0) s.slow = 0.0;
                    gr = std::fmax (s.fast, s.slow);
                }
                else
                {
                    s.fast += (wanted - s.fast) * (wanted > s.fast ? aFast : rFast);
                    if (s.fast < kRestDb && wanted == 0.0) s.fast = 0.0; // back at rest exactly: a gain of 1 is exactly 1
                    gr = s.fast;
                }
                deepest = std::fmax (deepest, gr);
                const double gain = gr == 0.0 ? 1.0 : std::exp (-gr * 0.11512925464970229);
                wet[c] = dry[c] * gain * makeup;
            }
            const bool pure = mix >= 1.0 - 1e-12;
            double out0 = pure ? wet[0] : dry[0] + mix * (wet[0] - dry[0]), out1 = pure ? wet[1] : dry[1] + mix * (wet[1] - dry[1]);
            // LEVEL MATCH: the RMS of the sound as it comes out against the sound as it came in (delayed like the output), over about 0.6 s; the gain that equals them follows at 0.15 s, within 12 dB.
            // Per sample, so the output does not depend on the block size; exactly 1.0 when off.
            msDry += matchEnergyK * (0.5 * (dry[0] * dry[0] + dry[1] * dry[1]) - msDry);
            msWet += matchEnergyK * (0.5 * (out0 * out0 + out1 * out1) - msWet);
            const double matchTarget = p.match ? std::clamp (std::sqrt ((msDry + 1.0e-9) / (msWet + 1.0e-9)), 0.25, 4.0) : 1.0;
            matchNow = std::fabs (matchTarget - matchNow) < 1e-9 ? matchTarget : matchNow + (matchTarget - matchNow) * matchGainK;
            out0 *= matchNow;
            out1 *= matchNow;
            // Monitoring, never saved: the detector's key after its high-pass (SC LISTEN), or only what the compressor changes (DELTA: output minus the sound that came in).
            if (keyOn)
            {
                out0 = heard[0];
                out1 = heard[1];
            }
            else if (deltaOn)
            {
                out0 -= dry[0];
                out1 -= dry[1];
            }
            left[i] = (float) out0;
            right[i] = (float) out1;
            peakIn = std::max (peakIn, (float) std::fmax (std::fabs (now[0]), std::fabs (now[1])));
            peakOut = std::max (peakOut, (float) std::fmax (std::fabs (out0), std::fabs (out1)));
        }
        matchDb.store ((float) (20.0 * std::log10 (matchNow)), std::memory_order_relaxed);
        if ((float) deepest > reductionMeter.load (std::memory_order_relaxed))
            reductionMeter.store ((float) deepest, std::memory_order_relaxed);
        if (peakIn > inPeak.load (std::memory_order_relaxed))
            inPeak.store (peakIn, std::memory_order_relaxed);
        if (peakOut > outPeak.load (std::memory_order_relaxed))
            outPeak.store (peakOut, std::memory_order_relaxed);
    }

    std::atomic<float> reductionMeter { 0.0f }; // the deepest reduction since the editor last asked, in dB
    std::atomic<bool> delta { false };          // DELTA: only what the compressor changes (output minus input). Monitoring, not a setting: never saved, never a parameter
    std::atomic<bool> scListen { false };       // SC LISTEN: the output is the detector's key after the sidechain high-pass, to hear what it hears. Monitoring, like delta
    std::atomic<float> matchDb { 0.0f };        // the gain LEVEL MATCH is applying now, for the display
    std::atomic<int> latencySamples { 0 };      // the look-ahead in samples: the processor tells the host
    std::atomic<float> inPeak { 0.0f }, outPeak { 0.0f };

private:
    struct Channel
    {
        pdsp::Biquad hpf;
        double ms = 0.0, env = 0.0, fast = 0.0, slow = 0.0;
        std::vector<double> ring; // the sound of the last look-ahead, for the delay
    };

    static inline double finite (float v) noexcept { return std::isfinite (v) ? (double) v : 0.0; }
    double coefOf (double ms) const noexcept { return 1.0 - std::exp (-1.0 / std::fmax (0.001 * ms * fs, 1.0)); }

    int delayOf (int index) const noexcept
    {
        return std::clamp ((int) std::lround (0.001 * (double) kLookaheadMs[(size_t) std::clamp (index, 0, kNumLookaheads - 1)] * fs), 0, ringSize - 2);
    }

    int ringSize = 2, writePos = 0, delaySamples = 0;
    double matchEnergyK = 0.001, matchGainK = 0.01, matchNow = 1.0, msDry = 1.0e-6, msWet = 1.0e-6;
    double fs = 48000.0, rmsCoef = 0.002, peakFall = 0.99, makeup = 1.0, mix = 1.0, hpfFor = -1.0;
    bool started = false;
    std::array<Channel, 2> ch;
};
} // namespace hera
