// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 The Pantheon authors

#pragma once

#include "BandGrid.h"
#include "ClipMeter.h"
#include "Common.h"
#include "FFT.h"
#include "FrameHistory.h"
#include "Fundamental.h"
#include "Spectrogram.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <memory>
#include <vector>

namespace themis
{
// The frequency ranges of the energy-over-time graph and of the LOW / LOW-MID / HIGH-MID / HIGH readouts. A band
// belongs to a range by its centre frequency.
struct EnergyRange
{
    const char* name;
    double loHz, hiHz;

    bool contains (int band) const
    {
        const double f = BandGrid::centreHz (band);
        return f >= loHz && f < hiHz;
    }
};
inline constexpr EnergyRange kEnergyRanges[kNumEnergyRanges] = { { "LOW", BandGrid::kF0, 120.0 },
                                                                 { "LOW-MID", 120.0, 1000.0 },
                                                                 { "HIGH-MID", 1000.0, 6000.0 },
                                                                 { "HIGH", 6000.0, BandGrid::kMaxHz } };

// Name of an energy-history column: one of the ranges, or the whole spectrum.
inline const char* energyColumnName (int column) { return column == kAllRanges ? "ALL" : kEnergyRanges[column].name; }

// Per-band stereo phase relationship, averaged over time like the tonal curve (see TonalAnalyzer::readPhase).
struct PhaseBands
{
    BandArray correlation {}; // (M - S) / (M + S) in -1..+1: +1 mono, 0 uncorrelated, -1 out of phase
    BandArray coherence {};   // 0..1: how stable the phase between the channels is (1 = a fixed delay / polarity relationship)
    BandArray phaseRad {};    // arg(L R*): phase of the left channel relative to the right, -pi..pi
    BandArray levelDb {};     // band level like the tonal curve (0 dB = anchor-region mean), but of mid + side, so it also exists for a pure-side signal
};

// Analysed bands at or below this level (dB, after anchor normalisation) carry no information: they lie
// above the Nyquist frequency of the analysed signal (published as about -200 dB) or are digital silence.
inline constexpr float kNoDataDb = -150.0f;

// Shifts a dB curve so its mean across the anchor region is 0.
inline void normaliseAnchor (BandArray& db)
{
    const int lo = static_cast<int> (std::ceil (BandGrid::indexForHz (BandGrid::kAnchorLoHz)));
    const int hi = static_cast<int> (std::floor (BandGrid::indexForHz (BandGrid::kAnchorHiHz)));
    double sum = 0.0;
    for (int i = lo; i <= hi; ++i)
        sum += db[static_cast<size_t> (i)];
    const float mean = static_cast<float> (sum / (hi - lo + 1));
    for (auto& v : db)
        v -= mean;
}

// Moving average over +/- `radius` bands (shorter at the edges).
inline void smoothBands (BandArray& v, int radius)
{
    const BandArray in = v;
    for (int i = 0; i < BandGrid::kNum; ++i)
    {
        double sum = 0.0;
        int n = 0;
        for (int j = std::max (0, i - radius); j <= std::min (BandGrid::kNum - 1, i + radius); ++j, ++n)
            sum += in[static_cast<size_t> (j)];
        v[static_cast<size_t> (i)] = static_cast<float> (sum / n);
    }
}

// Long-term constant-Q spectrum of a stereo signal (analysed as mid), independent of loudness.
//
// Level independence: every analysed frame is normalised by its own total band energy before it enters the
// average, so the result is the average spectral *shape*. Scaling the input by any gain, or having loud and
// quiet passages in the same programme, does not move the curve (a loud chorus does not outweigh a quiet
// verse). Frames that are silent, or far below the running level (fade tails, gaps, noise floor), are gated
// out relative to that running level, never against a fixed dBFS threshold. The published curve is then
// shifted so the anchor region averages 0 dB.
//
// Threading: the audio thread only calls push(), which copies the mid and side signals into lock-free rings and
// does no FFT work. A worker thread calls pump() every few milliseconds to run the analysis; UI threads
// read the published values. process() = push() + pump() on one thread, for offline use and tests.
// prepare() must not run concurrently with push()/pump().
//
// Mono fold: the same frames also yield, per band, how much of the stereo signal's power survives summing to mono,
// 10 log10(M / (M + S)) with M = |(L+R)/2|^2 and S = |(L-R)/2|^2: 0 dB for a mono signal, -3 dB for uncorrelated
// channels, strongly negative for out-of-phase content. Like the tonal curve it is level independent.
//
// Phase: the complex cross-spectrum L R* = (|M|^2 - |S|^2) - 2j Im(M S*) is averaged per band as well, giving the
// correlation, the coherence (how stable the inter-channel phase is, which tells a fixed delay or polarity flip
// from plain stereo width) and the phase itself; see PhaseCancellation.h for what is made of them.
//
// History: every frame also records the absolute energy of the stereo, mid, side, left and right signals in each
// EnergyRange (and in the whole spectrum), the level of the bass fundamental against its harmonics (Fundamental.h)
// and the peaks and clipping the audio had since the previous frame (ClipMeter.h) into a FrameHistory, for the time
// graphs. Digital-silence frames record the floor.
class TonalAnalyzer
{
public:
    static constexpr float kSilenceDb = -90.0f;      // frames below this RMS (dBFS, mean of L and R power) are digital silence / dither
    static constexpr float kRelativeGateDb = -35.0f; // frames this far below the running mean power are ignored
    static constexpr int kMinFrames = 8;             // frames averaged before a curve is reported (~1 s)

    TonalAnalyzer() = default;

    // phase (0..1) offsets the frame schedule so two analyzers never run an FFT in the same block.
    void prepare (double sampleRate, double phase = 0.0)
    {
        fs = sampleRate;
        hopPhase = std::clamp (phase, 0.0, 0.99);
        int order = 14; // 16384 -> ~2.9 Hz bins at 48 kHz
        if (fs > 50000.0)
            order = 15;
        if (fs > 100000.0)
            order = 16;
        fftSize = 1 << order;
        hop = fftSize / 4;
        fft = std::make_unique<RealFFT> (fftSize);
        window.resize (static_cast<size_t> (fftSize));
        double wsum2 = 0.0;
        for (int i = 0; i < fftSize; ++i)
        {
            const double w = 0.5 - 0.5 * std::cos (2.0 * kPi * i / fftSize);
            window[static_cast<size_t> (i)] = static_cast<float> (w);
            wsum2 += w * w;
        }
        powerNorm = static_cast<float> (1.0 / wsum2);
        frame.assign (static_cast<size_t> (fftSize), 0.0f);
        power.assign (static_cast<size_t> (fftSize / 2 + 1), 0.0f);
        ringSize = static_cast<uint64_t> (fftSize) * kRingFrames; // power of two: indices wrap with a mask
        ring.assign (static_cast<size_t> (ringSize), 0.0f);
        sideRing.assign (static_cast<size_t> (ringSize), 0.0f);
        sideFrame.assign (static_cast<size_t> (fftSize), 0.0f);
        const auto bins = static_cast<size_t> (fftSize / 2 + 1);
        for (auto* v : { &sidePower, &midRe, &midIm, &sideRe, &sideIm, &crossRe, &crossIm })
            v->assign (bins, 0.0f);
        history_.setFramePeriod (static_cast<double> (hop) / fs);
        spectrum_.setFramePeriod (static_cast<double> (hop) / fs);
        clip_.prepare();
        clipEnabled = true;
        writeCount.store (0, std::memory_order_relaxed);
        buildBandMap();
        setAveragingSeconds (kDefaultAveragingSeconds);
        reset();
    }

    // prepare() for a whole-track average (cumulative mean, no smoothing window). Clip detection is left out: it
    // would oversample every sample of the track for readouts nobody looks at.
    void prepareOffline (double sampleRate)
    {
        prepare (sampleRate);
        setAveragingSeconds (0.0f);
        clipEnabled = false;
    }

    // 0 = infinite (cumulative mean).
    void setAveragingSeconds (float s) { avgSeconds.store (s, std::memory_order_relaxed); }

    // Safe from any thread; applied on the next pump().
    void requestReset()
    {
        clip_.requestReset(); // its detector state belongs to the audio thread, which applies the reset in push()
        resetFlag.store (true, std::memory_order_release);
    }

    // Audio thread: queue n samples for analysis. Wait-free, no allocation, no FFT.
    void push (const float* l, const float* r, int n)
    {
        if (clipEnabled)
            clip_.process (l, r, n);
        const uint64_t w = writeCount.load (std::memory_order_relaxed);
        const uint64_t mask = ringSize - 1;
        for (int i = 0; i < n; ++i)
        {
            const size_t pos = static_cast<size_t> ((w + static_cast<uint64_t> (i)) & mask);
            // A glitched sample (NaN or inf) is read as silence: one of them would otherwise poison every average from here on, until Reset.
            const float xl = std::isfinite (l[i]) ? l[i] : 0.0f, xr = std::isfinite (r[i]) ? r[i] : 0.0f;
            ring[pos] = midOf (xl, xr);
            sideRing[pos] = sideOf (xl, xr);
        }
        writeCount.store (w + static_cast<uint64_t> (n), std::memory_order_release);
    }

    // Worker thread: analyse every frame that has become available since the last call. If the worker
    // falls so far behind that the ring would be overwritten, the oldest frames are skipped.
    void pump()
    {
        if (resetFlag.exchange (false, std::memory_order_acq_rel))
            reset();

        const uint64_t written = writeCount.load (std::memory_order_acquire);
        // A block still being written may overwrite up to one FFT length beyond `written`, so keep that clear.
        const uint64_t maxLag = ringSize - static_cast<uint64_t> (fftSize);
        while (nextFrameEnd <= written)
        {
            const uint64_t start = nextFrameEnd - static_cast<uint64_t> (fftSize);
            if (written - start > maxLag)
            {
                const uint64_t minEnd = written - maxLag + static_cast<uint64_t> (fftSize);
                const uint64_t hops = (minEnd - nextFrameEnd + static_cast<uint64_t> (hop) - 1) / static_cast<uint64_t> (hop);
                nextFrameEnd += hops * static_cast<uint64_t> (hop);
                continue;
            }
            analyseFrame (start);
            nextFrameEnd += static_cast<uint64_t> (hop);
        }
    }

    // push() + pump() on the calling thread.
    void process (const float* l, const float* r, int n)
    {
        for (int pos = 0; pos < n; pos += hop)
        {
            const int c = std::min (hop, n - pos);
            push (l + pos, r + pos, c);
            pump();
        }
    }

    // True once enough audio has been averaged to be meaningful (~1 s).
    bool hasData() const { return published.frames.load (std::memory_order_relaxed) >= kMinFrames; }

    // Latest long-term spectrum, normalised so the anchor region averages 0 dB.
    void readNormalisedDb (BandArray& out) const { load (published.db, out); }

    // True once enough audio has been averaged for the mono-fold curve (~1 s). Unlike hasData() this also holds
    // for a signal whose mid is silent (fully out-of-phase).
    bool hasMonoFoldData() const { return published.foldFrames.load (std::memory_order_relaxed) >= kMinFrames; }

    // Latest mono-fold curve in dB (<= 0): the share of each band's power that survives a sum to mono.
    // Bands above the Nyquist frequency read 0.
    void readMonoFoldDb (BandArray& out) const { load (published.fold, out); }

    // Per-band phase relationship between the channels (valid when hasMonoFoldData()); zeros before that.
    void readPhase (PhaseBands& out) const
    {
        load (published.correlation, out.correlation);
        load (published.coherence, out.coherence);
        load (published.phase, out.phaseRad);
        load (published.level, out.levelDb);
    }

    // Absolute energy, fundamental and clipping history, one entry per analysed frame (cleared by a reset).
    const FrameHistory& history() const { return history_; }
    const SpectrumHistory& spectrum() const { return spectrum_; }

    // Peaks and clipping of everything pushed since the last reset.
    ClipTotals clipTotals() const { return clip_.totals(); }

    // Analyse an entire buffer offline (reference-track learning). Returns false if too short/quiet.
    static bool analyseOffline (const float* l, const float* r, size_t numSamples, double sampleRate, BandArray& out)
    {
        const auto a = std::make_unique<TonalAnalyzer>(); // the history ring makes it too big for a worker thread's stack
        a->prepareOffline (sampleRate);
        constexpr int chunk = 4096;
        for (size_t pos = 0; pos < numSamples; pos += chunk)
            a->process (l + pos, r + pos, static_cast<int> (std::min<size_t> (chunk, numSamples - pos)));
        if (!a->hasData())
            return false;
        a->readNormalisedDb (out);
        return true;
    }

private:
    using PublishedBands = std::array<std::atomic<float>, BandGrid::kNum>;

    static void load (const PublishedBands& src, BandArray& out)
    {
        for (size_t i = 0; i < out.size(); ++i)
            out[i] = src[i].load (std::memory_order_relaxed);
    }

    struct BandMap
    {
        int firstBin = 0, lastBin = -1; // inclusive range of FFT bins inside the band
        int interpLo = 0;               // for bands narrower than one bin: interpolate between bins
        float interpFrac = 0.0f;
        float binsWide = 0.0f;          // true band width in bins (not the number of whole bins it happens to contain)
        bool valid = true;
    };

    void buildBandMap()
    {
        const double binHz = fs / fftSize;
        const double nyquist = fs * 0.5;
        for (int b = 0; b < BandGrid::kNum; ++b)
        {
            BandMap m;
            const double lo = BandGrid::lowEdgeHz (b), hi = BandGrid::highEdgeHz (b), c = BandGrid::centreHz (b);
            m.valid = lo < nyquist * 0.98;
            m.firstBin = static_cast<int> (std::ceil (lo / binHz));
            m.lastBin = std::min (static_cast<int> (std::ceil (hi / binHz)) - 1, fftSize / 2);
            m.binsWide = static_cast<float> ((hi - lo) / binHz);
            if (m.lastBin < m.firstBin)
            {
                const double pos = c / binHz;
                m.interpLo = static_cast<int> (std::floor (pos));
                m.interpFrac = static_cast<float> (pos - m.interpLo);
            }
            bandMap[static_cast<size_t> (b)] = m;
        }
    }

    // Energy per band: the mean power over the bins the band covers, times the band's true width in bins.
    // Returns the sum over the bands that exist at this sample rate.
    double bandEnergies (const std::vector<float>& pw, std::array<double, BandGrid::kNum>& e) const
    {
        const int half = fftSize / 2;
        double total = 0.0;
        for (int b = 0; b < BandGrid::kNum; ++b)
        {
            const auto& m = bandMap[static_cast<size_t> (b)];
            double v = 0.0;
            if (!m.valid)
                v = kPowerFloor;
            else if (m.lastBin >= m.firstBin)
            {
                double sum = 0.0;
                for (int k = m.firstBin; k <= m.lastBin; ++k)
                    sum += pw[static_cast<size_t> (k)];
                v = sum / (m.lastBin - m.firstBin + 1) * m.binsWide;
            }
            else
            {
                const double p0 = pw[static_cast<size_t> (m.interpLo)];
                const double p1 = pw[static_cast<size_t> (std::min (m.interpLo + 1, half))];
                v = (p0 + (p1 - p0) * static_cast<double> (m.interpFrac)) * static_cast<double> (m.binsWide);
            }
            e[static_cast<size_t> (b)] = v;
            if (m.valid)
                total += v;
        }
        return total;
    }

    // A frame with no signal, carrying the peaks and clips that came with its audio.
    static FrameStats silentFrame (const ClipWindow& clip)
    {
        FrameStats s;
        for (auto& signal : s.energyDb)
            signal.fill (kHistoryFloorDb);
        s.samplePeakDb = peakToDb (clip.samplePeak);
        s.truePeakDb = peakToDb (clip.truePeak);
        s.clipEvents = static_cast<float> (clip.clipEvents);
        s.clipSamples = static_cast<float> (clip.clipSamples);
        s.overSamples = static_cast<float> (clip.overSamples);
        return s;
    }

    static BandLevels silentBands()
    {
        BandLevels s;
        s.fill (kHistoryFloorDb);
        return s;
    }

    // The spectrogram's column for a frame: the stereo level (mid + side) of every band the sample rate can carry.
    BandLevels bandLevels (const std::array<double, BandGrid::kNum>& mid, const std::array<double, BandGrid::kNum>& side) const
    {
        BandLevels s = silentBands();
        const double toMeanSquare = 2.0 / fftSize;
        for (size_t b = 0; b < s.size(); ++b)
            if (bandMap[b].valid)
                s[b] = static_cast<float> (powerToDb ((mid[b] + side[b]) * toMeanSquare, 1.0e-12));
        return s;
    }

    // One history entry: the energy of the stereo, mid, side, left and right signals per range (from the band energies
    // of mid, side and their cross-spectrum), and the bass fundamental of the mid signal.
    FrameStats frameStats (const std::array<double, BandGrid::kNum>& mid, const std::array<double, BandGrid::kNum>& side,
                           const std::array<double, BandGrid::kNum>& cross, const ClipWindow& clip) const
    {
        FrameStats s = silentFrame (clip);
        const double toMeanSquare = 2.0 / fftSize; // the bin powers of a frame sum to (fftSize / 2) x its mean square
        const auto toDb = [&] (double bandPower) { return static_cast<float> (powerToDb (bandPower * toMeanSquare, 1.0e-12)); };
        for (int column = 0; column < kNumEnergyColumns; ++column)
        {
            double m = 0.0, sd = 0.0, l = 0.0, r = 0.0;
            for (size_t b = 0; b < mid.size(); ++b)
            {
                if (!bandMap[b].valid || (column != kAllRanges && !kEnergyRanges[column].contains (static_cast<int> (b))))
                    continue;
                const double both = mid[b] + side[b], x = 2.0 * cross[b]; // |M +/- S|^2 = |M|^2 + |S|^2 +/- 2 Re(M S*)
                m += mid[b];
                sd += side[b];
                l += std::max (both + x, 0.0);
                r += std::max (both - x, 0.0);
            }
            s.energy (Signal::stereo, column) = toDb (m + sd);
            s.energy (Signal::mid, column) = toDb (m);
            s.energy (Signal::side, column) = toDb (sd);
            s.energy (Signal::left, column) = toDb (l);
            s.energy (Signal::right, column) = toDb (r);
        }
        const auto f = estimateFundamental (power.data(), fftSize / 2 + 1, fs / fftSize, toMeanSquare);
        if (f.found)
        {
            s.f0Hz = static_cast<float> (f.hz);
            s.fundamentalDb = f.fundamentalDb;
            s.harmonicsDb = f.harmonicsDb;
        }
        return s;
    }

    void analyseFrame (uint64_t start)
    {
        const size_t mask = static_cast<size_t> (ringSize - 1);
        const size_t s0 = static_cast<size_t> (start & mask);
        double energy = 0.0;
        for (int i = 0; i < fftSize; ++i)
        {
            const size_t pos = (s0 + static_cast<size_t> (i)) & mask;
            const float m = ring[pos], sd = sideRing[pos];
            frame[static_cast<size_t> (i)] = m * window[static_cast<size_t> (i)];
            sideFrame[static_cast<size_t> (i)] = sd * window[static_cast<size_t> (i)];
            energy += static_cast<double> (m) * m + static_cast<double> (sd) * sd; // = (L^2 + R^2) / 2
        }
        const ClipWindow clip = clipEnabled ? clip_.take() : ClipWindow {};
        const double meanPower = energy / fftSize;
        if (powerToDb (meanPower) < kSilenceDb)
        {
            history_.push (silentFrame (clip));
            spectrum_.push (silentBands());
            return;
        }

        fft->forward (frame.data(), midRe.data(), midIm.data());
        fft->forward (sideFrame.data(), sideRe.data(), sideIm.data());
        for (size_t k = 0; k < power.size(); ++k)
        {
            const float mr = midRe[k], mi = midIm[k], sr = sideRe[k], si = sideIm[k];
            power[k] = (mr * mr + mi * mi) * powerNorm;
            sidePower[k] = (sr * sr + si * si) * powerNorm;
            crossRe[k] = (mr * sr + mi * si) * powerNorm; // Re(M S*)
            crossIm[k] = (mi * sr - mr * si) * powerNorm; // Im(M S*)
        }

        std::array<double, BandGrid::kNum> e {}, eSide {}, xRe {}, xIm {};
        const double total = bandEnergies (power, e);
        const double totalSide = bandEnergies (sidePower, eSide);
        bandEnergies (crossRe, xRe);
        bandEnergies (crossIm, xIm);
        history_.push (frameStats (e, eSide, xRe, clip));
        spectrum_.push (bandLevels (e, eSide));

        const float tau = avgSeconds.load (std::memory_order_relaxed);
        const double hopSec = static_cast<double> (hop) / fs;
        const double smoothing = emaWeight (hopSec, tau);

        // Running mean power (every non-silent frame feeds it, so it follows the programme down as well as up).
        const bool gated = refFrames > 0 && meanPower < refPower * std::pow (10.0, kRelativeGateDb / 10.0);
        ++refFrames;
        refPower += runningMeanWeight (smoothing, refFrames) * (meanPower - refPower);
        if (gated)
            return;

        const double totalBoth = total + totalSide;
        if (totalBoth < kPowerFloor)
            return;

        // Mono fold: each band's mid and side power as a share of this frame's total, averaged like the tonal curve.
        // The cross-spectrum of mid and side is averaged the same way (it carries the phase between the channels).
        ++foldCount;
        const double foldAlpha = runningMeanWeight (smoothing, foldCount);
        const double invBoth = 1.0 / totalBoth;
        for (size_t b = 0; b < e.size(); ++b)
        {
            if (!bandMap[b].valid)
                continue;
            foldMid[b] += foldAlpha * (e[b] * invBoth - foldMid[b]);
            foldSide[b] += foldAlpha * (eSide[b] * invBoth - foldSide[b]);
            foldCrossRe[b] += foldAlpha * (xRe[b] * invBoth - foldCrossRe[b]);
            foldCrossIm[b] += foldAlpha * (xIm[b] * invBoth - foldCrossIm[b]);
        }

        if (total >= kPowerFloor) // a fully out-of-phase frame has no mid and nothing to add to the tonal curve
        {
            ++frameCount;
            const double alpha = runningMeanWeight (smoothing, frameCount);
            const double invTotal = 1.0 / total;
            for (size_t b = 0; b < e.size(); ++b)
            {
                const double v = bandMap[b].valid ? e[b] * invTotal : kPowerFloor; // this frame's spectral shape, level removed
                avg[b] += alpha * (v - avg[b]);
            }
        }
        publish();
    }

    void publish()
    {
        BandArray db;
        for (size_t i = 0; i < db.size(); ++i)
            db[i] = static_cast<float> (powerToDb (avg[i]));
        normaliseAnchor (db);
        for (size_t i = 0; i < db.size(); ++i)
        {
            published.db[i].store (db[i], std::memory_order_relaxed);
            const double both = foldMid[i] + foldSide[i];
            published.fold[i].store (bandMap[i].valid && both > 0.0 ? static_cast<float> (powerToDb (foldMid[i] / both, 1.0e-12)) : 0.0f,
                                     std::memory_order_relaxed);
            publishPhase (i, both);
        }
        BandArray level;
        for (size_t i = 0; i < level.size(); ++i)
            level[i] = static_cast<float> (powerToDb (foldMid[i] + foldSide[i]));
        normaliseAnchor (level);
        for (size_t i = 0; i < level.size(); ++i)
            published.level[i].store (level[i], std::memory_order_relaxed);
        published.frames.store (frameCount, std::memory_order_relaxed);
        published.foldFrames.store (foldCount, std::memory_order_relaxed);
    }

    // The channel powers are M + S +/- 2 Re(M S*); the cross-spectrum L R* is (M - S) - 2j Im(M S*).
    void publishPhase (size_t i, double both)
    {
        float corr = 0.0f, coh = 0.0f, phase = 0.0f;
        if (bandMap[i].valid && both > 0.0)
        {
            const double re = foldMid[i] - foldSide[i], im = -2.0 * foldCrossIm[i];
            const double pl = both + 2.0 * foldCrossRe[i], pr = both - 2.0 * foldCrossRe[i];
            corr = static_cast<float> (std::clamp (re / both, -1.0, 1.0));
            if (pl > 0.0 && pr > 0.0)
                coh = static_cast<float> (std::min (1.0, std::hypot (re, im) / std::sqrt (pl * pr)));
            phase = static_cast<float> (std::atan2 (im, re));
        }
        published.correlation[i].store (corr, std::memory_order_relaxed);
        published.coherence[i].store (coh, std::memory_order_relaxed);
        published.phase[i].store (phase, std::memory_order_relaxed);
    }

    void reset()
    {
        avg.fill (0.0);
        foldMid.fill (0.0);
        foldSide.fill (0.0);
        foldCrossRe.fill (0.0);
        foldCrossIm.fill (0.0);
        history_.clear();
        spectrum_.clear();
        clip_.take(); // drop what was measured before the reset
        frameCount = 0;
        foldCount = 0;
        refFrames = 0;
        refPower = 0.0;
        // Only audio pushed from now on counts. Frames end every `hop` samples once a full frame of new data exists.
        const uint64_t base = writeCount.load (std::memory_order_acquire);
        const uint64_t offset = static_cast<uint64_t> (hop - static_cast<int> (hopPhase * hop));
        nextFrameEnd = base + offset;
        while (nextFrameEnd - base < static_cast<uint64_t> (fftSize))
            nextFrameEnd += static_cast<uint64_t> (hop);
        published.clear();
    }

    double fs = 48000.0;
    int fftSize = 16384, hop = 4096;
    static constexpr uint64_t kRingFrames = 16; // ring length in FFT frames (~5 s at 48 kHz)
    std::unique_ptr<RealFFT> fft;
    std::vector<float> window, ring, sideRing, frame, sideFrame, power, sidePower;
    std::vector<float> midRe, midIm, sideRe, sideIm, crossRe, crossIm; // complex spectra of the mid / side frames, and Re / Im of M S*
    float powerNorm = 1.0f;
    std::array<BandMap, BandGrid::kNum> bandMap {};
    std::array<double, BandGrid::kNum> avg {};
    std::array<double, BandGrid::kNum> foldMid {}, foldSide {}; // per-band mid / side power as a share of the frame total
    std::array<double, BandGrid::kNum> foldCrossRe {}, foldCrossIm {}; // per-band Re / Im of M S*, on the same scale
    FrameHistory history_;
    SpectrumHistory spectrum_;
    ClipMeter clip_;
    bool clipEnabled = true;
    uint64_t ringSize = 0;
    std::atomic<uint64_t> writeCount { 0 }; // total samples pushed since prepare(); the ring index is count & (ringSize-1)
    uint64_t nextFrameEnd = 0;              // worker-side: absolute sample index where the next frame ends
    int frameCount = 0;
    int foldCount = 0;
    int refFrames = 0;     // non-silent frames seen, for the running level
    double refPower = 0.0; // running mean frame power, the reference for the relative gate
    double hopPhase = 0.0;

    std::atomic<float> avgSeconds { kDefaultAveragingSeconds };
    std::atomic<bool> resetFlag { false };

    struct Published
    {
        static constexpr float kNoCurveDb = -100.0f; // what the tonal curve reads before any data
        PublishedBands db;
        PublishedBands fold, correlation, coherence, phase, level;
        std::atomic<int> frames { 0 };
        std::atomic<int> foldFrames { 0 };
        Published() { clear(); }
        void clear()
        {
            for (auto& v : db)
                v.store (kNoCurveDb, std::memory_order_relaxed);
            for (auto& v : level)
                v.store (kNoCurveDb, std::memory_order_relaxed);
            for (auto* bands : { &fold, &correlation, &coherence, &phase })
                for (auto& v : *bands)
                    v.store (0.0f, std::memory_order_relaxed);
            frames.store (0, std::memory_order_relaxed);
            foldFrames.store (0, std::memory_order_relaxed);
        }
    } published;
};
} // namespace themis
