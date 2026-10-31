// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 The Pantheon authors

// Self-contained tests for the Themis DSP core (no framework dependency).
#include <themis/Audition.h>
#include <themis/Compressor.h>
#include <themis/Engine.h>
#include <themis/Biquad.h>
#include <themis/ClipMeter.h>
#include <themis/EqFit.h>
#include <themis/FrameHistory.h>
#include <themis/Fundamental.h>
#include <themis/HistoryChecks.h>
#include <themis/Limiter.h>
#include <themis/LowCrestMeter.h>
#include <themis/Loudness.h>
#include <themis/LoudnessMonitor.h>
#include <themis/MasterAdvisor.h>
#include <themis/PhaseCancellation.h>
#include <themis/StereoScope.h>
#include <themis/StereoTools.h>
#include <themis/TargetCurve.h>
#include <themis/TonalAnalyzer.h>
#include <themis/Translation.h>

#include <chrono>
#include <functional>
#include <cstdio>
#include <random>

using namespace themis;

static int g_failures = 0, g_checks = 0;

#define CHECK(cond)                                                                       \
    do                                                                                    \
    {                                                                                     \
        ++g_checks;                                                                       \
        if (!(cond))                                                                      \
        {                                                                                 \
            ++g_failures;                                                                 \
            std::printf ("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);                \
        }                                                                                 \
    } while (0)

#define CHECK_NEAR(a, b, tol)                                                                              \
    do                                                                                                     \
    {                                                                                                      \
        ++g_checks;                                                                                        \
        const double va = (a), vb = (b), vt = (tol);                                                       \
        if (!(std::abs (va - vb) <= vt))                                                                   \
        {                                                                                                  \
            ++g_failures;                                                                                  \
            std::printf ("  FAIL %s:%d  %s = %.5f, expected %.5f +/- %.5f\n", __FILE__, __LINE__, #a, va, vb, vt); \
        }                                                                                                  \
    } while (0)

#define TEST(name)                          \
    static void name();                     \
    struct Reg_##name                       \
    {                                       \
        Reg_##name() { tests().push_back ({ #name, name }); } \
    } reg_##name;                           \
    static void name()

struct TestCase { const char* name; void (*fn)(); };
static std::vector<TestCase>& tests() { static std::vector<TestCase> t; return t; }

// ---- helpers ---------------------------------------------------------------------------------
static std::vector<float> sine (double fs, double hz, double ampDb, double seconds, double phase = 0.0)
{
    std::vector<float> v (static_cast<size_t> (fs * seconds));
    const double a = std::pow (10.0, ampDb / 20.0);
    for (size_t i = 0; i < v.size(); ++i)
        v[i] = static_cast<float> (a * std::sin (2.0 * kPi * hz * static_cast<double> (i) / fs + phase));
    return v;
}

static double rmsDb (const std::vector<float>& v, size_t from, size_t to)
{
    double s = 0;
    for (size_t i = from; i < to; ++i)
        s += static_cast<double> (v[i]) * v[i];
    return 10.0 * std::log10 (s / static_cast<double> (to - from) + 1e-20);
}

static std::vector<float> whiteNoise (size_t n, unsigned seed, float amp = 0.25f)
{
    std::mt19937 rng (seed);
    std::uniform_real_distribution<float> d (-1.0f, 1.0f);
    std::vector<float> v (n);
    for (auto& x : v)
        x = d (rng) * amp;
    return v;
}

// Paul Kellet's refined pink filter (+/-0.05 dB from 10 Hz to Nyquist).
static std::vector<float> pinkNoise (size_t n, unsigned seed, float amp = 0.25f)
{
    auto w = whiteNoise (n, seed, 1.0f);
    double b0 = 0, b1 = 0, b2 = 0, b3 = 0, b4 = 0, b5 = 0, b6 = 0;
    for (auto& x : w)
    {
        const double white = x;
        b0 = 0.99886 * b0 + white * 0.0555179;
        b1 = 0.99332 * b1 + white * 0.0750759;
        b2 = 0.96900 * b2 + white * 0.1538520;
        b3 = 0.86650 * b3 + white * 0.3104856;
        b4 = 0.55000 * b4 + white * 0.5329522;
        b5 = -0.7616 * b5 - white * 0.0168980;
        const double pink = b0 + b1 + b2 + b3 + b4 + b5 + b6 + white * 0.5362;
        b6 = white * 0.115926;
        x = static_cast<float> (pink * 0.11 * amp);
    }
    return w;
}

// Brute-force 8x oversampled true peak using a long windowed sinc (independent of TruePeakDetector).
static float referenceTruePeak (const std::vector<float>& x)
{
    constexpr int up = 8, halfLen = 32;
    float peak = 0;
    for (size_t i = static_cast<size_t> (halfLen); i + static_cast<size_t> (halfLen) < x.size(); ++i)
        for (int p = 0; p < up; ++p)
        {
            double acc = 0;
            const double frac = static_cast<double> (p) / up;
            for (int k = -halfLen + 1; k <= halfLen; ++k)
            {
                // signal value at time i + frac, interpolated from neighbours x[i + k]
                const double t = frac - k;
                const double sinc = std::abs (t) < 1e-9 ? 1.0 : std::sin (kPi * t) / (kPi * t);
                const double win = 0.5 + 0.5 * std::cos (kPi * t / (halfLen + 1));
                acc += x[static_cast<size_t> (static_cast<long> (i) + k)] * sinc * win;
            }
            peak = std::max (peak, static_cast<float> (std::abs (acc)));
        }
    return peak;
}

// ---- tests -----------------------------------------------------------------------------------
TEST (biquad_responses)
{
    const double fs = 48000;
    CHECK_NEAR (BiquadCoeffs::peak (fs, 1000, 6.0, 1.0).magnitudeDb (fs, 1000), 6.0, 0.01);
    CHECK_NEAR (BiquadCoeffs::peak (fs, 1000, -4.0, 2.0).magnitudeDb (fs, 20), 0.0, 0.05);
    CHECK_NEAR (BiquadCoeffs::lowShelf (fs, 200, 5.0, 0.707).magnitudeDb (fs, 20), 5.0, 0.15);
    CHECK_NEAR (BiquadCoeffs::highShelf (fs, 8000, -3.0, 0.707).magnitudeDb (fs, 20000), -3.0, 0.3);
    CHECK_NEAR (BiquadCoeffs::highPass (fs, 100, 0.7071).magnitudeDb (fs, 100), -3.01, 0.05);

    // Measured steady-state gain through the actual filter matches the analytic response.
    Biquad f;
    f.setCoeffs (BiquadCoeffs::peak (fs, 1000, 6.0, 1.0));
    auto s = sine (fs, 1000, -20, 1.0);
    for (auto& x : s)
        x = f.process (x);
    CHECK_NEAR (rmsDb (s, 24000, 48000) - rmsDb (sine (fs, 1000, -20, 1.0), 24000, 48000), 6.0, 0.05);
}

TEST (k_weighting_matches_itu_table_at_48k)
{
    const auto kw = makeKWeighting (48000.0);
    CHECK_NEAR (kw.first.b0, 1.53512485958697, 1e-6);
    CHECK_NEAR (kw.first.b1, -2.69169618940638, 1e-6);
    CHECK_NEAR (kw.first.b2, 1.19839281085285, 1e-6);
    CHECK_NEAR (kw.first.a1, -1.69065929318241, 1e-6);
    CHECK_NEAR (kw.first.a2, 0.73248077421585, 1e-6);
    CHECK_NEAR (kw.second.a1, -1.99004745483398, 1e-6);
    CHECK_NEAR (kw.second.a2, 0.99007225036621, 1e-6);
}

TEST (loudness_of_reference_sine)
{
    // BS.1770: a stereo 997 Hz sine at -20 dBFS peak in both channels reads -20.0 LUFS.
    for (double fs : { 44100.0, 48000.0, 96000.0 })
    {
        LoudnessMeter lm;
        lm.prepare (fs);
        auto s = sine (fs, 997, -20, 10.0);
        lm.process (s.data(), s.data(), static_cast<int> (s.size()));
        CHECK_NEAR (lm.momentary(), -20.0, 0.1);
        CHECK_NEAR (lm.shortTerm(), -20.0, 0.1);
        CHECK_NEAR (lm.integrated(), -20.0, 0.1);
    }
}

TEST (loudness_gating_ignores_quiet_passages)
{
    const double fs = 48000;
    LoudnessMeter lm;
    lm.prepare (fs);
    auto loud = sine (fs, 997, -20, 6.0);
    auto quiet = sine (fs, 997, -60, 6.0); // > -70 abs gate, but 40 LU under: removed by relative gate
    lm.process (loud.data(), loud.data(), static_cast<int> (loud.size()));
    lm.process (quiet.data(), quiet.data(), static_cast<int> (quiet.size()));
    CHECK_NEAR (lm.integrated(), -20.0, 0.15);
    CHECK_NEAR (lm.momentary(), -60.0, 0.2);
}

TEST (true_peak_finds_intersample_peaks)
{
    // fs/4 sine at 45 degrees: every sample is +/-0.7071 but the waveform peaks at 1.0.
    auto s = sine (48000, 12000, 0.0, 0.1, kPi / 4.0);
    float samplePeak = 0, tp = 0;
    TruePeakDetector d;
    for (float x : s)
    {
        samplePeak = std::max (samplePeak, std::abs (x));
        tp = std::max (tp, d.process (x));
    }
    CHECK_NEAR (samplePeak, 0.7071, 0.001);
    CHECK_NEAR (tp, 1.0, 0.03);
}

TEST (limiter_respects_ceiling_and_is_transparent_below_it)
{
    const double fs = 48000;
    const size_t n = 96000;

    // Hot programme: noise band-limited to 16 kHz (like real music) plus a tone, ~10 dB over the ceiling.
    auto bandLimit = [] (std::vector<float> x, double fc) {
        const int L = 255;
        std::vector<double> h (L);
        for (int k = 0; k < L; ++k)
        {
            const double t = k - (L - 1) / 2.0, f = fc / 24000.0;
            const double s = std::abs (t) < 1e-9 ? f : std::sin (kPi * f * t) / (kPi * t);
            h[static_cast<size_t> (k)] = s * (0.42 - 0.5 * std::cos (2 * kPi * k / (L - 1)) + 0.08 * std::cos (4 * kPi * k / (L - 1)));
        }
        std::vector<float> y (x.size(), 0.0f);
        for (size_t i = L; i < x.size(); ++i)
        {
            double a = 0;
            for (int k = 0; k < L; ++k)
                a += h[static_cast<size_t> (k)] * x[i - static_cast<size_t> (k)];
            y[i] = static_cast<float> (a);
        }
        return y;
    };
    auto tone = sine (fs, 3100, -2.0, 2.0);
    auto nl = bandLimit (whiteNoise (n, 7, 1.0f), 16000), nr = bandLimit (whiteNoise (n, 8, 1.0f), 16000);
    std::vector<float> l (n), r (n);
    for (size_t i = 0; i < n; ++i)
    {
        l[i] = nl[i] * 4.0f + tone[i];
        r[i] = nr[i] * 4.0f + tone[i];
    }
    Limiter lim;
    lim.prepare (fs);
    LimiterParams p;
    p.ceilingDb = -1.0f;
    p.releaseMs = 80.0f;
    lim.process (l.data(), r.data(), static_cast<int> (n), p, true);

    float sp = 0;
    for (size_t i = 0; i < n; ++i)
        sp = std::max ({ sp, std::abs (l[i]), std::abs (r[i]) });
    CHECK (sp <= dbToLin (-1.0f) * 1.001f);
    const float tpDb = linToDb (std::max (referenceTruePeak (l), referenceTruePeak (r)));
    std::printf ("  band-limited program: sample peak %.2f dB, true peak %.2f dB (ceiling -1.0)\n", linToDb (sp), tpDb);
    CHECK (tpDb <= -1.0f + 0.1f);
    CHECK (lim.gainReductionDb() < -3.0f);

    // Worst case: full-band white noise. Any 4x interpolator under-reads near Nyquist; stay within 1.5 dB.
    {
        auto wl = whiteNoise (n, 17, 1.0f), wr = whiteNoise (n, 18, 1.0f);
        for (size_t i = 0; i < n; ++i)
        {
            wl[i] *= 3.0f;
            wr[i] *= 3.0f;
        }
        Limiter lw;
        lw.prepare (fs);
        lw.process (wl.data(), wr.data(), static_cast<int> (n), p, true);
        const float wtp = linToDb (std::max (referenceTruePeak (wl), referenceTruePeak (wr)));
        std::printf ("  full-band noise: true peak %.2f dB (ceiling -1.0)\n", wtp);
        CHECK (wtp <= -1.0f + 1.5f);
    }

    // Below the ceiling the limiter is a pure delay.
    auto quiet = sine (fs, 440, -12, 1.0);
    std::vector<float> ql = quiet, qr = quiet;
    Limiter lim2;
    lim2.prepare (fs);
    lim2.process (ql.data(), qr.data(), static_cast<int> (quiet.size()), p, true);
    const int D = lim2.latencySamples();
    double maxErr = 0;
    for (size_t i = static_cast<size_t> (D); i < quiet.size(); ++i)
        maxErr = std::max (maxErr, static_cast<double> (std::abs (ql[i] - quiet[i - static_cast<size_t> (D)])));
    CHECK (maxErr < 1e-6);
}

TEST (compressor_static_curve)
{
    const double fs = 48000;
    Compressor c;
    c.prepare (fs);
    CompParams p;
    p.thresholdDb = -20;
    p.ratio = 4;
    p.attackMs = 1;
    p.releaseMs = 50;
    p.sidechainHpHz = 20;
    auto s = sine (fs, 1000, -10, 2.0);
    auto copy = s;
    c.process (s.data(), copy.data(), static_cast<int> (s.size()), p);
    // 10 dB over threshold at 4:1 => 7.5 dB reduction
    CHECK_NEAR (rmsDb (s, 72000, 96000) - rmsDb (sine (fs, 1000, -10, 2.0), 72000, 96000), -7.5, 0.4);
    CHECK_NEAR (c.gainReductionDb(), -7.5, 0.6);

    // Dry/wet mix = 0 is transparent
    p.mix = 0.0f;
    auto t = sine (fs, 1000, -10, 0.5), t2 = t;
    c.process (t.data(), t2.data(), static_cast<int> (t.size()), p);
    CHECK_NEAR (rmsDb (t, 0, t.size()), rmsDb (sine (fs, 1000, -10, 0.5), 0, t.size()), 0.001);
}

TEST (eq_mid_side_targeting)
{
    const double fs = 48000;
    auto run = [&] (EqTarget target, bool monoInput) {
        MasteringEq eq;
        eq.prepare (fs);
        std::array<EqBandParams, MasteringEq::kNumBands> b {};
        for (auto& x : b)
            x.enabled = false;
        b[3].enabled = true;
        b[3].freq = 1000;
        b[3].gainDb = 6;
        b[3].q = 1;
        b[3].target = target;
        eq.setBands (b);
        eq.reset();
        auto tone = sine (fs, 1000, -20, 1.0);
        std::vector<float> l = tone, r;
        if (monoInput)
            r = tone;
        else
        {
            r = tone;
            for (auto& x : r)
                x = -x; // pure side
        }
        eq.process (l.data(), r.data(), static_cast<int> (l.size()));
        return rmsDb (l, 24000, 48000) - rmsDb (tone, 24000, 48000);
    };
    CHECK_NEAR (run (EqTarget::Stereo, true), 6.0, 0.1);
    CHECK_NEAR (run (EqTarget::Mid, true), 6.0, 0.1);   // mono content is all mid
    CHECK_NEAR (run (EqTarget::Side, true), 0.0, 0.01); // ...and untouched by a side band
    CHECK_NEAR (run (EqTarget::Side, false), 6.0, 0.1);
    CHECK_NEAR (run (EqTarget::Mid, false), 0.0, 0.01);
}

TEST (stereo_tools)
{
    const double fs = 48000;
    StereoTools st;
    st.prepare (fs);
    auto tone = sine (fs, 5000, -12, 0.5);
    std::vector<float> l = tone, r = tone;
    for (auto& x : r)
        x *= -0.5f;
    auto l0 = l, r0 = r;

    st.process (l.data(), r.data(), static_cast<int> (l.size()), 100.0f, 0.0f);
    CHECK (l == l0 && r == r0); // unity width, no mono-below: untouched

    st.process (l.data(), r.data(), static_cast<int> (l.size()), 0.0f, 0.0f);
    double diff = 0;
    for (size_t i = 0; i < l.size(); ++i)
        diff = std::max (diff, static_cast<double> (std::abs (l[i] - r[i])));
    CHECK (diff < 1e-6); // width 0 => mono

    // Mono-below: a 50 Hz side signal is crushed, a 5 kHz one passes.
    for (double hz : { 50.0, 5000.0 })
    {
        StereoTools s2;
        s2.prepare (fs);
        auto t = sine (fs, hz, -12, 1.0);
        std::vector<float> a = t, b = t;
        for (auto& x : b)
            x = -x;
        s2.process (a.data(), b.data(), static_cast<int> (a.size()), 100.0f, 150.0f);
        const double outDb = rmsDb (a, 24000, 48000), inDb = rmsDb (t, 24000, 48000);
        if (hz < 100)
            CHECK (outDb - inDb < -20.0);
        else
            CHECK_NEAR (outDb - inDb, 0.0, 0.2);
    }
}

TEST (tonal_analyzer_pink_is_flat_white_tilts_3db_per_octave)
{
    const double fs = 48000;
    const size_t n = static_cast<size_t> (fs * 20);
    BandArray out {};
    auto at = [] (const BandArray& a, double hz) { return a[static_cast<size_t> (std::lround (BandGrid::indexForHz (hz)))]; };

    {
        auto pink = pinkNoise (n, 3);
        CHECK (TonalAnalyzer::analyseOffline (pink.data(), pink.data(), n, fs, out));
        for (double hz : { 63.0, 125.0, 250.0, 500.0, 1000.0, 2000.0, 4000.0, 8000.0, 12000.0 })
            CHECK_NEAR (at (out, hz), 0.0, 1.2);
    }
    {
        auto white = whiteNoise (n, 5);
        CHECK (TonalAnalyzer::analyseOffline (white.data(), white.data(), n, fs, out));
        CHECK_NEAR (at (out, 8000) - at (out, 1000), 9.0, 1.0);   // +3 dB/oct in constant-Q energy
        CHECK_NEAR (at (out, 1000) - at (out, 125), 9.0, 1.0);
    }
    {
        auto tone = sine (fs, 1000, -20, 10.0);
        auto bg = whiteNoise (tone.size(), 1, 0.001f);
        for (size_t i = 0; i < tone.size(); ++i)
            tone[i] += bg[i];
        CHECK (TonalAnalyzer::analyseOffline (tone.data(), tone.data(), n / 2, fs, out));
        int peak = 0;
        for (int i = 1; i < BandGrid::kNum; ++i)
            if (out[static_cast<size_t> (i)] > out[static_cast<size_t> (peak)])
                peak = i;
        CHECK (std::abs (BandGrid::centreHz (peak) - 1000.0) < 1000.0 * 0.12);
    }
}

static BandArray analyse (const std::vector<float>& x, double fs)
{
    BandArray out {};
    CHECK (TonalAnalyzer::analyseOffline (x.data(), x.data(), x.size(), fs, out));
    return out;
}

static std::vector<float> scaledToRmsDb (std::vector<float> x, double targetDb)
{
    const float g = static_cast<float> (std::pow (10.0, (targetDb - rmsDb (x, 0, x.size())) / 20.0));
    for (auto& v : x)
        v *= g;
    return x;
}

TEST (tonal_analyzer_is_independent_of_level)
{
    const double fs = 48000;
    const size_t n = static_cast<size_t> (fs * 20);
    // Coloured material (white noise: +3 dB/oct on this grid) so the check is not satisfied by a flat curve.
    const auto white = whiteNoise (n, 17);
    const auto ref = analyse (white, fs);
    for (double db : { -6.0, -30.0, -60.0, -80.0 })
    {
        const auto got = analyse (scaledToRmsDb (white, db), fs);
        for (int i = 0; i <= BandGrid::indexForHz (16000.0); ++i)
            CHECK_NEAR (got[static_cast<size_t> (i)], ref[static_cast<size_t> (i)], 0.01);
    }
}

TEST (tonal_analyzer_weights_loud_and_quiet_passages_equally)
{
    const double fs = 48000;
    const size_t half = static_cast<size_t> (fs * 12);
    const auto loud = scaledToRmsDb (pinkNoise (half, 3), -18.0);
    const auto white = whiteNoise (half, 5);

    // pink then white, the white part 20 dB or 10 dB below the pink: the result is the same either way
    auto programme = [&] (double whiteDb) {
        auto x = loud;
        const auto w = scaledToRmsDb (white, whiteDb);
        x.insert (x.end(), w.begin(), w.end());
        return analyse (x, fs);
    };
    const auto a = programme (-38.0), b = programme (-28.0);
    for (int i = 4; i <= BandGrid::indexForHz (16000.0); ++i)
        CHECK_NEAR (a[static_cast<size_t> (i)], b[static_cast<size_t> (i)], 0.3);

    // and the quiet half really does count: halfway between the pink (flat) and white (+9 dB over 1k..8k) shapes
    auto at = [] (const BandArray& x, double hz) { return x[static_cast<size_t> (std::lround (BandGrid::indexForHz (hz)))]; };
    CHECK_NEAR (at (a, 8000) - at (a, 1000), 4.5, 1.5);
}

TEST (tonal_analyzer_gates_far_quieter_frames_relative_to_the_programme)
{
    const double fs = 48000;
    const size_t half = static_cast<size_t> (fs * 12);
    auto x = scaledToRmsDb (pinkNoise (half, 3), -20.0);
    const auto hiss = scaledToRmsDb (whiteNoise (half, 5), -75.0); // 55 dB below the programme, still above digital silence
    x.insert (x.end(), hiss.begin(), hiss.end());
    const auto got = analyse (x, fs);
    auto at = [&] (double hz) { return got[static_cast<size_t> (std::lround (BandGrid::indexForHz (hz)))]; };
    CHECK_NEAR (at (8000) - at (1000), 0.0, 1.0); // would be ~4.5 dB if the hiss counted

    // the same programme 40 dB lower is gated identically (a fixed dBFS gate would let the hiss in)
    auto y = scaledToRmsDb (pinkNoise (half, 3), -60.0);
    const auto hiss2 = scaledToRmsDb (whiteNoise (half, 5), -115.0);
    y.insert (y.end(), hiss2.begin(), hiss2.end());
    const auto got2 = analyse (y, fs);
    for (int i = 0; i <= BandGrid::indexForHz (16000.0); ++i)
        CHECK_NEAR (got2[static_cast<size_t> (i)], got[static_cast<size_t> (i)], 0.05);
}

TEST (tonal_analyzer_low_bands_are_accurate)
{
    // Pink noise is flat on this grid; the lowest bands span only a few FFT bins, so this catches bin-count jitter.
    const double fs = 48000;
    const auto out = analyse (pinkNoise (static_cast<size_t> (fs * 60), 31), fs);
    for (int i = 0; i <= BandGrid::indexForHz (250.0); ++i)
        CHECK_NEAR (out[static_cast<size_t> (i)], 0.0, 1.0);
}

TEST (stereo_scope_correlation_and_samples)
{
    const double fs = 48000;
    const size_t n = static_cast<size_t> (fs * 3);
    const auto a = whiteNoise (n, 1), b = whiteNoise (n, 2);
    auto feed = [&] (StereoScope& s, const std::vector<float>& l, const std::vector<float>& r) {
        for (size_t pos = 0; pos < n; pos += 480)
            s.push (l.data() + pos, r.data() + pos, static_cast<int>(std::min<size_t> (480, n - pos)));
    };
    std::vector<float> neg (n);
    for (size_t i = 0; i < n; ++i)
        neg[i] = -a[i];

    StereoScope s;
    s.prepare (fs);
    CHECK (!s.hasData());
    feed (s, a, a);
    CHECK (s.hasData());
    CHECK_NEAR (s.correlation(), 1.0, 5e-3);
    feed (s, a, neg);
    CHECK_NEAR (s.correlation(), -1.0, 5e-3);
    feed (s, a, b);
    CHECK_NEAR (s.correlation(), 0.0, 0.15);

    // silence keeps the last value instead of dividing by zero
    const std::vector<float> zero (n, 0.0f);
    const float before = s.correlation();
    feed (s, zero, zero);
    CHECK_NEAR (s.correlation(), before, 1e-3);

    s.requestReset();
    float l1[4] = { 1, 2, 3, 4 }, r1[4] = { -1, -2, -3, -4 };
    s.push (l1, r1, 4);
    CHECK (!s.hasData() || s.correlation() < -0.9f); // reset applied, only the new block counts

    // newest samples, oldest first, and only as many as exist since prepare
    StereoScope t;
    t.prepare (fs);
    float lo[8] = {}, ro[8] = {};
    CHECK (t.copyRecent (lo, ro, 8) == 0);
    t.push (l1, r1, 4);
    CHECK (t.copyRecent (lo, ro, 8) == 4);
    CHECK (lo[0] == 1.0f && lo[3] == 4.0f && ro[3] == -4.0f);
    CHECK (t.copyRecent (lo, ro, 2) == 2);
    CHECK (lo[0] == 3.0f && lo[1] == 4.0f);

    float x = 0, y = 0;
    StereoScope::toXY (1.0f, 1.0f, x, y); // mono: vertical
    CHECK_NEAR (x, 0.0, 1e-6);
    CHECK (y > 1.4f);
    StereoScope::toXY (1.0f, 0.0f, x, y); // left only: up-left
    CHECK (x < 0.0f && y > 0.0f);
    StereoScope::toXY (0.0f, 1.0f, x, y);
    CHECK (x > 0.0f && y > 0.0f);
}

TEST (tonal_analyzer_cost_per_frame)
{
    TonalAnalyzer a;
    a.prepare (48000);
    auto noise = whiteNoise (48000 * 3, 11);
    const auto t0 = std::chrono::steady_clock::now();
    for (size_t pos = 0; pos < noise.size(); pos += 128)
        a.process (noise.data() + pos, noise.data() + pos, 128);
    const auto ms = std::chrono::duration<double, std::milli> (std::chrono::steady_clock::now() - t0).count();
    std::printf ("  (3 s of audio analysed in %.1f ms; ~%.2f ms per 4096-sample frame)\n", ms, ms / (3.0 * 48000 / 4096));
    CHECK (ms < 3000.0 * 0.05); // < 5% realtime
}

TEST (fft_real_and_complex_match_naive_dft)
{
    std::mt19937 rng (21);
    std::uniform_real_distribution<float> d (-1.0f, 1.0f);
    for (int n : { 8, 64, 1024 })
    {
        std::vector<float> x (static_cast<size_t> (n)), im (static_cast<size_t> (n));
        for (auto& v : x)
            v = d (rng);
        for (auto& v : im)
            v = d (rng);

        // complex transform
        std::vector<float> cr = x, ci = im;
        FFT (n).transform (cr.data(), ci.data());
        // real transform
        std::vector<float> rr (static_cast<size_t> (n / 2 + 1)), ri (rr.size()), pw (rr.size());
        RealFFT rf (n);
        rf.forward (x.data(), rr.data(), ri.data());
        rf.powerSpectrum (x.data(), pw.data(), 2.0f);

        double worstC = 0.0, worstR = 0.0, worstP = 0.0;
        for (int k = 0; k <= n / 2; ++k)
        {
            double xr = 0, xi = 0, yr = 0, yi = 0;
            for (int t = 0; t < n; ++t)
            {
                const double a = -2.0 * kPi * k * t / n, c = std::cos (a), sn = std::sin (a);
                xr += x[static_cast<size_t> (t)] * c;
                xi += x[static_cast<size_t> (t)] * sn;
                yr += x[static_cast<size_t> (t)] * c - im[static_cast<size_t> (t)] * sn;
                yi += x[static_cast<size_t> (t)] * sn + im[static_cast<size_t> (t)] * c;
            }
            worstC = std::max (worstC, std::abs (yr - cr[static_cast<size_t> (k)]) + std::abs (yi - ci[static_cast<size_t> (k)]));
            worstR = std::max (worstR, std::abs (xr - rr[static_cast<size_t> (k)]) + std::abs (xi - ri[static_cast<size_t> (k)]));
            worstP = std::max (worstP, std::abs (2.0 * (xr * xr + xi * xi) - pw[static_cast<size_t> (k)]));
        }
        const double tol = 1.0e-5 * n;
        CHECK (worstC < tol);
        CHECK (worstR < tol);
        CHECK (worstP < tol * n);
    }
}

TEST (tonal_analyzer_threaded_api_matches_process_and_survives_backlog)
{
    const double fs = 48000;
    const size_t n = static_cast<size_t> (fs * 12);
    const auto pink = pinkNoise (n, 9);
    BandArray ref {}, got {};

    TonalAnalyzer a;
    a.prepare (fs);
    a.setAveragingSeconds (0.0f);
    for (size_t pos = 0; pos < n; pos += 128)
        a.process (pink.data() + pos, pink.data() + pos, static_cast<int> (std::min<size_t> (128, n - pos)));
    CHECK (a.hasData());
    a.readNormalisedDb (ref);

    // push() in odd-sized blocks, pump() only now and then (wraps the ring several times, never overflows it)
    TonalAnalyzer b;
    b.prepare (fs);
    b.setAveragingSeconds (0.0f);
    int blocks = 0;
    for (size_t pos = 0; pos < n; pos += 1531, ++blocks)
    {
        const int c = static_cast<int> (std::min<size_t> (1531, n - pos));
        b.push (pink.data() + pos, pink.data() + pos, c);
        if (blocks % 20 == 0)
            b.pump();
    }
    b.pump();
    CHECK (b.hasData());
    b.readNormalisedDb (got);
    for (size_t i = 0; i < ref.size(); ++i)
        CHECK_NEAR (got[i], ref[i], 1.0e-3);

    // a worker that never keeps up skips frames but must still produce a sensible curve
    TonalAnalyzer c;
    c.prepare (fs);
    c.setAveragingSeconds (0.0f);
    for (size_t pos = 0; pos < n; pos += 512)
        c.push (pink.data() + pos, pink.data() + pos, static_cast<int> (std::min<size_t> (512, n - pos)));
    c.pump();
    CHECK (c.hasData());
    c.readNormalisedDb (got);
    for (double hz : { 125.0, 1000.0, 8000.0 })
        CHECK_NEAR (got[static_cast<size_t> (std::lround (BandGrid::indexForHz (hz)))], 0.0, 1.5);

    // reset discards earlier audio and is applied by pump()
    c.requestReset();
    c.pump();
    CHECK (!c.hasData());
    c.push (pink.data(), pink.data(), 8192);
    c.pump();
    CHECK (!c.hasData()); // less than one frame since the reset
}

TEST (low_crest_meter)
{
    const double fs = 48000;
    const int n = static_cast<int> (fs * 6);
    auto run = [&] (LowCrestMeter& m, const std::vector<float>& x) {
        for (int pos = 0; pos < n; pos += 512)
        {
            const int c = std::min (512, n - pos);
            m.process (x.data() + pos, x.data() + pos, c);
        }
    };

    // A steady low sine has a crest factor of sqrt(2) = 3.01 dB.
    std::vector<float> sine (static_cast<size_t> (n));
    for (int i = 0; i < n; ++i)
        sine[static_cast<size_t> (i)] = 0.5f * static_cast<float> (std::sin (2.0 * kPi * 60.0 * i / fs));
    LowCrestMeter a;
    a.prepare (fs);
    CHECK (!a.hasData());
    run (a, sine);
    CHECK (a.hasData());
    CHECK_NEAR (a.crestDb(), 3.01, 0.15);

    // Content above the band does not count: a 5 kHz sine is filtered out (gated as silence).
    std::vector<float> hi (static_cast<size_t> (n));
    for (int i = 0; i < n; ++i)
        hi[static_cast<size_t> (i)] = 0.5f * static_cast<float> (std::sin (2.0 * kPi * 5000.0 * i / fs));
    LowCrestMeter b;
    b.prepare (fs);
    run (b, hi);
    CHECK (!b.hasData());

    // Decaying 50 Hz "kicks" once per second are far more peaky than a sustained tone.
    std::vector<float> kick (static_cast<size_t> (n), 0.0f);
    for (int i = 0; i < n; ++i)
    {
        const double t = std::fmod (static_cast<double> (i) / fs, 1.0);
        kick[static_cast<size_t> (i)] = 0.8f * static_cast<float> (std::exp (-t * 18.0) * std::sin (2.0 * kPi * 50.0 * t));
    }
    LowCrestMeter c;
    c.prepare (fs);
    run (c, kick);
    CHECK (c.hasData());
    CHECK (c.crestDb() > a.crestDb() + 3.0f);

    // reset discards the measurement, immediately for readers even before more audio arrives
    c.requestReset();
    CHECK (!c.hasData());
    c.process (kick.data(), kick.data(), 64);
    CHECK (!c.hasData());

    // a burst of NaN must not poison the meter: it recovers once clean audio returns
    LowCrestMeter d;
    d.prepare (fs);
    std::vector<float> bad (4800, std::nanf (""));
    d.process (bad.data(), bad.data(), static_cast<int> (bad.size()));
    run (d, sine);
    CHECK (d.hasData());
    CHECK_NEAR (d.crestDb(), 3.01, 0.15);
}

TEST (target_curve_json_roundtrip_and_tracks)
{
    const auto& built = TargetCurve::builtIns();
    CHECK (built.size() >= 6);
    for (const auto& c : built)
    {
        // normalised to 0 dB mean over the anchor region
        double s = 0;
        int cnt = 0;
        for (int i = 0; i < BandGrid::kNum; ++i)
            if (BandGrid::centreHz (i) >= 200 && BandGrid::centreHz (i) <= 5000)
            {
                s += c.medianDb[static_cast<size_t> (i)];
                ++cnt;
            }
        CHECK_NEAR (s / cnt, 0.0, 0.3);
        TargetCurve back;
        CHECK (TargetCurve::fromJson (c.toJson(), back));
        CHECK (back.name == c.name);
        for (size_t i = 0; i < c.medianDb.size(); ++i)
        {
            CHECK_NEAR (back.medianDb[i], c.medianDb[i], 0.001);
            CHECK_NEAR (back.halfWidthDb[i], c.halfWidthDb[i], 0.001);
        }
    }
    TargetCurve junk;
    CHECK (!TargetCurve::fromJson ("{\"hello\":1}", junk));
    CHECK (!TargetCurve::fromJson ("{\"themisCurve\":1,\"median\":[1,2,3],\"width\":[1]}", junk));

    BandArray a {}, b {};
    for (size_t i = 0; i < a.size(); ++i)
    {
        a[i] = 1.0f;
        b[i] = -1.0f;
    }
    const auto avg = TargetCurve::fromTracks ("two", { a, b });
    CHECK_NEAR (avg.medianDb[30], 0.0, 0.01);
    CHECK (avg.halfWidthDb[30] >= 1.5f);

    // A low-sample-rate reference has no data above its Nyquist: that must not leak into the curve.
    BandArray lowRate {};
    for (size_t i = 0; i < lowRate.size(); ++i)
        lowRate[i] = i < 56 ? 3.0f : -195.0f;
    const auto lr = TargetCurve::fromTracks ("lowrate", { lowRate });
    for (size_t i = 0; i < lr.medianDb.size(); ++i)
        CHECK_NEAR (lr.medianDb[i], 0.0, 0.01); // flat input, extended flat, normalised to 0
    // Mixed with a full-bandwidth track, the empty bands take their statistics from that track alone.
    BandArray full {};
    full.fill (3.0f);
    full[58] = 9.0f;
    const auto mixed = TargetCurve::fromTracks ("mixed", { lowRate, full }, 1.5f, 2.0f);
    for (size_t i = 0; i < mixed.medianDb.size(); ++i)
        CHECK (mixed.medianDb[i] > -10.0f);
    CHECK (mixed.medianDb[59] > mixed.medianDb[40]);
}

TEST (eq_fit_reduces_error)
{
    const double fs = 48000;
    const auto target = TargetCurve::builtIns()[1];

    // Pretend the mix is the target "broken" by a known EQ: +4 dB bump at 2.5 kHz, -3 dB low shelf, -2 dB air.
    std::array<EqBandParams, MasteringEq::kNumBands> broken {};
    for (auto& b : broken)
        b.enabled = false;
    broken[1] = { true, 90.0f, -3.0f, 0.7071f, EqTarget::Stereo };
    broken[3] = { true, 2500.0f, 4.0f, 1.0f, EqTarget::Stereo };
    broken[5] = { true, 9000.0f, -2.0f, 0.7071f, EqTarget::Stereo };
    BandArray measured {};
    for (int i = 0; i < BandGrid::kNum; ++i)
        measured[static_cast<size_t> (i)] = target.medianDb[static_cast<size_t> (i)]
                                            + static_cast<float> (MasteringEq::responseDb (broken, fs, BandGrid::centreHz (i)));
    normaliseAnchor (measured);

    const auto fit = fitEqToTarget (measured, target, fs, 1.0f, 8.0f);
    std::printf ("  residual rms %.2f dB -> %.2f dB\n", fit.residualRmsBefore, fit.residualRmsAfter);
    CHECK (fit.residualRmsAfter < fit.residualRmsBefore * 0.45);

    // Applying the fit to the measured curve lands closer to the target than before.
    BandArray corrected = measured;
    for (int i = 0; i < BandGrid::kNum; ++i)
        corrected[static_cast<size_t> (i)] += static_cast<float> (MasteringEq::responseDb (fit.bands, fs, BandGrid::centreHz (i)));
    normaliseAnchor (corrected);
    double before = 0, after = 0;
    for (size_t i = 0; i < measured.size(); ++i)
    {
        before += std::pow (measured[i] - target.medianDb[i], 2);
        after += std::pow (corrected[i] - target.medianDb[i], 2);
    }
    std::printf ("  squared error vs target: %.1f -> %.1f\n", before, after);
    CHECK (after < before * 0.35);
}

TEST (engine_latency_bypass_ceiling_and_ab)
{
    const double fs = 48000;
    const int block = 256;

    EngineParams neutral;
    neutral.limOn = false;
    neutral.eqOn = false;
    for (auto& b : neutral.eq)
        b.enabled = false;

    auto runEngine = [&] (const EngineParams& p, const std::vector<float>& in, std::vector<float>& outL, Engine& e) {
        e.setParams (p);
        outL = in;
        std::vector<float> r = in;
        for (size_t pos = 0; pos < in.size(); pos += block)
        {
            const int n = static_cast<int> (std::min<size_t> (block, in.size() - pos));
            e.process (outL.data() + pos, r.data() + pos, n);
        }
    };

    // 1) neutral settings are a pure delay of latencySamples()
    {
        Engine e;
        e.prepare (fs, block);
        e.setParams (neutral);
        e.reset();
        auto in = whiteNoise (48000, 21, 0.3f);
        std::vector<float> out;
        runEngine (neutral, in, out, e);
        const size_t D = static_cast<size_t> (e.latencySamples());
        double err = 0;
        for (size_t i = D; i < in.size(); ++i)
            err = std::max (err, static_cast<double> (std::abs (out[i] - in[i - D])));
        CHECK (err < 1e-6);
    }

    // 2) full chain keeps true peak under the ceiling on a hot, EQ-boosted signal
    {
        Engine e;
        EngineParams p = neutral;
        p.eqOn = true;
        p.eq[2] = { true, 120.0f, 5.0f, 0.8f, EqTarget::Stereo };
        p.limOn = true;
        p.lim.driveDb = 9.0f;
        p.lim.ceilingDb = -1.0f;
        e.prepare (fs, block);
        e.setParams (p);
        e.reset();
        auto in = pinkNoise (48000 * 6, 4, 0.5f);
        std::vector<float> out;
        runEngine (p, in, out, e);
        CHECK (e.meters().truePeakDb < -1.0f + 0.4f);
        CHECK (e.meters().limiterGrDb < -0.5f);
        CHECK (e.meters().momentaryLufs > -30.0f);
    }

    // 3) hearOriginal returns the (delayed) input, level-matched to the processed loudness
    {
        Engine e;
        EngineParams p = neutral;
        p.outGainDb = 6.0f; // processed is 6 dB louder than the original
        e.prepare (fs, block);
        e.setParams (p);
        e.reset();
        auto in = pinkNoise (48000 * 20, 9, 0.2f);
        std::vector<float> out;
        runEngine (p, in, out, e); // lets the match gain converge
        CHECK_NEAR (e.meters().abMatchDb, 6.0, 0.4);

        EngineParams ab = p;
        ab.hearOriginal = true;
        auto tail = pinkNoise (48000 * 3, 10, 0.2f);
        std::vector<float> out2;
        runEngine (ab, tail, out2, e);
        const double outDb = rmsDb (out2, 96000, out2.size()), inDb = rmsDb (tail, 96000, tail.size());
        CHECK_NEAR (outDb - inDb, e.meters().abMatchDb, 0.3);
    }
}

// ---- mono fold + device translation --------------------------------------------------------------
// Whole-signal average (no smoothing window) of a stereo signal, fed in 4096-sample blocks.
static void analyseWhole (TonalAnalyzer& a, const std::vector<float>& l, const std::vector<float>& r, double fs)
{
    a.prepare (fs);
    a.setAveragingSeconds (0.0f);
    for (size_t pos = 0; pos < l.size(); pos += 4096)
        a.process (l.data() + pos, r.data() + pos, static_cast<int> (std::min<size_t> (4096, l.size() - pos)));
}

static BandArray analyseFold (const std::vector<float>& l, const std::vector<float>& r, double fs)
{
    TonalAnalyzer a;
    analyseWhole (a, l, r, fs);
    CHECK (a.hasMonoFoldData());
    BandArray fold {};
    a.readMonoFoldDb (fold);
    return fold;
}

static double foldAt (const BandArray& f, double hz) { return f[static_cast<size_t> (std::lround (BandGrid::indexForHz (hz)))]; }

TEST (mono_fold_follows_channel_relationship)
{
    const double fs = 48000;
    const size_t n = static_cast<size_t> (fs * 20);
    const auto a = pinkNoise (n, 3), b = pinkNoise (n, 4);
    std::vector<float> inverted (n), silent (n, 0.0f);
    for (size_t i = 0; i < n; ++i)
        inverted[i] = -a[i];

    const auto mono = analyseFold (a, a, fs), wide = analyseFold (a, b, fs), oneSided = analyseFold (a, silent, fs),
               opposed = analyseFold (a, inverted, fs);
    for (double hz : { 63.0, 250.0, 1000.0, 4000.0, 12000.0 })
    {
        CHECK_NEAR (foldAt (mono, hz), 0.0, 0.05);       // nothing lost
        CHECK_NEAR (foldAt (wide, hz), -3.0, 1.0);       // uncorrelated: half the power survives
        CHECK_NEAR (foldAt (oneSided, hz), -3.0, 0.05);  // one channel only: the same
        CHECK (foldAt (opposed, hz) < -30.0);            // cancels
    }
}

TEST (mono_fold_is_level_independent_and_resolves_frequency)
{
    const double fs = 48000;
    const size_t n = static_cast<size_t> (fs * 12);
    // Pink noise common to both channels, a 100 Hz tone in phase, a 4 kHz tone in opposite phase.
    auto l = pinkNoise (n, 5, 0.05f);
    auto r = l;
    const auto low = sine (fs, 100, -10, 12.0), high = sine (fs, 4000, -10, 12.0);
    for (size_t i = 0; i < n; ++i)
    {
        l[i] += low[i] + high[i];
        r[i] += low[i] - high[i];
    }
    const auto fold = analyseFold (l, r, fs);
    CHECK_NEAR (foldAt (fold, 100), 0.0, 0.5);
    CHECK (foldAt (fold, 4000) < -10.0);

    auto quiet = [&] (std::vector<float> v) {
        for (auto& x : v)
            x *= 0.001f;
        return v;
    };
    const auto fold2 = analyseFold (quiet (l), quiet (r), fs);
    for (int i = 0; i <= BandGrid::indexForHz (16000.0); ++i)
        CHECK_NEAR (fold2[static_cast<size_t> (i)], fold[static_cast<size_t> (i)], 0.05);
}

TEST (mono_fold_resets_and_is_zero_without_data)
{
    const double fs = 48000;
    const auto a = pinkNoise (static_cast<size_t> (fs * 6), 3), b = pinkNoise (a.size(), 4);
    TonalAnalyzer t;
    t.prepare (fs);
    CHECK (!t.hasMonoFoldData());
    t.process (a.data(), b.data(), static_cast<int> (a.size()));
    CHECK (t.hasMonoFoldData());
    t.requestReset();
    t.process (a.data(), b.data(), 1024);
    CHECK (!t.hasMonoFoldData());
    BandArray fold {};
    t.readMonoFoldDb (fold);
    for (float v : fold)
        CHECK (v == 0.0f);
}

TEST (device_profiles_have_plausible_responses)
{
    const auto& devices = DeviceProfile::builtIns();
    CHECK (devices.size() >= 5);
    for (const auto& d : devices)
    {
        // roll-off below the bass limit, nearly passband in the mids
        CHECK (d.responseDbAt (20.0) < d.responseDbAt (1000.0) - 6.0);
        CHECK (std::abs (d.responseDbAt (1000.0)) < 6.0);
        double lo = 0, hi = 0;
        d.usableRangeHz (translation::kRangeFloorDb, lo, hi);
        CHECK (lo >= 20.0 && lo < 1000.0 && hi > 5000.0);
    }
    const DeviceProfile& phone = devices[0];
    CHECK (phone.name == "Phone speaker");
    CHECK (phone.responseDbAt (100.0) < -20.0);
    CHECK_NEAR (phone.responseDbAt (450.0), -3.0 + phone.bumps[0].gainDb * std::exp (-0.5 * std::pow (std::log2 (450.0 / 2500.0) / 0.8, 2.0)), 0.1);
    // a bigger speaker reaches lower than a phone
    double phoneLo = 0, phoneHi = 0, monLo = 0, monHi = 0;
    phone.usableRangeHz (translation::kRangeFloorDb, phoneLo, phoneHi);
    DeviceProfile::builtIns().back().usableRangeHz (translation::kRangeFloorDb, monLo, monHi);
    CHECK (monLo < phoneLo);
    // the car profile has a bass lift
    const DeviceProfile* car = nullptr;
    for (const auto& d : devices)
        if (d.name == "Car")
            car = &d;
    CHECK (car != nullptr && car->responseDbAt (60.0) > 2.0);
}

TEST (translation_report_flags_bass_heavy_and_wide_low_end)
{
    const auto target = TargetCurve::builtIns().front();
    const DeviceProfile& phone = DeviceProfile::builtIns().front();

    // the track equals the target: no extra loss
    auto same = analyseTranslation (target.medianDb, nullptr, target, &phone);
    CHECK (same.haveDevice && !same.haveFold);
    CHECK_NEAR (same.extraLossDb, 0.0, 1.0e-4);
    CHECK (same.deviceChangeDb < -1.0); // a phone drops the energy below ~450 Hz
    CHECK (same.deviceLevel == Level::good);

    // +8 dB below 150 Hz: a phone loses much more of this one
    BandArray heavy = target.medianDb;
    for (int i = 0; i < BandGrid::kNum; ++i)
        if (BandGrid::centreHz (i) < 150.0)
            heavy[static_cast<size_t> (i)] += 8.0f;
    const auto bass = analyseTranslation (heavy, nullptr, target, &phone);
    CHECK (bass.extraLossDb > translation::kDeviceBadDb);
    CHECK (bass.deviceLevel == Level::bad);
    // ... a monitor that reaches down to 55 Hz copes much better, and earbuds with a bass lift are unaffected
    const auto onMonitors = analyseTranslation (heavy, nullptr, target, &DeviceProfile::builtIns().back());
    CHECK (onMonitors.extraLossDb < bass.extraLossDb * 0.5f);
    const auto onEarbuds = analyseTranslation (heavy, nullptr, target, &DeviceProfile::builtIns()[2]);
    CHECK (std::abs (onEarbuds.extraLossDb) < 0.5f);

    // overall mono loss of a uniform -3 dB fold, and a low end that cancels while the rest is mono
    BandArray fold {};
    fold.fill (-3.0f);
    auto wide = analyseTranslation (target.medianDb, &fold, target, nullptr);
    CHECK (wide.haveFold && !wide.haveDevice);
    CHECK_NEAR (wide.monoLossDb, 3.0, 0.01);
    CHECK_NEAR (wide.lowMonoLossDb, 3.0, 0.01);
    CHECK (wide.monoLevel == Level::warn && wide.lowMonoLevel == Level::bad);

    fold.fill (0.0f);
    for (int i = 0; i < BandGrid::kNum; ++i)
        if (BandGrid::centreHz (i) < 100.0)
            fold[static_cast<size_t> (i)] = -10.0f;
    fold[static_cast<size_t> (std::lround (BandGrid::indexForHz (60.0)))] = -20.0f;
    const auto cancel = analyseTranslation (target.medianDb, &fold, target, nullptr);
    CHECK (cancel.lowMonoLossDb > translation::kLowMonoBadDb);
    CHECK (cancel.lowMonoLevel == Level::bad);
    CHECK (cancel.monoLossDb < cancel.lowMonoLossDb);
    CHECK (cancel.worstBand == static_cast<int> (std::lround (BandGrid::indexForHz (60.0))));
    CHECK_NEAR (cancel.worstBandLossDb, 20.0, 1.0e-4);

    // single-band verdicts: bass is held to a tighter limit than the highs
    CHECK (translation::bandMonoLevel (0, 2.0f) == Level::warn);
    CHECK (translation::bandMonoLevel (40, 2.0f) == Level::good);
    CHECK (translation::bandMonoLevel (40, 7.0f) == Level::bad);

    // bands the signal does not contain (above Nyquist) are ignored
    BandArray lowRate = target.medianDb;
    for (int i = 50; i < BandGrid::kNum; ++i)
        lowRate[static_cast<size_t> (i)] = -200.0f;
    fold.fill (0.0f);
    const auto clean = analyseTranslation (lowRate, &fold, target, nullptr);
    CHECK_NEAR (clean.monoLossDb, 0.0, 1.0e-4);
    CHECK (clean.worstBand == -1);
}

// ---- shared helpers (Common.h / TonalAnalyzer.h / Translation.h) -------------------------------------
TEST (shared_averaging_and_db_helpers)
{
    // dB <-> power round trip; the floor only matters near silence
    CHECK_NEAR (dbToPower (-10.0), 0.1, 1.0e-12);
    CHECK_NEAR (powerToDb (dbToPower (-37.5), 0.0), -37.5, 1.0e-9);
    CHECK (powerToDb (0.0) < -190.0);

    // mid/side split reconstructs L and R
    CHECK_NEAR (midOf (0.5f, -0.25f) + sideOf (0.5f, -0.25f), 0.5, 1.0e-7);
    CHECK_NEAR (midOf (0.5f, -0.25f) - sideOf (0.5f, -0.25f), -0.25, 1.0e-7);

    // EMA weight: tau <= 0 is the cumulative mean; otherwise 1 - e^(-step/tau)
    CHECK_NEAR (emaWeight (0.1, 0.0), 0.0, 0.0);
    CHECK_NEAR (emaWeight (1.0, 1.0), 1.0 - std::exp (-1.0), 1.0e-12);
    // a running mean starts as a plain mean and settles on the EMA weight
    CHECK_NEAR (runningMeanWeight (0.0, 1), 1.0, 0.0);
    CHECK_NEAR (runningMeanWeight (0.0, 4), 0.25, 0.0);
    CHECK_NEAR (runningMeanWeight (0.1, 100), 0.1, 0.0);

    // the stereo scope's correlation window uses the same formula
    StereoScope sc;
    sc.prepare (48000.0);
    const float l[1] = { 1.0f };
    sc.push (l, l, 1);
    CHECK (sc.hasData());
}

TEST (smooth_bands_averages_neighbours_and_shrinks_at_the_edges)
{
    BandArray v {};
    v[10] = 6.0f;
    smoothBands (v, 1);
    CHECK_NEAR (v[9], 2.0, 1.0e-6);
    CHECK_NEAR (v[10], 2.0, 1.0e-6);
    CHECK_NEAR (v[11], 2.0, 1.0e-6);
    CHECK_NEAR (v[8], 0.0, 1.0e-6);

    BandArray edge {};
    edge[0] = 4.0f;
    smoothBands (edge, 1); // only two neighbours at the edge
    CHECK_NEAR (edge[0], 2.0, 1.0e-6);
    CHECK_NEAR (edge[1], 4.0 / 3.0, 1.0e-6);
}

TEST (tolerance_levels_use_one_scale)
{
    using translation::toleranceLevel;
    CHECK (toleranceLevel (-1.5f, 2.0f) == Level::good);
    CHECK (toleranceLevel (2.0f, 2.0f) == Level::good);
    CHECK (toleranceLevel (-3.0f, 2.0f) == Level::warn);
    CHECK (toleranceLevel (4.0f, 2.0f) == Level::warn);
    CHECK (toleranceLevel (4.1f, 2.0f) == Level::bad);
}


// ---- phase cancellation ----------------------------------------------------------------------
struct PhaseResult
{
    PhaseBands bands;
    PhaseReport report;
};

static PhaseResult analysePhaseOf (const std::vector<float>& l, const std::vector<float>& r, double fs = 48000.0)
{
    TonalAnalyzer a;
    analyseWhole (a, l, r, fs);
    PhaseResult out;
    CHECK (a.hasMonoFoldData());
    a.readPhase (out.bands);
    out.report = analysePhase (out.bands);
    return out;
}

static std::vector<float> delayed (const std::vector<float>& x, size_t samples)
{
    std::vector<float> y (x.size(), 0.0f);
    for (size_t i = samples; i < x.size(); ++i)
        y[i] = x[i - samples];
    return y;
}

TEST (phase_bands_follow_the_channel_relationship)
{
    const double fs = 48000;
    const size_t n = static_cast<size_t> (fs * 20);
    const auto a = pinkNoise (n, 3), b = pinkNoise (n, 4);
    std::vector<float> inverted (n);
    for (size_t i = 0; i < n; ++i)
        inverted[i] = -a[i];
    const auto at = [] (const BandArray& v, double hz) { return v[static_cast<size_t> (std::lround (BandGrid::indexForHz (hz)))]; };

    const auto mono = analysePhaseOf (a, a), wide = analysePhaseOf (a, b), opposed = analysePhaseOf (a, inverted);
    for (double hz : { 100.0, 1000.0, 8000.0 })
    {
        CHECK_NEAR (at (mono.bands.correlation, hz), 1.0, 0.01);
        CHECK_NEAR (at (mono.bands.coherence, hz), 1.0, 0.01);
        CHECK_NEAR (at (mono.bands.phaseRad, hz), 0.0, 0.05);
        CHECK (std::abs (at (wide.bands.correlation, hz)) < 0.3);
        CHECK (at (wide.bands.coherence, hz) < 0.6); // independent channels: no stable phase
        CHECK_NEAR (at (opposed.bands.correlation, hz), -1.0, 0.01);
        CHECK_NEAR (at (opposed.bands.coherence, hz), 1.0, 0.01);
        CHECK_NEAR (std::abs (at (opposed.bands.phaseRad, hz)), kPi, 0.05);
    }
}

TEST (phase_check_is_quiet_for_mono_and_wide_signals)
{
    const double fs = 48000;
    const size_t n = static_cast<size_t> (fs * 30);
    const auto a = pinkNoise (n, 3), b = pinkNoise (n, 4);
    const auto mono = analysePhaseOf (a, a), wide = analysePhaseOf (a, b);
    CHECK (mono.report.have && mono.report.cause == PhaseCause::none && mono.report.level == Level::good && mono.report.flaggedBands == 0);
    CHECK (wide.report.have && wide.report.cause == PhaseCause::none && wide.report.level == Level::good);
}

TEST (phase_check_finds_a_polarity_flip)
{
    const double fs = 48000;
    const size_t n = static_cast<size_t> (fs * 20);
    const auto a = pinkNoise (n, 3);
    std::vector<float> inverted (n);
    for (size_t i = 0; i < n; ++i)
        inverted[i] = -a[i];
    const auto r = analysePhaseOf (a, inverted).report;
    CHECK (r.cause == PhaseCause::polarity);
    CHECK (r.level == Level::bad);
    CHECK (r.flaggedBands > 40);
    CHECK (r.fit > 0.95f);
    CHECK (r.delayMs == 0.0f);
    CHECK (r.loHz < 50.0 && r.hiHz > 10000.0);
}

TEST (phase_check_finds_a_channel_delay_and_its_first_notch)
{
    const double fs = 48000;
    const size_t n = static_cast<size_t> (fs * 20);
    const auto a = pinkNoise (n, 3);
    for (size_t samples : { size_t (24), size_t (60) }) // 0.5 ms and 1.25 ms
    {
        const double ms = 1000.0 * static_cast<double> (samples) / fs;
        const auto r = analysePhaseOf (a, delayed (a, samples)).report; // right channel late
        CHECK (r.cause == PhaseCause::delay);
        CHECK (r.level != Level::good && r.flaggedBands >= 2);
        CHECK_NEAR (r.delayMs, ms, 0.03);
        CHECK (!r.leftLate);
        CHECK_NEAR (r.notchHz, 500.0 / ms, 500.0 / ms * 0.06);
        CHECK (r.fit > 0.9f);
        CHECK (analysePhaseOf (delayed (a, samples), a).report.leftLate);
    }
}

TEST (phase_check_localises_cancellation_that_is_not_a_plain_delay)
{
    // Left and right share pink noise, but a 150 Hz low-pass part is inverted in the right channel. The filter's
    // phase rotation leaves only the lowest octave truly out of phase (correlation < 0 below ~45 Hz).
    const double fs = 48000;
    const size_t n = static_cast<size_t> (fs * 20);
    const auto a = pinkNoise (n, 3);
    Biquad lp1, lp2;
    lp1.setCoeffs (BiquadCoeffs::lowPass (fs, 150.0, 0.7071));
    lp2.setCoeffs (BiquadCoeffs::lowPass (fs, 150.0, 0.7071));
    std::vector<float> l = a, r = a;
    for (size_t i = 0; i < n; ++i)
    {
        const float low = lp2.process (lp1.process (a[i]));
        r[i] = a[i] - 2.0f * low; // the low part flips sign, the rest is untouched
    }
    const auto res = analysePhaseOf (l, r).report;
    CHECK (res.level != Level::good);
    CHECK (res.cause == PhaseCause::other);
    CHECK (res.loHz < 40.0 && res.hiHz > 40.0 && res.hiHz < 120.0);
    CHECK (BandGrid::centreHz (res.worstBand) < 40.0);
}

TEST (phase_check_does_not_name_one_delay_for_sources_with_different_offsets)
{
    // Equal-level pink-noise sources, each delayed differently in the right channel. The channels are no longer copies of
    // each other, so no single delay explains the cancelling region: it is reported as "other" with its range.
    const double fs = 48000;
    const size_t n = static_cast<size_t> (fs * 20);
    const size_t offsets[] = { 10, 24, 43, 60, 80, 100 }; // 0.21 ... 2.08 ms
    for (int sources : { 2, 3, 4, 6 })
    {
        std::vector<float> l (n, 0.0f), r (n, 0.0f);
        for (int s = 0; s < sources; ++s)
        {
            const auto x = pinkNoise (n, 100u + static_cast<unsigned> (s));
            const auto xd = delayed (x, offsets[s]);
            for (size_t i = 0; i < n; ++i)
            {
                l[i] += x[i];
                r[i] += xd[i];
            }
        }
        const auto res = analysePhaseOf (l, r).report;
        CHECK (res.level != Level::good);
        CHECK (res.cause == PhaseCause::other);
        CHECK (res.delayMs == 0.0f && res.notchHz == 0.0f);
        CHECK (res.hiHz > res.loHz);
    }

    // A real delay with something else going on is still a delay: 20 dB-down independent noise in the left channel.
    const auto a = pinkNoise (n, 3);
    auto l = a;
    const auto bed = pinkNoise (n, 77, 0.1f);
    for (size_t i = 0; i < n; ++i)
        l[i] += bed[i];
    const auto res = analysePhaseOf (l, delayed (a, 24)).report;
    CHECK (res.cause == PhaseCause::delay);
    CHECK_NEAR (res.delayMs, 0.5, 0.03);
}

TEST (phase_check_ignores_bands_without_energy)
{
    PhaseBands ph;
    ph.levelDb.fill (-60.0f); // below translation::kWorstBandFloorDb everywhere
    ph.correlation.fill (-1.0f);
    ph.coherence.fill (1.0f);
    ph.phaseRad.fill (3.14159f);
    const auto r = analysePhase (ph);
    CHECK (r.have && r.cause == PhaseCause::none && r.flaggedBands == 0);
}

// ---- energy history + fundamental ------------------------------------------------------------
TEST (frame_history_keeps_the_newest_frames_in_order)
{
    FrameHistory h;
    FrameStats out[8];
    CHECK (h.copyRecent (out, 8) == 0);
    for (int i = 0; i < FrameHistory::kCapacity + 5; ++i)
    {
        FrameStats s;
        s.f0Hz = static_cast<float> (i);
        s.energy (Signal::stereo, 2) = static_cast<float> (-i);
        h.push (s);
    }
    CHECK (h.sequence() == static_cast<uint64_t> (FrameHistory::kCapacity + 5));
    CHECK (h.copyRecent (out, 8) == 8);
    for (int i = 0; i < 8; ++i)
    {
        CHECK_NEAR (out[i].f0Hz, FrameHistory::kCapacity + 5 - 8 + i, 0.0);
        CHECK_NEAR (out[i].energy (Signal::stereo, 2), -(FrameHistory::kCapacity + 5 - 8 + i), 0.0);
    }
    std::vector<FrameStats> all (FrameHistory::kCapacity + 50);
    CHECK (h.copyRecent (all.data(), static_cast<int> (all.size())) == FrameHistory::kCapacity);
    h.clear();
    CHECK (h.copyRecent (out, 8) == 0 && h.sequence() == 0);
}

// Spectrum of a mono signal as the analyzer sees it: Hann window, power normalised, FFT length 16384.
struct Spectrum
{
    std::vector<float> power;
    double binHz;
    double msScale;
};

static Spectrum spectrumOf (const std::vector<float>& x, double fs, size_t offset = 0)
{
    constexpr int n = 16384;
    std::vector<float> frame (n);
    double w2 = 0.0;
    for (int i = 0; i < n; ++i)
    {
        const double w = 0.5 - 0.5 * std::cos (2.0 * kPi * i / n);
        frame[static_cast<size_t> (i)] = x[offset + static_cast<size_t> (i)] * static_cast<float> (w);
        w2 += w * w;
    }
    Spectrum s;
    s.power.resize (n / 2 + 1);
    RealFFT fft (n);
    fft.powerSpectrum (frame.data(), s.power.data(), static_cast<float> (1.0 / w2));
    s.binHz = fs / n;
    s.msScale = 2.0 / n;
    return s;
}

static FundamentalEstimate estimate (const std::vector<float>& x, double fs = 48000.0)
{
    const auto s = spectrumOf (x, fs);
    return estimateFundamental (s.power.data(), static_cast<int> (s.power.size()), s.binHz, s.msScale);
}

static std::vector<float> mixOf (std::initializer_list<std::vector<float>> parts)
{
    std::vector<float> out (parts.begin()->size(), 0.0f);
    for (const auto& p : parts)
        for (size_t i = 0; i < out.size(); ++i)
            out[i] += p[i];
    return out;
}

TEST (fundamental_level_and_pitch_of_a_pure_tone)
{
    const double fs = 48000;
    for (double hz : { 38.0, 55.0, 97.3, 200.0 })
    {
        const auto e = estimate (sine (fs, hz, -12.0, 0.5));
        CHECK (e.found);
        CHECK_NEAR (e.hz, hz, 0.6);
        CHECK_NEAR (e.fundamentalDb, -12.0 - 3.01, 0.3); // mean square of a sine is half its squared amplitude
        CHECK (e.harmonicsDb <= kHistoryFloorDb + 1.0f);
    }
}

TEST (fundamental_is_measured_against_its_harmonics)
{
    const double fs = 48000;
    // Saw-like bass: harmonics 2 and 3 at -6 and -12 dB relative to the fundamental.
    const auto strong = mixOf ({ sine (fs, 55, -10, 0.5), sine (fs, 110, -16, 0.5), sine (fs, 165, -22, 0.5) });
    auto e = estimate (strong);
    CHECK (e.found);
    CHECK_NEAR (e.hz, 55.0, 0.6);
    CHECK_NEAR (e.fundamentalDb - e.harmonicsDb, 10.0 * std::log10 (1.0 / (std::pow (10.0, -0.6) + std::pow (10.0, -1.2))), 0.4);
    CHECK (fundamental::balanceOf (e.fundamentalDb - e.harmonicsDb) == fundamental::Balance::balanced);

    // A bass whose fundamental sits 15 dB under its first harmonic is still tracked from the fundamental, not the harmonic.
    const auto weak = mixOf ({ sine (fs, 60, -33, 0.5), sine (fs, 120, -18, 0.5), sine (fs, 180, -21, 0.5) });
    e = estimate (weak);
    CHECK (e.found);
    CHECK_NEAR (e.hz, 60.0, 0.6);
    CHECK_NEAR (e.fundamentalDb - e.harmonicsDb, -15.0 - 10.0 * std::log10 (1.0 + std::pow (10.0, -0.3)), 0.4);
    CHECK (fundamental::balanceOf (e.fundamentalDb - e.harmonicsDb) == fundamental::Balance::harmonicRich);

    CHECK (fundamental::balanceOf (e.fundamentalDb - (e.fundamentalDb - 20.0f)) == fundamental::Balance::subHeavy);
}

TEST (fundamental_is_not_reported_for_noise_or_silence)
{
    const double fs = 48000;
    for (unsigned seed = 1; seed <= 6; ++seed)
    {
        CHECK (!estimate (whiteNoise (16384, seed, 0.1f)).found);
        CHECK (!estimate (pinkNoise (16384, seed, 0.25f)).found);
    }
    CHECK (!estimate (sine (fs, 55, -95.0, 0.5)).found);          // too quiet
    CHECK (!estimate (sine (fs, 700, -10.0, 0.5)).found);         // outside the bass range
    CHECK (!estimate (std::vector<float> (16384, 0.0f)).found);
}

TEST (analyzer_records_energy_per_range_and_silence)
{
    const double fs = 48000;
    TonalAnalyzer a;
    a.prepare (fs);
    const auto tone = sine (fs, 2000, -20.0, 5.0);
    a.process (tone.data(), tone.data(), static_cast<int> (tone.size()));
    const std::vector<float> silence (static_cast<size_t> (fs * 2), 0.0f);
    a.process (silence.data(), silence.data(), static_cast<int> (silence.size()));

    const auto& h = a.history();
    CHECK_NEAR (h.framePeriodSeconds(), 4096.0 / fs, 1.0e-9);
    std::vector<FrameStats> frames (FrameHistory::kCapacity);
    const int n = h.copyRecent (frames.data(), FrameHistory::kCapacity);
    CHECK (n > 40);
    const auto& mid = frames[static_cast<size_t> (n / 4)]; // well inside the tone
    CHECK_NEAR (mid.energy (Signal::stereo, 2), -20.0 - 3.01, 1.0);        // HIGH-MID holds the tone: mean square of a -20 dB sine
    CHECK (mid.energy (Signal::stereo, 0) < -70.0f && mid.energy (Signal::stereo, 1) < -70.0f && mid.energy (Signal::stereo, 3) < -70.0f);
    CHECK (mid.f0Hz == 0.0f);
    CHECK_NEAR (frames[static_cast<size_t> (n - 1)].energy (Signal::stereo, 2), kHistoryFloorDb, 1.0e-6); // digital silence
    // Level independence of the curve does not touch the absolute energy history: a quieter tone reads lower.
    TonalAnalyzer q;
    q.prepare (fs);
    const auto quiet = sine (fs, 2000, -40.0, 3.0);
    q.process (quiet.data(), quiet.data(), static_cast<int> (quiet.size()));
    std::vector<FrameStats> qf (FrameHistory::kCapacity);
    const int qn = q.history().copyRecent (qf.data(), FrameHistory::kCapacity);
    CHECK (qn > 10);
    CHECK_NEAR (qf[static_cast<size_t> (qn - 1)].energy (Signal::stereo, 2), -43.01, 1.0);

    a.requestReset();
    a.process (silence.data(), silence.data(), 4096);
    CHECK (a.history().sequence() <= 1);
}

TEST (analyzer_tracks_a_bass_fundamental_over_time)
{
    const double fs = 48000;
    TonalAnalyzer a;
    a.prepare (fs);
    // 3 s of a 50 Hz bass with two harmonics, then 3 s of a 70 Hz one.
    const auto first = mixOf ({ sine (fs, 50, -14, 3.0), sine (fs, 100, -20, 3.0), sine (fs, 150, -26, 3.0) });
    const auto second = mixOf ({ sine (fs, 70, -14, 3.0), sine (fs, 140, -20, 3.0), sine (fs, 210, -26, 3.0) });
    a.process (first.data(), first.data(), static_cast<int> (first.size()));
    a.process (second.data(), second.data(), static_cast<int> (second.size()));
    std::vector<FrameStats> frames (FrameHistory::kCapacity);
    const int n = a.history().copyRecent (frames.data(), FrameHistory::kCapacity);
    CHECK (n > 60);
    const auto& early = frames[static_cast<size_t> (n / 4)];
    const auto& late = frames[static_cast<size_t> (n - 3)];
    CHECK_NEAR (early.f0Hz, 50.0, 0.7);
    CHECK_NEAR (late.f0Hz, 70.0, 0.7);
    CHECK_NEAR (early.fundamentalDb, -14.0 - 3.01, 0.5);
    CHECK_NEAR (early.fundamentalDb - early.harmonicsDb, 10.0 * std::log10 (1.0 / (std::pow (10.0, -0.6) + std::pow (10.0, -1.2))), 0.5);
    CHECK (late.energy (Signal::stereo, 0) > late.energy (Signal::stereo, 3)); // the energy sits in the LOW range
}


// ---- clipping ---------------------------------------------------------------------------------
static ClipWindow clipOf (const std::vector<float>& l, const std::vector<float>& r, int block = 512)
{
    ClipMeter m;
    m.prepare();
    ClipWindow total;
    for (size_t pos = 0; pos < l.size(); pos += static_cast<size_t> (block))
    {
        const int n = static_cast<int> (std::min<size_t> (static_cast<size_t> (block), l.size() - pos));
        m.process (l.data() + pos, r.data() + pos, n);
        const auto w = m.take();
        total.samplePeak = std::max (total.samplePeak, w.samplePeak);
        total.truePeak = std::max (total.truePeak, w.truePeak);
        total.clipEvents += w.clipEvents;
        total.clipSamples += w.clipSamples;
        total.overSamples += w.overSamples;
    }
    return total;
}

TEST (clip_meter_ignores_clean_audio_and_finds_hard_clipping)
{
    const double fs = 48000;
    const auto clean = sine (fs, 997, -3.0, 2.0);
    const auto w0 = clipOf (clean, clean);
    CHECK (w0.clipEvents == 0 && w0.overSamples == 0);
    CHECK_NEAR (linToDb (w0.samplePeak), -3.0, 0.05);

    // The same tone driven 6 dB into a hard clipper at -1 dBFS: flat tops at a ceiling that is not 0 dBFS.
    const float ceiling = dbToLin (-1.0f);
    auto hot = sine (fs, 997, 3.0, 2.0);
    for (auto& x : hot)
        x = std::clamp (x, -ceiling, ceiling);
    const auto w1 = clipOf (hot, hot);
    // 2 s of a 997 Hz tone: 1994 cycles with two flat tops each, in both channels.
    CHECK (w1.clipEvents > 7500 && w1.clipEvents < 8500);
    CHECK (w1.clipSamples >= w1.clipEvents * ClipMeter::kMinRun);
    CHECK (w1.overSamples == 0); // not over 0 dBFS, just clipped
    CHECK_NEAR (linToDb (w1.samplePeak), -1.0, 0.01);

    // Clipping only one channel is found in that channel.
    const auto w2 = clipOf (hot, clean);
    CHECK (w2.clipEvents > 3500 && w2.clipEvents < 4500);
}

TEST (clip_meter_ignores_non_finite_samples)
{
    // A glitched buffer (NaN, +/-inf) is not clipping and must not pin the peak at +inf for the rest of the session.
    const double fs = 48000;
    auto tone = sine (fs, 997, -6.0, 1.0);
    tone[1000] = std::nanf ("");
    tone[2000] = std::numeric_limits<float>::infinity();
    tone[3000] = -std::numeric_limits<float>::infinity();
    const auto w = clipOf (tone, tone);
    CHECK (std::isfinite (w.samplePeak) && std::isfinite (w.truePeak));
    CHECK_NEAR (linToDb (w.samplePeak), -6.0, 0.05);
    CHECK (w.overSamples == 0 && w.clipEvents == 0);
}

TEST (clip_meter_counts_overs_and_inter_sample_peaks)
{
    const double fs = 48000;
    // fs/4 sine at 45 degrees: every sample is +/-0.7071 but the waveform peaks at 1.0.
    const auto s = sine (fs, 12000, 0.0, 0.2, kPi / 4.0);
    const auto w = clipOf (s, s);
    CHECK_NEAR (linToDb (w.samplePeak), -3.0, 0.05);
    CHECK (linToDb (w.truePeak) > -0.5f && linToDb (w.truePeak) < 0.5f);
    CHECK (w.overSamples == 0 && w.clipEvents == 0);

    // A few samples above full scale.
    auto over = sine (fs, 100, -6.0, 0.1);
    over[1000] = 1.2f;
    over[1001] = -1.0f;
    const auto wo = clipOf (over, over);
    CHECK (wo.overSamples == 4); // two samples in each channel
}

TEST (clip_meter_does_not_mistake_a_quantised_bass_sine_for_clipping)
{
    // A loud 35 Hz sine stored with 16 bits has several equal samples at each crest, but reaches them smoothly.
    const double fs = 48000;
    auto bass = sine (fs, 35, -1.0, 2.0);
    for (auto& x : bass)
        x = std::round (x * 32768.0f) / 32768.0f;
    const auto w = clipOf (bass, bass);
    CHECK (w.clipEvents == 0);
}

TEST (clip_meter_finds_a_plateau_that_spans_blocks_and_ignores_quiet_ones)
{
    const double fs = 48000;
    std::vector<float> x (4096, 0.0f);
    for (size_t i = 0; i < 2000; ++i)
        x[i] = 0.3f * static_cast<float> (std::sin (0.05 * static_cast<double> (i)));
    for (size_t i = 2000; i < 2010; ++i) // ten equal samples at 0.9, split by the 2005 block boundary below
        x[i] = 0.9f;
    for (size_t i = 3000; i < 3010; ++i) // a plateau, but only at 0.2: silence-level, not clipping
        x[i] = 0.2f;
    ClipMeter m;
    m.prepare();
    m.process (x.data(), x.data(), 2005);
    m.process (x.data() + 2005, x.data() + 2005, 4096 - 2005);
    const auto w = m.take();
    CHECK (w.clipEvents == 2); // one per channel
    CHECK (w.clipSamples == 20);
    (void) fs;
}

TEST (clip_meter_totals_accumulate_and_reset)
{
    const double fs = 48000;
    auto hot = sine (fs, 997, 3.0, 0.5);
    for (auto& x : hot)
        x = std::clamp (x, -0.9f, 0.9f);
    ClipMeter m;
    m.prepare();
    CHECK (!m.totals().have);
    m.process (hot.data(), hot.data(), static_cast<int> (hot.size()));
    auto t = m.totals();
    CHECK (t.have && t.clipEvents > 500 && t.clipSamples > t.clipEvents);
    CHECK_NEAR (t.samplePeakDb, -0.915, 0.02);
    CHECK (history::clip::level (t.truePeakDb, t.clipEvents, t.overSamples) == Level::bad);
    m.take();
    m.requestReset();
    CHECK (!m.totals().have); // a pending reset reads as "no data" even before audio arrives
    m.process (hot.data(), hot.data(), 16);
    CHECK (m.totals().have && m.totals().clipEvents == 0);
}

TEST (clip_verdict_follows_the_true_peak_and_any_clip)
{
    using history::clip::level;
    CHECK (level (-3.0f, 0, 0) == Level::good);
    CHECK (level (-0.5f, 0, 0) == Level::warn);
    CHECK (level (0.4f, 0, 0) == Level::bad);
    CHECK (level (-6.0f, 1, 0) == Level::bad);
    CHECK (level (-6.0f, 0, 1) == Level::bad);
}

TEST (analyzer_history_carries_peaks_and_clips)
{
    const double fs = 48000;
    TonalAnalyzer a;
    a.prepare (fs);
    const auto quiet = sine (fs, 500, -20.0, 3.0);
    auto hot = sine (fs, 500, 6.0, 3.0);
    for (auto& x : hot)
        x = std::clamp (x, -0.95f, 0.95f);
    a.process (quiet.data(), quiet.data(), static_cast<int> (quiet.size()));
    a.process (hot.data(), hot.data(), static_cast<int> (hot.size()));
    std::vector<FrameStats> frames (FrameHistory::kCapacity);
    const int n = a.history().copyRecent (frames.data(), FrameHistory::kCapacity);
    CHECK (n > 60);
    const auto& early = frames[static_cast<size_t> (n / 4)];
    const auto& late = frames[static_cast<size_t> (n - 4)];
    CHECK (!early.clipped() && early.samplePeakDb < -19.0f && early.samplePeakDb > -21.0f);
    CHECK (late.clipped() && late.clipEvents > 10.0f);
    CHECK_NEAR (late.samplePeakDb, -0.446, 0.02);
    CHECK (late.truePeakDb >= late.samplePeakDb);
    const auto report = history::analysePeaks (frames.data(), n);
    CHECK (report.have && report.level == Level::bad && report.clippedFrames > 20 && report.clippedFrames < n / 2 + 8);
    const auto totals = a.clipTotals();
    CHECK (totals.have && totals.clipEvents > 0);
    a.requestReset();
    CHECK (!a.clipTotals().have);

    // Offline analysis (learning from files) skips clip detection.
    TonalAnalyzer offline;
    offline.prepareOffline (fs);
    offline.process (hot.data(), hot.data(), static_cast<int> (hot.size()));
    CHECK (!offline.clipTotals().have);
}

// ---- mid / side / left / right energy ----------------------------------------------------------
TEST (analyzer_records_mid_side_left_right_energy)
{
    const double fs = 48000;
    const auto tone = sine (fs, 2000, -20.0, 4.0);
    const std::vector<float> silence (tone.size(), 0.0f);
    std::vector<FrameStats> frames (FrameHistory::kCapacity);
    const auto last = [&] (const std::vector<float>& l, const std::vector<float>& r) {
        TonalAnalyzer a;
        a.prepare (fs);
        a.process (l.data(), r.data(), static_cast<int> (l.size()));
        const int n = a.history().copyRecent (frames.data(), FrameHistory::kCapacity);
        return frames[static_cast<size_t> (n - 3)];
    };
    constexpr int kHighMid = 2; // 1-6 kHz, where the tone sits
    const auto centred = last (tone, tone);
    CHECK_NEAR (centred.energy (Signal::left, kHighMid), -23.01, 0.7);
    CHECK_NEAR (centred.energy (Signal::right, kHighMid), -23.01, 0.7);
    CHECK_NEAR (centred.energy (Signal::mid, kHighMid), -23.01, 0.7);
    CHECK (centred.energy (Signal::side, kHighMid) < -60.0f);
    CHECK_NEAR (centred.energy (Signal::stereo, kHighMid), -23.01, 0.7);
    CHECK_NEAR (centred.energy (Signal::left, kAllRanges), centred.energy (Signal::left, kHighMid), 0.3); // all of it is in that range

    const auto leftOnly = last (tone, silence);
    CHECK_NEAR (leftOnly.energy (Signal::left, kHighMid), -23.01, 0.7);
    CHECK (leftOnly.energy (Signal::right, kHighMid) < -60.0f);
    CHECK_NEAR (leftOnly.energy (Signal::mid, kHighMid), -23.01 - 6.02, 0.7);
    CHECK_NEAR (leftOnly.energy (Signal::side, kHighMid), -23.01 - 6.02, 0.7);
    CHECK_NEAR (leftOnly.energy (Signal::stereo, kHighMid), -23.01 - 3.01, 0.7); // mean power of the two channels

    std::vector<float> inverted (tone.size());
    for (size_t i = 0; i < tone.size(); ++i)
        inverted[i] = -tone[i];
    const auto outOfPhase = last (tone, inverted);
    CHECK (outOfPhase.energy (Signal::mid, kHighMid) < -60.0f);
    CHECK_NEAR (outOfPhase.energy (Signal::side, kHighMid), -23.01, 0.7);
    CHECK_NEAR (outOfPhase.energy (Signal::left, kHighMid), -23.01, 0.7);
}

// ---- verdicts on the history ---------------------------------------------------------------------
static std::vector<FrameStats> framesWith (int n, const std::function<void (int, FrameStats&)>& fill)
{
    std::vector<FrameStats> v (static_cast<size_t> (n));
    for (int i = 0; i < n; ++i)
        fill (i, v[static_cast<size_t> (i)]);
    return v;
}

TEST (history_swing_names_the_range_that_moves_most)
{
    const auto frames = framesWith (200, [] (int i, FrameStats& f) {
        for (auto& signal : f.energyDb)
            signal.fill (-30.0f);
        f.energy (Signal::stereo, 1) = (i / 20) % 2 == 0 ? -30.0f : -42.0f; // LOW-MID alternates by 12 dB between sections
        f.energy (Signal::stereo, 0) = -30.0f + 0.5f * static_cast<float> (i % 3);
    });
    const auto r = history::analyseSwing (frames.data(), 200);
    CHECK (r.have && r.widest == 1 && r.steadiest != 1);
    CHECK_NEAR (r.swingDb, 12.0, 0.1);
    CHECK (r.level == Level::bad);
    CHECK (r.steadiestDb < history::kSwingWarnDb);
    CHECK (!history::analyseSwing (frames.data(), 3).have); // too little to judge
}

TEST (history_width_uses_the_mono_fold_limits)
{
    const auto make = [] (float sideMinusMid) {
        return framesWith (100, [sideMinusMid] (int, FrameStats& f) {
            for (auto& signal : f.energyDb)
                signal.fill (kHistoryFloorDb);
            for (int c = 0; c < kNumEnergyColumns; ++c)
            {
                f.energy (Signal::mid, c) = -20.0f;
                f.energy (Signal::side, c) = -20.0f + sideMinusMid;
            }
        });
    };
    const auto narrow = make (-14.0f);
    const auto r = history::analyseWidth (narrow.data(), 100, 0);
    CHECK (r.have);
    CHECK_NEAR (r.sideMinusMidDb, -14.0, 0.01);
    CHECK_NEAR (r.monoLossDb, 10.0 * std::log10 (1.0 + std::pow (10.0, -1.4)), 0.01); // ~0.17 dB
    CHECK (r.level == Level::good);

    const auto wide = make (-3.0f); // 1.76 dB of mono loss: fine for a whole programme, too much for the bass
    CHECK (history::analyseWidth (wide.data(), 100, kAllRanges).level == Level::good);
    CHECK (history::analyseWidth (wide.data(), 100, 0).level == Level::warn);
    CHECK (history::analyseWidth (wide.data(), 100, 2).level == Level::good);
    const auto uncorrelated = make (0.0f); // 3 dB lost
    CHECK (history::analyseWidth (uncorrelated.data(), 100, kAllRanges).level == Level::warn);
    CHECK (history::analyseWidth (uncorrelated.data(), 100, 0).level == Level::bad);

    const auto silent = framesWith (100, [] (int, FrameStats& f) {
        for (auto& signal : f.energyDb)
            signal.fill (kHistoryFloorDb);
    });
    CHECK (!history::analyseWidth (silent.data(), 100, 0).have);
}

TEST (history_balance_reports_the_louder_channel)
{
    const auto frames = framesWith (100, [] (int, FrameStats& f) {
        for (auto& signal : f.energyDb)
            signal.fill (-25.0f);
        f.energy (Signal::left, kAllRanges) = -24.0f;
    });
    const auto r = history::analyseBalance (frames.data(), 100, kAllRanges);
    CHECK (r.have);
    CHECK_NEAR (r.leftMinusRightDb, 1.0, 0.01);
    CHECK (r.level == Level::warn);
    CHECK (history::analyseBalance (frames.data(), 100, 0).level == Level::good); // the LOW column is balanced
}

TEST (history_bass_report_counts_balance_and_sub_floor_time)
{
    const auto frames = framesWith (100, [] (int i, FrameStats& f) {
        if (i % 10 == 9) // no tonal bass in one frame out of ten
            return;
        const bool low = i < 40;
        f.f0Hz = low ? 36.0f : 55.0f;
        f.fundamentalDb = -20.0f;
        f.harmonicsDb = low ? -40.0f : -22.0f; // 20 dB above the harmonics (sub-heavy) at first, then 2 dB (balanced)
    });
    const auto r = history::analyseBass (frames.data(), 100);
    CHECK (r.have && r.tracked == 90);
    CHECK_NEAR (r.lowHz, 36.0, 0.01);
    CHECK_NEAR (r.highHz, 55.0, 0.01);
    CHECK_NEAR (r.medianHz, 55.0, 0.01);
    CHECK_NEAR (r.belowFloorShare, 36.0 / 90.0, 0.01);
    CHECK (r.dominant == fundamental::Balance::balanced);
    CHECK_NEAR (r.share[static_cast<size_t> (fundamental::Balance::subHeavy)], 36.0 / 90.0, 0.01);
    CHECK (r.floorLevel == Level::warn && r.balanceLevel == Level::good);

    CHECK_NEAR (fundamental::tiltDb ([] { FrameStats f; f.f0Hz = 50; f.fundamentalDb = -20; return f; }()), fundamental::kMaxTiltDb, 0.0); // no harmonics
    CHECK (fundamental::tiltDb (FrameStats {}) <= kHistoryFloorDb);
    std::vector<FrameStats> none (50);
    CHECK (!history::analyseBass (none.data(), 50).have);
}

// ---- audition (solo / mute) ---------------------------------------------------------------------------------------
namespace
{
// Runs `left` / `right` through an Audition with the given switches, in blocks, and returns the output.
struct Stereo
{
    std::vector<float> l, r;
};

Stereo audition (const Switches& sw, const std::vector<float>& l, const std::vector<float>& r, double fs = 48000.0, int block = 256)
{
    Audition a;
    a.prepare (fs);
    a.setSwitches (sw);
    Stereo out { l, r };
    for (size_t pos = 0; pos < l.size(); pos += static_cast<size_t> (block))
    {
        const int n = static_cast<int> (std::min<size_t> (static_cast<size_t> (block), l.size() - pos));
        a.process (out.l.data() + pos, out.r.data() + pos, n);
    }
    return out;
}

Switches solo (std::initializer_list<Source> s)
{
    Switches sw;
    for (auto x : s)
        sw.solo |= Switches::bit (x);
    return sw;
}
Switches mute (std::initializer_list<Source> s)
{
    Switches sw;
    for (auto x : s)
        sw.mute |= Switches::bit (x);
    return sw;
}

// Level of the second half of a signal (past the fade and the filters' settling) in dB.
double tailDb (const std::vector<float>& v) { return rmsDb (v, v.size() / 2, v.size()); }
} // namespace

TEST (audition_leaves_the_audio_bit_exact_when_nothing_is_engaged)
{
    const auto l = whiteNoise (20000, 1), r = whiteNoise (20000, 2);
    const auto out = audition ({}, l, r);
    CHECK (out.l == l && out.r == r);

    // ... and again after something was engaged and released.
    Audition a;
    a.prepare (48000.0);
    auto ll = l, rr = r;
    a.setSwitches (mute ({ Source::side, Source::low }));
    a.process (ll.data(), rr.data(), 9000);
    a.setSwitches ({});
    a.process (ll.data() + 9000, rr.data() + 9000, 5000); // fades back
    ll = l;
    rr = r;
    a.process (ll.data(), rr.data(), 20000);
    CHECK (ll == l && rr == r);
}

TEST (audition_resolves_solo_and_mute_within_each_group)
{
    auto a = resolve ({});
    CHECK (a.untouched());
    a = resolve (mute ({ Source::side }));
    CHECK (a.mid && !a.side && a.left && a.right && a.bandsAll());
    a = resolve (solo ({ Source::side }));
    CHECK (!a.mid && a.side);
    a = resolve (solo ({ Source::mid, Source::side }));
    CHECK (a.mid && a.side);
    Switches both = solo ({ Source::mid });
    both.mute = Switches::bit (Source::mid);
    a = resolve (both);
    CHECK (!a.mid && !a.side); // soloed and muted: silent, and the solo still silences the rest of the group
    a = resolve (solo ({ Source::highMid }));
    CHECK (!a.band[0] && !a.band[1] && a.band[2] && !a.band[3] && a.mid && a.left); // other groups are not touched
    a = resolve (mute ({ Source::low, Source::high }));
    CHECK (!a.band[0] && a.band[1] && a.band[2] && !a.band[3]);
    CHECK (Switches::unpack (solo ({ Source::side, Source::high }).pack()) == solo ({ Source::side, Source::high }));
    CHECK (Switches::unpack (mute ({ Source::left }).pack()) == mute ({ Source::left }));
}

TEST (audition_plays_the_mid_and_side_signals)
{
    const double fs = 48000.0;
    const auto tone = sine (fs, 500, -12, 1.0), other = sine (fs, 1300, -12, 1.0);
    // L = M + S, R = M - S with M = a 500 Hz tone and S = a 1300 Hz tone.
    std::vector<float> l (tone.size()), r (tone.size());
    for (size_t i = 0; i < l.size(); ++i)
    {
        l[i] = tone[i] + other[i];
        r[i] = tone[i] - other[i];
    }
    const auto midOnly = audition (solo ({ Source::mid }), l, r);
    CHECK (midOnly.l.size() == l.size());
    for (size_t i = 24000; i < l.size(); i += 101)
    {
        CHECK_NEAR (midOnly.l[i], tone[i], 1.0e-5);
        CHECK_NEAR (midOnly.r[i], tone[i], 1.0e-5);
    }
    const auto sideOnly = audition (solo ({ Source::side }), l, r);
    const auto muteMid = audition (mute ({ Source::mid }), l, r);
    for (size_t i = 24000; i < l.size(); i += 997)
    {
        CHECK_NEAR (sideOnly.l[i], other[i], 1.0e-5);
        CHECK_NEAR (sideOnly.r[i], -other[i], 1.0e-5);
        CHECK_NEAR (muteMid.l[i], other[i], 1.0e-5);
    }
    const auto mono = audition (mute ({ Source::side }), l, r);
    for (size_t i = 24000; i < l.size(); i += 997)
    {
        CHECK_NEAR (mono.l[i], tone[i], 1.0e-5); // muting the side is the mono sum
        CHECK_NEAR (mono.r[i], tone[i], 1.0e-5);
    }
    // A mono signal has no side: soloing it is silence.
    const auto silent = audition (solo ({ Source::side }), tone, tone);
    CHECK (tailDb (silent.l) < -120.0 && tailDb (silent.r) < -120.0);
}

TEST (audition_solos_and_mutes_the_channels)
{
    const double fs = 48000.0;
    const auto a = sine (fs, 500, -12, 1.0), b = sine (fs, 1300, -12, 1.0);
    const auto soloL = audition (solo ({ Source::left }), a, b);
    const auto soloR = audition (solo ({ Source::right }), a, b);
    const auto muteL = audition (mute ({ Source::left }), a, b);
    const auto none = audition (mute ({ Source::left, Source::right }), a, b);
    const auto bothSoloed = audition (solo ({ Source::left, Source::right }), a, b);
    for (size_t i = 24000; i < a.size(); i += 997)
    {
        CHECK_NEAR (soloL.l[i], a[i], 1.0e-6); // a soloed channel is played on both sides
        CHECK_NEAR (soloL.r[i], a[i], 1.0e-6);
        CHECK_NEAR (soloR.l[i], b[i], 1.0e-6);
        CHECK_NEAR (soloR.r[i], b[i], 1.0e-6);
        CHECK_NEAR (muteL.l[i], 0.0, 1.0e-6); // a muted one is silent where it was
        CHECK_NEAR (muteL.r[i], b[i], 1.0e-6);
        CHECK_NEAR (bothSoloed.l[i], a[i], 1.0e-6);
        CHECK_NEAR (bothSoloed.r[i], b[i], 1.0e-6);
    }
    CHECK (tailDb (none.l) < -120.0 && tailDb (none.r) < -120.0);
    Switches soloAndMute = solo ({ Source::left });
    soloAndMute.mute = Switches::bit (Source::left);
    const auto silent = audition (soloAndMute, a, b);
    CHECK (tailDb (silent.l) < -120.0 && tailDb (silent.r) < -120.0);
}

TEST (audition_isolates_the_energy_ranges)
{
    const double fs = 48000.0;
    // A tone in the middle of each range: 60 Hz, 400 Hz, 2.5 kHz, 12 kHz.
    const double tones[kNumEnergyRanges] = { 60.0, 400.0, 2500.0, 12000.0 };
    for (int soloed = 0; soloed < kNumEnergyRanges; ++soloed)
        for (int t = 0; t < kNumEnergyRanges; ++t)
        {
            const auto x = sine (fs, tones[t], -12, 1.0);
            const auto out = audition (solo ({ static_cast<Source> (kFirstBandSource + soloed) }), x, x);
            const double dB = tailDb (out.l) - tailDb (x);
            if (t == soloed)
                CHECK_NEAR (dB, 0.0, 0.7);
            else
                CHECK (dB < -18.0); // 4th-order crossovers: well under a decade away is at least 18 dB down
        }
}

TEST (audition_muting_a_range_removes_only_that_range)
{
    const double fs = 48000.0;
    // Muting LOW and HIGH leaves the middle two ranges intact (flat magnitude through the crossovers and their all-passes).
    // (A 4th-order crossover is 0.5 dB down half an octave from its corner; the tones are at least that far in.)
    for (double hz : { 300.0, 500.0, 2000.0, 3000.0 })
    {
        const auto x = sine (fs, hz, -12, 1.0);
        const auto out = audition (mute ({ Source::low, Source::high }), x, x);
        CHECK_NEAR (tailDb (out.l) - tailDb (x), 0.0, 0.8);
    }
    // The four ranges add back up to a flat-magnitude signal: at each corner, where the neighbours overlap most.
    for (double hz : { 120.0, 1000.0, 6000.0 })
    {
        const auto x = sine (fs, hz, -12, 1.0);
        std::vector<float> sum (x.size(), 0.0f);
        for (int r = 0; r < kNumEnergyRanges; ++r)
        {
            const auto part = audition (solo ({ static_cast<Source> (kFirstBandSource + r) }), x, x);
            for (size_t i = 0; i < sum.size(); ++i)
                sum[i] += part.l[i];
        }
        CHECK_NEAR (tailDb (sum) - tailDb (x), 0.0, 0.3);
    }
    // Muting one range leaves a noise signal's other ranges at the same level: the total loses just what that range held.
    const auto noise = whiteNoise (96000, 5);
    const auto out = audition (mute ({ Source::high }), noise, noise);
    // White noise is flat to 24 kHz, so what is left is the 4th-order low-pass at 6 kHz: 5 / 24 of the power (the integral of
    // 1 / (1 + x^4)^2 is 0.833 corner-frequencies wide), i.e. 6.8 dB down.
    const double lost = tailDb (noise) - tailDb (out.l);
    CHECK_NEAR (lost, 6.8, 0.4);
}

TEST (audition_fades_without_clicks)
{
    const double fs = 48000.0;
    const auto x = sine (fs, 100, -6, 1.0);
    for (const auto& sw : { mute ({ Source::mid, Source::side }), solo ({ Source::side }), mute ({ Source::low }), mute ({ Source::left }) })
    {
        const auto out = audition (sw, x, x, fs, 480);
        double maxStep = 0.0;
        for (size_t i = 1; i < out.l.size(); ++i)
            maxStep = std::max (maxStep, static_cast<double> (std::abs (out.l[i] - out.l[i - 1])));
        // The input's own steepest step is 2 pi 100 / 48000 * 0.5 = 0.0065; the fade adds at most 1 / 480 of the level per sample.
        CHECK (maxStep < 0.012);
    }
}

TEST (audition_follows_the_switches_in_both_directions)
{
    const double fs = 48000.0;
    Audition a;
    a.prepare (fs);
    const auto x = sine (fs, 500, -12, 1.0);
    auto run = [&] (const Switches& sw) {
        a.setSwitches (sw);
        auto l = x, r = x;
        a.process (l.data(), r.data(), static_cast<int> (l.size()));
        return l;
    };
    CHECK (tailDb (run (solo ({ Source::side }))) < -120.0);
    CHECK_NEAR (tailDb (run ({})), tailDb (x), 1.0e-6);
    CHECK (tailDb (run (mute ({ Source::lowMid }))) < -18.0);
    CHECK_NEAR (tailDb (run ({})), tailDb (x), 0.01);
}

// ---- loudness monitor --------------------------------------------------------------------------------------------
TEST (loudness_monitor_reads_the_reference_sine_and_resets)
{
    const double fs = 48000.0;
    LoudnessMonitor m;
    m.prepare (fs);
    CHECK (!m.hasData());
    const auto s = sine (fs, 997, -20, 10.0);
    m.process (s.data(), s.data(), 48000);
    CHECK (!m.hasData()); // 1 s is not enough yet
    m.process (s.data(), s.data(), 48000 * 4);
    CHECK (m.hasData());
    CHECK_NEAR (m.integrated(), -20.0, 0.15);
    CHECK_NEAR (m.shortTerm(), -20.0, 0.15);
    CHECK_NEAR (m.maxShortTerm(), -20.0, 0.15);
    CHECK_NEAR (m.seconds(), 5.0, 1.0e-6);

    m.requestReset();
    CHECK (!m.hasData()); // a pending reset counts at once
    m.process (s.data(), s.data(), 480);
    CHECK_NEAR (m.seconds(), 0.01, 1.0e-6);
    CHECK (m.integrated() < -100.0f);
}

TEST (loudness_monitor_tracks_the_loudest_section)
{
    const double fs = 48000.0;
    LoudnessMonitor m;
    m.prepare (fs);
    const auto loud = sine (fs, 997, -12, 6.0), quiet = sine (fs, 997, -30, 6.0);
    m.process (loud.data(), loud.data(), static_cast<int> (loud.size()));
    m.process (quiet.data(), quiet.data(), static_cast<int> (quiet.size()));
    CHECK_NEAR (m.maxShortTerm(), -12.0, 0.2);
    CHECK (m.shortTerm() < -29.0f); // the last 3 s are the quiet part
    CHECK (m.integrated() > -14.0f); // the relative gate drops the quiet part from the integrated value
}

TEST (loudness_monitor_recovers_from_non_finite_samples)
{
    const double fs = 48000.0;
    LoudnessMonitor m;
    m.prepare (fs);
    const auto first = sine (fs, 997, -20, 5.0), second = sine (fs, 997, -10, 10.0);
    m.process (first.data(), first.data(), static_cast<int> (first.size()));
    const float before = m.integrated();
    CHECK_NEAR (before, -20.0, 0.2);

    // A burst of NaN and inf, as a host hands over on a glitch, must not freeze the meter: a NaN in the K-weighting
    // filters would stay there for good, leaving the short-term value NaN and the integrated one stuck.
    std::vector<float> bad (480, std::nanf (""));
    for (size_t i = 0; i < bad.size(); i += 2)
        bad[i] = std::numeric_limits<float>::infinity();
    m.process (bad.data(), bad.data(), static_cast<int> (bad.size()));
    m.process (second.data(), second.data(), static_cast<int> (second.size()));
    CHECK (m.hasData());
    CHECK (std::isfinite (m.shortTerm()));
    CHECK_NEAR (m.shortTerm(), -10.0, 0.3);
    CHECK (m.integrated() > before + 3.0f); // follows the louder passage that came after
}

TEST (loudness_monitor_reads_as_cleared_while_a_reset_is_pending)
{
    const double fs = 48000.0;
    LoudnessMonitor m;
    m.prepare (fs);
    const auto tone = sine (fs, 997, -14, 6.0);
    m.process (tone.data(), tone.data(), static_cast<int> (tone.size()));
    CHECK (m.hasData() && m.seconds() > 5.0 && m.maxShortTerm() > -20.0f);

    // No audio flows (a stopped host), yet every reader must already see the cleared meter, not the old programme: the
    // master plan counts these seconds, and a plan built on stale ones next to a cleared CLIPPING cell is the "stuck" UI.
    m.requestReset();
    CHECK (!m.hasData());
    CHECK (m.seconds() == 0.0);
    CHECK (m.integrated() <= LoudnessMeter::kSilence && m.shortTerm() <= LoudnessMeter::kSilence && m.maxShortTerm() <= LoudnessMeter::kSilence);

    m.process (tone.data(), tone.data(), 64);
    CHECK (m.seconds() > 0.0 && m.seconds() < 0.1);
}

// ---- master advisor ----------------------------------------------------------------------------------------------
namespace
{
// A programme that measures clean everywhere: mono-safe, no clips, at the delivery loudness.
MasterInputs cleanMaster()
{
    MasterInputs in;
    in.analysedSeconds = 120.0;
    in.haveLoudness = true;
    in.integratedLufs = -14.0f;
    in.clip.have = true;
    in.clip.truePeakDb = -1.5f;
    in.clip.samplePeakDb = -1.8f;
    in.translation.haveFold = true;
    in.haveCrest = true;
    in.lowCrestDb = 8.0f;
    return in;
}

const Advice* find (const MasterAdvice& a, Topic t)
{
    for (const auto& i : a.items)
        if (i.topic == t)
            return &i;
    return nullptr;
}

// The item about `t` for these inputs. Valid until the next call (the advice lives in a static).
const Advice* pick (const MasterInputs& in, Topic t)
{
    static MasterAdvice last;
    last = advise (in);
    return find (last, t);
}
} // namespace

TEST (advisor_has_nothing_to_say_about_a_clean_master)
{
    const auto a = advise (cleanMaster());
    CHECK (!a.measuring);
    CHECK (a.items.empty() && a.level() == Level::good);
    CHECK (a.checks >= 5 && a.passed == a.checks);
}

TEST (advisor_waits_for_enough_audio_but_still_reports_clipping)
{
    auto in = cleanMaster();
    in.analysedSeconds = 5.0;
    in.clip.clipEvents = 12;
    in.integratedLufs = -6.0f; // would be a loudness item
    in.lowCrestDb = 2.0f;
    const auto a = advise (in);
    CHECK (a.measuring);
    CHECK (a.items.size() == 1 && a.items[0].topic == Topic::clipping && a.items[0].level == Level::bad);
}

TEST (advisor_names_the_ceiling_move_for_clipping_and_hot_peaks)
{
    auto in = cleanMaster();
    in.clip.truePeakDb = 0.4f;
    in.clip.clipEvents = 30;
    auto* clip = pick (in, Topic::clipping);
    CHECK (clip != nullptr && clip->level == Level::bad && clip->headline.find ("lower the ceiling 1.4 dB") != std::string::npos);

    in.clip.clipEvents = 0;
    in.clip.truePeakDb = -0.4f; // 0.6 dB over the -1 dBTP ceiling
    clip = pick (in, Topic::clipping);
    CHECK (clip != nullptr && clip->level == Level::warn && clip->headline.find ("by 0.6 dB") != std::string::npos);

    in.clip.truePeakDb = -1.0f;
    CHECK (pick (in, Topic::clipping) == nullptr);

    in.clip.clipEvents = 5; // flat tops under the ceiling: nothing to lower, the clipper has to ease off
    in.clip.truePeakDb = -1.2f;
    clip = pick (in, Topic::clipping);
    CHECK (clip != nullptr && clip->level == Level::bad && clip->headline.find ("ease the clipper or limiter drive until") != std::string::npos);
}

TEST (advisor_compares_loudness_with_the_delivery_target)
{
    auto in = cleanMaster();
    in.integratedLufs = -8.0f; // 6 LU over a -14 LUFS target
    in.clip.truePeakDb = -1.0f;
    auto* l = pick (in, Topic::loudness);
    CHECK (l != nullptr && l->level == Level::bad);
    CHECK (l != nullptr && l->headline.rfind ("Take about 6.0 dB off", 0) == 0);
    CHECK (l != nullptr && l->why.find ("Streaming services") != std::string::npos);

    in.targetLufs = -9.0f; // a loud release: -8 LUFS is 1 LU over, fine
    in.deliveryName = "Loud";
    CHECK (pick (in, Topic::loudness) == nullptr);

    in.targetLufs = -14.0f;
    in.integratedLufs = -19.0f; // 5 LU under, with 5.5 dB of peak headroom... 4 dB of it is needed
    in.clip.truePeakDb = -7.0f;
    l = pick (in, Topic::loudness);
    CHECK (l != nullptr && l->level == Level::bad && l->headline.rfind ("Raise the level about 5.0 dB", 0) == 0);
    CHECK (l != nullptr && l->why.find ("plain gain") != std::string::npos);

    in.integratedLufs = -15.0f; // within 2 LU
    in.clip.truePeakDb = -1.5f;
    CHECK (pick (in, Topic::loudness) == nullptr);
}

TEST (advisor_speaks_for_each_stereo_problem)
{
    auto in = cleanMaster();
    in.translation.lowMonoLossDb = 2.0f;
    in.translation.lowMonoLevel = Level::bad;
    in.translation.monoLossDb = 3.0f;
    in.translation.monoLevel = Level::warn;
    in.translation.worstBand = 30;
    in.translation.worstBandLossDb = 8.0f;
    in.havePhase = true;
    in.phase.have = true;
    in.phase.level = Level::bad;
    in.phase.cause = PhaseCause::delay;
    in.phase.delayMs = 0.5f;
    in.phase.notchHz = 1000.0f;
    in.phase.worstLossDb = 19.0f;
    in.phase.loHz = 500.0;
    in.phase.hiHz = 1500.0;
    const auto a = advise (in);
    const auto* phase = find (a, Topic::phase);
    const auto* low = find (a, Topic::lowEndMono);
    const auto* fold = find (a, Topic::monoFold);
    CHECK (phase != nullptr && phase->headline.find ("0.50 ms") != std::string::npos && phase->why.find ("1.0 kHz") != std::string::npos);
    CHECK (low != nullptr && low->hear.solo == (Switches::bit (Source::side) | Switches::bit (Source::low)));
    CHECK (fold != nullptr && fold->level == Level::warn && fold->hear.solo == Switches::bit (Source::side));
    // Worst first: both bad items come before the warning.
    CHECK (a.items.size() >= 3 && a.items[0].level == Level::bad && a.items[1].level == Level::bad && a.items[2].level == Level::warn);
    CHECK (a.items[0].topic == Topic::phase); // phase is ahead of the low end in priority

    in.phase.cause = PhaseCause::polarity;
    CHECK (pick (in, Topic::phase)->headline.rfind ("Flip the polarity", 0) == 0);
}

TEST (advisor_reads_the_low_end_dynamics_and_the_history)
{
    auto in = cleanMaster();
    in.lowCrestDb = 4.0f; // 2 dB under the typical zone
    auto* crest = pick (in, Topic::lowCrest);
    CHECK (crest != nullptr && crest->headline.rfind ("Open up the low end", 0) == 0);
    in.lowCrestDb = 13.0f;
    crest = pick (in, Topic::lowCrest);
    CHECK (crest != nullptr && crest->headline.rfind ("Tame the low-end peaks", 0) == 0 && crest->hear.solo == Switches::bit (Source::low));

    in = cleanMaster();
    std::vector<FrameStats> frames (100);
    for (size_t i = 0; i < frames.size(); ++i)
        for (int s = 0; s < kNumSignals; ++s)
            for (int c = 0; c < kNumEnergyColumns; ++c)
                frames[i].energy (static_cast<Signal> (s), c) = -30.0f;
    for (auto& f : frames)
        f.energy (Signal::left, kAllRanges) = -28.0f; // left 2 dB louder than right
    in.frames = frames;
    auto a = advise (in);
    const auto* bal = find (a, Topic::balance);
    CHECK (bal != nullptr && bal->level == Level::bad && bal->headline.find ("left channel down about 2.0 dB") != std::string::npos);
    CHECK (find (a, Topic::swing) == nullptr && find (a, Topic::bass) == nullptr); // flat levels, no bass tracked

    for (size_t i = 0; i < frames.size(); ++i)
        frames[i].energy (Signal::stereo, 0) = i % 2 ? -20.0f : -40.0f; // the low range jumps by 20 dB
    in.frames = frames;
    a = advise (in);
    const auto* swing = find (a, Topic::swing);
    CHECK (swing != nullptr && swing->level == Level::warn); // a hint, never an alarm

    for (auto& f : frames)
    {
        f.f0Hz = 35.0f; // a bass that lives under 40 Hz
        f.fundamentalDb = -30.0f;
        f.harmonicsDb = -32.0f;
    }
    in.frames = frames;
    a = advise (in);
    const auto* bass = find (a, Topic::bass);
    CHECK (bass != nullptr && bass->headline.find ("under 40 Hz") != std::string::npos && bass->hear.solo == Switches::bit (Source::low));
}

TEST (advisor_lists_faults_not_differences_from_the_reference_curve)
{
    // The device preview is measured against the reference curve, so even a bad DEVICE LOSS is not a to-do; the same goes
    // for the tonal curve (there are no inputs for it any more). The mono fold is a fault and is still listed.
    auto in = cleanMaster();
    in.translation.haveDevice = true;
    in.translation.extraLossDb = 6.0f;
    in.translation.deviceLevel = Level::bad;
    const auto a = advise (in);
    CHECK (a.items.empty() && a.level() == Level::good);

    in.translation.monoLevel = Level::bad;
    in.translation.monoLossDb = 5.0f;
    CHECK (pick (in, Topic::monoFold) != nullptr);
}

TEST (mono_fold_report_needs_no_target_or_device)
{
    BandArray measured {}, fold {};
    for (int i = 0; i < BandGrid::kNum; ++i)
    {
        measured[static_cast<size_t> (i)] = -20.0f;
        fold[static_cast<size_t> (i)] = BandGrid::centreHz (i) < 100.0 ? -6.0f : 0.0f; // the low end cancels in mono
    }
    TranslationReport alone;
    addMonoFold (alone, measured, fold);
    const auto full = analyseTranslation (measured, &fold, TargetCurve::builtIns().front(), nullptr);
    CHECK (alone.haveFold && !alone.haveDevice);
    CHECK (alone.lowMonoLossDb > 3.0f && alone.lowMonoLevel != Level::good);
    CHECK (alone.monoLossDb == full.monoLossDb && alone.lowMonoLossDb == full.lowMonoLossDb && alone.worstBand == full.worstBand);
}

int main()
{
    for (const auto& t : tests())
    {
        const int before = g_failures;
        std::printf ("[ RUN  ] %s\n", t.name);
        t.fn();
        std::printf ("[ %s ] %s\n", g_failures == before ? " OK " : "FAIL", t.name);
    }
    std::printf ("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
