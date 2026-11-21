// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 The Pantheon authors

#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_core/juce_core.h>
#include <juce_dsp/juce_dsp.h>
#include <pantheon/dsp/Biquad.h>
#include <pantheon/dsp/MusicBands.h>
#include <pantheon/dsp/TruePeak.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <numeric>
#include <vector>

namespace olympus::analyze
{
inline constexpr int kNumSpectrumBands = 61;

struct AnchorPoint
{
    double freqHz;
    double db;
};

struct TargetProfile
{
    juce::String name { "modern_pop" };
    std::array<double, kNumSpectrumBands> targetDb {};
    std::array<double, kNumSpectrumBands> toleranceDb {};
};

struct BandBalance
{
    juce::String name;
    double loHz = 0.0;
    double hiHz = 0.0;
    double measuredDb = 0.0;
    double targetDb = 0.0;
    double deltaDb = 0.0;
    juce::String status { "ok" };
};

struct SpectralDeviation
{
    double loHz = 0.0;
    double hiHz = 0.0;
    double peakHz = 0.0;
    double deltaDb = 0.0;
    double toleranceDb = 0.0;
    juce::String direction;
};

struct ResonanceRing
{
    double freqHz = 0.0;
    double standOutDb = 0.0;
    double measuredDb = 0.0;
};

struct HarshnessInfo
{
    bool detected = false;
    double worstFreqHz = 0.0;
    double jumpDb = 0.0;
};

struct Recommendation
{
    juce::String category;
    juce::String severity;
    juce::String plugin;
    juce::String action;
};

struct AnalysisResult
{
    bool valid = false;
    double sampleRate = 0.0;
    int numChannels = 0;
    int numSamples = 0;
    double durationSeconds = 0.0;
    juce::String targetName { "modern_pop" };
    double targetLufs = -14.0;

    // Loudness & Dynamics (ITU-R BS.1770-4)
    double integratedLufs = -120.0;
    double shortTermMaxLufs = -120.0;
    double shortTermMinLufs = -120.0;
    double lraLu = 0.0;

    // Peaks, Headroom & DC
    double truePeakDb = -120.0;
    double samplePeakDb = -120.0;
    int64_t clippedSamples = 0;
    double dcOffsetDb = -120.0;
    double rmsDb = -120.0;
    double crestFactorDb = 0.0;
    double plrDb = 0.0;
    double headroomDb = 120.0;
    double lowEndCrestDb = 0.0;

    // 1/6-Octave Spectrum & Tonal Balance
    std::array<double, kNumSpectrumBands> bandFreqsHz {};
    std::array<double, kNumSpectrumBands> measuredDb {};
    std::array<double, kNumSpectrumBands> targetDb {};
    double rmsDistanceDb = 0.0;
    double tiltDbPerOct = 0.0;
    double targetTiltDbPerOct = 0.0;
    double tiltDiffDbPerOct = 0.0;
    std::array<BandBalance, pdsp::bands::kCount> bands {};
    std::vector<SpectralDeviation> deviations;
    std::vector<ResonanceRing> resonanceRings;
    HarshnessInfo harshness;

    // Stereo, Phase & Mono Compatibility
    double stereoCorrelationMean = 1.0;
    double stereoCorrelationMin = 1.0;
    double lrBalanceDb = 0.0;
    double monoLossDb = 0.0;
    double lowEndMonoLossDb = 0.0;
    std::array<double, pdsp::bands::kCount> bandCorrelation { 1.0, 1.0, 1.0, 1.0, 1.0, 1.0 };

    std::vector<Recommendation> recommendations;

    juce::var toVar() const;
};

struct TrackAudio
{
    juce::String name;
    juce::AudioBuffer<float> audio;
};

struct TrackSummary
{
    juce::String name;
    double integratedLufs = -120.0;
    double truePeakDb = -120.0;
    double crestFactorDb = 0.0;
    juce::String dominantBand { "Midrange" };
    double sub80Ratio = 0.0;
};

struct MaskingConflict
{
    juce::String trackA;
    juce::String trackB;
    juce::String band;
    double loHz = 0.0;
    double hiHz = 0.0;
    double overlapDb = 0.0;
    juce::String suggestion;
};

struct MaskingResult
{
    std::vector<TrackSummary> tracks;
    std::vector<MaskingConflict> conflicts;

    juce::var toVar() const;
};

namespace detail
{
inline double sanitize (float x) noexcept
{
    return std::isfinite (x) ? static_cast<double> (x) : 0.0;
}

inline double bandCentreHz (int index) noexcept
{
    return 20.0 * std::pow (2.0, static_cast<double> (index) / 6.0);
}

inline double interpolateAnchors (const AnchorPoint* pts, size_t count, double freqHz) noexcept
{
    if (count == 0)
        return 0.0;
    if (freqHz <= pts[0].freqHz)
        return pts[0].db;
    if (freqHz >= pts[count - 1].freqHz)
        return pts[count - 1].db;

    const double logF = std::log2 (freqHz);
    for (size_t i = 0; i + 1 < count; ++i)
    {
        if (freqHz <= pts[i + 1].freqHz)
        {
            const double log0 = std::log2 (pts[i].freqHz);
            const double log1 = std::log2 (pts[i + 1].freqHz);
            const double t = (logF - log0) / std::max (1e-12, log1 - log0);
            return pts[i].db + t * (pts[i + 1].db - pts[i].db);
        }
    }
    return pts[count - 1].db;
}

inline void normalizeMidBand (std::array<double, kNumSpectrumBands>& db) noexcept
{
    double sum = 0.0;
    int count = 0;
    for (int i = 0; i < kNumSpectrumBands; ++i)
    {
        const double f = bandCentreHz (i);
        if (f >= 200.0 && f <= 5000.0)
        {
            sum += db[static_cast<size_t> (i)];
            ++count;
        }
    }
    if (count > 0)
    {
        const double offset = sum / static_cast<double> (count);
        for (double& v : db)
            v -= offset;
    }
}

// ITU-R BS.1770-4 K-weighting two-stage biquad pre-filter via bilinear transform for any sample rate.
struct KWeightingFilter
{
    pdsp::Biquad stage1;
    pdsp::Biquad stage2;

    void prepare (double fs) noexcept
    {
        const double safeFs = std::max (8000.0, fs);

        // Stage 1: High-shelf (+4 dB around 1682 Hz)
        {
            const double f0 = 1681.974450955533;
            const double g = 3.999843853973347;
            const double q = 0.7071752369554196;
            const double k = std::tan (M_PI * f0 / safeFs);
            const double vh = std::pow (10.0, g / 20.0);
            const double vb = std::pow (vh, 0.4996667741545416);
            const double a0 = 1.0 + k / q + k * k;
            stage1.set (vh + vb * k / q + k * k,
                        2.0 * (k * k - vh),
                        vh - vb * k / q + k * k,
                        a0,
                        2.0 * (k * k - 1.0),
                        1.0 - k / q + k * k);
        }

        // Stage 2: 2nd-order RLB high-pass (~38 Hz)
        {
            const double f0 = 38.13547087602444;
            const double q = 0.5003270373238773;
            const double k = std::tan (M_PI * f0 / safeFs);
            const double a0 = 1.0 + k / q + k * k;
            stage2.set (1.0, -2.0, 1.0, a0, 2.0 * (k * k - 1.0), 1.0 - k / q + k * k);
        }

        reset();
    }

    void reset() noexcept
    {
        stage1.reset();
        stage2.reset();
    }

    inline double process (double x) noexcept
    {
        return stage2.process (stage1.process (x));
    }
};

inline double percentileSorted (const std::vector<double>& sorted, double p) noexcept
{
    if (sorted.empty())
        return 0.0;
    if (sorted.size() == 1)
        return sorted[0];

    const double idx = std::clamp (p, 0.0, 1.0) * static_cast<double> (sorted.size() - 1);
    const size_t lo = static_cast<size_t> (std::floor (idx));
    const size_t hi = std::min (lo + 1, sorted.size() - 1);
    const double frac = idx - static_cast<double> (lo);
    return sorted[lo] + frac * (sorted[hi] - sorted[lo]);
}

inline double computeSlopeDbPerOct (const std::array<double, kNumSpectrumBands>& db, double maxFreqHz) noexcept
{
    double sumX = 0.0, sumY = 0.0, sumXX = 0.0, sumXY = 0.0;
    int n = 0;
    for (int i = 0; i < kNumSpectrumBands; ++i)
    {
        const double f = bandCentreHz (i);
        if (f > maxFreqHz)
            break;
        const double x = std::log2 (f);
        const double y = db[static_cast<size_t> (i)];
        sumX += x;
        sumY += y;
        sumXX += x * x;
        sumXY += x * y;
        ++n;
    }
    if (n < 2)
        return 0.0;
    const double denom = static_cast<double> (n) * sumXX - sumX * sumX;
    if (std::abs (denom) < 1e-12)
        return 0.0;
    return (static_cast<double> (n) * sumXY - sumX * sumY) / denom;
}

inline juce::String formatDb (double db, bool withSign = true)
{
    const double rounded = std::round (db * 10.0) / 10.0;
    if (withSign && rounded > 0.0)
        return "+" + juce::String (rounded, 1);
    return juce::String (rounded, 1);
}

struct LoudnessAndPeaks
{
    double integratedLufs = -120.0;
    double shortTermMaxLufs = -120.0;
    double shortTermMinLufs = -120.0;
    double lraLu = 0.0;
    double truePeakDb = -120.0;
    double samplePeakDb = -120.0;
    int64_t clippedSamples = 0;
    double dcOffsetDb = -120.0;
    double rmsDb = -120.0;
    double crestFactorDb = 0.0;
    double plrDb = 0.0;
    double headroomDb = 120.0;
};

inline LoudnessAndPeaks computeLoudnessAndPeaks (const juce::AudioBuffer<float>& audio,
                                                 double sampleRate,
                                                 bool computeShortTermAndLra)
{
    LoudnessAndPeaks out;
    const int numChannels = audio.getNumChannels();
    const int numSamples = audio.getNumSamples();
    if (! std::isfinite (sampleRate) || sampleRate <= 0.0 || numChannels <= 0 || numSamples <= 0)
        return out;

    // 1. Peaks, True Peak, Clipping, DC Offset, RMS & K-weighted squared sum
    const int kWeightCh = std::min (numChannels, 2);
    std::array<KWeightingFilter, 2> kFilters;
    for (int c = 0; c < kWeightCh; ++c)
        kFilters[static_cast<size_t> (c)].prepare (sampleRate);

    std::vector<const float*> chPtrs (static_cast<size_t> (numChannels), nullptr);
    for (int c = 0; c < numChannels; ++c)
        chPtrs[static_cast<size_t> (c)] = audio.getReadPointer (c);

    std::vector<pdsp::TruePeakReader> tpReaders (static_cast<size_t> (numChannels));
    std::vector<double> dcSums (static_cast<size_t> (numChannels), 0.0);
    const int stepSamples = std::max (1, static_cast<int> (std::llround (0.1 * sampleRate)));
    const int numSteps = numSamples / stepSamples;
    std::vector<double> kStepPrefix (static_cast<size_t> (numSteps + 1), 0.0);

    double maxSamplePeak = 0.0;
    double maxTruePeak = 0.0;
    double totalSumSq = 0.0;
    double totalKWeightedPower = 0.0;
    double stepKPower = 0.0;
    int stepPos = 0;
    size_t stepIdx = 0;
    int64_t clipped = 0;

    for (int i = 0; i < numSamples; ++i)
    {
        double kSamplePower = 0.0;
        for (int c = 0; c < numChannels; ++c)
        {
            const double x = sanitize (chPtrs[static_cast<size_t> (c)][i]);
            const double ax = std::abs (x);
            maxSamplePeak = std::max (maxSamplePeak, ax);
            if (ax >= 1.0)
                ++clipped;

            const double tp = tpReaders[static_cast<size_t> (c)].push (x, true);
            maxTruePeak = std::max (maxTruePeak, std::max (tp, ax));

            dcSums[static_cast<size_t> (c)] += x;
            totalSumSq += x * x;

            if (c < kWeightCh)
            {
                const double kx = kFilters[static_cast<size_t> (c)].process (x);
                kSamplePower += kx * kx;
            }
        }
        totalKWeightedPower += kSamplePower;
        stepKPower += kSamplePower;
        if (++stepPos == stepSamples)
        {
            kStepPrefix[stepIdx + 1] = kStepPrefix[stepIdx] + stepKPower;
            ++stepIdx;
            stepKPower = 0.0;
            stepPos = 0;
        }
    }

    // Flush true-peak interpolator delay tail
    for (int c = 0; c < numChannels; ++c)
        for (int j = 0; j < pdsp::kTruePeakDelay; ++j)
            maxTruePeak = std::max (maxTruePeak, tpReaders[static_cast<size_t> (c)].push (0.0, true));

    double maxDc = 0.0;
    for (int c = 0; c < numChannels; ++c)
        maxDc = std::max (maxDc, std::abs (dcSums[static_cast<size_t> (c)] / static_cast<double> (numSamples)));

    const double rmsLin = std::sqrt (totalSumSq / (static_cast<double> (numSamples) * static_cast<double> (numChannels)));
    out.samplePeakDb = maxSamplePeak > 1e-12 ? 20.0 * std::log10 (maxSamplePeak) : -120.0;
    out.truePeakDb = maxTruePeak > 1e-12 ? 20.0 * std::log10 (maxTruePeak) : -120.0;
    out.clippedSamples = clipped;
    out.dcOffsetDb = maxDc > 1e-12 ? 20.0 * std::log10 (maxDc) : -120.0;
    out.rmsDb = rmsLin > 1e-12 ? 20.0 * std::log10 (rmsLin) : -120.0;
    out.crestFactorDb = (maxSamplePeak > 1e-12 && rmsLin > 1e-12) ? (out.samplePeakDb - out.rmsDb) : 0.0;
    out.headroomDb = -out.truePeakDb;

    // 2. ITU-R BS.1770-4 Integrated Loudness, Short-Term Max/Min, LRA
    const int blockSamples = std::max (1, static_cast<int> (std::llround (0.4 * sampleRate)));
    const int blockSteps = std::max (1, static_cast<int> (std::llround (static_cast<double> (blockSamples) / static_cast<double> (stepSamples))));
    const int actualBlockSamples = blockSteps * stepSamples;

    std::vector<double> blockPowers;
    if (numSamples < blockSamples || numSteps < blockSteps)
    {
        const double z = totalKWeightedPower / static_cast<double> (numSamples);
        if (z > 0.0)
            blockPowers.push_back (z);
    }
    else
    {
        blockPowers.reserve (static_cast<size_t> (numSteps - blockSteps + 1));
        for (int b = 0; b + blockSteps <= numSteps; ++b)
        {
            const double sum = kStepPrefix[static_cast<size_t> (b + blockSteps)] - kStepPrefix[static_cast<size_t> (b)];
            blockPowers.push_back (sum / static_cast<double> (actualBlockSamples));
        }
    }

    double gate1Sum = 0.0;
    int gate1Count = 0;
    for (double z : blockPowers)
    {
        if (z > 1e-12)
        {
            const double l = -0.691 + 10.0 * std::log10 (z);
            if (l > -70.0)
            {
                gate1Sum += z;
                ++gate1Count;
            }
        }
    }

    if (gate1Count > 0)
    {
        const double gammaR = -0.691 + 10.0 * std::log10 (gate1Sum / static_cast<double> (gate1Count)) - 10.0;
        double gate2Sum = 0.0;
        int gate2Count = 0;
        for (double z : blockPowers)
        {
            if (z > 1e-12)
            {
                const double l = -0.691 + 10.0 * std::log10 (z);
                if (l > -70.0 && l >= gammaR)
                {
                    gate2Sum += z;
                    ++gate2Count;
                }
            }
        }
        if (gate2Count > 0)
            out.integratedLufs = -0.691 + 10.0 * std::log10 (gate2Sum / static_cast<double> (gate2Count));
    }

    out.plrDb = (out.integratedLufs > -70.0 && out.truePeakDb > -120.0) ? (out.truePeakDb - out.integratedLufs) : 0.0;

    if (computeShortTermAndLra)
    {
        const int stSamples = std::max (1, static_cast<int> (std::llround (3.0 * sampleRate)));
        const int stSteps = std::max (1, static_cast<int> (std::llround (static_cast<double> (stSamples) / static_cast<double> (stepSamples))));
        const int actualStSamples = stSteps * stepSamples;

        std::vector<double> stLufs;
        if (numSamples < stSamples || numSteps < stSteps)
        {
            const double z = totalKWeightedPower / static_cast<double> (numSamples);
            if (z > 1e-12)
            {
                const double l = -0.691 + 10.0 * std::log10 (z);
                if (l > -70.0)
                    stLufs.push_back (l);
            }
        }
        else
        {
            for (int b = 0; b + stSteps <= numSteps; ++b)
            {
                const double sum = kStepPrefix[static_cast<size_t> (b + stSteps)] - kStepPrefix[static_cast<size_t> (b)];
                const double z = sum / static_cast<double> (actualStSamples);
                if (z > 1e-12)
                {
                    const double l = -0.691 + 10.0 * std::log10 (z);
                    if (l > -70.0)
                        stLufs.push_back (l);
                }
            }
        }

        if (! stLufs.empty())
        {
            out.shortTermMaxLufs = *std::max_element (stLufs.begin(), stLufs.end());
            out.shortTermMinLufs = *std::min_element (stLufs.begin(), stLufs.end());

            std::vector<double> lraBlocks;
            const double lraThresh = out.integratedLufs - 20.0;
            for (double l : stLufs)
            {
                if (l >= lraThresh)
                    lraBlocks.push_back (l);
            }
            if (lraBlocks.size() >= 2)
            {
                std::sort (lraBlocks.begin(), lraBlocks.end());
                out.lraLu = percentileSorted (lraBlocks, 0.95) - percentileSorted (lraBlocks, 0.10);
            }
        }
    }

    return out;
}
} // namespace detail

inline TargetProfile getTargetProfile (const juce::String& targetName)
{
    static constexpr std::array<AnchorPoint, 18> kModernPop { {
        { 20.0, -14.0 }, { 30.0, -7.0 }, { 45.0, -1.5 }, { 60.0, 1.5 }, { 90.0, 2.5 }, { 130.0, 2.0 },
        { 200.0, 1.0 }, { 300.0, 0.3 }, { 500.0, 0.0 }, { 800.0, -0.5 }, { 1200.0, -1.2 }, { 2000.0, -2.2 },
        { 3000.0, -3.0 }, { 5000.0, -4.0 }, { 8000.0, -5.5 }, { 12000.0, -8.5 }, { 16000.0, -12.0 }, { 20000.0, -20.0 }
    } };
    static constexpr std::array<AnchorPoint, 15> kElectronic { {
        { 20.0, -8.0 }, { 30.0, -2.0 }, { 45.0, 2.5 }, { 60.0, 3.5 }, { 90.0, 3.0 }, { 130.0, 2.0 },
        { 200.0, 0.5 }, { 500.0, 0.0 }, { 1200.0, -1.0 }, { 3000.0, -2.5 }, { 5000.0, -3.5 }, { 8000.0, -4.5 },
        { 12000.0, -7.0 }, { 16000.0, -11.0 }, { 20000.0, -18.0 }
    } };
    static constexpr std::array<AnchorPoint, 15> kRock { {
        { 20.0, -18.0 }, { 30.0, -11.0 }, { 45.0, -5.0 }, { 60.0, -1.0 }, { 90.0, 1.5 }, { 130.0, 2.0 },
        { 200.0, 1.5 }, { 500.0, 0.5 }, { 1200.0, 0.0 }, { 2500.0, -1.0 }, { 5000.0, -3.0 }, { 8000.0, -5.5 },
        { 12000.0, -9.0 }, { 16000.0, -14.0 }, { 20000.0, -22.0 }
    } };
    static constexpr std::array<AnchorPoint, 15> kAcoustic { {
        { 20.0, -22.0 }, { 30.0, -15.0 }, { 45.0, -8.0 }, { 60.0, -3.0 }, { 90.0, 0.0 }, { 130.0, 0.5 },
        { 200.0, 0.5 }, { 500.0, 0.0 }, { 1200.0, -0.8 }, { 3000.0, -2.2 }, { 5000.0, -3.5 }, { 8000.0, -5.0 },
        { 12000.0, -8.0 }, { 16000.0, -12.0 }, { 20000.0, -20.0 }
    } };
    static constexpr std::array<AnchorPoint, 14> kTolerances { {
        { 20.0, 4.0 }, { 30.0, 4.0 }, { 45.0, 3.5 }, { 60.0, 3.0 }, { 90.0, 2.5 }, { 200.0, 2.0 },
        { 500.0, 1.6 }, { 1000.0, 1.5 }, { 3000.0, 1.8 }, { 5000.0, 2.2 }, { 8000.0, 2.8 }, { 12000.0, 3.5 },
        { 16000.0, 4.5 }, { 20000.0, 6.0 }
    } };

    const juce::String key = targetName.trim().toLowerCase();
    TargetProfile profile;
    const AnchorPoint* anchors = kModernPop.data();
    size_t anchorCount = kModernPop.size();
    bool isFlat = false;

    if (key == "electronic")
    {
        profile.name = "electronic";
        anchors = kElectronic.data();
        anchorCount = kElectronic.size();
    }
    else if (key == "rock")
    {
        profile.name = "rock";
        anchors = kRock.data();
        anchorCount = kRock.size();
    }
    else if (key == "acoustic")
    {
        profile.name = "acoustic";
        anchors = kAcoustic.data();
        anchorCount = kAcoustic.size();
    }
    else if (key == "flat")
    {
        profile.name = "flat";
        isFlat = true;
    }
    else
    {
        profile.name = "modern_pop";
    }

    for (int i = 0; i < kNumSpectrumBands; ++i)
    {
        const double f = detail::bandCentreHz (i);
        profile.targetDb[static_cast<size_t> (i)] = isFlat ? 0.0 : detail::interpolateAnchors (anchors, anchorCount, f);
        profile.toleranceDb[static_cast<size_t> (i)] = detail::interpolateAnchors (kTolerances.data(), kTolerances.size(), f);
    }

    if (! isFlat)
        detail::normalizeMidBand (profile.targetDb);

    return profile;
}

inline AnalysisResult analyzeBuffer (const juce::AudioBuffer<float>& audio,
                                     double sampleRate,
                                     const juce::String& targetName = "modern_pop",
                                     double targetLufs = -14.0)
{
    if (! std::isfinite (targetLufs))
        targetLufs = -14.0;
    AnalysisResult res;
    const int numChannels = audio.getNumChannels();
    const int numSamples = audio.getNumSamples();
    const TargetProfile profile = getTargetProfile (targetName);

    res.sampleRate = std::isfinite (sampleRate) ? sampleRate : 0.0;
    res.numChannels = numChannels;
    res.numSamples = numSamples;
    res.targetName = profile.name;
    res.targetLufs = targetLufs;
    res.targetDb = profile.targetDb;
    for (int i = 0; i < kNumSpectrumBands; ++i)
        res.bandFreqsHz[static_cast<size_t> (i)] = detail::bandCentreHz (i);
    for (int b = 0; b < pdsp::bands::kCount; ++b)
    {
        res.bands[static_cast<size_t> (b)].name = pdsp::bands::kBands[static_cast<size_t> (b)].name;
        res.bands[static_cast<size_t> (b)].loHz = pdsp::bands::kBands[static_cast<size_t> (b)].loHz;
        res.bands[static_cast<size_t> (b)].hiHz = pdsp::bands::kBands[static_cast<size_t> (b)].hiHz;
    }

    if (! std::isfinite (sampleRate) || sampleRate < 25.0 || numChannels <= 0 || numSamples <= 0)
        return res;

    res.valid = true;
    res.durationSeconds = static_cast<double> (numSamples) / sampleRate;

    // -------------------------------------------------------------------------
    // 1 & 2. Peaks, True Peak, Clipping, DC Offset, RMS & ITU-R BS.1770-4 Loudness
    // -------------------------------------------------------------------------
    {
        const auto lp = detail::computeLoudnessAndPeaks (audio, sampleRate, true);
        res.integratedLufs = lp.integratedLufs;
        res.shortTermMaxLufs = lp.shortTermMaxLufs;
        res.shortTermMinLufs = lp.shortTermMinLufs;
        res.lraLu = lp.lraLu;
        res.truePeakDb = lp.truePeakDb;
        res.samplePeakDb = lp.samplePeakDb;
        res.clippedSamples = lp.clippedSamples;
        res.dcOffsetDb = lp.dcOffsetDb;
        res.rmsDb = lp.rmsDb;
        res.crestFactorDb = lp.crestFactorDb;
        res.plrDb = lp.plrDb;
        res.headroomDb = lp.headroomDb;
    }

    const bool isStereo = (numChannels >= 2);
    const float* ptrL = audio.getReadPointer (0);
    const float* ptrR = isStereo ? audio.getReadPointer (1) : ptrL;

    // -------------------------------------------------------------------------
    // 3. Low-End Crest Factor (20-250 Hz) & Stereo / Phase / Mono Compatibility
    // -------------------------------------------------------------------------
    {
        pdsp::LR4 lowCrestHp, lowCrestLp;
        lowCrestHp.set (sampleRate, 20.0);
        lowCrestLp.set (sampleRate, 250.0);

        pdsp::LR4 lowMonoLpL, lowMonoLpR;
        if (isStereo)
        {
            lowMonoLpL.set (sampleRate, 150.0);
            lowMonoLpR.set (sampleRate, 150.0);
        }

        const int corrWin = std::max (1, static_cast<int> (std::llround (0.1 * sampleRate)));
        const int lowCrestWin = std::max (1, static_cast<int> (std::llround (0.4 * sampleRate)));

        double winLR = 0.0, winL2 = 0.0, winR2 = 0.0;
        int corrCount = 0;
        double corrSum = 0.0;
        double corrMin = 1.0;
        int activeCorrWins = 0;

        double totalL2 = 0.0, totalR2 = 0.0, totalMono2 = 0.0;
        double lowStereoAvgSum = 0.0, lowMono2 = 0.0;

        double lcPeak = 0.0, lcSumSq = 0.0;
        int lcCount = 0;
        std::vector<double> lcCrests;

        for (int i = 0; i < numSamples; ++i)
        {
            const double l = detail::sanitize (ptrL[i]);
            const double r = isStereo ? detail::sanitize (ptrR[i]) : l;
            const double mid = 0.5 * (l + r);

            totalL2 += l * l;
            totalR2 += r * r;
            totalMono2 += mid * mid;

            // Low-end (<150 Hz) mono loss
            if (isStereo)
            {
                double loL = 0.0, loR = 0.0, hiDummy = 0.0;
                lowMonoLpL.split (l, loL, hiDummy);
                lowMonoLpR.split (r, loR, hiDummy);
                const double loMid = 0.5 * (loL + loR);
                lowStereoAvgSum += 0.5 * (loL * loL + loR * loR);
                lowMono2 += loMid * loMid;
            }

            // Low-end crest (20-250 Hz)
            double dummy = 0.0, bp = 0.0;
            lowCrestHp.split (mid, dummy, bp);
            lowCrestLp.split (bp, bp, dummy);
            lcPeak = std::max (lcPeak, std::abs (bp));
            lcSumSq += bp * bp;
            ++lcCount;
            if (lcCount == lowCrestWin || i == numSamples - 1)
            {
                if (lcCount > 1 && (lcCount >= lowCrestWin / 2 || lcCrests.empty()))
                {
                    const double winRms = std::sqrt (lcSumSq / static_cast<double> (lcCount));
                    if (winRms > 1e-3 && lcPeak > 1e-6) // > -60 dBFS
                        lcCrests.push_back (20.0 * std::log10 (lcPeak / winRms));
                }
                lcPeak = 0.0;
                lcSumSq = 0.0;
                lcCount = 0;
            }

            // 100 ms stereo correlation windows
            if (isStereo)
            {
                winLR += l * r;
                winL2 += l * l;
                winR2 += r * r;
                ++corrCount;
                if (corrCount == corrWin || i == numSamples - 1)
                {
                    const double avgPwr = 0.5 * (winL2 + winR2) / static_cast<double> (corrCount);
                    if (avgPwr >= 1e-7) // > -70 dBFS
                    {
                        const double denom = std::sqrt (winL2 * winR2);
                        const double c = denom > 1e-24 ? std::clamp (winLR / denom, -1.0, 1.0) : 0.0;
                        corrSum += c;
                        corrMin = (activeCorrWins == 0) ? c : std::min (corrMin, c);
                        ++activeCorrWins;
                    }
                    winLR = winL2 = winR2 = 0.0;
                    corrCount = 0;
                }
            }
        }

        if (! lcCrests.empty())
        {
            std::sort (lcCrests.begin(), lcCrests.end());
            res.lowEndCrestDb = detail::percentileSorted (lcCrests, 0.5);
        }

        if (isStereo)
        {
            if (activeCorrWins > 0)
            {
                res.stereoCorrelationMean = corrSum / static_cast<double> (activeCorrWins);
                res.stereoCorrelationMin = corrMin;
            }
            if (totalL2 > 1e-12 || totalR2 > 1e-12)
                res.lrBalanceDb = std::clamp (10.0 * std::log10 ((totalL2 + 1e-20) / (totalR2 + 1e-20)), -60.0, 60.0);

            const double stereoAvg = 0.5 * (totalL2 + totalR2);
            if (stereoAvg > 1e-12)
                res.monoLossDb = std::max (0.0, -10.0 * std::log10 (std::max (totalMono2 / stereoAvg, 1e-12)));

            if (lowStereoAvgSum > 1e-12)
                res.lowEndMonoLossDb = std::max (0.0, -10.0 * std::log10 (std::max (lowMono2 / lowStereoAvgSum, 1e-12)));
        }
    }

    // -------------------------------------------------------------------------
    // 4. 1/6-Octave Spectrum (16384-pt Hann FFT, hop 4096, gated) & Per-Band Corr
    // -------------------------------------------------------------------------
    bool hasSpectrum = false;
    {
        constexpr int fftOrder = 14;
        constexpr int fftSize = 1 << fftOrder; // 16384
        constexpr int hopSize = 4096;

        juce::dsp::FFT fft (fftOrder);
        std::vector<float> hann (fftSize);
        for (int n = 0; n < fftSize; ++n)
            hann[static_cast<size_t> (n)] = 0.5f * (1.0f - std::cos (2.0f * juce::MathConstants<float>::pi * static_cast<float> (n) / static_cast<float> (fftSize - 1)));

        const int numFrames = (numSamples <= fftSize) ? 1 : ((numSamples - fftSize) / hopSize + 1);
        std::vector<double> framePowers (static_cast<size_t> (numFrames), 0.0);

        double activeFrameSum = 0.0;
        int activeFrameCount = 0;

        if (numSamples <= fftSize)
        {
            double pwr = 0.0;
            for (int n = 0; n < numSamples; ++n)
            {
                const double l = detail::sanitize (ptrL[n]);
                const double r = isStereo ? detail::sanitize (ptrR[n]) : l;
                pwr += 0.5 * (l * l + r * r);
            }
            pwr /= static_cast<double> (std::max (1, numSamples));
            framePowers[0] = pwr;
            if (pwr >= 1e-9) // >= -90 dBFS
            {
                activeFrameSum += pwr;
                ++activeFrameCount;
            }
        }
        else
        {
            const int numChunks = numFrames + 3; // fftSize == 4 * hopSize
            std::vector<double> chunkSums (static_cast<size_t> (numChunks), 0.0);
            for (int c = 0; c < numChunks; ++c)
            {
                const int start = c * hopSize;
                double s = 0.0;
                for (int n = 0; n < hopSize; ++n)
                {
                    const double l = detail::sanitize (ptrL[start + n]);
                    const double r = isStereo ? detail::sanitize (ptrR[start + n]) : l;
                    s += 0.5 * (l * l + r * r);
                }
                chunkSums[static_cast<size_t> (c)] = s;
            }

            for (int f = 0; f < numFrames; ++f)
            {
                const size_t fi = static_cast<size_t> (f);
                const double pwr = (chunkSums[fi] + chunkSums[fi + 1] + chunkSums[fi + 2] + chunkSums[fi + 3]) / static_cast<double> (fftSize);
                framePowers[fi] = pwr;
                if (pwr >= 1e-9) // >= -90 dBFS
                {
                    activeFrameSum += pwr;
                    ++activeFrameCount;
                }
            }
        }

        if (activeFrameCount > 0)
        {
            const double meanFramePwr = activeFrameSum / static_cast<double> (activeFrameCount);
            const double relThresh = meanFramePwr * std::pow (10.0, -35.0 / 10.0);

            std::vector<float> fftBufL (2 * fftSize, 0.0f);
            std::vector<float> fftBufR (2 * fftSize, 0.0f);

            std::array<double, kNumSpectrumBands> bandPwr {};
            std::array<double, pdsp::bands::kCount> corrCross {}, corrL {}, corrR {};
            int usedFrames = 0;

            const double binHz = sampleRate / static_cast<double> (fftSize);
            const int maxBin = fftSize / 2;

            // Precompute bin ranges for the 61 constant-Q bands
            std::array<int, kNumSpectrumBands> bandBinLo {}, bandBinHi {};
            const double halfStep = std::pow (2.0, 1.0 / 12.0);
            for (int b = 0; b < kNumSpectrumBands; ++b)
            {
                const double fc = res.bandFreqsHz[static_cast<size_t> (b)];
                const double fLo = fc / halfStep;
                const double fHi = fc * halfStep;
                int bLo = static_cast<int> (std::ceil (fLo / binHz));
                int bHi = static_cast<int> (std::floor (fHi / binHz));
                if (bLo > bHi)
                {
                    const int nearest = static_cast<int> (std::llround (fc / binHz));
                    bLo = nearest;
                    bHi = nearest;
                }
                bandBinLo[static_cast<size_t> (b)] = std::clamp (bLo, 1, maxBin);
                bandBinHi[static_cast<size_t> (b)] = std::clamp (bHi, 1, maxBin);
            }

            // Precompute bin ranges for the 6 MusicBands
            std::array<int, pdsp::bands::kCount> mbBinLo {}, mbBinHi {};
            mbBinLo.fill (maxBin + 1);
            mbBinHi.fill (0);
            if (isStereo)
            {
                for (int k = 1; k <= maxBin; ++k)
                {
                    const double hz = static_cast<double> (k) * binHz;
                    if (hz < 20.0 || hz > 20000.0)
                        continue;
                    const int mb = pdsp::bands::bandAt (hz);
                    mbBinLo[static_cast<size_t> (mb)] = std::min (mbBinLo[static_cast<size_t> (mb)], k);
                    mbBinHi[static_cast<size_t> (mb)] = std::max (mbBinHi[static_cast<size_t> (mb)], k);
                }
            }

            for (int f = 0; f < numFrames; ++f)
            {
                const double pwr = framePowers[static_cast<size_t> (f)];
                if (pwr < 1e-9 || pwr < relThresh)
                    continue;

                std::fill (fftBufL.begin(), fftBufL.end(), 0.0f);
                if (isStereo)
                    std::fill (fftBufR.begin(), fftBufR.end(), 0.0f);

                const int start = f * hopSize;
                const int len = std::min (fftSize, numSamples - start);
                for (int n = 0; n < len; ++n)
                {
                    const float w = (len == fftSize)
                        ? hann[static_cast<size_t> (n)]
                        : (len == 1 ? 1.0f : 0.5f * (1.0f - std::cos (2.0f * juce::MathConstants<float>::pi * static_cast<float> (n) / static_cast<float> (len - 1))));
                    fftBufL[static_cast<size_t> (n)] = static_cast<float> (detail::sanitize (ptrL[start + n])) * w;
                    if (isStereo)
                        fftBufR[static_cast<size_t> (n)] = static_cast<float> (detail::sanitize (ptrR[start + n])) * w;
                }

                fft.performRealOnlyForwardTransform (fftBufL.data());
                if (isStereo)
                    fft.performRealOnlyForwardTransform (fftBufR.data());

                for (int b = 0; b < kNumSpectrumBands; ++b)
                {
                    double sum = 0.0;
                    for (int k = bandBinLo[static_cast<size_t> (b)]; k <= bandBinHi[static_cast<size_t> (b)]; ++k)
                    {
                        const double lRe = fftBufL[static_cast<size_t> (2 * k)];
                        const double lIm = fftBufL[static_cast<size_t> (2 * k + 1)];
                        const double l2 = lRe * lRe + lIm * lIm;
                        if (isStereo)
                        {
                            const double rRe = fftBufR[static_cast<size_t> (2 * k)];
                            const double rIm = fftBufR[static_cast<size_t> (2 * k + 1)];
                            const double r2 = rRe * rRe + rIm * rIm;
                            sum += 0.5 * (l2 + r2);
                        }
                        else
                        {
                            sum += l2;
                        }
                    }
                    bandPwr[static_cast<size_t> (b)] += sum;
                }

                if (isStereo)
                {
                    for (int mb = 0; mb < pdsp::bands::kCount; ++mb)
                    {
                        double cross = 0.0, lSum = 0.0, rSum = 0.0;
                        for (int k = mbBinLo[static_cast<size_t> (mb)]; k <= mbBinHi[static_cast<size_t> (mb)]; ++k)
                        {
                            const double lRe = fftBufL[static_cast<size_t> (2 * k)];
                            const double lIm = fftBufL[static_cast<size_t> (2 * k + 1)];
                            const double rRe = fftBufR[static_cast<size_t> (2 * k)];
                            const double rIm = fftBufR[static_cast<size_t> (2 * k + 1)];
                            cross += (lRe * rRe + lIm * rIm);
                            lSum += (lRe * lRe + lIm * lIm);
                            rSum += (rRe * rRe + rIm * rIm);
                        }
                        corrCross[static_cast<size_t> (mb)] += cross;
                        corrL[static_cast<size_t> (mb)] += lSum;
                        corrR[static_cast<size_t> (mb)] += rSum;
                    }
                }

                ++usedFrames;
            }

            if (usedFrames > 0)
            {
                hasSpectrum = true;
                for (int b = 0; b < kNumSpectrumBands; ++b)
                {
                    const double avgPwr = bandPwr[static_cast<size_t> (b)] / static_cast<double> (usedFrames);
                    res.measuredDb[static_cast<size_t> (b)] = 10.0 * std::log10 (avgPwr + 1e-20);
                }
                detail::normalizeMidBand (res.measuredDb);

                if (isStereo)
                {
                    for (int mb = 0; mb < pdsp::bands::kCount; ++mb)
                    {
                        const double lE = corrL[static_cast<size_t> (mb)];
                        const double rE = corrR[static_cast<size_t> (mb)];
                        const double denom = std::sqrt (lE * rE);
                        if (denom > 1e-24)
                            res.bandCorrelation[static_cast<size_t> (mb)] = std::clamp (corrCross[static_cast<size_t> (mb)] / denom, -1.0, 1.0);
                        else
                            res.bandCorrelation[static_cast<size_t> (mb)] = (lE > 1e-24 || rE > 1e-24) ? 0.0 : 1.0;
                    }
                }
            }
        }
    }

    // -------------------------------------------------------------------------
    // 5. Tonal Balance, Deviations, Resonance Rings & Harshness
    // -------------------------------------------------------------------------
    if (hasSpectrum)
    {
        const double maxAnalyzedHz = std::min (20000.0, 0.45 * sampleRate);

        double sqDiffSum = 0.0;
        int diffCount = 0;
        for (int i = 0; i < kNumSpectrumBands; ++i)
        {
            if (res.bandFreqsHz[static_cast<size_t> (i)] <= maxAnalyzedHz)
            {
                const double d = res.measuredDb[static_cast<size_t> (i)] - res.targetDb[static_cast<size_t> (i)];
                sqDiffSum += d * d;
                ++diffCount;
            }
        }
        res.rmsDistanceDb = diffCount > 0 ? std::sqrt (sqDiffSum / static_cast<double> (diffCount)) : 0.0;

        const double slopeMaxHz = std::min (16000.0, 0.45 * sampleRate);
        res.tiltDbPerOct = detail::computeSlopeDbPerOct (res.measuredDb, slopeMaxHz);
        res.targetTiltDbPerOct = detail::computeSlopeDbPerOct (res.targetDb, slopeMaxHz);
        res.tiltDiffDbPerOct = res.tiltDbPerOct - res.targetTiltDbPerOct;

        // 6 MusicBands aggregation
        for (int mb = 0; mb < pdsp::bands::kCount; ++mb)
        {
            const auto& info = pdsp::bands::kBands[static_cast<size_t> (mb)];
            double mSum = 0.0, tSum = 0.0, tolSum = 0.0;
            int cnt = 0;
            for (int i = 0; i < kNumSpectrumBands; ++i)
            {
                const double f = res.bandFreqsHz[static_cast<size_t> (i)];
                const bool inBand = (mb == pdsp::bands::kCount - 1) ? (f >= info.loHz && f <= info.hiHz)
                                                                    : (f >= info.loHz && f < info.hiHz);
                if (inBand && f <= maxAnalyzedHz)
                {
                    mSum += res.measuredDb[static_cast<size_t> (i)];
                    tSum += res.targetDb[static_cast<size_t> (i)];
                    tolSum += profile.toleranceDb[static_cast<size_t> (i)];
                    ++cnt;
                }
            }
            auto& out = res.bands[static_cast<size_t> (mb)];
            if (cnt > 0)
            {
                out.measuredDb = mSum / static_cast<double> (cnt);
                out.targetDb = tSum / static_cast<double> (cnt);
                out.deltaDb = out.measuredDb - out.targetDb;
                const double tol = tolSum / static_cast<double> (cnt);
                const double absD = std::abs (out.deltaDb);
                out.status = (absD <= tol) ? "ok" : ((absD <= tol * 1.6) ? "warn" : "bad");
            }
        }

        // Deviations outside tolerance (merge adjacent same-sign deviations)
        bool inDev = false;
        SpectralDeviation curDev;
        for (int i = 0; i < kNumSpectrumBands; ++i)
        {
            const double f = res.bandFreqsHz[static_cast<size_t> (i)];
            if (f > maxAnalyzedHz)
                break;
            const double delta = res.measuredDb[static_cast<size_t> (i)] - res.targetDb[static_cast<size_t> (i)];
            const double tol = profile.toleranceDb[static_cast<size_t> (i)];
            const bool outOfTol = std::abs (delta) > tol;
            const juce::String dir = (delta >= 0.0) ? "excess" : "deficit";

            if (outOfTol)
            {
                if (! inDev || curDev.direction != dir)
                {
                    if (inDev)
                        res.deviations.push_back (curDev);
                    inDev = true;
                    curDev.loHz = f;
                    curDev.hiHz = f;
                    curDev.peakHz = f;
                    curDev.deltaDb = delta;
                    curDev.toleranceDb = tol;
                    curDev.direction = dir;
                }
                else
                {
                    curDev.hiHz = f;
                    if (std::abs (delta) > std::abs (curDev.deltaDb))
                    {
                        curDev.deltaDb = delta;
                        curDev.peakHz = f;
                        curDev.toleranceDb = tol;
                    }
                }
            }
            else if (inDev)
            {
                res.deviations.push_back (curDev);
                inDev = false;
            }
        }
        if (inDev)
            res.deviations.push_back (curDev);

        // Resonance rings (>= 250 Hz, standOutDb >= 4.0 dB, local peak)
        for (int b = 3; b + 3 < kNumSpectrumBands; ++b)
        {
            const double f = res.bandFreqsHz[static_cast<size_t> (b)];
            if (f < 250.0)
                continue;
            if (res.bandFreqsHz[static_cast<size_t> (b + 3)] > maxAnalyzedHz)
                break;
            const double m = res.measuredDb[static_cast<size_t> (b)];
            if (m <= res.measuredDb[static_cast<size_t> (b - 1)] || m < res.measuredDb[static_cast<size_t> (b + 1)])
                continue;
            const double leftMean = 0.5 * (res.measuredDb[static_cast<size_t> (b - 2)] + res.measuredDb[static_cast<size_t> (b - 3)]);
            const double rightMean = 0.5 * (res.measuredDb[static_cast<size_t> (b + 2)] + res.measuredDb[static_cast<size_t> (b + 3)]);
            const double standOut = m - std::max (leftMean, rightMean);
            if (standOut >= 4.0)
                res.resonanceRings.push_back ({ f, standOut, m });
        }
        std::sort (res.resonanceRings.begin(), res.resonanceRings.end(),
                   [] (const ResonanceRing& a, const ResonanceRing& b) { return a.standOutDb > b.standOutDb; });
        if (res.resonanceRings.size() > 5)
            res.resonanceRings.resize (5);

        // Harshness / sibilance (4..10 kHz, p90 - mean >= 6.0 dB)
        std::vector<double> harshVals;
        double harshMean = 0.0;
        double worstVal = -1e9;
        double worstHz = 0.0;
        for (int b = 0; b < kNumSpectrumBands; ++b)
        {
            const double f = res.bandFreqsHz[static_cast<size_t> (b)];
            if (f >= 4000.0 && f <= 10000.0 && f <= maxAnalyzedHz)
            {
                const double v = res.measuredDb[static_cast<size_t> (b)];
                harshVals.push_back (v);
                harshMean += v;
                if (v > worstVal)
                {
                    worstVal = v;
                    worstHz = f;
                }
            }
        }
        if (harshVals.size() >= 3)
        {
            harshMean /= static_cast<double> (harshVals.size());
            std::sort (harshVals.begin(), harshVals.end());
            const double p90 = detail::percentileSorted (harshVals, 0.90);
            const double jump = p90 - harshMean;
            res.harshness.worstFreqHz = worstHz;
            res.harshness.jumpDb = jump;
            res.harshness.detected = (jump >= 6.0);
        }
    }

    // -------------------------------------------------------------------------
    // 6. Actionable Recommendations
    // -------------------------------------------------------------------------
    if (res.clippedSamples > 0 || res.truePeakDb > 0.0)
    {
        res.recommendations.push_back ({
            "headroom",
            "bad",
            "Aegis Ceiling",
            "True peak hits " + detail::formatDb (res.truePeakDb) + " dBTP (" + juce::String (res.clippedSamples)
                + " clipped samples). Engage Aegis Ceiling at -1.0 dBTP and reduce input trim by "
                + detail::formatDb (res.truePeakDb + 1.0, false) + " dB."
        });
    }
    else if (res.truePeakDb > -1.0)
    {
        res.recommendations.push_back ({
            "headroom",
            "warn",
            "Aegis Ceiling",
            "True peak is " + detail::formatDb (res.truePeakDb) + " dBTP (under 1.0 dB headroom). Set Aegis Ceiling to -1.0 dBTP to prevent inter-sample codec clipping."
        });
    }

    if (res.integratedLufs > -70.0 && std::abs (res.integratedLufs - targetLufs) > 2.0)
    {
        const double diff = targetLufs - res.integratedLufs;
        res.recommendations.push_back ({
            "loudness",
            "warn",
            diff > 0.0 ? "Aegis Ceiling" : "Hermes Level",
            "Integrated loudness is " + detail::formatDb (res.integratedLufs, false) + " LUFS vs target "
                + detail::formatDb (targetLufs, false) + " LUFS (" + detail::formatDb (diff)
                + " dB). Adjust master gain or limiter drive by " + detail::formatDb (diff) + " dB."
        });
    }

    if (numChannels >= 2 && (res.stereoCorrelationMean < 0.0 || res.stereoCorrelationMin < -0.2 || res.monoLossDb > 6.0))
    {
        const bool severe = (res.stereoCorrelationMean < 0.0 || res.stereoCorrelationMin < -0.5 || res.monoLossDb > 12.0);
        res.recommendations.push_back ({
            "phase",
            severe ? "bad" : "warn",
            "Castor Pair",
            "Phase cancellation detected (mean correlation " + juce::String (res.stereoCorrelationMean, 2)
                + ", min " + juce::String (res.stereoCorrelationMin, 2) + ", mono loss "
                + detail::formatDb (res.monoLossDb, false) + " dB). Narrow stereo width in Castor Pair or check inverted polarity."
        });
    }

    if (numChannels >= 2 && res.lowEndMonoLossDb > 3.0)
    {
        res.recommendations.push_back ({
            "phase",
            "warn",
            "Castor Pair",
            "Low-end (<150 Hz) loses " + detail::formatDb (res.lowEndMonoLossDb, false)
                + " dB in mono. Set Castor Pair Sub/Bass width to 0% below 150 Hz."
        });
    }

    for (size_t i = 0; i < std::min<size_t> (res.resonanceRings.size(), 3); ++i)
    {
        const auto& ring = res.resonanceRings[i];
        const double cutDb = -std::min (8.0, ring.standOutDb * 0.6);
        res.recommendations.push_back ({
            "resonance",
            "warn",
            "Iris Prism",
            "Narrow resonance at " + juce::String (std::llround (ring.freqHz)) + " Hz (+"
                + detail::formatDb (ring.standOutDb, false) + " dB above neighbours). Cut "
                + detail::formatDb (cutDb) + " dB at " + juce::String (std::llround (ring.freqHz)) + " Hz with Q 7.0."
        });
    }

    if (res.harshness.detected)
    {
        res.recommendations.push_back ({
            "harshness",
            "warn",
            "Iris Prism",
            "Upper-mid harshness spike around " + juce::String (std::llround (res.harshness.worstFreqHz))
                + " Hz (+" + detail::formatDb (res.harshness.jumpDb, false) + " dB jump). Apply a dynamic cut of -2.5 dB at "
                + juce::String (std::llround (res.harshness.worstFreqHz)) + " Hz (Q 3.5)."
        });
    }

    if (hasSpectrum)
    {
        for (const auto& b : res.bands)
        {
            if (b.status == "ok")
                continue;
            const double moveDb = -b.deltaDb * 0.65;
            const double centerHz = std::sqrt (b.loHz * b.hiHz);
            res.recommendations.push_back ({
                "tonal_balance",
                b.status,
                "Iris Prism",
                b.name + " (" + juce::String (static_cast<int> (b.loHz)) + "-" + juce::String (static_cast<int> (b.hiHz))
                    + " Hz) is " + detail::formatDb (b.deltaDb) + " dB vs " + res.targetName + ". Apply "
                    + detail::formatDb (moveDb) + " dB bell around " + juce::String (std::llround (centerHz)) + " Hz (Q 1.0)."
            });
        }
    }

    if (res.integratedLufs > -70.0 && res.plrDb < 6.0)
    {
        res.recommendations.push_back ({
            "dynamics",
            "warn",
            "Atlas Span",
            "Low peak-to-loudness ratio (" + detail::formatDb (res.plrDb, false)
                + " dB PLR). Relax bus compression ratio or raise threshold in Atlas Span by +2.0 dB."
        });
    }
    else if (res.lraLu > 14.0)
    {
        res.recommendations.push_back ({
            "dynamics",
            "info",
            "Atlas Span",
            "Wide loudness range (" + detail::formatDb (res.lraLu, false)
                + " LU). Consider gentle glue compression in Atlas Span (2:1, threshold -18 dBFS) for streaming consistency."
        });
    }

    return res;
}

inline void analyzeTrackForMasking (const juce::String& name,
                                    const juce::AudioBuffer<float>& audio,
                                    double sampleRate,
                                    TrackSummary& summary,
                                    std::array<double, pdsp::bands::kCount>& outBandDb,
                                    double& loudestBandDb)
{
    summary = {};
    summary.name = name;
    summary.dominantBand = "None";
    outBandDb.fill (-120.0);

    if (! std::isfinite (sampleRate) || sampleRate < 25.0)
        return;

    const int nCh = audio.getNumChannels();
    const int nSamp = audio.getNumSamples();
    if (nCh <= 0 || nSamp <= 0)
        return;

    const auto lp = detail::computeLoudnessAndPeaks (audio, sampleRate, false);
    summary.integratedLufs = lp.integratedLufs;
    summary.truePeakDb = lp.truePeakDb;
    summary.crestFactorDb = lp.crestFactorDb;

    // Compute per-band absolute RMS dB via BandSplitter so track-to-track levels are directly comparable
    std::array<double, pdsp::bands::kCount> bandSumSq {};
    double sub80SumSq = 0.0;
    double totalSumSq = 0.0;

    pdsp::BandSplitter splitter;
    splitter.prepare (sampleRate);
    pdsp::LR4 sub80Lp;
    sub80Lp.set (sampleRate, 80.0);

    const float* ptrL = audio.getReadPointer (0);
    const float* ptrR = (nCh >= 2) ? audio.getReadPointer (1) : ptrL;

    for (int i = 0; i < nSamp; ++i)
    {
        const double l = detail::sanitize (ptrL[i]);
        const double r = (nCh >= 2) ? detail::sanitize (ptrR[i]) : l;
        const double mono = 0.5 * (l + r);

        std::array<double, pdsp::bands::kCount> parts {};
        splitter.split (0, mono, parts);
        for (int b = 0; b < pdsp::bands::kCount; ++b)
            bandSumSq[static_cast<size_t> (b)] += parts[static_cast<size_t> (b)] * parts[static_cast<size_t> (b)];

        double lo80 = 0.0, hiDummy = 0.0;
        sub80Lp.split (mono, lo80, hiDummy);
        sub80SumSq += lo80 * lo80;
        totalSumSq += mono * mono;
    }

    summary.sub80Ratio = totalSumSq > 1e-18 ? std::clamp (sub80SumSq / totalSumSq, 0.0, 1.0) : 0.0;

    int domIdx = 0;
    double domDb = -120.0;
    for (int b = 0; b < pdsp::bands::kCount; ++b)
    {
        const double meanSq = bandSumSq[static_cast<size_t> (b)] / static_cast<double> (nSamp);
        const double db = meanSq > 1e-12 ? 10.0 * std::log10 (meanSq) : -120.0;
        outBandDb[static_cast<size_t> (b)] = db;
        if (db > domDb)
        {
            domDb = db;
            domIdx = b;
        }
        if (db > loudestBandDb)
            loudestBandDb = db;
    }
    if (domDb > -120.0)
        summary.dominantBand = pdsp::bands::kBands[static_cast<size_t> (domIdx)].name;
}

inline MaskingResult buildMaskingResult (std::vector<TrackSummary> summaries,
                                         const std::vector<std::array<double, pdsp::bands::kCount>>& trackBands,
                                         double loudestBandDb)
{
    MaskingResult out;
    out.tracks = std::move (summaries);

    // Pairwise masking conflicts
    for (size_t i = 0; i < out.tracks.size(); ++i)
    {
        for (size_t j = i + 1; j < out.tracks.size(); ++j)
        {
            for (int b = 0; b < pdsp::bands::kCount; ++b)
            {
                const double eA = trackBands[i][static_cast<size_t> (b)];
                const double eB = trackBands[j][static_cast<size_t> (b)];
                if (eA <= -90.0 || eB <= -90.0)
                    continue;
                const double diff = std::abs (eA - eB);
                const double peakBand = std::max (eA, eB);
                if (diff <= 6.0 && peakBand >= loudestBandDb - 15.0)
                {
                    const auto& info = pdsp::bands::kBands[static_cast<size_t> (b)];
                    const double centerHz = std::sqrt (info.loHz * info.hiHz);
                    juce::String sug;
                    if (b <= 1)
                        sug = "High-pass the non-bass element above " + juce::String (static_cast<int> (info.hiHz))
                            + " Hz or carve -3.0 dB at " + juce::String (std::llround (centerHz)) + " Hz on " + out.tracks[j].name + ".";
                    else
                        sug = "Cut -2.5 dB at " + juce::String (std::llround (centerHz)) + " Hz (Q 1.5) on " + out.tracks[j].name
                            + " or pan tracks apart for separation.";

                    out.conflicts.push_back ({
                        out.tracks[i].name,
                        out.tracks[j].name,
                        info.name,
                        info.loHz,
                        info.hiHz,
                        diff,
                        sug
                    });
                }
            }
        }
    }

    return out;
}

inline MaskingResult analyzeMasking (const std::vector<TrackAudio>& tracks, double sampleRate)
{
    MaskingResult out;
    if (! std::isfinite (sampleRate) || sampleRate < 25.0 || tracks.empty())
        return out;

    std::vector<TrackSummary> summaries (tracks.size());
    std::vector<std::array<double, pdsp::bands::kCount>> trackBands (tracks.size());
    double loudestBandDb = -120.0;

    for (size_t idx = 0; idx < tracks.size(); ++idx)
        analyzeTrackForMasking (tracks[idx].name, tracks[idx].audio, sampleRate,
                                summaries[idx], trackBands[idx], loudestBandDb);

    return buildMaskingResult (std::move (summaries), trackBands, loudestBandDb);
}

inline juce::var AnalysisResult::toVar() const
{
    auto* root = new juce::DynamicObject();
    root->setProperty ("valid", valid);
    root->setProperty ("sample_rate", sampleRate);
    root->setProperty ("num_channels", numChannels);
    root->setProperty ("num_samples", numSamples);
    root->setProperty ("duration_seconds", durationSeconds);
    root->setProperty ("target_name", targetName);
    root->setProperty ("target_lufs", targetLufs);

    {
        auto* loud = new juce::DynamicObject();
        loud->setProperty ("integrated_lufs", integratedLufs);
        loud->setProperty ("short_term_max_lufs", shortTermMaxLufs);
        loud->setProperty ("short_term_min_lufs", shortTermMinLufs);
        loud->setProperty ("lra_lu", lraLu);
        root->setProperty ("loudness", juce::var (loud));
    }

    {
        auto* pk = new juce::DynamicObject();
        pk->setProperty ("true_peak_db", truePeakDb);
        pk->setProperty ("sample_peak_db", samplePeakDb);
        pk->setProperty ("clipped_samples", static_cast<juce::int64> (clippedSamples));
        pk->setProperty ("dc_offset_db", dcOffsetDb);
        pk->setProperty ("rms_db", rmsDb);
        pk->setProperty ("crest_factor_db", crestFactorDb);
        pk->setProperty ("plr_db", plrDb);
        pk->setProperty ("headroom_db", headroomDb);
        pk->setProperty ("low_end_crest_db", lowEndCrestDb);
        root->setProperty ("peaks", juce::var (pk));
    }

    {
        auto* spec = new juce::DynamicObject();
        spec->setProperty ("rms_distance_db", rmsDistanceDb);
        spec->setProperty ("tilt_db_per_oct", tiltDbPerOct);
        spec->setProperty ("target_tilt_db_per_oct", targetTiltDbPerOct);
        spec->setProperty ("tilt_diff_db_per_oct", tiltDiffDbPerOct);

        juce::Array<juce::var> bandArr;
        for (const auto& b : bands)
        {
            auto* bo = new juce::DynamicObject();
            bo->setProperty ("name", b.name);
            bo->setProperty ("lo_hz", b.loHz);
            bo->setProperty ("hi_hz", b.hiHz);
            bo->setProperty ("measured_db", b.measuredDb);
            bo->setProperty ("target_db", b.targetDb);
            bo->setProperty ("delta_db", b.deltaDb);
            bo->setProperty ("status", b.status);
            bandArr.add (juce::var (bo));
        }
        spec->setProperty ("bands", bandArr);

        juce::Array<juce::var> devArr;
        for (const auto& d : deviations)
        {
            auto* dobj = new juce::DynamicObject();
            dobj->setProperty ("lo_hz", d.loHz);
            dobj->setProperty ("hi_hz", d.hiHz);
            dobj->setProperty ("peak_hz", d.peakHz);
            dobj->setProperty ("delta_db", d.deltaDb);
            dobj->setProperty ("tolerance_db", d.toleranceDb);
            dobj->setProperty ("direction", d.direction);
            devArr.add (juce::var (dobj));
        }
        spec->setProperty ("deviations", devArr);

        juce::Array<juce::var> ringArr;
        for (const auto& r : resonanceRings)
        {
            auto* ro = new juce::DynamicObject();
            ro->setProperty ("freq_hz", r.freqHz);
            ro->setProperty ("stand_out_db", r.standOutDb);
            ro->setProperty ("measured_db", r.measuredDb);
            ringArr.add (juce::var (ro));
        }
        spec->setProperty ("resonance_rings", ringArr);

        auto* ho = new juce::DynamicObject();
        ho->setProperty ("detected", harshness.detected);
        ho->setProperty ("worst_freq_hz", harshness.worstFreqHz);
        ho->setProperty ("jump_db", harshness.jumpDb);
        spec->setProperty ("harshness", juce::var (ho));

        root->setProperty ("spectrum", juce::var (spec));
    }

    {
        auto* st = new juce::DynamicObject();
        st->setProperty ("correlation_mean", stereoCorrelationMean);
        st->setProperty ("correlation_min", stereoCorrelationMin);
        st->setProperty ("lr_balance_db", lrBalanceDb);
        st->setProperty ("mono_loss_db", monoLossDb);
        st->setProperty ("low_end_mono_loss_db", lowEndMonoLossDb);

        auto* bc = new juce::DynamicObject();
        for (int b = 0; b < pdsp::bands::kCount; ++b)
            bc->setProperty (pdsp::bands::kBands[static_cast<size_t> (b)].name, bandCorrelation[static_cast<size_t> (b)]);
        st->setProperty ("band_correlation", juce::var (bc));
        root->setProperty ("stereo", juce::var (st));
    }

    {
        juce::Array<juce::var> recArr;
        for (const auto& r : recommendations)
        {
            auto* ro = new juce::DynamicObject();
            ro->setProperty ("category", r.category);
            ro->setProperty ("severity", r.severity);
            ro->setProperty ("plugin", r.plugin);
            ro->setProperty ("action", r.action);
            recArr.add (juce::var (ro));
        }
        root->setProperty ("recommendations", recArr);
    }

    return juce::var (root);
}

inline juce::var MaskingResult::toVar() const
{
    auto* root = new juce::DynamicObject();

    juce::Array<juce::var> trackArr;
    for (const auto& t : tracks)
    {
        auto* to = new juce::DynamicObject();
        to->setProperty ("name", t.name);
        to->setProperty ("integrated_lufs", t.integratedLufs);
        to->setProperty ("true_peak_db", t.truePeakDb);
        to->setProperty ("crest_factor_db", t.crestFactorDb);
        to->setProperty ("dominant_band", t.dominantBand);
        to->setProperty ("sub80_ratio", t.sub80Ratio);
        trackArr.add (juce::var (to));
    }
    root->setProperty ("tracks", trackArr);

    juce::Array<juce::var> confArr;
    for (const auto& c : conflicts)
    {
        auto* co = new juce::DynamicObject();
        co->setProperty ("track_a", c.trackA);
        co->setProperty ("track_b", c.trackB);
        co->setProperty ("band", c.band);
        juce::Array<juce::var> rangeArr;
        rangeArr.add (c.loHz);
        rangeArr.add (c.hiHz);
        co->setProperty ("freq_range_hz", rangeArr);
        co->setProperty ("overlap_db", c.overlapDb);
        co->setProperty ("suggestion", c.suggestion);
        confArr.add (juce::var (co));
    }
    root->setProperty ("conflicts", confArr);

    return juce::var (root);
}
} // namespace olympus::analyze
