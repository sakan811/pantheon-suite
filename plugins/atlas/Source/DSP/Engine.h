// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 The Pantheon authors

#pragma once

#include "Compressor.h"
#include "LinearSplit.h"
#include <atomic>

namespace atlas
{
enum class PhaseMode { zeroLatency, linear };
inline constexpr int kNumOsModes = 3; // off, 2x, 4x

// Look-ahead: the compressors read the audio this much before it is heard, so a fast peak is already being turned down when it arrives.
// The whole sound is delayed by it (the host compensates). A choice, not a knob: the delay is the plugin's latency, and a latency that
// moved with a knob would re-align the host's track on every turn.
inline constexpr int kNumLookahead = 5;
inline constexpr std::array<double, kNumLookahead> kLookaheadMs { 0.0, 1.0, 2.0, 5.0, 10.0 };
inline int lookaheadSamples (double sampleRate, int index) noexcept
{
    return juce::roundToInt (kLookaheadMs[(size_t) std::clamp (index, 0, kNumLookahead - 1)] * 0.001 * sampleRate);
}

struct EngineParams
{
    std::array<BandParams, kBands> bands;
    std::array<double, kCrossovers> crossover = pdsp::bands::defaultCrossovers();
    std::array<bool, kCrossovers> split { true, true, true, true, true }; // a split that is off joins the band above it to the one below
    PhaseMode phase = PhaseMode::zeroLatency;
    int oversampling = 0; // 0 = off, 1 = 2x, 2 = 4x
    int lookahead = 0;    // index into kLookaheadMs
    double outGainDb = 0.0;
    double mix = 1.0;     // 0 = the dry bands, 1 = the compressed ones: what the compressors take off or add is scaled by it (parallel compression)
};

// The edges are kept in order and at least this ratio apart; the editor and the engine agree on it.
inline constexpr double kMinCrossoverRatio = 1.2;
inline std::array<double, kCrossovers> orderedCrossovers (std::array<double, kCrossovers> hz) noexcept
{
    for (int k = 1; k < kCrossovers; ++k)
        hz[(size_t) k] = std::max (hz[(size_t) k], hz[(size_t) (k - 1)] * kMinCrossoverRatio);
    return hz;
}

// Which band each of the six pieces of the split belongs to: its own, or the nearest one below it whose upper split is on.
// So one to six bands: a band is piece b when b is 0 or split b - 1 is on, and it takes every piece up to the next split.
inline std::array<int, kBands> bandOwners (const std::array<bool, kCrossovers>& split) noexcept
{
    std::array<int, kBands> owner {};
    for (int b = 1; b < kBands; ++b)
        owner[(size_t) b] = split[(size_t) (b - 1)] ? b : owner[(size_t) (b - 1)];
    return owner;
}

// Whole-sample delay: the dry band waits for the oversampled path.
class IntDelay
{
public:
    void prepare (int maxDelay) { buffer.assign ((size_t) maxDelay + 1, 0.0f); pos = 0; delay = 0; }
    void setDelay (int d) noexcept { delay = std::clamp (d, 0, (int) buffer.size() - 1); std::fill (buffer.begin(), buffer.end(), 0.0f); pos = 0; }
    void clear() noexcept { std::fill (buffer.begin(), buffer.end(), 0.0f); pos = 0; }
    // One sample in, the one from `delay` samples ago out.
    inline float push (float x) noexcept
    {
        if (delay == 0)
            return x;
        const float out = buffer[(size_t) pos];
        buffer[(size_t) pos] = x;
        if (++pos == delay) pos = 0;
        return out;
    }
    inline void process (float* x, int n) noexcept
    {
        if (delay == 0)
            return;
        for (int i = 0; i < n; ++i)
        {
            const float in = x[i];
            x[i] = buffer[(size_t) pos];
            buffer[(size_t) pos] = in;
            if (++pos == delay) pos = 0;
        }
    }
private:
    std::vector<float> buffer;
    int pos = 0, delay = 0;
};

// The whole signal path: split into the six bands, compress each, add them up.
//
//   input -> split (Linkwitz-Riley, or the linear-phase kernels) -> per band: [what the compressor takes off, made at 1x, 2x or 4x
//   the sample rate] added to the band -> makeup and gain match -> solo / delta -> sum -> output gain
//
// A band adds its *difference* (band x (gain - 1)) to itself, so a band whose gain is 1 is its input bit for bit, and only the
// difference goes through the oversampling filters.
class Engine
{
public:
    static constexpr int tick = 64;            // everything that changes with time steps here, counted from the start: the output does not depend on the host's block size
    static constexpr double kMatchSeconds = 0.4;   // the loudness window of gain match
    static constexpr double kMatchLimitDb = 12.0;
    static constexpr double kFadeSeconds = 0.01;   // solo and delta fade in and out

    // Monitoring, not settings: never saved, and the editor ends them when it closes. Bit b = band b.
    struct Monitor
    {
        std::atomic<unsigned> solo { 0 };  // only these bands are heard (0: all)
        std::atomic<unsigned> delta { 0 }; // of these bands only what the compressor changes is heard (output minus input)
    } monitor;

    std::array<std::atomic<float>, kBands> reductionMeter {}; // dB taken off (negative: lifted), the largest change over the last tick
    std::array<std::atomic<float>, kBands> matchMeter {};     // dB gain match is adding
    std::array<std::atomic<float>, kBands> levelMeter {};     // the level the band's compressor read (dB), the most over the last tick
    std::atomic<float> inPeak { 0.0f }, outPeak { 0.0f };
    std::atomic<int> latencySamples { 0 };

    void prepare (double sampleRate, PhaseMode initialPhase = PhaseMode::zeroLatency, int initialOs = 0, int initialLookahead = 0)
    {
        fs = sampleRate;
        linear.prepare (fs);
        scLinear.prepare (fs);
        int maxOsLatency = 0;
        for (int m = 1; m < kNumOsModes; ++m)
        {
            for (auto& b : band)
            {
                auto os = std::make_unique<juce::dsp::Oversampling<float>> ((size_t) 2, (size_t) m, juce::dsp::Oversampling<float>::filterHalfBandFIREquiripple, true, true);
                os->initProcessing ((size_t) tick);
                osLatency[(size_t) m] = juce::roundToInt (os->getLatencyInSamples());
                b.os[(size_t) m] = std::move (os);
            }
            maxOsLatency = std::max (maxOsLatency, osLatency[(size_t) m]);
        }
        const int maxLook = lookaheadSamples (fs, kNumLookahead - 1);
        for (auto& b : band)
        {
            for (auto& d : b.dry) d.prepare (maxOsLatency + maxLook);
            for (auto& d : b.look) d.prepare (maxLook * (1 << (kNumOsModes - 1)));
        }
        phase = initialPhase;
        osMode = std::clamp (initialOs, 0, kNumOsModes - 1);
        lookIndex = std::clamp (initialLookahead, 0, kNumLookahead - 1);
        reset();
    }

    void reset()
    {
        counter = 0;
        splitter.prepare (fs);
        scSplitter.prepare (fs);
        scActive = false;
        appliedCrossover = {};
        outGain = outGainStep = 0.0;
        first = true;
        configure();
        for (auto& b : band)
        {
            b.grA = b.grB = b.msIn = b.msOut = 0.0;
            b.levelA.reset();
            b.levelB.reset();
            b.gain = b.gainTarget = 1.0;
            b.gainStep = 0.0;
            b.heard = b.heardTarget = 1.0;
            b.dryHeard = b.dryHeardTarget = 0.0;
            b.heardStep = b.dryHeardStep = 0.0;
            b.idle = true;
        }
    }

    // The largest latency any mode combination can report (the host's bypass delay is sized for it).
    int maxLatency() const noexcept { return latencyFor (PhaseMode::linear, kNumOsModes - 1, kNumLookahead - 1); }

    int latencyFor (PhaseMode p, int os, int look = 0) const noexcept { return (p == PhaseMode::linear ? linear.latency() : 0) + osLatency[(size_t) os] + lookaheadSamples (fs, look); }

    // In place. Must not allocate, lock or log. `scL` / `scR` are the sidechain input, only listened to (never heard): null when
    // none is connected, and then a band set to listen to it listens to itself. A mono sidechain is `scL` alone.
    void process (float* l, float* r, int numSamples, const EngineParams& p, const float* scL = nullptr, const float* scR = nullptr) noexcept
    {
        if (scL != nullptr && scR == nullptr)
            scR = scL;
        scPresent = scL != nullptr;
        float peakIn = 0.0f, peakOut = 0.0f;
        for (int start = 0; start < numSamples;)
        {
            const int phaseInTick = (int) (counter % tick);
            if (phaseInTick == 0)
                control (p);
            const int n = std::min (numSamples - start, tick - phaseInTick);
            segment (l + start, r + start, scPresent ? scL + start : nullptr, scPresent ? scR + start : nullptr, n, peakIn, peakOut);
            counter += n;
            start += n;
        }
        inPeak.store (std::max (inPeak.load (std::memory_order_relaxed), peakIn), std::memory_order_relaxed);
        outPeak.store (std::max (outPeak.load (std::memory_order_relaxed), peakOut), std::memory_order_relaxed);
    }

private:
    struct BandState
    {
        GainFollower follower;
        Channels channels = Channels::stereo;
        Level levelA, levelB;
        double grA = 0.0, grB = 0.0;                 // dB taken off: left / mid (and both, when linked), right / side
        double gain = 1.0, gainTarget = 1.0, gainStep = 0.0; // makeup and gain match
        double msIn = 0.0, msOut = 0.0;              // the band's power before and after the compressor (before makeup)
        double heard = 1.0, heardTarget = 1.0, heardStep = 0.0;          // solo
        double dryHeard = 0.0, dryHeardTarget = 0.0, dryHeardStep = 0.0; // delta: the input is taken back out
        bool idle = true;                            // switched off and at rest: the compressor path is not run
        bool listenSc = false;                       // its compressor reads the sidechain's band (latched at a tick)
        float maxReduction = 0.0f;                   // the largest change of the last tick, dB: positive took off, negative lifted
        double maxLevelSq = 0.0;
        std::array<std::unique_ptr<juce::dsp::Oversampling<float>>, kNumOsModes> os; // [1] 2x, [2] 4x
        std::array<IntDelay, 2> dry;
        std::array<IntDelay, 2> look;                // the audio waiting for the look-ahead, at the compressors' rate
    };

    // The phase or oversampling mode changed (or playback starts): new latency, clean history.
    void configure()
    {
        linear.reset();
        splitter.reset();
        scLinear.reset();
        scSplitter.reset();
        const int lookSamples = lookaheadSamples (fs, lookIndex);
        for (auto& b : band)
        {
            for (auto& d : b.dry) d.setDelay (osLatency[(size_t) osMode] + lookSamples);
            for (auto& d : b.look) d.setDelay (lookSamples * (1 << osMode));
            for (auto& o : b.os)
                if (o != nullptr) o->reset();
            b.grA = b.grB = 0.0;
            b.levelA.reset();
            b.levelB.reset();
        }
        latencySamples.store (latencyFor (phase, osMode, lookIndex), std::memory_order_relaxed);
    }

    // ---- once per tick -------------------------------------------------------------------------
    void control (const EngineParams& p) noexcept
    {
        const int os = std::clamp (p.oversampling, 0, kNumOsModes - 1);
        const int look = std::clamp (p.lookahead, 0, kNumLookahead - 1);
        if (p.phase != phase || os != osMode || look != lookIndex)
        {
            phase = p.phase;
            osMode = os;
            lookIndex = look;
            configure();
        }
        if (const auto xo = orderedCrossovers (p.crossover); xo != appliedCrossover)
        {
            appliedCrossover = xo;
            splitter.setCrossovers (fs, xo);
            linear.setCrossovers (xo);
            scSplitter.setCrossovers (fs, xo);
            scLinear.setCrossovers (xo);
        }

        owner = bandOwners (p.split);

        // The sidechain is split only while some band listens to it; it starts from clean filters when it is first needed.
        bool anySc = false;
        for (int i = 0; i < kBands; ++i)
        {
            auto& s = band[(size_t) i];
            s.listenSc = scPresent && owner[(size_t) i] == i && p.bands[(size_t) i].on && p.bands[(size_t) i].sidechain;
            anySc = anySc || s.listenSc;
        }
        if (anySc && ! scActive)
        {
            scSplitter.reset();
            scLinear.reset();
        }
        scActive = anySc;

        const unsigned solo = monitor.solo.load (std::memory_order_relaxed), delta = monitor.delta.load (std::memory_order_relaxed);
        const double fade = (double) tick / (kFadeSeconds * fs);
        const double rate = fs * (double) (1 << osMode);
        for (int i = 0; i < kBands; ++i)
        {
            auto& s = band[(size_t) i];
            if (owner[(size_t) i] != i)
            {
                // Part of the band below: nothing of its own runs, and it starts clean if it becomes a band again.
                s.idle = true;
                s.grA = s.grB = 0.0;
                s.maxReduction = 0.0f;
                reductionMeter[(size_t) i].store (0.0f, std::memory_order_relaxed);
                matchMeter[(size_t) i].store (0.0f, std::memory_order_relaxed);
                levelMeter[(size_t) i].store (-120.0f, std::memory_order_relaxed);
                continue;
            }
            const auto& t = p.bands[(size_t) i];
            s.follower.set (t, rate, i == 0 ? pdsp::bands::kBands[0].loHz : appliedCrossover[(size_t) (i - 1)]);
            s.channels = t.channels;

            reductionMeter[(size_t) i].store (s.maxReduction, std::memory_order_relaxed);
            levelMeter[(size_t) i].store ((float) (10.0 * std::log10 (std::max (s.maxLevelSq, 1.0e-12))), std::memory_order_relaxed);
            s.maxReduction = 0.0f;
            s.maxLevelSq = 0.0;

            // Gain match: the band's loudness after the compressor against before it, never more than the limit either way.
            double matchDb = 0.0;
            if (t.match && t.on)
                matchDb = std::clamp (10.0 * std::log10 ((s.msIn + 1.0e-12) / (s.msOut + 1.0e-12)), -kMatchLimitDb, kMatchLimitDb);
            matchMeter[(size_t) i].store ((float) matchDb, std::memory_order_relaxed);
            const double wanted = std::pow (10.0, ((t.on ? t.makeupDb : 0.0) + matchDb) / 20.0);

            // Ramps end exactly on their targets, so a band at rest is multiplied by exactly 1.
            const auto ramp = [&] (double& value, double& target, double& step, double next, double maxStep)
            {
                value = target;
                target = first ? next : std::clamp (next, value - maxStep, value + maxStep);
                if (first) value = target;
                step = (target - value) / tick;
            };
            ramp (s.gain, s.gainTarget, s.gainStep, wanted, 1.0e9);
            const bool heard = solo == 0u || (solo & (1u << i)) != 0u;
            ramp (s.heard, s.heardTarget, s.heardStep, heard ? 1.0 : 0.0, fade);
            ramp (s.dryHeard, s.dryHeardTarget, s.dryHeardStep, heard && (delta & (1u << i)) != 0u ? 1.0 : 0.0, fade);
        }
        outGain = first ? std::pow (10.0, p.outGainDb / 20.0) : outGainTarget;
        outGainTarget = std::pow (10.0, p.outGainDb / 20.0);
        outGainStep = (outGainTarget - outGain) / tick;
        mixNow = first ? std::clamp (p.mix, 0.0, 1.0) : mixTarget; // a ramp ends exactly on its target, so a mix at rest is exact
        mixTarget = std::clamp (p.mix, 0.0, 1.0);
        mixStep = (mixTarget - mixNow) / tick;
        first = false;
    }

    // ---- up to one tick of samples -------------------------------------------------------------
    // One signal split into the six bands, then the bands of pieces that join added into the band that owns them.
    template <typename Splitter, typename Linear, typename Buffers>
    void splitInto (Splitter& zero, Linear& lin, Buffers& out, const float* l, const float* r, int n) noexcept
    {
        for (int i = 0; i < n; ++i)
        {
            const float x = std::isfinite (l[i]) ? l[i] : 0.0f, y = std::isfinite (r[i]) ? r[i] : 0.0f;
            if (phase == PhaseMode::linear)
            {
                std::array<std::array<float, 2>, kBands> parts;
                lin.process (x, y, parts);
                for (int b = 0; b < kBands; ++b)
                {
                    out[(size_t) b][0][(size_t) i] = parts[(size_t) b][0];
                    out[(size_t) b][1][(size_t) i] = parts[(size_t) b][1];
                }
            }
            else
            {
                std::array<double, kBands> partL, partR;
                zero.split (0, (double) x, partL);
                zero.split (1, (double) y, partR);
                for (int b = 0; b < kBands; ++b)
                {
                    out[(size_t) b][0][(size_t) i] = (float) partL[(size_t) b];
                    out[(size_t) b][1][(size_t) i] = (float) partR[(size_t) b];
                }
            }
        }
        for (int b = 1; b < kBands; ++b)
            if (const int to = owner[(size_t) b]; to != b)
                for (int c = 0; c < 2; ++c)
                    for (int i = 0; i < n; ++i)
                        out[(size_t) to][(size_t) c][(size_t) i] += out[(size_t) b][(size_t) c][(size_t) i];
    }

    void segment (float* l, float* r, const float* scL, const float* scR, int n, float& peakIn, float& peakOut) noexcept
    {
        for (int i = 0; i < n; ++i)
        {
            // An inf or NaN would stay in the filters for good.
            if (! std::isfinite (l[i])) l[i] = 0.0f;
            if (! std::isfinite (r[i])) r[i] = 0.0f;
            peakIn = std::max (peakIn, std::max (std::fabs (l[i]), std::fabs (r[i])));
        }
        splitInto (splitter, linear, bandBuffer, l, r, n);
        if (scActive)
            splitInto (scSplitter, scLinear, scBuffer, scL, scR, n); // what a band that listens to the sidechain reads
        for (int i = 0; i < n; ++i)
            l[i] = r[i] = 0.0f;

        for (int b = 0; b < kBands; ++b)
            if (owner[(size_t) b] == b)
                runBand (band[(size_t) b], bandBuffer[(size_t) b][0].data(), bandBuffer[(size_t) b][1].data(),
                         band[(size_t) b].listenSc ? scBuffer[(size_t) b][0].data() : nullptr, band[(size_t) b].listenSc ? scBuffer[(size_t) b][1].data() : nullptr,
                         l, r, n);

        mixNow += mixStep * n; // the bands above read the ramp from the start of the segment; the next one starts here
        for (int i = 0; i < n; ++i)
        {
            outGain += outGainStep;
            l[i] = (float) (l[i] * outGain);
            r[i] = (float) (r[i] * outGain);
            peakOut = std::max (peakOut, std::max (std::fabs (l[i]), std::fabs (r[i])));
        }
    }

    // `sl` / `sr`: the sidechain's piece of this band, when the band listens to it (else null).
    void runBand (BandState& s, float* dl, float* dr, const float* sl, const float* sr, float* outL, float* outR, int n) noexcept
    {
        const bool ms = usesMidSide (s.channels);
        const bool run = s.follower.on || s.grA != 0.0 || s.grB != 0.0;
        float* w[2] = { workBuffer[0].data(), workBuffer[1].data() };
        if (run)
        {
            auto* os = s.os[(size_t) osMode].get();
            if (s.idle)
            {
                // Their memory is of before the band stopped.
                s.levelA.reset();
                s.levelB.reset();
                for (auto& d : s.look) d.clear();
                if (os != nullptr)
                    os->reset();
            }
            for (int i = 0; i < n; ++i)
            {
                w[0][i] = ms ? 0.5f * (dl[i] + dr[i]) : dl[i];
                w[1][i] = ms ? 0.5f * (dl[i] - dr[i]) : dr[i];
            }

            // What the compressor reads, at the rate it runs: the band's own signal, or the sidechain's piece of the band (made the same
            // way, held to the oversampled rate: the level is a smoothed mean square, which a held sample leaves exactly as it was).
            const int m = 1 << osMode;
            const float* det[2] = { nullptr, nullptr };
            if (sl != nullptr)
            {
                for (int i = 0; i < n; ++i)
                {
                    const float a = ms ? 0.5f * (sl[i] + sr[i]) : sl[i], b = ms ? 0.5f * (sl[i] - sr[i]) : sr[i];
                    for (int j = 0; j < m; ++j)
                    {
                        detBuffer[0][(size_t) (i * m + j)] = a;
                        detBuffer[1][(size_t) (i * m + j)] = b;
                    }
                }
                det[0] = detBuffer[0].data();
                det[1] = detBuffer[1].data();
            }

            if (os != nullptr)
            {
                const float* const in[2] = { w[0], w[1] };
                auto up = os->processSamplesUp (juce::dsp::AudioBlock<const float> (in, 2, (size_t) n));
                float* ua = up.getChannelPointer (0);
                float* ub = up.getChannelPointer (1);
                compress (s, ua, ub, det[0] != nullptr ? det[0] : ua, det[1] != nullptr ? det[1] : ub, (int) up.getNumSamples());
                juce::dsp::AudioBlock<float> down (w, 2, (size_t) n);
                os->processSamplesDown (down);
            }
            else
            {
                compress (s, w[0], w[1], det[0] != nullptr ? det[0] : w[0], det[1] != nullptr ? det[1] : w[1], n);
            }
        }
        s.idle = ! run;

        s.dry[0].process (dl, n);
        s.dry[1].process (dr, n);

        const double k = 1.0 - std::exp (-1.0 / (kMatchSeconds * fs));
        for (int i = 0; i < n; ++i)
        {
            const double dryL = dl[i], dryR = dr[i];
            double yL = dryL, yR = dryR;
            if (run)
            {
                // The difference goes back where it came from: Mid to both, Side to left and minus to right. MIX scales it (1 is exact).
                const double mixHere = mixNow + mixStep * (i + 1);
                const double a = w[0][i] * mixHere, b = w[1][i] * mixHere;
                yL += ms ? a + b : a;
                yR += ms ? a - b : b;
            }
            s.msIn += k * (dryL * dryL + dryR * dryR - s.msIn);
            s.msOut += k * (yL * yL + yR * yR - s.msOut);
            s.gain += s.gainStep;
            s.heard += s.heardStep;
            s.dryHeard += s.dryHeardStep;
            outL[i] += (float) (s.heard * s.gain * yL - s.dryHeard * dryL);
            outR[i] += (float) (s.heard * s.gain * yR - s.dryHeard * dryR);
        }
    }

    // Turns the two signals into what the compressor takes off them: x * (gain - 1), where the level comes from `detA` / `detB`
    // (the signals themselves, or the sidechain's) and the gain is applied to the signal after the look-ahead delay. At the
    // oversampled rate when there is one. With no look-ahead and no sidechain this is exactly the compressor without either.
    void compress (BandState& s, float* a, float* b, const float* detA, const float* detB, int n) noexcept
    {
        const auto& f = s.follower;
        double grA = s.grA, grB = s.grB, most = 0.0, loudest = 0.0;
        if (isLinked (s.channels))
        {
            for (int i = 0; i < n; ++i)
            {
                const double level = std::max (s.levelA.push (detA[i], f.detector), s.levelB.push (detB[i], f.detector));
                loudest = std::max (loudest, level);
                const double g = f.step (grA, level) - 1.0;
                if (std::fabs (grA) > std::fabs (most)) most = grA;
                const float xa = s.look[0].push (a[i]), xb = s.look[1].push (b[i]);
                a[i] = (float) (xa * g);
                b[i] = (float) (xb * g);
            }
            grB = grA;
        }
        else
        {
            const bool first_ = runsFirst (s.channels), second = runsSecond (s.channels);
            if (! first_) grA = 0.0;
            if (! second) grB = 0.0;
            for (int i = 0; i < n; ++i)
            {
                const double la = first_ ? s.levelA.push (detA[i], f.detector) : 0.0, lb = second ? s.levelB.push (detB[i], f.detector) : 0.0;
                loudest = std::max (loudest, std::max (la, lb));
                const float xa = s.look[0].push (a[i]), xb = s.look[1].push (b[i]);
                a[i] = first_ ? (float) (xa * (f.step (grA, la) - 1.0)) : 0.0f;
                b[i] = second ? (float) (xb * (f.step (grB, lb) - 1.0)) : 0.0f;
                if (std::fabs (grA) > std::fabs (most)) most = grA;
                if (std::fabs (grB) > std::fabs (most)) most = grB;
            }
        }
        s.grA = grA;
        s.grB = grB;
        if (std::fabs ((float) most) > std::fabs (s.maxReduction)) s.maxReduction = (float) most;
        s.maxLevelSq = std::max (s.maxLevelSq, loudest);
    }

    double fs = 48000.0;
    juce::int64 counter = 0;
    PhaseMode phase = PhaseMode::zeroLatency;
    int osMode = 0, lookIndex = 0;
    bool first = true, scPresent = false, scActive = false;
    std::array<int, kNumOsModes> osLatency {};
    std::array<double, kCrossovers> appliedCrossover {};
    std::array<int, kBands> owner { 0, 1, 2, 3, 4, 5 };
    double outGain = 1.0, outGainTarget = 1.0, outGainStep = 0.0;
    double mixNow = 1.0, mixTarget = 1.0, mixStep = 0.0; // MIX: the value at the start of the segment, its target at the end of the tick, and the step per sample

    pdsp::BandSplitter splitter, scSplitter; // the sidechain gets its own, so the main signal's filters never hear it
    LinearSplit linear, scLinear;
    std::array<BandState, kBands> band;
    std::array<std::array<std::array<float, tick>, 2>, kBands> bandBuffer {}, scBuffer {};
    std::array<std::array<float, tick * (1 << (kNumOsModes - 1))>, 2> detBuffer {}; // the sidechain's piece of a band, at the oversampled rate
    std::array<std::array<float, tick>, 2> workBuffer {};
};
} // namespace atlas
