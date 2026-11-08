// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 The Pantheon authors

#pragma once

#include "Session.h"

namespace orpheus
{
// Plays the session's audio: the corrected output, or the original (ORIGINAL on the transport switches between them with no gap,
// at the same position, so a change can be judged by ear). Optionally loops a range. Also plays a reference tone for the keys.
// Wrap it in a juce::AudioTransportSource to get start, stop and sample-rate conversion to the device.
class Playback : public juce::PositionableAudioSource
{
public:
    explicit Playback (Session& s) : session (s) {}

    std::atomic<bool> corrected { true };
    std::atomic<bool> looping { false };
    std::atomic<juce::int64> loopStart { 0 }, loopEnd { 0 };

    juce::int64 position() const noexcept { return pos.load(); }

    // A sine with a few harmonics, for checking a pitch by ear against the keys. Fades out on its own after about 1.2 s.
    void playTone (double midi)
    {
        toneHz.store (midiToHz (midi));
        toneAge.store (0);
    }
    void stopTone() { toneAge.store (kToneSamples); }

    void prepareToPlay (int, double rate) override { deviceRate = rate; }
    void releaseResources() override {}

    void getNextAudioBlock (const juce::AudioSourceChannelInfo& info) override
    {
        auto* buffer = info.buffer;
        buffer->clear (info.startSample, info.numSamples);
        int done = 0;
        {
            const juce::SpinLock::ScopedLockType lock (session.outputLock);
            const auto& audio = corrected.load() ? session.output : session.source;
            const juce::int64 length = audio.getNumSamples();
            const int sourceChannels = audio.getNumChannels();
            while (done < info.numSamples && sourceChannels > 0)
            {
                juce::int64 p = pos.load();
                const bool loop = looping.load();
                const juce::int64 end = loop && loopEnd.load() > loopStart.load() ? std::min (length, loopEnd.load()) : length;
                if (p >= end)
                {
                    if (! loop)
                        break;
                    p = loopStart.load();
                    pos.store (p);
                }
                const int n = (int) std::min<juce::int64> (info.numSamples - done, end - p);
                for (int ch = 0; ch < buffer->getNumChannels(); ++ch)
                    buffer->copyFrom (ch, info.startSample + done, audio, ch % sourceChannels, (int) p, n);
                pos.store (p + n);
                done += n;
            }
        }
        const int age = toneAge.load();
        if (age < kToneSamples)
        {
            const double hz = toneHz.load();
            for (int i = 0; i < info.numSamples && age + i < kToneSamples; ++i)
            {
                const double t = (age + i) / deviceRate, fade = std::min (1.0, (age + i) / (0.01 * deviceRate)) * std::min (1.0, (kToneSamples - age - i) / (0.2 * deviceRate));
                const double phase = 2.0 * M_PI * hz * t;
                const float v = (float) (0.16 * fade * (std::sin (phase) + 0.25 * std::sin (2.0 * phase) + 0.08 * std::sin (3.0 * phase)));
                for (int ch = 0; ch < buffer->getNumChannels(); ++ch)
                    buffer->addSample (ch, info.startSample + i, v);
            }
            toneAge.store (std::min (kToneSamples, age + info.numSamples));
        }
    }

    void setNextReadPosition (juce::int64 p) override { pos.store (std::max<juce::int64> (0, p)); }
    juce::int64 getNextReadPosition() const override { return pos.load(); }
    juce::int64 getTotalLength() const override { return session.numSamples(); }
    bool isLooping() const override { return false; }
    void setLooping (bool) override {}

private:
    static constexpr int kToneSamples = 60000; // at the device rate; about 1.2 s at 48 kHz
    Session& session;
    std::atomic<juce::int64> pos { 0 };
    std::atomic<double> toneHz { 440.0 };
    std::atomic<int> toneAge { kToneSamples };
    double deviceRate = 48000.0;
};
} // namespace orpheus
