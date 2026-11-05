// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 The Pantheon authors

#pragma once

#include "Band.h"
#include <pantheon/dsp/Biquad.h>
#include "Waveshaper.h"
#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_dsp/juce_dsp.h>
#include <array>
#include <memory>

namespace hep
{
// Multi-band harmonic saturator with user-placed bands.
//
//   for every active band:  input -> band-pass -> [oversample -> ADAA waveshaper -> decimate]
//   output = sum of what the shapers ADD (their distortion), not the shaped signal itself
//
// The caller adds that sum to a clean, latency-matched copy of the input. This replaces the old
// fixed LR4 split: bands may overlap, leave gaps or sit anywhere, and the signal outside the
// shapers is never filtered, so the plugin is exactly transparent at small signal for any
// arrangement of bands (a split that must sum flat cannot offer that). The distortion of a band is
// not confined to the band: its harmonics land above it, which is the point.
//
// The only latency comes from the linear-phase oversampling filters.
class Saturator
{
public:
    static constexpr int maxChannels = 2;
    static constexpr int numOsModes = 4; // off, 2x, 4x, 8x

    struct Band
    {
        bool active = false;
        double lo = 100.0, hi = 1000.0;
        double drive = 1.0; // linear
    };

    struct Params
    {
        int curve = 0;     // 0 tanh, 1 atan, 2 algebraic
        double bias = 0.0; // pre-shaper offset; > 0 adds even harmonics
        std::array<Band, maxBands> band;
    };

    void prepare (double sampleRate, int maxBlock)
    {
        fs = sampleRate;

        for (auto& b : scratch)
            b.setSize (1, maxBlock, false, false, true);

        for (int mode = 1; mode < numOsModes; ++mode)
            for (int b = 0; b < maxBands; ++b)
            {
                auto os = std::make_unique<juce::dsp::Oversampling<float>> (
                    (size_t) maxChannels, (size_t) mode,
                    juce::dsp::Oversampling<float>::filterHalfBandFIREquiripple, true, true);
                os->initProcessing ((size_t) maxBlock);
                oversampler[(size_t) mode][(size_t) b] = std::move (os);
            }

        reset();
    }

    // Latency (whole samples) introduced by an oversampling mode.
    int latencyFor (int osMode) const
    {
        if (osMode <= 0) return 0;
        return (int) std::lround (oversampler[(size_t) osMode][0]->getLatencyInSamples());
    }

    void reset()
    {
        for (auto& band : state)
            band = BandState {};
        resetShapers();
    }

    // Writes the summed distortion of the active bands to `out` (nCh channels, n samples, delayed
    // by latencyFor(osMode)). `in` is not modified. With `soloOut`, the distortion of band `soloBand`
    // alone (its share of `out`, faded as it is) is written there too, for the band solo.
    void process (const float* const* in, float* const* out, int nCh, int n, int osMode, const Params& p,
                  int soloBand = -1, float* const* soloOut = nullptr)
    {
        for (int c = 0; c < nCh; ++c)
        {
            std::fill_n (out[c], n, 0.0f);
            if (soloOut != nullptr)
                std::fill_n (soloOut[c], n, 0.0f);
        }

        for (int b = 0; b < maxBands; ++b)
        {
            auto& st = state[(size_t) b];
            const auto& band = p.band[(size_t) b];

            // A band that was switched off and has faded out costs nothing until it comes back.
            if (! band.active && st.gain <= 0.0f)
            {
                st.running = false;
                continue;
            }

            if (! st.running)
                startBand (b, band);
            st.running = true;

            updateBandFilters (b, band);

            float* data[maxChannels] = { scratch[0].getWritePointer (0), scratch[1].getWritePointer (0) };
            for (int c = 0; c < nCh; ++c)
                for (int i = 0; i < n; ++i)
                    data[c][i] = (float) st.filter[(size_t) c].process ((double) in[c][i]);

            const double dcR = dcCoefficient (osMode);
            const double dStart = st.drive, dEnd = band.drive;
            st.drive = dEnd;

            if (osMode <= 0)
            {
                for (int c = 0; c < nCh; ++c)
                    shape (p.curve, data[c], n, dStart, dEnd, p.bias, dcR, st.adaa[(size_t) c]);
            }
            else
            {
                auto& os = *oversampler[(size_t) osMode][(size_t) b];
                juce::dsp::AudioBlock<float> block (data, (size_t) nCh, (size_t) n);
                auto up = os.processSamplesUp (block);

                for (int c = 0; c < nCh; ++c)
                    shape (p.curve, up.getChannelPointer ((size_t) c), (int) up.getNumSamples(),
                           dStart, dEnd, p.bias, dcR, st.adaa[(size_t) c]);

                os.processSamplesDown (block);
            }

            // Fade a band in or out across the block so switching it never clicks.
            const float target = band.active ? 1.0f : 0.0f;
            const float step = (target - st.gain) / (float) n;
            for (int c = 0; c < nCh; ++c)
            {
                float g = st.gain;
                for (int i = 0; i < n; ++i)
                {
                    g += step;
                    out[c][i] += data[c][i] * g;
                    if (soloOut != nullptr && b == soloBand)
                        soloOut[c][i] = data[c][i] * g;
                }
            }
            st.gain = target;
        }
    }

    // Switching oversampling invalidates the shaper history (it was recorded at another rate).
    void onOversamplingChanged() { resetShapers(); }

private:
    struct BandState
    {
        std::array<pdsp::Bandpass, maxChannels> filter;
        std::array<AdaaState, maxChannels> adaa;
        double cutLo = -1.0, cutHi = -1.0, drive = 1.0;
        float gain = 0.0f; // fade level of the band's contribution
        bool running = false;
    };

    void resetShapers()
    {
        for (auto& modes : oversampler)
            for (auto& os : modes)
                if (os != nullptr)
                    os->reset();

        for (auto& band : state)
            for (auto& s : band.adaa)
                s.reset();
    }

    // A band coming back after a pause starts from clean history, at its own drive.
    void startBand (int b, const Band& band)
    {
        auto& st = state[(size_t) b];
        for (auto& f : st.filter) f.reset();
        for (auto& s : st.adaa) s.reset();
        st.cutLo = st.cutHi = -1.0;
        st.drive = band.drive;
        st.gain = 0.0f;
        for (auto& modes : oversampler)
            if (modes[(size_t) b] != nullptr)
                modes[(size_t) b]->reset();
    }

    // 5 Hz DC blocker coefficient at the rate the shaper runs at.
    double dcCoefficient (int osMode) const noexcept
    {
        return 1.0 - 2.0 * juce::MathConstants<double>::pi * 5.0 / (fs * (double) (1 << std::max (osMode, 0)));
    }

    static void shape (int curve, float* data, int n, double d0, double d1, double bias, double dcR,
                       AdaaState& st) noexcept
    {
        switch (curve)
        {
            case 1:  shapeBlock<AtanCurve, false>      (data, n, d0, d1, bias, dcR, st); break;
            case 2:  shapeBlock<AlgebraicCurve, false> (data, n, d0, d1, bias, dcR, st); break;
            default: shapeBlock<TanhCurve, false>      (data, n, d0, d1, bias, dcR, st); break;
        }
    }

    void updateBandFilters (int b, const Band& band)
    {
        auto& st = state[(size_t) b];
        const double ceiling = fs * 0.45;
        const auto range = effectiveRange (BandMode::saturate, band.lo, band.hi);
        const double hiGoal = std::min (range.hi, ceiling);
        const double loGoal = std::min (range.lo, hiGoal / minRangeRatio);

        // Glide towards the target so moving a band edge doesn't zipper.
        st.cutLo = st.cutLo < 0.0 ? loGoal : st.cutLo + 0.25 * (loGoal - st.cutLo);
        st.cutHi = st.cutHi < 0.0 ? hiGoal : st.cutHi + 0.25 * (hiGoal - st.cutHi);

        for (auto& f : st.filter)
            f.setup (fs, 4, st.cutLo, 4, st.cutHi);
    }

    double fs = 48000.0;
    std::array<juce::AudioBuffer<float>, maxChannels> scratch; // one channel each, reused by every band
    std::array<std::array<std::unique_ptr<juce::dsp::Oversampling<float>>, maxBands>, numOsModes> oversampler;
    std::array<BandState, maxBands> state;
};
}
