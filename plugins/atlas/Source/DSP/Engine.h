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

struct EngineParams
{
    std::array<BandParams, kBands> bands;
    std::array<double, kCrossovers> crossover = pdsp::bands::defaultCrossovers();
    PhaseMode phase = PhaseMode::zeroLatency;
    int oversampling = 0; // 0 = off, 1 = 2x, 2 = 4x
    double outGainDb = 0.0;
};

// The edges are kept in order and at least this ratio apart; the editor and the engine agree on it.
inline constexpr double kMinCrossoverRatio = 1.2;
inline std::array<double, kCrossovers> orderedCrossovers (std::array<double, kCrossovers> hz) noexcept
{
    for (int k = 1; k < kCrossovers; ++k)
        hz[(size_t) k] = std::max (hz[(size_t) k], hz[(size_t) (k - 1)] * kMinCrossoverRatio);
    return hz;
}

// Whole-sample delay: the dry band waits for the oversampled path.
class IntDelay
{
public:
    void prepare (int maxDelay) { buffer.assign ((size_t) maxDelay + 1, 0.0f); pos = 0; delay = 0; }
    void setDelay (int d) noexcept { delay = std::clamp (d, 0, (int) buffer.size() - 1); std::fill (buffer.begin(), buffer.end(), 0.0f); pos = 0; }
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

    std::array<std::atomic<float>, kBands> reductionMeter {}; // dB taken off, the most over the last tick
    std::array<std::atomic<float>, kBands> matchMeter {};     // dB gain match is adding
    std::atomic<float> inPeak { 0.0f }, outPeak { 0.0f };
    std::atomic<int> latencySamples { 0 };

    void prepare (double sampleRate, PhaseMode initialPhase = PhaseMode::zeroLatency, int initialOs = 0)
    {
        fs = sampleRate;
        linear.prepare (fs);
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
        for (auto& b : band)
            for (auto& d : b.dry) d.prepare (maxOsLatency);
        phase = initialPhase;
        osMode = std::clamp (initialOs, 0, kNumOsModes - 1);
        reset();
    }

    void reset()
    {
        counter = 0;
        splitter.prepare (fs);
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

    int latencyFor (PhaseMode p, int os) const noexcept { return (p == PhaseMode::linear ? linear.latency() : 0) + osLatency[(size_t) os]; }

    // In place. Must not allocate, lock or log.
    void process (float* l, float* r, int numSamples, const EngineParams& p) noexcept
    {
        float peakIn = 0.0f, peakOut = 0.0f;
        for (int start = 0; start < numSamples;)
        {
            const int phaseInTick = (int) (counter % tick);
            if (phaseInTick == 0)
                control (p);
            const int n = std::min (numSamples - start, tick - phaseInTick);
            segment (l + start, r + start, n, peakIn, peakOut);
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
        float maxReduction = 0.0f;
        std::array<std::unique_ptr<juce::dsp::Oversampling<float>>, kNumOsModes> os; // [1] 2x, [2] 4x
        std::array<IntDelay, 2> dry;
    };

    // The phase or oversampling mode changed (or playback starts): new latency, clean history.
    void configure()
    {
        linear.reset();
        splitter.reset();
        for (auto& b : band)
        {
            for (auto& d : b.dry) d.setDelay (osLatency[(size_t) osMode]);
            for (auto& o : b.os)
                if (o != nullptr) o->reset();
            b.grA = b.grB = 0.0;
            b.levelA.reset();
            b.levelB.reset();
        }
        latencySamples.store (latencyFor (phase, osMode), std::memory_order_relaxed);
    }

    // ---- once per tick -------------------------------------------------------------------------
    void control (const EngineParams& p) noexcept
    {
        const int os = std::clamp (p.oversampling, 0, kNumOsModes - 1);
        if (p.phase != phase || os != osMode)
        {
            phase = p.phase;
            osMode = os;
            configure();
        }
        if (const auto xo = orderedCrossovers (p.crossover); xo != appliedCrossover)
        {
            appliedCrossover = xo;
            splitter.setCrossovers (fs, xo);
            linear.setCrossovers (xo);
        }

        const unsigned solo = monitor.solo.load (std::memory_order_relaxed), delta = monitor.delta.load (std::memory_order_relaxed);
        const double fade = (double) tick / (kFadeSeconds * fs);
        const double rate = fs * (double) (1 << osMode);
        for (int i = 0; i < kBands; ++i)
        {
            auto& s = band[(size_t) i];
            const auto& t = p.bands[(size_t) i];
            s.follower.set (t, rate, i == 0 ? pdsp::bands::kBands[0].loHz : appliedCrossover[(size_t) (i - 1)]);
            s.channels = t.channels;

            reductionMeter[(size_t) i].store (s.maxReduction, std::memory_order_relaxed);
            s.maxReduction = 0.0f;

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
        first = false;
    }

    // ---- up to one tick of samples -------------------------------------------------------------
    void segment (float* l, float* r, int n, float& peakIn, float& peakOut) noexcept
    {
        for (int i = 0; i < n; ++i)
        {
            // An inf or NaN would stay in the filters for good.
            if (! std::isfinite (l[i])) l[i] = 0.0f;
            if (! std::isfinite (r[i])) r[i] = 0.0f;
            peakIn = std::max (peakIn, std::max (std::fabs (l[i]), std::fabs (r[i])));

            if (phase == PhaseMode::linear)
            {
                std::array<std::array<float, 2>, kBands> parts;
                linear.process (l[i], r[i], parts);
                for (int b = 0; b < kBands; ++b)
                {
                    bandBuffer[(size_t) b][0][(size_t) i] = parts[(size_t) b][0];
                    bandBuffer[(size_t) b][1][(size_t) i] = parts[(size_t) b][1];
                }
            }
            else
            {
                std::array<double, kBands> partL, partR;
                splitter.split (0, (double) l[i], partL);
                splitter.split (1, (double) r[i], partR);
                for (int b = 0; b < kBands; ++b)
                {
                    bandBuffer[(size_t) b][0][(size_t) i] = (float) partL[(size_t) b];
                    bandBuffer[(size_t) b][1][(size_t) i] = (float) partR[(size_t) b];
                }
            }
            l[i] = r[i] = 0.0f;
        }

        for (int b = 0; b < kBands; ++b)
            runBand (band[(size_t) b], bandBuffer[(size_t) b][0].data(), bandBuffer[(size_t) b][1].data(), l, r, n);

        for (int i = 0; i < n; ++i)
        {
            outGain += outGainStep;
            l[i] = (float) (l[i] * outGain);
            r[i] = (float) (r[i] * outGain);
            peakOut = std::max (peakOut, std::max (std::fabs (l[i]), std::fabs (r[i])));
        }
    }

    void runBand (BandState& s, float* dl, float* dr, float* outL, float* outR, int n) noexcept
    {
        const bool ms = usesMidSide (s.channels);
        const bool run = s.follower.on || s.grA > 0.0 || s.grB > 0.0;
        float* w[2] = { workBuffer[0].data(), workBuffer[1].data() };
        if (run)
        {
            auto* os = s.os[(size_t) osMode].get();
            if (s.idle)
            {
                // Their memory is of before the band stopped.
                s.levelA.reset();
                s.levelB.reset();
                if (os != nullptr)
                    os->reset();
            }
            for (int i = 0; i < n; ++i)
            {
                w[0][i] = ms ? 0.5f * (dl[i] + dr[i]) : dl[i];
                w[1][i] = ms ? 0.5f * (dl[i] - dr[i]) : dr[i];
            }
            if (os != nullptr)
            {
                const float* const in[2] = { w[0], w[1] };
                auto up = os->processSamplesUp (juce::dsp::AudioBlock<const float> (in, 2, (size_t) n));
                compress (s, up.getChannelPointer (0), up.getChannelPointer (1), (int) up.getNumSamples());
                juce::dsp::AudioBlock<float> down (w, 2, (size_t) n);
                os->processSamplesDown (down);
            }
            else
            {
                compress (s, w[0], w[1], n);
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
                // The difference goes back where it came from: Mid to both, Side to left and minus to right.
                const double a = w[0][i], b = w[1][i];
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

    // Turns the two signals into what the compressor takes off them: x * (gain - 1). At the oversampled rate when there is one.
    void compress (BandState& s, float* a, float* b, int n) noexcept
    {
        const auto& f = s.follower;
        double grA = s.grA, grB = s.grB, most = 0.0;
        if (isLinked (s.channels))
        {
            for (int i = 0; i < n; ++i)
            {
                const double g = f.step (grA, std::max (s.levelA.push (a[i], f.detector), s.levelB.push (b[i], f.detector))) - 1.0;
                most = std::max (most, grA);
                a[i] = (float) (a[i] * g);
                b[i] = (float) (b[i] * g);
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
                a[i] = first_ ? (float) (a[i] * (f.step (grA, s.levelA.push (a[i], f.detector)) - 1.0)) : 0.0f;
                b[i] = second ? (float) (b[i] * (f.step (grB, s.levelB.push (b[i], f.detector)) - 1.0)) : 0.0f;
                most = std::max (most, std::max (grA, grB));
            }
        }
        s.grA = grA;
        s.grB = grB;
        s.maxReduction = std::max (s.maxReduction, (float) most);
    }

    double fs = 48000.0;
    juce::int64 counter = 0;
    PhaseMode phase = PhaseMode::zeroLatency;
    int osMode = 0;
    bool first = true;
    std::array<int, kNumOsModes> osLatency {};
    std::array<double, kCrossovers> appliedCrossover {};
    double outGain = 1.0, outGainTarget = 1.0, outGainStep = 0.0;

    pdsp::BandSplitter splitter;
    LinearSplit linear;
    std::array<BandState, kBands> band;
    std::array<std::array<std::array<float, tick>, 2>, kBands> bandBuffer {};
    std::array<std::array<float, tick>, 2> workBuffer {};
};
} // namespace atlas
