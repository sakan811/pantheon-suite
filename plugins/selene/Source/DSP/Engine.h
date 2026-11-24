// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 The Pantheon authors

#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <vector>

// Plain C++20 polyphonic synthesizer engine: no JUCE headers, no heap allocations in process().
namespace selene
{
inline constexpr int kMaxVoices = 16;
inline constexpr int kUnisonCount = 4;
inline constexpr int kScopeSamples = 512;
inline constexpr double kPi = 3.14159265358979323846;
inline constexpr double kTwoPi = 2.0 * kPi;

enum class VoiceMode { poly = 0, mono = 1, unison = 2 };
inline constexpr int kNumVoiceModes = 3;

enum class OscWave { saw = 0, pulse = 1, triangle = 2, sine = 3 };
inline constexpr int kNumOscWaves = 4;

enum class FilterMode { lp24 = 0, lp12 = 1, bp12 = 2, hp12 = 3 };
inline constexpr int kNumFilterModes = 4;

enum class LfoWave { sine = 0, triangle = 1, square = 2, sampleHold = 3 };
inline constexpr int kNumLfoWaves = 4;

enum class ChorusMode { off = 0, mode1 = 1, mode2 = 2, both = 3 };
inline constexpr int kNumChorusModes = 4;

struct MidiEvent
{
    enum class Type { noteOn, noteOff, pitchBend, controlChange, allNotesOff, allSoundOff };
    Type type = Type::noteOn;
    int sampleOffset = 0;
    int channel = 0;
    int data1 = 0;      // MIDI note (0..127) or CC number
    float value = 0.0f; // velocity (0..1), pitch bend (-1..1), or CC value (0..1)
};

struct EngineParams
{
    VoiceMode voiceMode = VoiceMode::poly;
    float glideMs = 0.0f;           // 0..500 ms
    float drift = 0.20f;            // 0..1

    OscWave osc1Wave = OscWave::saw;
    float osc1Pwm = 0.50f;          // 0.05..0.95
    float subLevel = 0.35f;         // 0..1

    OscWave osc2Wave = OscWave::saw;
    int osc2Semi = 0;               // -24..24
    float osc2DetuneCents = 8.0f;   // -50..50
    float osc2Fm = 0.0f;            // 0..1
    float oscMix = 0.45f;           // 0..1 (0 = Osc1, 1 = Osc2)
    float noiseLevel = 0.0f;        // 0..1

    FilterMode filterMode = FilterMode::lp24;
    float cutoffHz = 3600.0f;       // 20..20000
    float resonance = 0.18f;        // 0..0.98
    float filterEnvAmt = 0.35f;     // -1..1
    float keytrack = 0.50f;         // 0..1
    float drive = 0.15f;            // 0..1

    float ampAttackMs = 8.0f;
    float ampDecayMs = 280.0f;
    float ampSustain = 0.75f;
    float ampReleaseMs = 220.0f;
    float velAmp = 0.70f;

    float fltAttackMs = 12.0f;
    float fltDecayMs = 380.0f;
    float fltSustain = 0.35f;
    float fltReleaseMs = 250.0f;
    float velFlt = 0.55f;

    LfoWave lfoWave = LfoWave::sine;
    float lfoRateHz = 4.2f;         // 0.05..20
    float lfoPitchCents = 0.0f;     // 0..100
    float lfoFilter = 0.0f;         // 0..1
    float lfoPwm = 0.0f;            // 0..1

    ChorusMode chorusMode = ChorusMode::mode1;
    float echoTimeMs = 320.0f;      // 40..1000
    float echoFeedback = 0.28f;     // 0..0.85
    float echoMix = 0.12f;          // 0..1

    float masterGainDb = -3.0f;     // -24..12
};

namespace detail
{
inline double wrapPhase (double p) noexcept
{
    p -= std::floor (p);
    return p < 0.0 ? p + 1.0 : p;
}

inline double polyBlep (double t, double dt) noexcept
{
    if (dt <= 0.0 || dt >= 0.5)
        return 0.0;
    if (t < dt)
    {
        t /= dt;
        return t + t - t * t - 1.0;
    }
    if (t > 1.0 - dt)
    {
        t = (t - 1.0) / dt;
        return t * t + t + t + 1.0;
    }
    return 0.0;
}

inline double blampCorner (double t, double dt) noexcept
{
    if (dt <= 0.0 || dt >= 0.25)
        return 0.0;
    if (t < dt)
    {
        const double u = (dt - t) / dt;
        return (4.0 * dt / 3.0) * (u * u * u);
    }
    if (t > 1.0 - dt)
    {
        const double u = (t - (1.0 - dt)) / dt;
        return (4.0 * dt / 3.0) * (u * u * u);
    }
    return 0.0;
}

inline double evalOsc (OscWave wave, double phase, double dt, double pwm) noexcept
{
    const double t = wrapPhase (phase);
    switch (wave)
    {
        case OscWave::saw:
            return 2.0 * t - 1.0 - polyBlep (t, dt);
        case OscWave::pulse:
        {
            const double pw = std::clamp (pwm, 0.05, 0.95);
            double v = (t < pw ? 1.0 : -1.0) - 2.0 * (pw - 0.5);
            v += polyBlep (t, dt);
            v -= polyBlep (wrapPhase (t + 1.0 - pw), dt);
            return v;
        }
        case OscWave::triangle:
        {
            // Sharp triangle: -1 at t=0, +1 at t=0.5, -1 at t=1, smoothed at both corners with PolyBLAMP.
            const double tri = 1.0 - 4.0 * std::fabs (t - 0.5);
            const double c0 = blampCorner (t, dt);
            const double c1 = blampCorner (wrapPhase (t + 0.5), dt);
            return tri + c0 - c1;
        }
        case OscWave::sine:
        default:
            return std::sin (kTwoPi * t);
    }
}

inline double svfDamping (FilterMode mode, double resonance) noexcept
{
    const double r = std::clamp (resonance, 0.0, 0.98);
    if (mode == FilterMode::lp24)
        return std::max (0.12, 2.0 * (1.0 - 0.92 * r));
    return std::max (0.06, 2.0 * (1.0 - 0.96 * r));
}

struct SvfStage
{
    double ic1eq = 0.0;
    double ic2eq = 0.0;

    void reset() noexcept
    {
        ic1eq = 0.0;
        ic2eq = 0.0;
    }

    void process (double v0, double g, double k, double driveAmt, double& lp, double& bp, double& hp) noexcept
    {
        const double a1 = 1.0 / (1.0 + g * (g + k));
        const double a2 = g * a1;
        const double a3 = g * a2;
        const double v3 = v0 - ic2eq;
        const double v1 = a1 * ic1eq + a2 * v3;
        const double v2 = ic2eq + a2 * ic1eq + a3 * v3;
        const double sat = 1.0 + 1.5 * driveAmt;
        ic1eq = std::tanh ((2.0 * v1 - ic1eq) * sat) / sat;
        ic2eq = 2.0 * v2 - ic2eq;
        lp = v2;
        bp = v1;
        hp = v0 - k * v1 - v2;
    }
};

struct Adsr
{
    enum class Stage { idle, attack, decay, sustain, release };

    Stage stage = Stage::idle;
    double level = 0.0;

    void reset() noexcept
    {
        stage = Stage::idle;
        level = 0.0;
    }

    void gateOn (bool retrigger) noexcept
    {
        if (retrigger || stage == Stage::idle || stage == Stage::release)
            stage = Stage::attack;
    }

    void gateOff() noexcept
    {
        if (stage != Stage::idle)
            stage = Stage::release;
    }

    double next (double sampleRate, float attackMs, float decayMs, float sustainLevel, float releaseMs) noexcept
    {
        const double fs = std::max (sampleRate, 1000.0);
        const double sus = std::clamp ((double) sustainLevel, 0.0, 1.0);
        switch (stage)
        {
            case Stage::idle:
                level = 0.0;
                return 0.0;
            case Stage::attack:
            {
                const double sec = std::max ((double) attackMs * 0.001, 0.0005);
                // Exponential rise toward 1.25 reaches 1.0 in ~1.61 * tau; scaling tau by 0.62 matches attackMs closely.
                const double coeff = 1.0 - std::exp (-1.0 / (sec * 0.62 * fs));
                level += (1.25 - level) * coeff;
                if (level >= 1.0)
                {
                    level = 1.0;
                    stage = Stage::decay;
                }
                return level;
            }
            case Stage::decay:
            {
                const double sec = std::max ((double) decayMs * 0.001, 0.001);
                const double coeff = std::exp (-1.0 / (sec * 0.35 * fs));
                level = sus + (level - sus) * coeff;
                if (std::fabs (level - sus) < 1e-4)
                {
                    level = sus;
                    stage = Stage::sustain;
                }
                return level;
            }
            case Stage::sustain:
                level = sus;
                return level;
            case Stage::release:
            {
                const double sec = std::max ((double) releaseMs * 0.001, 0.001);
                const double coeff = std::exp (-1.0 / (sec * 0.28 * fs));
                level *= coeff;
                if (level < 1e-5)
                {
                    level = 0.0;
                    stage = Stage::idle;
                }
                return level;
            }
        }
        return 0.0;
    }
};
} // namespace detail

// Small-signal magnitude response of the ZDF SVF at freqHz, shared between the UI filter display and DSP tests.
inline double filterMagnitudeAt (const EngineParams& p, double freqHz, double sampleRate = 48000.0) noexcept
{
    const double fs = std::max (sampleRate, 8000.0);
    const double nyq = fs * 0.45;
    const double fc = std::clamp ((double) p.cutoffHz, 20.0, nyq);
    const double f = std::clamp (freqHz, 1.0, nyq);
    const double g = std::tan (kPi * fc / fs);
    const double w = std::tan (kPi * f / fs) / std::max (g, 1e-9);
    const double k = detail::svfDamping (p.filterMode, (double) p.resonance);
    const double w2 = w * w;
    const double re = 1.0 - w2;
    const double im = k * w;
    const double denom = std::sqrt (re * re + im * im);

    switch (p.filterMode)
    {
        case FilterMode::lp24:
        {
            const double m2 = 1.0 / std::max (denom, 1e-9);
            return m2 * m2;
        }
        case FilterMode::lp12:
            return 1.0 / std::max (denom, 1e-9);
        case FilterMode::bp12:
            return w / std::max (denom, 1e-9);
        case FilterMode::hp12:
            return w2 / std::max (denom, 1e-9);
    }
    return 1.0;
}

class Engine
{
public:
    std::atomic<int> activeVoices { 0 };
    std::atomic<float> outputPeak { 0.0f };

    Engine()
    {
        prepare (48000.0, 512);
    }

    void prepare (double newSampleRate, int maxBlockSize = 512)
    {
        (void) maxBlockSize;
        sampleRate = std::max (newSampleRate, 8000.0);
        const int echoSamples = (int) std::ceil (1.25 * sampleRate) + 8;
        echoBufL.assign ((size_t) echoSamples, 0.0f);
        echoBufR.assign ((size_t) echoSamples, 0.0f);
        reset();
    }

    void reset() noexcept
    {
        for (size_t i = 0; i < voices.size(); ++i)
        {
            voices[i].reset();
            // Deterministic per-voice slow drift starting phases
            voices[i].driftPhaseA = detail::wrapPhase ((double) i * 0.1731 + 0.11);
            voices[i].driftPhaseB = detail::wrapPhase ((double) i * 0.3179 + 0.43);
        }
        monoHeldCount = 0;
        sustainPedal = false;
        modWheel = 0.0f;
        pitchBendSemis = 0.0f;
        lastMonoNote = -1;
        lastPolyNote = 60.0;
        hasPlayedPoly = false;
        nextTriggerOrder = 1;
        nextGroupId = 1;
        lfoPhase = 0.0;
        lfoShValue = 0.0;
        lfoPrevPhase = 0.0;
        noiseRng = 0x9e3779b9u;

        chorusBufL.fill (0.0f);
        chorusBufR.fill (0.0f);
        chorusWrite = 0;
        chorusPhase1 = 0.0;
        chorusPhase2 = 0.0;
        chorusLpL = 0.0;
        chorusLpR = 0.0;
        activeChorusMode = ChorusMode::mode1;
        smoothChorusWet = 0.0;

        std::fill (echoBufL.begin(), echoBufL.end(), 0.0f);
        std::fill (echoBufR.begin(), echoBufR.end(), 0.0f);
        echoWrite = 0;
        echoLpL = 0.0;
        echoLpR = 0.0;
        echoHpL = 0.0;
        echoHpR = 0.0;
        prevEchoLpL = 0.0;
        prevEchoLpR = 0.0;
        smoothEchoFeedback = 0.0;
        smoothEchoMix = 0.0;

        scopeBuffer.fill (0.0f);
        scopeWritePos.store (0, std::memory_order_relaxed);
        activeVoices.store (0, std::memory_order_relaxed);
        outputPeak.store (0.0f, std::memory_order_relaxed);
        smoothedInited = false;
    }

    void allNotesOff() noexcept
    {
        sustainPedal = false;
        monoHeldCount = 0;
        for (auto& v : voices)
        {
            if (v.active)
            {
                v.keyHeld = false;
                v.sustained = false;
                v.ampEnv.gateOff();
                v.fltEnv.gateOff();
            }
        }
    }

    void allSoundOff() noexcept
    {
        sustainPedal = false;
        monoHeldCount = 0;
        for (auto& v : voices)
            v.reset();
        activeVoices.store (0, std::memory_order_relaxed);
    }

    void noteOn (int channel, int note, float velocity, VoiceMode mode = VoiceMode::poly, float glideMs = 0.0f) noexcept
    {
        const int clampedNote = std::clamp (note, 0, 127);
        const float vel = std::clamp (velocity, 0.0f, 1.0f);
        if (vel <= 0.0f)
        {
            noteOff (channel, clampedNote, mode);
            return;
        }

        if (mode == VoiceMode::mono)
        {
            const bool wasHeld = monoHeldCount > 0 || (voices[0].active && voices[0].sustained);
            // Remove duplicate if already present, then push to back (last-note priority)
            eraseMonoHeldNote (channel, clampedNote);
            if (monoHeldCount >= (int) monoHeldNotes.size())
            {
                for (int j = 0; j + 1 < monoHeldCount; ++j)
                    monoHeldNotes[(size_t) j] = monoHeldNotes[(size_t) (j + 1)];
                --monoHeldCount;
            }
            monoHeldNotes[(size_t) monoHeldCount++] = { channel, clampedNote, vel };

            //Silence any other voices if switched from poly/unison
            for (size_t i = 1; i < voices.size(); ++i)
                voices[i].reset();

            Voice& v = voices[0];
            const bool retrigger = ! v.active || v.ampEnv.stage == detail::Adsr::Stage::idle || (! wasHeld);
            if (! v.active || lastMonoNote < 0)
            {
                v.currentMidiNote = (double) clampedNote;
                v.phase1 = 0.0;
                v.phase2 = 0.0;
                v.subPhase = 0.0;
                v.svf1.reset();
                v.svf2.reset();
            }
            v.active = true;
            v.keyHeld = true;
            v.sustained = false;
            v.channel = channel;
            v.note = clampedNote;
            v.velocity = vel;
            v.targetMidiNote = (double) clampedNote;
            v.glideCoeff = computeGlideCoeff (glideMs);
            v.panL = 0.70710678f;
            v.panR = 0.70710678f;
            v.unisonDetuneCents = 0.0;
            v.triggerOrder = nextTriggerOrder++;
            v.noteGroup = nextGroupId++;
            v.ampEnv.gateOn (retrigger);
            v.fltEnv.gateOn (retrigger);
            lastMonoNote = clampedNote;
            return;
        }

        monoHeldCount = 0;

        if (mode == VoiceMode::unison)
        {
            // Release any previous held voices with same note? Allow retrigger by stealing or allocating a 4-voice group.
            const uint64_t group = nextGroupId++;
            static constexpr std::array<double, kUnisonCount> kDetuneOffsets { -1.0, -0.33, 0.33, 1.0 };
            static constexpr std::array<double, kUnisonCount> kPanPositions { -0.82, -0.28, 0.28, 0.82 };
            const double startNote = hasPlayedPoly && glideMs > 0.0f ? lastPolyNote : (double) clampedNote;

            for (int u = 0; u < kUnisonCount; ++u)
            {
                const int idx = allocateVoiceForUnison (u);
                Voice& v = voices[(size_t) idx];
                v.reset();
                v.active = true;
                v.keyHeld = true;
                v.sustained = false;
                v.channel = channel;
                v.note = clampedNote;
                v.velocity = vel;
                v.currentMidiNote = startNote;
                v.targetMidiNote = (double) clampedNote;
                v.glideCoeff = computeGlideCoeff (glideMs);
                // Slight deterministic initial phase spread in unison so voices don't spike at t=0
                v.phase1 = (double) u * 0.25;
                v.phase2 = (double) u * 0.17;
                v.subPhase = (double) u * 0.25;
                const double panAngle = (kPanPositions[(size_t) u] * 0.5 + 0.5) * (kPi * 0.5);
                v.panL = (float) std::cos (panAngle);
                v.panR = (float) std::sin (panAngle);
                v.unisonDetuneCents = kDetuneOffsets[(size_t) u] * 14.0;
                v.triggerOrder = nextTriggerOrder++;
                v.noteGroup = group;
                v.ampEnv.gateOn (true);
                v.fltEnv.gateOn (true);
            }
            lastPolyNote = (double) clampedNote;
            hasPlayedPoly = true;
            return;
        }

        // Poly mode
        const int idx = allocateVoiceForPoly (channel, clampedNote);
        Voice& v = voices[(size_t) idx];
        const double startNote = hasPlayedPoly && glideMs > 0.0f ? lastPolyNote : (double) clampedNote;
        v.reset();
        v.active = true;
        v.keyHeld = true;
        v.sustained = false;
        v.channel = channel;
        v.note = clampedNote;
        v.velocity = vel;
        v.currentMidiNote = startNote;
        v.targetMidiNote = (double) clampedNote;
        v.glideCoeff = computeGlideCoeff (glideMs);
        v.panL = 0.70710678f;
        v.panR = 0.70710678f;
        v.unisonDetuneCents = 0.0;
        v.triggerOrder = nextTriggerOrder++;
        v.noteGroup = nextGroupId++;
        v.ampEnv.gateOn (true);
        v.fltEnv.gateOn (true);
        lastPolyNote = (double) clampedNote;
        hasPlayedPoly = true;
    }

    void noteOff (int channel, int note, VoiceMode mode = VoiceMode::poly) noexcept
    {
        const int clampedNote = std::clamp (note, 0, 127);
        eraseMonoHeldNote (channel, clampedNote);

        const auto releaseVoice = [this] (Voice& rv) noexcept
        {
            rv.keyHeld = false;
            if (sustainPedal)
                rv.sustained = true;
            else
            {
                rv.sustained = false;
                rv.ampEnv.gateOff();
                rv.fltEnv.gateOff();
            }
        };

        if (mode == VoiceMode::mono)
        {
            Voice& v = voices[0];
            if (v.active)
            {
                if (monoHeldCount > 0)
                {
                    // Legato return to previous held note
                    const auto& prev = monoHeldNotes[(size_t) (monoHeldCount - 1)];
                    v.channel = prev.channel;
                    v.note = prev.note;
                    v.targetMidiNote = (double) prev.note;
                    v.velocity = prev.velocity;
                    v.keyHeld = true;
                    v.sustained = false;
                    lastMonoNote = prev.note;
                }
                else
                {
                    releaseVoice (v);
                }
            }

            // Gate off any leftover poly/unison voices when switching to Mono while keys are held
            for (size_t i = 1; i < voices.size(); ++i)
            {
                Voice& pv = voices[i];
                if (pv.active && pv.keyHeld && ((pv.channel == channel && pv.note == clampedNote) || monoHeldCount == 0))
                    releaseVoice (pv);
            }
            return;
        }

        for (auto& v : voices)
        {
            if (v.active && v.keyHeld && v.channel == channel && v.note == clampedNote)
                releaseVoice (v);
        }
    }

    void process (float* outL, float* outR, int numSamples, const EngineParams& params) noexcept
    {
        process (outL, outR, numSamples, nullptr, 0, params);
    }

    void process (float* outL, float* outR, int numSamples, const MidiEvent* events, int numEvents, const EngineParams& params) noexcept
    {
        if (outL == nullptr || outR == nullptr || numSamples <= 0 || echoBufL.empty())
            return;

        std::fill (outL, outL + numSamples, 0.0f);
        std::fill (outR, outR + numSamples, 0.0f);

        if (! smoothedInited)
        {
            smoothCutoff = params.cutoffHz;
            smoothEchoMs = params.echoTimeMs;
            smoothMasterGain = std::pow (10.0f, std::clamp (params.masterGainDb, -24.0f, 12.0f) / 20.0f);
            smoothChorusWet = chorusTargetWet (params.chorusMode);
            smoothEchoFeedback = std::clamp ((double) params.echoFeedback, 0.0, 0.85);
            smoothEchoMix = std::clamp ((double) params.echoMix, 0.0, 1.0);
            smoothedInited = true;
        }

        const double fs = sampleRate;
        const double nyq = fs * 0.45;
        const double paramSmooth = 1.0 - std::exp (-1.0 / (0.005 * fs));
        const double fxSmooth = 1.0 - std::exp (-1.0 / (0.015 * fs));
        const double echoTimeSmooth = 1.0 - std::exp (-1.0 / (0.025 * fs));
        const float targetMasterGain = std::pow (10.0f, std::clamp (params.masterGainDb, -24.0f, 12.0f) / 20.0f);
        const double targetChorusWet = chorusTargetWet (params.chorusMode);
        const double targetEchoFeedback = std::clamp ((double) params.echoFeedback, 0.0, 0.85);
        const double targetEchoMix = std::clamp ((double) params.echoMix, 0.0, 1.0);

        int eventIdx = 0;
        float blockPeak = 0.0f;

        for (int i = 0; i < numSamples; ++i)
        {
            while (events != nullptr && eventIdx < numEvents && events[eventIdx].sampleOffset <= i)
            {
                handleMidi (events[eventIdx], params);
                ++eventIdx;
            }

            smoothCutoff += ((double) params.cutoffHz - smoothCutoff) * paramSmooth;
            smoothEchoMs += ((double) params.echoTimeMs - smoothEchoMs) * echoTimeSmooth;
            smoothMasterGain += (targetMasterGain - smoothMasterGain) * (float) paramSmooth;
            smoothChorusWet += (targetChorusWet - smoothChorusWet) * fxSmooth;
            smoothEchoFeedback += (targetEchoFeedback - smoothEchoFeedback) * fxSmooth;
            smoothEchoMix += (targetEchoMix - smoothEchoMix) * fxSmooth;

            // Global LFO
            const double lfoDt = std::clamp ((double) params.lfoRateHz, 0.05, 20.0) / fs;
            lfoPhase = detail::wrapPhase (lfoPhase + lfoDt);
            if (lfoPhase < lfoPrevPhase)
                lfoShValue = nextRandomBipolar();
            lfoPrevPhase = lfoPhase;

            double lfoVal = 0.0;
            switch (params.lfoWave)
            {
                case LfoWave::sine:
                    lfoVal = std::sin (kTwoPi * lfoPhase);
                    break;
                case LfoWave::triangle:
                    lfoVal = 1.0 - 4.0 * std::fabs (lfoPhase - 0.5);
                    break;
                case LfoWave::square:
                    lfoVal = lfoPhase < 0.5 ? 1.0 : -1.0;
                    break;
                case LfoWave::sampleHold:
                    lfoVal = lfoShValue;
                    break;
            }

            // Mod wheel adds vibrato + subtle filter shimmer
            const double effectivePitchLfoCents = (double) params.lfoPitchCents + (double) modWheel * 35.0;
            const double effectiveFilterLfo = std::clamp ((double) params.lfoFilter + (double) modWheel * 0.25, 0.0, 1.0);
            const double lfoPitchSemis = (lfoVal * effectivePitchLfoCents) / 100.0;
            const double lfoPwmOffset = lfoVal * (double) params.lfoPwm * 0.35;
            const double osc1Pwm = std::clamp ((double) params.osc1Pwm + lfoPwmOffset, 0.05, 0.95);
            const double osc2OffsetSemis = (double) params.osc2Semi + (double) params.osc2DetuneCents / 100.0;
            const double fmDepth = std::clamp ((double) params.osc2Fm, 0.0, 1.0) * 0.45;
            const double oscMix = std::clamp ((double) params.oscMix, 0.0, 1.0);
            const double subLvl = std::clamp ((double) params.subLevel, 0.0, 1.0);
            const double noiseLvl = std::clamp ((double) params.noiseLevel, 0.0, 1.0);
            const double driveAmt = std::clamp ((double) params.drive, 0.0, 1.0);
            const double preGain = 1.0 + driveAmt * 2.5;
            const double k = detail::svfDamping (params.filterMode, (double) params.resonance);
            const double voiceGainScale = (params.voiceMode == VoiceMode::unison) ? 0.22 : 0.32;

            double sumL = 0.0;
            double sumR = 0.0;

            for (auto& v : voices)
            {
                if (! v.active)
                    continue;

                const double ampEnv = v.ampEnv.next (fs, params.ampAttackMs, params.ampDecayMs, params.ampSustain, params.ampReleaseMs);
                if (v.ampEnv.stage == detail::Adsr::Stage::idle)
                {
                    v.reset();
                    continue;
                }
                const double fltEnv = v.fltEnv.next (fs, params.fltAttackMs, params.fltDecayMs, params.fltSustain, params.fltReleaseMs);

                // Portamento / glide
                v.currentMidiNote += (v.targetMidiNote - v.currentMidiNote) * v.glideCoeff;

                // Per-voice slow analog drift
                v.driftPhaseA = detail::wrapPhase (v.driftPhaseA + 0.19 / fs);
                v.driftPhaseB = detail::wrapPhase (v.driftPhaseB + 0.31 / fs);
                const double driftSig = 0.6 * std::sin (kTwoPi * v.driftPhaseA) + 0.4 * std::sin (kTwoPi * v.driftPhaseB);
                const double driftCents = driftSig * 10.0 * (double) params.drift;
                const double driftCutoffOct = driftSig * 0.10 * (double) params.drift;

                const double baseNote = v.currentMidiNote + (double) pitchBendSemis + lfoPitchSemis + (v.unisonDetuneCents + driftCents) / 100.0;
                const double freq1 = std::clamp (440.0 * std::pow (2.0, (baseNote - 69.0) / 12.0), 8.0, nyq);
                const double freq2 = std::clamp (440.0 * std::pow (2.0, (baseNote + osc2OffsetSemis - 69.0) / 12.0), 8.0, nyq);
                const double subFreq = freq1 * 0.5;

                const double dt1 = freq1 / fs;
                const double dt2 = freq2 / fs;
                const double dtSub = subFreq / fs;

                v.phase1 = detail::wrapPhase (v.phase1 + dt1);
                v.phase2 = detail::wrapPhase (v.phase2 + dt2);
                v.subPhase = detail::wrapPhase (v.subPhase + dtSub);

                const double o2 = detail::evalOsc (params.osc2Wave, v.phase2, dt2, 0.5);
                const double o1 = detail::evalOsc (params.osc1Wave, v.phase1 + fmDepth * o2, dt1, osc1Pwm);

                // Warm rounded square/sine sub blend at -1 octave
                const double subSquare = detail::evalOsc (OscWave::pulse, v.subPhase, dtSub, 0.5);
                const double subSine = std::sin (kTwoPi * v.subPhase);
                const double sub = 0.55 * subSquare + 0.45 * subSine;

                const double noise = nextRandomBipolar();
                double sig = (1.0 - oscMix) * o1 + oscMix * o2 + subLvl * 0.65 * sub + noiseLvl * 0.45 * noise;

                if (driveAmt > 0.001)
                    sig = std::tanh (sig * preGain) / std::tanh (preGain);

                // Cutoff calculation: base + keytrack + filter env (vel-scaled) + LFO + drift
                const double velFltScale = (1.0 - (double) params.velFlt) + (double) params.velFlt * (double) v.velocity;
                const double envOct = (double) params.filterEnvAmt * fltEnv * velFltScale * 5.0;
                const double keyOct = (double) params.keytrack * ((v.currentMidiNote - 60.0) / 12.0);
                const double lfoOct = lfoVal * effectiveFilterLfo * 2.5;
                const double fc = std::clamp (smoothCutoff * std::pow (2.0, envOct + keyOct + lfoOct + driftCutoffOct), 20.0, nyq);
                const double g = std::tan (kPi * fc / fs);

                double lp1 = 0.0, bp1 = 0.0, hp1 = 0.0;
                v.svf1.process (sig, g, k, driveAmt, lp1, bp1, hp1);

                double filtered = lp1;
                switch (params.filterMode)
                {
                    case FilterMode::lp24:
                    {
                        double lp2 = 0.0, bp2 = 0.0, hp2 = 0.0;
                        v.svf2.process (lp1, g, k, driveAmt, lp2, bp2, hp2);
                        filtered = lp2;
                        break;
                    }
                    case FilterMode::lp12:
                        filtered = lp1;
                        break;
                    case FilterMode::bp12:
                        filtered = bp1;
                        break;
                    case FilterMode::hp12:
                        filtered = hp1;
                        break;
                }

                const double velAmpScale = (1.0 - (double) params.velAmp) + (double) params.velAmp * (double) v.velocity;
                const double vOut = filtered * ampEnv * velAmpScale * voiceGainScale;
                sumL += vOut * (double) v.panL;
                sumR += vOut * (double) v.panR;
            }

            // Drain any remaining MIDI events at sample >= numSamples? Handled below after loop.

            // BBD Stereo Chorus
            processChorus (sumL, sumR, params.chorusMode, smoothChorusWet, fs);

            // Stereo Tape Echo
            processEcho (sumL, sumR, smoothEchoMs, smoothEchoFeedback, smoothEchoMix, fs);

            // Master gain + NaN/Inf sanitization
            float sampleL = (float) (sumL * (double) smoothMasterGain);
            float sampleR = (float) (sumR * (double) smoothMasterGain);
            if (! std::isfinite (sampleL)) sampleL = 0.0f;
            if (! std::isfinite (sampleR)) sampleR = 0.0f;

            outL[i] = sampleL;
            outR[i] = sampleR;

            const float absMax = std::max (std::fabs (sampleL), std::fabs (sampleR));
            if (absMax > blockPeak)
                blockPeak = absMax;

            const int wp = scopeWritePos.load (std::memory_order_relaxed);
            scopeBuffer[(size_t) wp] = 0.5f * (sampleL + sampleR);
            scopeWritePos.store ((wp + 1) % kScopeSamples, std::memory_order_relaxed);
        }

        // Handle any events whose sampleOffset >= numSamples
        while (events != nullptr && eventIdx < numEvents)
        {
            handleMidi (events[eventIdx], params);
            ++eventIdx;
        }

        int count = 0;
        for (const auto& v : voices)
            if (v.active)
                ++count;
        activeVoices.store (count, std::memory_order_relaxed);
        outputPeak.store (blockPeak, std::memory_order_relaxed);
    }

    void copyScope (float* dest, int count) const noexcept
    {
        if (dest == nullptr || count <= 0)
            return;
        const int n = std::min (count, kScopeSamples);
        const int wp = scopeWritePos.load (std::memory_order_relaxed);
        const int start = (wp - n + kScopeSamples) % kScopeSamples;
        for (int i = 0; i < n; ++i)
            dest[i] = scopeBuffer[(size_t) ((start + i) % kScopeSamples)];
    }

private:
    struct Voice
    {
        bool active = false;
        bool keyHeld = false;
        bool sustained = false;
        int channel = 0;
        int note = 60;
        float velocity = 0.0f;
        uint64_t triggerOrder = 0;
        uint64_t noteGroup = 0;
        double currentMidiNote = 60.0;
        double targetMidiNote = 60.0;
        double glideCoeff = 1.0;
        double phase1 = 0.0;
        double phase2 = 0.0;
        double subPhase = 0.0;
        float panL = 0.70710678f;
        float panR = 0.70710678f;
        double unisonDetuneCents = 0.0;
        double driftPhaseA = 0.0;
        double driftPhaseB = 0.0;
        detail::Adsr ampEnv;
        detail::Adsr fltEnv;
        detail::SvfStage svf1;
        detail::SvfStage svf2;

        void reset() noexcept
        {
            active = false;
            keyHeld = false;
            sustained = false;
            channel = 0;
            note = 60;
            velocity = 0.0f;
            triggerOrder = 0;
            noteGroup = 0;
            currentMidiNote = 60.0;
            targetMidiNote = 60.0;
            glideCoeff = 1.0;
            phase1 = 0.0;
            phase2 = 0.0;
            subPhase = 0.0;
            panL = 0.70710678f;
            panR = 0.70710678f;
            unisonDetuneCents = 0.0;
            ampEnv.reset();
            fltEnv.reset();
            svf1.reset();
            svf2.reset();
        }
    };

    struct HeldMonoNote
    {
        int channel = 0;
        int note = 60;
        float velocity = 0.0f;
    };

    static constexpr int kChorusDelaySize = 8192;

    void eraseMonoHeldNote (int channel, int note) noexcept
    {
        for (int i = 0; i < monoHeldCount;)
        {
            if (monoHeldNotes[(size_t) i].note == note && monoHeldNotes[(size_t) i].channel == channel)
            {
                for (int j = i; j + 1 < monoHeldCount; ++j)
                    monoHeldNotes[(size_t) j] = monoHeldNotes[(size_t) (j + 1)];
                --monoHeldCount;
            }
            else
                ++i;
        }
    }

    static double chorusTargetWet (ChorusMode mode) noexcept
    {
        switch (mode)
        {
            case ChorusMode::off: return 0.0;
            case ChorusMode::mode1: return 0.45;
            case ChorusMode::mode2: return 0.50;
            case ChorusMode::both: return 0.55;
        }
        return 0.0;
    }

    double computeGlideCoeff (float glideMs) const noexcept
    {
        if (glideMs <= 0.1f)
            return 1.0;
        return 1.0 - std::exp (-1.0 / ((double) glideMs * 0.00035 * sampleRate));
    }

    int allocateVoiceForPoly (int channel, int note) noexcept
    {
        // Re-use releasing voice of the same note if present? Prefer truly idle first.
        for (int i = 0; i < kMaxVoices; ++i)
            if (! voices[(size_t) i].active)
                return i;

        (void) channel;
        (void) note;

        // Steal releasing voice with lowest amp level first (tie-break within 1e-5 by older triggerOrder)
        int bestRelease = -1;
        double minLevel = 1e9;
        uint64_t bestOrder = UINT64_MAX;
        for (int i = 0; i < kMaxVoices; ++i)
        {
            const auto& v = voices[(size_t) i];
            if (v.ampEnv.stage == detail::Adsr::Stage::release)
            {
                const bool lowerLevel = v.ampEnv.level < minLevel - 1e-5;
                const bool tiedOlder = std::fabs (v.ampEnv.level - minLevel) <= 1e-5 && v.triggerOrder < bestOrder;
                if (bestRelease < 0 || lowerLevel || tiedOlder)
                {
                    minLevel = v.ampEnv.level;
                    bestOrder = v.triggerOrder;
                    bestRelease = i;
                }
            }
        }
        if (bestRelease >= 0)
            return bestRelease;

        // Otherwise steal oldest active voice
        int oldest = 0;
        uint64_t oldestOrder = voices[0].triggerOrder;
        for (int i = 1; i < kMaxVoices; ++i)
        {
            if (voices[(size_t) i].triggerOrder < oldestOrder)
            {
                oldestOrder = voices[(size_t) i].triggerOrder;
                oldest = i;
            }
        }
        return oldest;
    }

    bool stealOldestGroup() noexcept
    {
        bool foundRelease = false;
        uint64_t releaseGroup = 0;
        uint64_t oldestReleaseOrder = UINT64_MAX;
        bool foundAny = false;
        uint64_t anyGroup = 0;
        uint64_t oldestAnyOrder = UINT64_MAX;

        for (const auto& v : voices)
        {
            if (! v.active)
                continue;

            if (v.ampEnv.stage == detail::Adsr::Stage::release)
            {
                if (! foundRelease || v.triggerOrder < oldestReleaseOrder)
                {
                    foundRelease = true;
                    oldestReleaseOrder = v.triggerOrder;
                    releaseGroup = v.noteGroup;
                }
            }

            if (! foundAny || v.triggerOrder < oldestAnyOrder)
            {
                foundAny = true;
                oldestAnyOrder = v.triggerOrder;
                anyGroup = v.noteGroup;
            }
        }

        if (! foundAny)
            return false;

        const uint64_t victimGroup = foundRelease ? releaseGroup : anyGroup;
        for (auto& v : voices)
        {
            if (v.active && v.noteGroup == victimGroup)
                v.reset();
        }
        return true;
    }

    int allocateVoiceForUnison (int slotInGroup) noexcept
    {
        auto countIdle = [this]() noexcept
        {
            int n = 0;
            for (const auto& v : voices)
                if (! v.active)
                    ++n;
            return n;
        };

        if (slotInGroup == 0)
        {
            while (countIdle() < kUnisonCount)
                if (! stealOldestGroup())
                    break;
        }

        for (int i = 0; i < kMaxVoices; ++i)
            if (! voices[(size_t) i].active)
                return i;

        if (stealOldestGroup())
        {
            for (int i = 0; i < kMaxVoices; ++i)
                if (! voices[(size_t) i].active)
                    return i;
        }

        return 0;
    }

    void handleMidi (const MidiEvent& ev, const EngineParams& params) noexcept
    {
        switch (ev.type)
        {
            case MidiEvent::Type::noteOn:
                noteOn (ev.channel, ev.data1, ev.value, params.voiceMode, params.glideMs);
                break;
            case MidiEvent::Type::noteOff:
                noteOff (ev.channel, ev.data1, params.voiceMode);
                break;
            case MidiEvent::Type::pitchBend:
                pitchBendSemis = std::clamp (ev.value, -1.0f, 1.0f) * 2.0f;
                break;
            case MidiEvent::Type::controlChange:
                if (ev.data1 == 1)
                    modWheel = std::clamp (ev.value, 0.0f, 1.0f);
                else if (ev.data1 == 64)
                    setSustainPedal (ev.value >= 0.5f, params.voiceMode);
                else if (ev.data1 == 120)
                    allSoundOff();
                else if (ev.data1 == 123)
                    allNotesOff();
                break;
            case MidiEvent::Type::allNotesOff:
                allNotesOff();
                break;
            case MidiEvent::Type::allSoundOff:
                allSoundOff();
                break;
        }
    }

    void setSustainPedal (bool down, VoiceMode mode) noexcept
    {
        sustainPedal = down;
        if (! down)
        {
            if (mode == VoiceMode::mono && monoHeldCount > 0)
                return;
            for (auto& v : voices)
            {
                if (v.active && v.sustained && ! v.keyHeld)
                {
                    v.sustained = false;
                    v.ampEnv.gateOff();
                    v.fltEnv.gateOff();
                }
            }
        }
    }

    double nextRandomBipolar() noexcept
    {
        noiseRng = noiseRng * 1664525u + 1013904223u;
        const int32_t signedVal = (int32_t) noiseRng;
        return (double) signedVal / 2147483648.0;
    }

    static double readDelayLinear (const float* buf, int size, int writePos, double delaySamples) noexcept
    {
        const double d = std::clamp (delaySamples, 1.0, (double) (size - 2));
        double readPos = (double) writePos - d;
        while (readPos < 0.0)
            readPos += (double) size;
        const int i0 = ((int) readPos) % size;
        const int i1 = (i0 + 1) % size;
        const double frac = readPos - std::floor (readPos);
        return (double) buf[i0] * (1.0 - frac) + (double) buf[i1] * frac;
    }

    void processChorus (double& inL, double& inR, ChorusMode mode, double wetAmt, double fs) noexcept
    {
        chorusBufL[(size_t) chorusWrite] = (float) inL;
        chorusBufR[(size_t) chorusWrite] = (float) inR;

        chorusPhase1 = detail::wrapPhase (chorusPhase1 + 0.51 / fs);
        chorusPhase2 = detail::wrapPhase (chorusPhase2 + 0.86 / fs);

        if (mode != ChorusMode::off)
            activeChorusMode = mode;

        if (wetAmt > 1e-6)
        {
            const double tri1L = 1.0 - 4.0 * std::fabs (chorusPhase1 - 0.5);
            const double tri1R = -tri1L;
            const double tri2L = 1.0 - 4.0 * std::fabs (chorusPhase2 - 0.5);
            const double tri2R = -tri2L;

            double delayMsL = 5.2;
            double delayMsR = 5.2;

            if (activeChorusMode == ChorusMode::mode1)
            {
                delayMsL = 5.2 + 2.1 * tri1L;
                delayMsR = 5.2 + 2.1 * tri1R;
            }
            else if (activeChorusMode == ChorusMode::mode2)
            {
                delayMsL = 6.0 + 3.0 * tri2L;
                delayMsR = 6.0 + 3.0 * tri2R;
            }
            else // ChorusMode::both
            {
                delayMsL = 5.6 + 1.6 * tri1L + 1.8 * tri2L;
                delayMsR = 5.6 + 1.6 * tri1R + 1.8 * tri2R;
            }

            const double rawWetL = readDelayLinear (chorusBufL.data(), kChorusDelaySize, chorusWrite, delayMsL * 0.001 * fs);
            const double rawWetR = readDelayLinear (chorusBufR.data(), kChorusDelaySize, chorusWrite, delayMsR * 0.001 * fs);

            // Gentle ~9 kHz BBD low-pass on wet signal
            const double lpCoeff = 1.0 - std::exp (-kTwoPi * 9000.0 / fs);
            chorusLpL += (rawWetL - chorusLpL) * lpCoeff;
            chorusLpR += (rawWetR - chorusLpR) * lpCoeff;

            inL = inL * (1.0 - 0.35 * wetAmt) + chorusLpL * wetAmt;
            inR = inR * (1.0 - 0.35 * wetAmt) + chorusLpR * wetAmt;
        }

        chorusWrite = (chorusWrite + 1) % kChorusDelaySize;
    }

    void processEcho (double& inL, double& inR, double echoTimeMs, double feedback, double mix, double fs) noexcept
    {
        const int bufSize = (int) echoBufL.size();
        if (bufSize <= 4)
            return;

        const double fb = std::clamp (feedback, 0.0, 0.85);
        const double wetMix = std::clamp (mix, 0.0, 1.0);
        const double delaySamplesL = std::clamp (echoTimeMs * 0.001 * fs, 1.0, (double) (bufSize - 4));
        const double delaySamplesR = std::clamp (echoTimeMs * 0.001 * 0.75 * fs, 1.0, (double) (bufSize - 4));

        const double tapL = readDelayLinear (echoBufL.data(), bufSize, echoWrite, delaySamplesL);
        const double tapR = readDelayLinear (echoBufR.data(), bufSize, echoWrite, delaySamplesR);

        // Tape dark LP (~3.8 kHz) and gentle sub HP (~90 Hz) in feedback loop
        const double lpCoeff = 1.0 - std::exp (-kTwoPi * 3800.0 / fs);
        const double hpCoeff = std::exp (-kTwoPi * 90.0 / fs);

        echoLpL += (tapL - echoLpL) * lpCoeff;
        echoLpR += (tapR - echoLpR) * lpCoeff;
        echoHpL = hpCoeff * (echoHpL + echoLpL - prevEchoLpL);
        echoHpR = hpCoeff * (echoHpR + echoLpR - prevEchoLpR);
        prevEchoLpL = echoLpL;
        prevEchoLpR = echoLpR;

        // Cross-fed stereo tape feedback with soft tanh saturation
        const double writeL = std::tanh (inL + fb * echoHpR);
        const double writeR = std::tanh (inR + fb * echoHpL);

        echoBufL[(size_t) echoWrite] = (float) writeL;
        echoBufR[(size_t) echoWrite] = (float) writeR;
        echoWrite = (echoWrite + 1) % bufSize;

        if (wetMix > 0.0001)
        {
            inL += wetMix * tapL;
            inR += wetMix * tapR;
        }
    }

    double sampleRate = 48000.0;
    std::array<Voice, kMaxVoices> voices {};
    std::array<HeldMonoNote, 128> monoHeldNotes {};
    int monoHeldCount = 0;
    bool sustainPedal = false;
    float modWheel = 0.0f;
    float pitchBendSemis = 0.0f;
    int lastMonoNote = -1;
    double lastPolyNote = 60.0;
    bool hasPlayedPoly = false;
    uint64_t nextTriggerOrder = 1;
    uint64_t nextGroupId = 1;

    double lfoPhase = 0.0;
    double lfoPrevPhase = 0.0;
    double lfoShValue = 0.0;
    uint32_t noiseRng = 0x9e3779b9u;

    bool smoothedInited = false;
    double smoothCutoff = 3600.0;
    double smoothEchoMs = 320.0;
    float smoothMasterGain = 0.70794578f;
    double smoothChorusWet = 0.0;
    double smoothEchoFeedback = 0.0;
    double smoothEchoMix = 0.0;

    std::array<float, kChorusDelaySize> chorusBufL {};
    std::array<float, kChorusDelaySize> chorusBufR {};
    int chorusWrite = 0;
    double chorusPhase1 = 0.0;
    double chorusPhase2 = 0.0;
    double chorusLpL = 0.0;
    double chorusLpR = 0.0;
    ChorusMode activeChorusMode = ChorusMode::mode1;

    std::vector<float> echoBufL;
    std::vector<float> echoBufR;
    int echoWrite = 0;
    double echoLpL = 0.0;
    double echoLpR = 0.0;
    double echoHpL = 0.0;
    double echoHpR = 0.0;
    double prevEchoLpL = 0.0;
    double prevEchoLpR = 0.0;

    std::array<float, kScopeSamples> scopeBuffer {};
    std::atomic<int> scopeWritePos { 0 };
};
} // namespace selene
