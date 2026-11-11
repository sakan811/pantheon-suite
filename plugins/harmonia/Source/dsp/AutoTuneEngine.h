// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 The Pantheon authors

#pragma once

#include "Kernels.h"
#include "KeyTracker.h"
#include "PitchCorrector.h"
#include "PitchDetector.h"
#include "Rings.h"
#include "Scales.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <vector>

namespace harmonia
{
// Pitch search range per voice type. A narrower range tracks faster and cannot jump octaves.
// The limits are for a song tuned to A4 = 440 Hz and move with the Tuning knob (a G4 sung at A4 = 466 Hz
// is 415 Hz). Each top sits a whole tone above the usual top note of the voices it names (G4, F5, C6),
// so that note still tracks with a wide vibrato on it.
struct VoiceRange
{
    const char* name;
    float minHz, maxHz;
};

inline constexpr float kLowestHz = 75.0f; // the detector is built to reach down to this pitch

inline constexpr VoiceRange kVoiceRanges[] = {
    { "Auto",                  kLowestHz, 1200.0f },
    { "Low (bass / baritone)", kLowestHz,  440.0f },
    { "Mid (tenor / alto)",    100.0f,     780.0f },
    { "High (soprano)",        180.0f,    1200.0f },
};
inline constexpr int kNumVoiceRanges = (int) (sizeof (kVoiceRanges) / sizeof (kVoiceRanges[0]));

struct EngineParams
{
    CorrectorParams corrector;
    float formantSemitones = 0.0f; // 0 keeps the singer's own vocal character
    float mix = 1.0f;              // 0 = dry (delayed), 1 = fully tuned
    bool autoKey = false;          // follow the key of the vocal instead of corrector.noteMask (once it is known)
    float minHz = kVoiceRanges[0].minHz;
    float maxHz = kVoiceRanges[0].maxHz;

    // Sets the search range from a voice type, for a song tuned to `refA4`.
    void setVoiceRange (int index, float refA4 = 440.0f)
    {
        const auto& r = kVoiceRanges[std::clamp (index, 0, kNumVoiceRanges - 1)];
        const float scale = refA4 / 440.0f;
        minHz = std::max (kLowestHz, r.minHz * scale);
        maxHz = r.maxHz * scale;
    }
};

// Real-time pitch corrector for monophonic voice.
//
//   detect (YIN)  ->  decide target note + smooth correction  ->  PSOLA re-synthesis
//
// PSOLA cuts the voice into pitch-synchronous grains and re-spaces them. Each grain keeps
// its original spectral envelope, so the formants (the "voice colour") stay put while the
// pitch moves, which is what keeps the result natural.
//
// The engine runs a fixed look-ahead (latencySamples()) so the pitch track is always
// known slightly before the audio is re-synthesised.
class AutoTuneEngine
{
public:
    static constexpr int kMaxChannels = 2;

    struct HopInfo
    {
        float inMidi = 0.0f;
        float outMidi = 0.0f;
        float targetMidi = 0.0f;
        float voiced = 0.0f;
        float confidence = 0.0f; // detector certainty (0..1) for this hop
    };
    static constexpr int kHistory = 1024;

    void prepare (double sampleRate, int numChannels)
    {
        fs = sampleRate;
        channels = std::clamp (numChannels, 1, kMaxChannels);
        hop = std::max (32, (int) std::lround (fs / 250.0));
        latency = (int) std::ceil (0.046 * fs);
        unvoicedPeriod = std::max (32, (int) std::lround (0.006 * fs));

        detector.prepare (fs, kLowestHz);
        corrector.prepare ((double) hop / fs);
        keyTracker.prepare (fs / (double) hop); // not part of reset(): the key survives the host jumping around

        // Synthesis reads the input at most `latency` + two grains back; keep generous headroom.
        const int maxLag = detector.maxLagSamples();
        historySize = 2 * latency + 4 * maxLag + 2 * hop;
        detector.reserveHistory (historySize); // the detector's full-rate ring doubles as the mono history
        for (auto& h : hist)
            h.resize (historySize);

        int os = 1;
        while (os < latency * 3 + 4 * maxLag)
            os <<= 1;
        outMask = os - 1;
        for (auto& a : acc)
            a.assign ((size_t) os, 0.0f);
        wsum.assign ((size_t) os, 0.0f);

        scores.assign ((size_t) (2 * std::max (2, (int) (0.2f * (float) maxLag)) + 1), 0.0f);
        mixCoef = 1.0f - std::exp (-1.0f / (0.01f * (float) fs));

        reset();
    }

    void reset()
    {
        for (auto& h : hist)
            h.clear();
        for (auto& a : acc)
            std::fill (a.begin(), a.end(), 0.0f);
        std::fill (wsum.begin(), wsum.end(), 0.0f);
        detector.reset();
        corrector.reset();

        sampleCount = 0;
        emitted = 0;
        hopCounter = 0;
        track.clear();
        marks.clear();
        synthStarted = false;
        ts = 0.0;
        voicedRun = unvoicedRun = 0;
        inVoiced = false;
        haveReference = false;
        outlierRun = sinceReference = 0;
        lastPeriod = 0.0f;
        mixSmoothed = params.mix;
        starved.store (0, std::memory_order_relaxed);
        histWrite.store (0);

        runHop (0); // pre-roll: finalises the first hop of output
    }

    int latencySamples() const { return latency; }
    int hopSamples() const { return hop; }

    void setParams (const EngineParams& p) { params = p; }

    // What the corrector is actually snapping to right now (differs from the parameters under autoKey),
    // and the key being followed. Safe to read from the UI thread.
    uint16_t activeNoteMask() const { return (uint16_t) (liveState.load (std::memory_order_relaxed) & 0xFFFFu); }
    KeyTracker::Result trackedKey() const
    {
        const uint32_t v = liveState.load (std::memory_order_relaxed);
        return { (v & 0x10000u) != 0, (int) ((v >> 17) & 15u), (v & 0x200000u) != 0, 0.0f };
    }

    // Key learning: collect the sung pitch classes, then ask for the best matching key.
    void startLearning()
    {
        chroma.fill (0.0f);
        learning = true;
    }
    KeyGuess stopLearning()
    {
        learning = false;
        return guessKey (chroma.data());
    }
    bool isLearning() const { return learning; }
    float learnedAmount() const
    {
        float s = 0.0f;
        for (float c : chroma)
            s += c;
        return s;
    }

    // Diagnostics (read from the UI thread).
    int starvedEvents() const { return starved.load (std::memory_order_relaxed); }
    int historyWriteIndex() const { return histWrite.load (std::memory_order_acquire); }
    HopInfo historyAt (int absoluteIndex) const { return history[(size_t) (absoluteIndex & (kHistory - 1))]; }
    double hopRateHz() const { return fs / (double) hop; }

    void process (float* const* io, int numChannels, int numSamples)
    {
        const int nch = std::min (numChannels, channels);
        const float mixTarget = params.mix;

        for (int i = 0; i < numSamples; ++i)
        {
            float mono = 0.0f;
            for (int c = 0; c < nch; ++c)
            {
                // An inf or NaN is taken as silence: one of them in the detector's filters or the correction's smoothing stays there
                // for good, and the voice then never comes back in tune (dsp_test, section 9).
                const float v = std::isfinite (io[c][i]) ? io[c][i] : 0.0f;
                hist[(size_t) c].push (v);
                mono += v;
            }
            detector.push (mono / (float) nch);
            ++sampleCount;

            if (++hopCounter == hop)
            {
                hopCounter = 0;
                runHop (sampleCount);
            }

            mixSmoothed += mixCoef * (mixTarget - mixSmoothed);

            const size_t slot = (size_t) (emitted & outMask);
            const float w = wsum[slot];
            const float norm = 1.0f / std::max (w, 0.3f);
            const float dryAmt = std::max (0.0f, 1.0f - w / 0.3f);

            for (int c = 0; c < nch; ++c)
            {
                const float dry = hist[(size_t) c].tail (latency + 1)[0]; // the input, `latency` samples ago
                const float wet = acc[(size_t) c][slot] * norm + dry * dryAmt;
                io[c][i] = dry + (wet - dry) * mixSmoothed;
                acc[(size_t) c][slot] = 0.0f;
            }
            wsum[slot] = 0.0f;
            ++emitted;
        }
    }

private:
    struct TrackEntry
    {
        double t = 0.0;
        float period = 0.0f;
        float cents = 0.0f;
        bool voiced = false;
    };
    struct TrackSample
    {
        float period = 0.0f;
        float cents = 0.0f;
        bool voiced = false;
    };
    struct Mark
    {
        double pos = 0.0; // sub-sample accurate
        float period = 0.0f;
        bool voiced = false;
    };

    static constexpr int kTrackCap = 512;
    static constexpr int kMarkCap = 4096;

    // Pointer to input sample `pos`; everything up to the newest sample follows contiguously.
    const float* monoAt (int64_t pos) const
    {
        const int len = (int) std::clamp<int64_t> (sampleCount - pos, 1, detector.fullRateHistory().capacity());
        return detector.fullRateHistory().tail (len);
    }

    TrackSample queryTrack (double t) const
    {
        TrackSample out;
        if (track.empty())
            return out;

        int i = track.size() - 1;
        while (i > 0 && track[i].t > t)
            --i;

        const TrackEntry& a = track[i];
        if (i == track.size() - 1 || a.t >= t)
            return { a.voiced ? a.period : 0.0f, a.cents, a.voiced };

        const TrackEntry& b = track[i + 1];
        const float f = (float) ((t - a.t) / std::max (1e-9, b.t - a.t));
        if (a.voiced && b.voiced)
            return { a.period + f * (b.period - a.period), a.cents + f * (b.cents - a.cents), true };

        const TrackEntry& n = f < 0.5f ? a : b;
        return { n.voiced ? n.period : 0.0f, n.cents, n.voiced };
    }

    // A detection far from where the voice has been a moment ago (more than an octave) is almost
    // always a glitch (a sub-harmonic lock on noise or a creaky onset), not a leap: such a pitch
    // would pull the correction off and re-space grains at the wrong period. It is treated as
    // unvoiced, so that stretch of audio passes through untouched. A voice that really has moved
    // is accepted once the new pitch has persisted for a quarter of a second.
    void rejectOutlier (PitchDetector::Result& det)
    {
        constexpr float kMaxLeapCents = 1300.0f;
        constexpr int kPersistHops = 60;

        ++sinceReference;
        if (sinceReference > 250) // a second without a pitch: nothing to compare with
            haveReference = false;
        if (! det.voiced)
            return;

        const float c = 1200.0f * std::log2 ((float) fs / det.period);
        if (haveReference && std::fabs (c - referenceCents) > kMaxLeapCents && outlierRun < kPersistHops)
        {
            ++outlierRun;
            det.voiced = false;
            return;
        }
        outlierRun = 0;
        sinceReference = 0;
        referenceCents = haveReference ? referenceCents + 0.02f * (c - referenceCents) : c;
        haveReference = true;
    }

    // ---- per-hop processing ----
    void runHop (int64_t tIn)
    {
        const float prev = inVoiced ? lastPeriod : 0.0f;
        auto det = detector.detect (params.minHz, params.maxHz, prev, inVoiced);
        rejectOutlier (det);

        if (det.voiced)
        {
            ++voicedRun;
            unvoicedRun = 0;
            lastPeriod = det.period;
        }
        else
        {
            ++unvoicedRun;
            voicedRun = 0;
        }
        if (! inVoiced && voicedRun >= 2)
            inVoiced = true;
        else if (inVoiced && unvoicedRun > 3)
            inVoiced = false;

        const float period = inVoiced ? (det.voiced ? det.period : lastPeriod) : 0.0f;
        const float f0 = period > 0.0f ? (float) fs / period : 0.0f;

        CorrectorParams cp = params.corrector;
        KeyTracker::Result followed;
        if (params.autoKey)
        {
            followed = keyTracker.key();
            if (followed.valid)
                cp.noteMask = scaleMask (followed.root, followed.minor ? kMinorScale : kMajorScale);
        }
        liveState.store (cp.noteMask | (followed.valid ? 0x10000u : 0u) | ((uint32_t) followed.root << 17) | (followed.minor ? 0x200000u : 0u),
                         std::memory_order_relaxed);

        const auto out = corrector.process (inVoiced, f0, cp);

        // The estimate describes the middle of the analysed span: (window + period) / 2 ago.
        const double centerDelay = 0.5 * ((double) detector.windowSamples() + (period > 0.0f ? (double) period : fs / 200.0));

        TrackEntry e;
        e.t = std::max ((double) tIn - centerDelay, track.empty() ? -1e9 : track.back().t + 1.0);
        e.period = period;
        e.cents = out.active ? std::clamp (out.correctionCents, -600.0f, 600.0f) : 0.0f;
        e.voiced = inVoiced && out.active;
        track.push (e);

        // UI history
        const int w = histWrite.load (std::memory_order_relaxed);
        history[(size_t) (w & (kHistory - 1))] = { out.inMidi, out.outMidi, out.targetMidi, e.voiced ? 1.0f : 0.0f, det.voiced ? det.confidence : 0.0f };
        histWrite.store (w + 1, std::memory_order_release);

        if (params.autoKey && e.voiced && det.voiced)
            keyTracker.push (corrector.centreCents() / 100.0f, det.confidence, corrector.centreSpeedCentsPerSec());

        if (learning && e.voiced)
            chroma[(size_t) pitchClass ((int) std::lround (out.inMidi))] += std::max (0.0f, det.confidence - 0.5f) * 2.0f;

        generateMarks (tIn);
        synthesize (tIn);
    }

    // ---- analysis marks ----
    double alignByPeak (int64_t pred, float T) const
    {
        const int half = std::max (2, (int) (T * 0.5f));
        const float* x = monoAt (pred - half - 1); // x[i + 1] is the sample at p = pred - half + i
        float best = -1.0f;
        int bestI = half;
        for (int i = 0; i <= 2 * half; ++i)
        {
            const float s = 0.25f * x[i] + 0.5f * x[i + 1] + 0.25f * x[i + 2];
            const float v = s * s;
            if (v > best)
            {
                best = v;
                bestI = i;
            }
        }
        return (double) (pred - half + bestI);
    }

    // Finds where the waveform around the previous mark reappears near `pred`, to a
    // fraction of a sample (parabolic peak of the normalised correlation).
    double alignByCorrelation (double lastPos, int64_t pred, float T)
    {
        const int half = std::max (2, (int) (T * 0.5f));
        const int search = std::max (2, (int) (T * 0.2f));
        const int len = 2 * half;
        const int count = 2 * search + 1;
        const int64_t base = (int64_t) std::llround (lastPos);

        const float* ref = monoAt (base - half);                // the waveform around the previous mark
        const float* win = monoAt (pred - search - half);       // candidate i is win[i .. i + len)
        double energy = (double) kernels::sumSquares (win, len) + 1e-9;

        int bestIdx = 0;
        for (int i = 0; i < count; ++i)
        {
            scores[(size_t) i] = kernels::dot (ref, win + i, len) / (float) std::sqrt (energy);
            if (scores[(size_t) i] > scores[(size_t) bestIdx])
                bestIdx = i;
            if (i + 1 < count) // slide the window energy instead of recomputing it
                energy = std::max (1e-9, energy + (double) win[i + len] * win[i + len] - (double) win[i] * win[i]);
        }

        double frac = 0.0;
        if (bestIdx > 0 && bestIdx < count - 1)
        {
            const float c0 = scores[(size_t) bestIdx - 1], c1 = scores[(size_t) bestIdx], c2 = scores[(size_t) bestIdx + 1];
            const float den = c0 - 2.0f * c1 + c2;
            if (den < -1e-12f)
                frac = std::clamp (0.5f * (c0 - c2) / den, -1.0f, 1.0f);
        }
        const double cStar = (double) (pred - search + bestIdx) + frac;
        return lastPos + (cStar - (double) base);
    }

    void generateMarks (int64_t tIn)
    {
        if (track.empty())
            return;
        const double limit = track.back().t;

        if (marks.empty())
        {
            const double p = std::floor (limit);
            if (p < 0.0)
                return;
            marks.push ({ p, (float) unvoicedPeriod, false });
        }

        for (int guard = 0; guard < 64; ++guard)
        {
            const Mark last = marks.back();
            const TrackSample atLast = queryTrack (last.pos);
            const float stepP = atLast.voiced ? atLast.period : (float) unvoicedPeriod;
            const int64_t pred = (int64_t) std::llround (last.pos + (double) stepP);

            if ((double) pred > limit)
                break;

            const TrackSample at = queryTrack ((double) pred);
            if (at.voiced)
            {
                const float T = at.period;
                if (pred + (int64_t) (0.75f * T) + 3 > tIn)
                    break;

                double pos = last.voiced ? alignByCorrelation (last.pos, pred, T) : alignByPeak (pred, T);
                pos = std::max (pos, last.pos + std::max (2.0, 0.5 * (double) T));
                marks.push ({ pos, T, true });
            }
            else
            {
                marks.push ({ (double) pred, (float) unvoicedPeriod, false });
            }
        }
    }

    // ---- synthesis ----
    // Four-point cubic (Catmull-Rom) interpolation weights for a fractional position.
    struct CubicTaps
    {
        int64_t first; // the taps are the samples first .. first + 3
        float w[4];
    };

    static CubicTaps cubicTaps (double pos)
    {
        const int64_t i = (int64_t) std::floor (pos);
        const float f = (float) (pos - (double) i);
        return { i - 1,
                 { ((-0.5f * f + 1.0f) * f - 0.5f) * f,
                   ((1.5f * f - 2.5f) * f) * f + 1.0f,
                   ((-1.5f * f + 2.0f) * f + 0.5f) * f,
                   (0.5f * f - 0.5f) * f * f } };
    }

    // The input samples a tap set reads, or nullptr if they are not (or no longer) available.
    const float* tapSource (const MirrorRing& ring, const CubicTaps& taps) const
    {
        const int64_t len = sampleCount - taps.first;
        return len >= 4 && len <= ring.capacity() ? ring.tail ((int) len) : nullptr;
    }

    void placeGrain (double markPos, double center, double halfLen, double formantRatio)
    {
        const int64_t n0 = (int64_t) std::floor (center);
        const double frac = center - (double) n0;
        const int jLo = (int) std::max<int64_t> ((int64_t) std::ceil (frac - halfLen), emitted - n0);
        const int jHi = (int) std::floor (frac + halfLen);

        // Hann window by recurrence, cos (a + d) = 2 cos (d) cos (a) - cos (a - d): no libm call per sample.
        const double d = 3.14159265358979323846 / halfLen;
        const double k = 2.0 * std::cos (d);
        const double a0 = ((double) jLo - frac) * d;
        double cPrev = std::cos (a0 - d), cCur = std::cos (a0);

        for (int j = jLo; j <= jHi; ++j)
        {
            const float w = 0.5f * (1.0f + (float) cCur);
            const double cNext = k * cCur - cPrev;
            cPrev = cCur;
            cCur = cNext;

            const size_t slot = (size_t) ((n0 + j) & outMask);
            const auto taps = cubicTaps (markPos + ((double) j - frac) * formantRatio);
            for (int c = 0; c < channels; ++c)
                if (const float* x = tapSource (hist[(size_t) c], taps))
                    acc[(size_t) c][slot] += w * (taps.w[0] * x[0] + taps.w[1] * x[1] + taps.w[2] * x[2] + taps.w[3] * x[3]);
            wsum[slot] += w;
        }
    }

    void synthesize (int64_t tIn)
    {
        if (marks.empty())
            return;

        const int64_t finalizeUpTo = tIn + hop; // outputs before this must be complete
        const double formantRatio = std::pow (2.0, (double) params.formantSemitones / 12.0);

        if (! synthStarted)
        {
            ts = marks[0].pos + (double) latency;
            synthStarted = true;
        }

        for (int guard = 0; guard < 256; ++guard)
        {
            const double src = ts - (double) latency;
            if (marks.back().pos <= src)
            {
                if (ts - 0.5 * unvoicedPeriod < (double) finalizeUpTo)
                    starved.fetch_add (1, std::memory_order_relaxed);
                break;
            }

            // nearest analysis mark to the source time
            int j = marks.size() - 1;
            while (j > 0 && marks[j].pos > src)
                --j;
            const Mark& m0 = marks[j];
            const Mark& m1 = marks[std::min (j + 1, marks.size() - 1)];
            const Mark& mk = (src - m0.pos <= m1.pos - src) ? m0 : m1;

            double Ts, Hw;
            if (mk.voiced)
            {
                const TrackSample q = queryTrack (src);
                const float T = q.voiced ? q.period : mk.period;
                const float cents = q.voiced ? q.cents : 0.0f;
                const double r = std::pow (2.0, (double) cents / 1200.0);
                Ts = (double) T / r;
                Hw = (double) mk.period;
            }
            else
            {
                Ts = Hw = (double) mk.period;
            }

            if (ts - Hw >= (double) finalizeUpTo)
                break;

            placeGrain (mk.pos, ts, Hw, mk.voiced ? formantRatio : 1.0);
            ts += Ts;
        }
    }

    // ---- state ----
    double fs = 48000.0;
    int channels = 2, hop = 192, latency = 2208, unvoicedPeriod = 288, historySize = 0;
    float mixCoef = 0.002f;
    EngineParams params;

    PitchDetector detector;
    PitchCorrector corrector;

    std::array<MirrorRing, kMaxChannels> hist; // input history per channel
    std::array<std::vector<float>, kMaxChannels> acc; // overlap-add output, indexed by time & outMask
    std::vector<float> wsum, scores;
    int64_t outMask = 0;

    int64_t sampleCount = 0, emitted = 0;
    int hopCounter = 0;

    HistoryRing<TrackEntry, kTrackCap> track;
    HistoryRing<Mark, kMarkCap> marks;

    bool synthStarted = false;
    double ts = 0.0;

    int voicedRun = 0, unvoicedRun = 0;
    bool inVoiced = false;
    bool haveReference = false;
    float referenceCents = 0.0f;
    int outlierRun = 0, sinceReference = 0;
    float lastPeriod = 0.0f;
    float mixSmoothed = 1.0f;
    std::atomic<int> starved { 0 };

    bool learning = false;
    std::array<float, 12> chroma {};
    KeyTracker keyTracker;
    std::atomic<uint32_t> liveState { 0 }; // active note mask | followed key (valid, root, minor)

    std::array<HopInfo, kHistory> history {};
    std::atomic<int> histWrite { 0 };
};
} // namespace harmonia
