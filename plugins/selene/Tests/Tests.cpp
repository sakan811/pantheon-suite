// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 The Pantheon authors

// Standalone DSP tests of Selene Poly: no JUCE, no GUI.

#include "Check.h"
#include "DSP/Engine.h"
#include <cmath>
#include <vector>

using namespace selene;
using test::check;
using test::fmt;

namespace
{
constexpr double kFs = 48000.0;

struct Stereo
{
    std::vector<float> l, r;
};

Stereo render (Engine& e, int totalSamples, const std::vector<MidiEvent>& events, const EngineParams& p, int block = 256)
{
    Stereo out { std::vector<float> ((size_t) totalSamples, 0.0f), std::vector<float> ((size_t) totalSamples, 0.0f) };
    std::vector<MidiEvent> blockEvents;
    for (int start = 0; start < totalSamples; start += block)
    {
        const int m = std::min (block, totalSamples - start);
        blockEvents.clear();
        for (const auto& ev : events)
        {
            if (ev.sampleOffset >= start && ev.sampleOffset < start + m)
            {
                auto local = ev;
                local.sampleOffset -= start;
                blockEvents.push_back (local);
            }
        }
        e.process (out.l.data() + start, out.r.data() + start, m, blockEvents.data(), (int) blockEvents.size(), p);
    }
    return out;
}

double rmsOf (const std::vector<float>& x, size_t from, size_t count)
{
    double sum = 0.0;
    for (size_t i = from; i < from + count; ++i)
        sum += (double) x[i] * (double) x[i];
    return std::sqrt (sum / (double) std::max<size_t> (count, 1));
}

double peakOf (const std::vector<float>& x, size_t from, size_t count)
{
    double pk = 0.0;
    for (size_t i = from; i < from + count; ++i)
        pk = std::fmax (pk, std::fabs ((double) x[i]));
    return pk;
}

double db (double lin) { return 20.0 * std::log10 (std::fmax (lin, 1e-12)); }

double toneLevelDb (const std::vector<float>& x, double hz, double fs, size_t from, size_t count)
{
    double re = 0.0, im = 0.0;
    for (size_t i = 0; i < count; ++i)
    {
        const double w = 0.5 * (1.0 - std::cos (kTwoPi * (double) i / (double) count));
        const double ang = kTwoPi * hz * (double) (from + i) / fs;
        re += (double) x[from + i] * w * std::cos (ang);
        im += (double) x[from + i] * w * std::sin (ang);
    }
    return db (4.0 * std::sqrt (re * re + im * im) / (double) count);
}

// Finds the fundamental frequency by Hann-windowed DFT peak search around expected Hz range.
double peakFreqInRange (const std::vector<float>& x, double fs, size_t from, size_t count, double minHz, double maxHz, double stepHz = 0.5)
{
    double bestHz = minHz;
    double bestMag = -1.0;
    for (double hz = minHz; hz <= maxHz; hz += stepHz)
    {
        double re = 0.0, im = 0.0;
        for (size_t i = 0; i < count; ++i)
        {
            const double w = 0.5 * (1.0 - std::cos (kTwoPi * (double) i / (double) count));
            const double ang = kTwoPi * hz * (double) i / fs;
            re += (double) x[from + i] * w * std::cos (ang);
            im += (double) x[from + i] * w * std::sin (ang);
        }
        const double mag = re * re + im * im;
        if (mag > bestMag)
        {
            bestMag = mag;
            bestHz = hz;
        }
    }
    return bestHz;
}

EngineParams cleanSynthParams()
{
    EngineParams p;
    p.drift = 0.0f;
    p.subLevel = 0.0f;
    p.oscMix = 0.0f;
    p.noiseLevel = 0.0f;
    p.chorusMode = ChorusMode::off;
    p.echoMix = 0.0f;
    p.filterEnvAmt = 0.0f;
    p.keytrack = 0.0f;
    p.drive = 0.0f;
    p.resonance = 0.0f;
    p.cutoffHz = 18000.0f;
    p.ampAttackMs = 2.0f;
    p.ampDecayMs = 10.0f;
    p.ampSustain = 1.0f;
    p.ampReleaseMs = 30.0f;
    p.velAmp = 0.0f;
    p.masterGainDb = 0.0f;
    return p;
}
} // namespace

int main()
{
    std::printf ("Selene Poly DSP tests (48 kHz)\n");

    // [1] NoteOn produces audio, NoteOff decays to silence
    std::printf ("\n[1] NoteOn and NoteOff envelope\n");
    {
        Engine e;
        e.prepare (kFs);
        auto p = cleanSynthParams();
        p.ampReleaseMs = 40.0f;
        std::vector<MidiEvent> ev {
            { MidiEvent::Type::noteOn, 0, 0, 60, 0.9f },
            { MidiEvent::Type::noteOff, 12000, 0, 60, 0.0f },
        };
        const auto out = render (e, 48000, ev, p);
        const double onRms = rmsOf (out.l, 2000, 8000);
        const double tailRms = rmsOf (out.l, 40000, 8000);
        check (onRms > 0.05 && tailRms < 1e-6 && e.activeVoices.load() == 0,
               fmt ("NoteOn produces RMS %.4f, NoteOff decays to %.2e (activeVoices=%d)", onRms, tailRms, e.activeVoices.load()));
    }

    // [2] Polyphony (16 voices + voice stealing)
    std::printf ("\n[2] Polyphony and voice stealing\n");
    {
        Engine e;
        e.prepare (kFs);
        auto p = cleanSynthParams();
        std::vector<MidiEvent> ev;
        for (int n = 48; n < 48 + kMaxVoices; ++n)
            ev.push_back ({ MidiEvent::Type::noteOn, 0, 0, n, 0.8f });
        render (e, 1024, ev, p);
        const int held16 = e.activeVoices.load();

        // Trigger 4 more notes -> voice stealing keeps activeVoices <= 16
        std::vector<MidiEvent> extra;
        for (int n = 70; n < 74; ++n)
            extra.push_back ({ MidiEvent::Type::noteOn, 0, 0, n, 0.8f });
        const auto out = render (e, 2048, extra, p);
        const int afterSteal = e.activeVoices.load();
        check (held16 == kMaxVoices && afterSteal == kMaxVoices && peakOf (out.l, 0, out.l.size()) > 0.1,
               fmt ("16 voices active (%d), 20 notes triggered -> voice stealing keeps %d voices", held16, afterSteal));
    }

    // [3] Mono glide & Unison stereo width
    std::printf ("\n[3] Mono glide and Unison stereo width\n");
    {
        Engine monoEng;
        monoEng.prepare (kFs);
        auto p = cleanSynthParams();
        p.voiceMode = VoiceMode::mono;
        p.osc1Wave = OscWave::sine;
        p.glideMs = 150.0f;
        // Hold C4 (261.63 Hz), then overlap C5 (523.25 Hz)
        std::vector<MidiEvent> ev {
            { MidiEvent::Type::noteOn, 0, 0, 60, 0.8f },
            { MidiEvent::Type::noteOn, 6000, 0, 72, 0.8f },
        };
        const auto out = render (monoEng, 36000, ev, p);
        const double midFreq = peakFreqInRange (out.l, kFs, 7500, 2048, 250.0, 540.0, 2.0);
        const double endFreq = peakFreqInRange (out.l, kFs, 30000, 4096, 480.0, 560.0, 1.0);
        check (monoEng.activeVoices.load() == 1 && midFreq > 290.0 && midFreq < 490.0 && std::fabs (endFreq - 523.25) < 3.0,
               fmt ("Mono glide: 1 voice, mid-glide %.1f Hz, settled at %.1f Hz (target 523.3 Hz)", midFreq, endFreq));

        Engine uniEng;
        uniEng.prepare (kFs);
        auto pu = cleanSynthParams();
        pu.voiceMode = VoiceMode::unison;
        std::vector<MidiEvent> uev { { MidiEvent::Type::noteOn, 0, 0, 60, 0.9f } };
        const auto uout = render (uniEng, 12000, uev, pu);
        double diffSum = 0.0;
        for (size_t i = 2000; i < 12000; ++i)
            diffSum += std::fabs ((double) uout.l[i] - (double) uout.r[i]);
        check (uniEng.activeVoices.load() == kUnisonCount && diffSum > 10.0,
               fmt ("Unison stacks %d voices with stereo spread (L-R diff=%.1f)", uniEng.activeVoices.load(), diffSum));
    }

    // [4] Filter cutoff & mode (LP24 vs LP12 vs HP12) + filterMagnitudeAt
    std::printf ("\n[4] Filter modes and cutoff response\n");
    {
        auto measureHighDrop = [] (FilterMode mode, float cutoff)
        {
            Engine e;
            e.prepare (kFs);
            auto p = cleanSynthParams();
            p.osc1Wave = OscWave::saw;
            p.filterMode = mode;
            p.cutoffHz = cutoff;
            // A2 = 110 Hz, 9th harmonic = 990 Hz, 18th harmonic = 1980 Hz
            std::vector<MidiEvent> ev { { MidiEvent::Type::noteOn, 0, 0, 45, 0.9f } };
            const auto out = render (e, 24000, ev, p);
            const double fund = toneLevelDb (out.l, 110.0, kFs, 8000, 16000);
            const double high = toneLevelDb (out.l, 1980.0, kFs, 8000, 16000);
            return std::make_pair (fund, high);
        };

        const auto lp24 = measureHighDrop (FilterMode::lp24, 300.0f);
        const auto lp12 = measureHighDrop (FilterMode::lp12, 300.0f);
        const auto hp12 = measureHighDrop (FilterMode::hp12, 1200.0f);

        check (lp24.second < lp12.second - 12.0,
               fmt ("LP24 attenuates 1980 Hz more than LP12 (LP24 %.1f dBFS vs LP12 %.1f dBFS)", lp24.second, lp12.second));
        check (hp12.first < lp12.first - 20.0 && hp12.second > lp24.second + 10.0,
               fmt ("HP12 at 1.2 kHz cuts 110 Hz fundamental (%.1f dBFS vs %.1f dBFS) and passes 1980 Hz (%.1f dBFS)",
                    hp12.first, lp12.first, hp12.second));

        EngineParams mp = cleanSynthParams();
        mp.filterMode = FilterMode::lp24;
        mp.cutoffHz = 1000.0f;
        mp.resonance = 0.0f;
        const double m100 = db (filterMagnitudeAt (mp, 100.0));
        const double m4000 = db (filterMagnitudeAt (mp, 4000.0));
        check (m100 > -0.5 && m4000 < -22.0,
               fmt ("filterMagnitudeAt LP24 @ 1 kHz: 100 Hz = %.2f dB, 4 kHz = %.2f dB", m100, m4000));
    }

    // [5] Filter envelope bipolar modulation
    std::printf ("\n[5] Bipolar filter envelope\n");
    {
        auto runEnvAmt = [] (float amt)
        {
            Engine e;
            e.prepare (kFs);
            auto p = cleanSynthParams();
            p.cutoffHz = 1000.0f;
            p.filterEnvAmt = amt;
            p.fltAttackMs = 1.0f;
            p.fltDecayMs = 2000.0f;
            p.fltSustain = 1.0f;
            p.velFlt = 0.0f;
            std::vector<MidiEvent> ev { { MidiEvent::Type::noteOn, 0, 0, 45, 0.9f } }; // 110 Hz saw
            const auto out = render (e, 16000, ev, p);
            return toneLevelDb (out.l, 1980.0, kFs, 4000, 12000);
        };
        const double neg = runEnvAmt (-0.8f);
        const double zero = runEnvAmt (0.0f);
        const double pos = runEnvAmt (+0.8f);
        check (neg < zero - 10.0 && pos > zero + 10.0,
               fmt ("Bipolar filter env on 1980 Hz harmonic: -80%% = %.1f dB, 0%% = %.1f dB, +80%% = %.1f dB", neg, zero, pos));
    }

    // [6] Osc2 FM, Sub, and Noise contributions
    std::printf ("\n[6] Osc2 FM, Sub, and Noise\n");
    {
        // FM: pure sine Osc1 + Osc2 at +12 semi -> FM adds harmonics at 2x, 3x...
        Engine ePlain, eFm;
        ePlain.prepare (kFs);
        eFm.prepare (kFs);
        auto pPlain = cleanSynthParams();
        pPlain.osc1Wave = OscWave::sine;
        pPlain.osc2Wave = OscWave::sine;
        pPlain.osc2Semi = 12;
        pPlain.osc2DetuneCents = 0.0f;
        pPlain.oscMix = 0.0f;
        pPlain.osc2Fm = 0.0f;

        auto pFm = pPlain;
        pFm.osc2Fm = 0.75f;

        std::vector<MidiEvent> ev { { MidiEvent::Type::noteOn, 0, 0, 57, 0.9f } }; // A3 = 220 Hz
        const auto outPlain = render (ePlain, 16000, ev, pPlain);
        const auto outFm = render (eFm, 16000, ev, pFm);
        const double h3Plain = toneLevelDb (outPlain.l, 660.0, kFs, 4000, 12000);
        const double h3Fm = toneLevelDb (outFm.l, 660.0, kFs, 4000, 12000);
        check (h3Fm > h3Plain + 30.0,
               fmt ("Osc2 FM adds sidebands (660 Hz: plain %.1f dBFS -> FM %.1f dBFS)", h3Plain, h3Fm));

        // Sub oscillator (-1 octave): note A3 (220 Hz) -> sub at 110 Hz
        Engine eSub;
        eSub.prepare (kFs);
        auto pSub = pPlain;
        pSub.subLevel = 0.8f;
        const auto outSub = render (eSub, 16000, ev, pSub);
        const double subPlain = toneLevelDb (outPlain.l, 110.0, kFs, 4000, 12000);
        const double subAdded = toneLevelDb (outSub.l, 110.0, kFs, 4000, 12000);
        check (subAdded > subPlain + 30.0,
               fmt ("Sub oscillator adds -1 oct 110 Hz (plain %.1f dBFS -> sub %.1f dBFS)", subPlain, subAdded));

        // Noise: even with Sub & Oscs off via silence? Wait: set osc1=sine at 220, measure off-harmonic noise RMS or compare difference
        Engine eNoise;
        eNoise.prepare (kFs);
        auto pNoise = pPlain;
        pNoise.noiseLevel = 0.6f;
        const auto outNoise = render (eNoise, 16000, ev, pNoise);
        const double offTonePlain = toneLevelDb (outPlain.l, 1733.0, kFs, 4000, 12000);
        const double offToneNoise = toneLevelDb (outNoise.l, 1733.0, kFs, 4000, 12000);
        check (offToneNoise > offTonePlain + 25.0,
               fmt ("Noise adds broadband energy at 1733 Hz (%.1f dBFS -> %.1f dBFS)", offTonePlain, offToneNoise));
    }

    // [7] BBD Chorus stereo difference
    std::printf ("\n[7] BBD Stereo Chorus\n");
    {
        auto runChorus = [] (ChorusMode m)
        {
            Engine e;
            e.prepare (kFs);
            auto p = cleanSynthParams();
            p.chorusMode = m;
            std::vector<MidiEvent> ev { { MidiEvent::Type::noteOn, 0, 0, 60, 0.8f } };
            return render (e, 24000, ev, p);
        };
        const auto off = runChorus (ChorusMode::off);
        const auto c1 = runChorus (ChorusMode::mode1);
        const auto cBoth = runChorus (ChorusMode::both);

        double diffOff = 0.0, diff1 = 0.0, diffBoth = 0.0;
        for (size_t i = 4000; i < 24000; ++i)
        {
            diffOff += std::fabs ((double) off.l[i] - (double) off.r[i]);
            diff1 += std::fabs ((double) c1.l[i] - (double) c1.r[i]);
            diffBoth += std::fabs ((double) cBoth.l[i] - (double) cBoth.r[i]);
        }
        check (diffOff < 1e-6 && diff1 > 20.0 && diffBoth > 20.0,
               fmt ("Chorus Off L-R=%.2e, Mode I L-R=%.1f, I+II L-R=%.1f", diffOff, diff1, diffBoth));
    }

    // [8] Tape Echo repeats after note stops
    std::printf ("\n[8] Stereo Tape Echo\n");
    {
        Engine e;
        e.prepare (kFs);
        auto p = cleanSynthParams();
        p.ampReleaseMs = 5.0f;
        p.echoTimeMs = 200.0f; // 9600 samples
        p.echoFeedback = 0.55f;
        p.echoMix = 0.60f;
        std::vector<MidiEvent> ev {
            { MidiEvent::Type::noteOn, 0, 0, 60, 0.9f },
            { MidiEvent::Type::noteOff, 2400, 0, 60, 0.0f },
        };
        const auto out = render (e, 48000, ev, p);
        const double gapRms = rmsOf (out.l, 5000, 3000);   // quiet gap before 200 ms echo
        const double tap1Rms = rmsOf (out.l, 10000, 2000); // first repeat around 9600..12000
        const double tap2Rms = rmsOf (out.r, 17000, 2000); // later repeat in R
        check (gapRms < 1e-3 && tap1Rms > 0.01 && tap2Rms > 0.005,
               fmt ("Tape Echo: gap=%.5f, L tap1=%.4f, R tap2=%.4f", gapRms, tap1Rms, tap2Rms));
    }

    // [9] Sustain pedal (CC64)
    std::printf ("\n[9] Sustain pedal (CC64)\n");
    {
        Engine e;
        e.prepare (kFs);
        auto p = cleanSynthParams();
        p.ampReleaseMs = 20.0f;
        std::vector<MidiEvent> ev {
            { MidiEvent::Type::controlChange, 0, 0, 64, 1.0f },     // pedal down
            { MidiEvent::Type::noteOn, 0, 0, 60, 0.8f },
            { MidiEvent::Type::noteOff, 4000, 0, 60, 0.0f },        // key released while pedal held
            { MidiEvent::Type::controlChange, 24000, 0, 64, 0.0f }, // pedal up at 0.5 s
        };
        const auto out = render (e, 36000, ev, p);
        const double heldRms = rmsOf (out.l, 14000, 6000);
        const double afterRms = rmsOf (out.l, 30000, 4000);
        check (heldRms > 0.05 && afterRms < 1e-5,
               fmt ("CC64 holds note after NoteOff (RMS=%.4f) and releases on pedal up (RMS=%.2e)", heldRms, afterRms));
    }

    // [10] Pitch bend shifts frequency
    std::printf ("\n[10] Pitch bend\n");
    {
        Engine e;
        e.prepare (kFs);
        auto p = cleanSynthParams();
        p.osc1Wave = OscWave::sine;
        std::vector<MidiEvent> ev {
            { MidiEvent::Type::noteOn, 0, 0, 69, 0.9f },          // A4 = 440 Hz
            { MidiEvent::Type::pitchBend, 12000, 0, 0, 1.0f },    // +2 semitones -> ~493.88 Hz
        };
        const auto out = render (e, 28000, ev, p);
        const double f0 = peakFreqInRange (out.l, kFs, 2000, 4096, 420.0, 520.0, 0.5);
        const double f1 = peakFreqInRange (out.l, kFs, 18000, 4096, 420.0, 520.0, 0.5);
        check (std::fabs (f0 - 440.0) < 2.0 && std::fabs (f1 - 493.88) < 2.5,
               fmt ("Pitch bend: A4=%.1f Hz -> +2 semi=%.1f Hz (target 493.9 Hz)", f0, f1));
    }

    // [11] Sample-rate independence (44.1k vs 48k vs 96k)
    std::printf ("\n[11] Sample-rate independence\n");
    {
        auto testRate = [] (double fs)
        {
            Engine e;
            e.prepare (fs);
            auto p = cleanSynthParams();
            p.osc1Wave = OscWave::sine;
            const int total = (int) (0.5 * fs);
            std::vector<MidiEvent> ev { { MidiEvent::Type::noteOn, 0, 0, 69, 0.9f } };
            const auto out = render (e, total, ev, p);
            return peakFreqInRange (out.l, fs, (size_t) (0.1 * fs), (size_t) (0.2 * fs), 425.0, 455.0, 0.5);
        };
        const double f441 = testRate (44100.0);
        const double f48 = testRate (48000.0);
        const double f96 = testRate (96000.0);
        check (std::fabs (f441 - 440.0) < 2.0 && std::fabs (f48 - 440.0) < 2.0 && std::fabs (f96 - 440.0) < 2.0,
               fmt ("A4 pitch across sample rates: 44.1k=%.1f Hz, 48k=%.1f Hz, 96k=%.1f Hz", f441, f48, f96));
    }

    // [12] Zero NaN/Inf and reset() clears all voices/tails
    std::printf ("\n[12] Finite output and reset()\n");
    {
        Engine e;
        e.prepare (kFs);
        EngineParams extreme;
        extreme.voiceMode = VoiceMode::unison;
        extreme.osc2Fm = 1.0f;
        extreme.resonance = 0.98f;
        extreme.drive = 1.0f;
        extreme.noiseLevel = 1.0f;
        extreme.chorusMode = ChorusMode::both;
        extreme.echoFeedback = 0.85f;
        extreme.echoMix = 1.0f;
        extreme.masterGainDb = 12.0f;

        std::vector<MidiEvent> ev {
            { MidiEvent::Type::noteOn, 0, 0, 60, 1.0f },
            { MidiEvent::Type::noteOn, 0, 0, 64, 1.0f },
            { MidiEvent::Type::noteOn, 0, 0, 67, 1.0f },
        };
        const auto noisy = render (e, 24000, ev, extreme);
        bool allFinite = true;
        for (size_t i = 0; i < noisy.l.size(); ++i)
            allFinite = allFinite && std::isfinite (noisy.l[i]) && std::isfinite (noisy.r[i]);

        e.reset();
        const auto afterReset = render (e, 4096, {}, extreme);
        const double afterPeak = std::max (peakOf (afterReset.l, 0, afterReset.l.size()),
                                           peakOf (afterReset.r, 0, afterReset.r.size()));
        check (allFinite && afterPeak == 0.0 && e.activeVoices.load() == 0,
               fmt ("Extreme settings stay finite (ok=%d), reset() leaves exact silence (peak=%.2e)", allFinite ? 1 : 0, afterPeak));
    }

    // [13] Engine::reset() clears echo filter states (prevEchoLpL/R) over 20000 silent samples
    std::printf ("\n[13] Reset clears echo/chorus state over 20000 samples\n");
    {
        Engine e;
        e.prepare (kFs);
        auto p = cleanSynthParams();
        p.chorusMode = ChorusMode::both;
        p.echoTimeMs = 120.0f;
        p.echoFeedback = 0.85f;
        p.echoMix = 1.0f;

        std::vector<MidiEvent> ev {
            { MidiEvent::Type::noteOn, 0, 0, 60, 1.0f },
            { MidiEvent::Type::noteOff, 4000, 0, 60, 0.0f },
        };
        render (e, 24000, ev, p);

        e.reset();
        const auto silence = render (e, 20000, {}, p);
        const double peak = std::max (peakOf (silence.l, 0, silence.l.size()),
                                      peakOf (silence.r, 0, silence.r.size()));
        check (peak < 1e-7f && e.activeVoices.load() == 0,
               fmt ("20000 samples after reset() remain silent (peak=%.2e)", peak));
    }

    // [14] Switching VoiceMode from mono to poly while holding keys leaves no phantom mono notes
    std::printf ("\n[14] Mono-to-poly mode switch clears monoHeldNotes\n");
    {
        Engine e;
        e.prepare (kFs);
        auto p = cleanSynthParams();
        p.ampReleaseMs = 20.0f;
        p.voiceMode = VoiceMode::mono;

        render (e, 2048, { { MidiEvent::Type::noteOn, 0, 0, 60, 0.8f },
                           { MidiEvent::Type::noteOn, 512, 0, 64, 0.8f } }, p);

        p.voiceMode = VoiceMode::poly;
        render (e, 16000, { { MidiEvent::Type::noteOff, 0, 0, 60, 0.0f },
                            { MidiEvent::Type::noteOff, 0, 0, 64, 0.0f } }, p);
        const int afterPolyOff = e.activeVoices.load();

        // Switch back to mono, play and release note 72: previously released 60/64 must not retrigger
        p.voiceMode = VoiceMode::mono;
        render (e, 2048, { { MidiEvent::Type::noteOn, 0, 0, 72, 0.8f } }, p);
        const auto tail = render (e, 16000, { { MidiEvent::Type::noteOff, 0, 0, 72, 0.0f } }, p);
        const double tailPeak = std::max (peakOf (tail.l, 12000, 4000), peakOf (tail.r, 12000, 4000));
        check (afterPolyOff == 0 && e.activeVoices.load() == 0 && tailPeak < 1e-6,
               fmt ("Mono->Poly->Mono release leaves 0 voices (afterPolyOff=%d, final=%d, tail=%.2e)",
                    afterPolyOff, e.activeVoices.load(), tailPeak));
    }

    // [15] Unison voice stealing reclaims whole 4-voice noteGroups cleanly
    std::printf ("\n[15] Unison whole-group voice stealing\n");
    {
        Engine e;
        e.prepare (kFs);
        auto p = cleanSynthParams();
        p.voiceMode = VoiceMode::unison;
        p.ampReleaseMs = 20.0f;

        render (e, 1024, { { MidiEvent::Type::noteOn, 0, 0, 48, 0.8f },
                           { MidiEvent::Type::noteOn, 64, 0, 52, 0.8f },
                           { MidiEvent::Type::noteOn, 128, 0, 55, 0.8f },
                           { MidiEvent::Type::noteOn, 192, 0, 59, 0.8f } }, p);
        const int fourNotesVoices = e.activeVoices.load();

        // 5th note steals note 48's entire 4-voice group
        render (e, 1024, { { MidiEvent::Type::noteOn, 0, 0, 64, 0.8f } }, p);
        const int fiveNotesVoices = e.activeVoices.load();

        // Releasing only 52, 55, 59, 64 must leave 0 voices (none of note 48's 4 voices stranded)
        const auto after4Off = render (e, 16000, { { MidiEvent::Type::noteOff, 0, 0, 52, 0.0f },
                                                   { MidiEvent::Type::noteOff, 0, 0, 55, 0.0f },
                                                   { MidiEvent::Type::noteOff, 0, 0, 59, 0.0f },
                                                   { MidiEvent::Type::noteOff, 0, 0, 64, 0.0f } }, p);
        const int afterStealRelease = e.activeVoices.load();
        const double afterStealPeak = std::max (peakOf (after4Off.l, 12000, 4000), peakOf (after4Off.r, 12000, 4000));

        // Verify releasing-group priority: trigger 4 notes, release 55, trigger 67 -> 55 is stolen, 48/52/59 remain
        render (e, 1024, { { MidiEvent::Type::noteOn, 0, 0, 48, 0.8f },
                           { MidiEvent::Type::noteOn, 64, 0, 52, 0.8f },
                           { MidiEvent::Type::noteOn, 128, 0, 55, 0.8f },
                           { MidiEvent::Type::noteOn, 192, 0, 59, 0.8f } }, p);
        render (e, 512, { { MidiEvent::Type::noteOff, 0, 0, 55, 0.0f } }, p);
        render (e, 1024, { { MidiEvent::Type::noteOn, 0, 0, 67, 0.8f } }, p);
        render (e, 16000, { { MidiEvent::Type::noteOff, 0, 0, 52, 0.0f },
                            { MidiEvent::Type::noteOff, 0, 0, 59, 0.0f },
                            { MidiEvent::Type::noteOff, 0, 0, 67, 0.0f } }, p);
        const int remainingHeld48 = e.activeVoices.load();
        render (e, 16000, { { MidiEvent::Type::noteOff, 0, 0, 48, 0.0f } }, p);

        check (fourNotesVoices == 16 && fiveNotesVoices == 16
                   && afterStealRelease == 0 && afterStealPeak < 1e-6
                   && remainingHeld48 == kUnisonCount && e.activeVoices.load() == 0,
               fmt ("Unison stealing: 4->%d, 5->%d voices, clean release=%d (tail=%.2e), release-priority left %d voices for held note -> %d",
                    fourNotesVoices, fiveNotesVoices, afterStealRelease, afterStealPeak, remainingHeld48, e.activeVoices.load()));
    }

    return test::finish ("SeleneTests");
}
