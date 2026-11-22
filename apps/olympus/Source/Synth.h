// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 The Pantheon authors

#pragma once

#include <juce_audio_basics/juce_audio_basics.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace olympus
{
// Lightweight 16-voice polyphonic synthesizer (sine + soft triangle/harmonics, ADSR envelope, velocity scaling).
// Sample-rate aware and real-time safe with zero heap allocations in process().
class BasicSynth
{
public:
    static constexpr int kMaxVoices = 16;

    BasicSynth() { prepare (sr); }

    void prepare (double sampleRate) noexcept
    {
        if (sampleRate > 0.0 && std::isfinite (sampleRate))
            sr = sampleRate;
        attackRate = 1.0f / std::max (1.0f, static_cast<float> (0.004 * sr));
        decayRate = (1.0f - sustainLevel) / std::max (1.0f, static_cast<float> (0.15 * sr));
        releaseRate = sustainLevel / std::max (1.0f, static_cast<float> (0.12 * sr));
        for (auto& v : voices)
            if (v.active && v.note >= 0)
                v.phaseInc = noteFreq (v.note) / sr;
    }

    double getSampleRate() const noexcept { return sr; }

    void reset() noexcept
    {
        for (auto& v : voices)
            v = {};
        ageCounter = 0;
    }

    void noteOn (int channel, int noteNumber, float velocity) noexcept
    {
        const int note = std::clamp (noteNumber, 0, 127);
        const int ch = std::clamp (channel, 1, 16);
        const float vel = std::clamp (velocity, 0.0f, 1.0f);
        if (vel <= 0.0f)
        {
            noteOff (ch, note);
            return;
        }

        Voice* target = nullptr;
        for (auto& v : voices)
        {
            if (v.active && v.channel == ch && v.note == note)
            {
                target = &v;
                break;
            }
        }
        if (target == nullptr)
        {
            for (auto& v : voices)
            {
                if (! v.active)
                {
                    target = &v;
                    break;
                }
            }
        }
        if (target == nullptr)
        {
            Voice* bestReleasing = nullptr;
            Voice* oldest = &voices[0];
            for (auto& v : voices)
            {
                if (v.stage == Stage::Release)
                {
                    if (bestReleasing == nullptr || v.env < bestReleasing->env)
                        bestReleasing = &v;
                }
                if (v.age < oldest->age)
                    oldest = &v;
            }
            target = bestReleasing != nullptr ? bestReleasing : oldest;
        }

        const bool sameRetrigger = target->active && target->channel == ch && target->note == note;
        target->active = true;
        target->channel = ch;
        target->note = note;
        target->velocity = vel;
        if (! sameRetrigger)
        {
            target->phase = 0.0;
            target->env = 0.0f;
        }
        target->phaseInc = noteFreq (note) / sr;
        target->stage = Stage::Attack;
        target->age = ++ageCounter;
    }

    void noteOn (int channel, int noteNumber, int midiVelocity) noexcept
    {
        noteOn (channel, noteNumber, static_cast<float> (std::clamp (midiVelocity, 0, 127)) / 127.0f);
    }

    void noteOff (int channel, int noteNumber) noexcept
    {
        const int note = std::clamp (noteNumber, 0, 127);
        const int ch = std::clamp (channel, 1, 16);
        for (auto& v : voices)
        {
            if (v.active && v.channel == ch && v.note == note && v.stage != Stage::Release)
                v.stage = Stage::Release;
        }
    }

    void allNotesOff() noexcept
    {
        for (auto& v : voices)
        {
            if (v.active && v.stage != Stage::Release)
                v.stage = Stage::Release;
        }
    }

    bool isNoteActive (int channel, int noteNumber) const noexcept
    {
        const int note = std::clamp (noteNumber, 0, 127);
        const int ch = std::clamp (channel, 1, 16);
        for (const auto& v : voices)
            if (v.active && v.stage != Stage::Release && v.channel == ch && v.note == note)
                return true;
        return false;
    }

    int activeVoiceCount() const noexcept
    {
        int count = 0;
        for (const auto& v : voices)
            if (v.active)
                ++count;
        return count;
    }

    // Renders (adds) active voices and incoming MIDI events into `buffer` with zero allocations.
    void process (juce::AudioBuffer<float>& buffer, const juce::MidiBuffer& midi) noexcept
    {
        const int numSamples = buffer.getNumSamples();
        const int numChannels = buffer.getNumChannels();
        if (numSamples <= 0 || numChannels <= 0)
            return;

        int cursor = 0;
        for (const auto metadata : midi)
        {
            const int eventPos = std::clamp (metadata.samplePosition, 0, numSamples);
            if (eventPos > cursor)
            {
                renderRange (buffer, cursor, eventPos);
                cursor = eventPos;
            }
            handleRawMidi (metadata.data, metadata.numBytes);
        }
        if (cursor < numSamples)
            renderRange (buffer, cursor, numSamples);
    }

private:
    enum class Stage { Idle, Attack, Decay, Sustain, Release };

    struct Voice
    {
        bool active = false;
        int channel = 1;
        int note = -1;
        float velocity = 0.0f;
        double phase = 0.0;
        double phaseInc = 0.0;
        Stage stage = Stage::Idle;
        float env = 0.0f;
        std::uint64_t age = 0;
    };

    static constexpr float sustainLevel = 0.7f;
    static constexpr float kVoiceGain = 0.25f;

    std::array<Voice, kMaxVoices> voices {};
    double sr = 48000.0;
    float attackRate = 0.005f;
    float decayRate = 0.0001f;
    float releaseRate = 0.0001f;
    std::uint64_t ageCounter = 0;

    static double noteFreq (int note) noexcept
    {
        return 440.0 * std::pow (2.0, static_cast<double> (note - 69) / 12.0);
    }

    void handleRawMidi (const std::uint8_t* data, int numBytes) noexcept
    {
        if (data == nullptr || numBytes <= 0)
            return;
        const std::uint8_t status = data[0];
        const std::uint8_t type = status & 0xF0;
        const int ch = static_cast<int> (status & 0x0F) + 1;
        if (type == 0x90 && numBytes >= 3)
        {
            const int note = data[1] & 0x7F;
            const int vel = data[2] & 0x7F;
            if (vel == 0)
                noteOff (ch, note);
            else
                noteOn (ch, note, static_cast<float> (vel) / 127.0f);
        }
        else if (type == 0x80 && numBytes >= 2)
        {
            noteOff (ch, data[1] & 0x7F);
        }
        else if (type == 0xB0 && numBytes >= 3)
        {
            const int cc = data[1] & 0x7F;
            if (cc == 123)
                allNotesOff();
            else if (cc == 120)
                reset();
        }
    }

    void renderRange (juce::AudioBuffer<float>& buffer, int from, int to) noexcept
    {
        constexpr double twoPi = 6.28318530717958647692;
        const int numChannels = buffer.getNumChannels();
        for (auto& v : voices)
        {
            if (! v.active)
                continue;
            for (int i = from; i < to; ++i)
            {
                switch (v.stage)
                {
                    case Stage::Attack:
                        v.env += attackRate;
                        if (v.env >= 1.0f)
                        {
                            v.env = 1.0f;
                            v.stage = Stage::Decay;
                        }
                        break;
                    case Stage::Decay:
                        v.env -= decayRate;
                        if (v.env <= sustainLevel)
                        {
                            v.env = sustainLevel;
                            v.stage = Stage::Sustain;
                        }
                        break;
                    case Stage::Sustain:
                        break;
                    case Stage::Release:
                        v.env -= releaseRate;
                        if (v.env <= 0.0f)
                        {
                            v.env = 0.0f;
                            v.active = false;
                            v.stage = Stage::Idle;
                        }
                        break;
                    case Stage::Idle:
                        v.active = false;
                        break;
                }
                if (! v.active)
                    break;

                const double p = v.phase;
                const float sine = static_cast<float> (std::sin (twoPi * p));
                const float tri = static_cast<float> (4.0 * std::abs (p - 0.5) - 1.0);
                const float harmonic2 = static_cast<float> (std::sin (2.0 * twoPi * p));
                const float osc = 0.65f * sine + 0.25f * tri + 0.10f * harmonic2;
                const float sample = osc * v.env * v.velocity * kVoiceGain;

                for (int ch = 0; ch < numChannels; ++ch)
                    buffer.getWritePointer (ch)[i] += sample;

                v.phase += v.phaseInc;
                if (v.phase >= 1.0)
                    v.phase -= std::floor (v.phase);
            }
        }
    }
};

// Lock-free single-producer / single-consumer ring buffer for live 3-byte MIDI events.
class MidiFifo
{
public:
    static constexpr int kCapacity = 1024;

    struct MidiEvent
    {
        std::uint8_t status = 0;
        std::uint8_t data1 = 0;
        std::uint8_t data2 = 0;
        int sampleOffset = 0;
    };

    bool push (const MidiEvent& ev) noexcept
    {
        const auto scope = fifo.write (1);
        if (scope.blockSize1 < 1)
            return false;
        events[static_cast<size_t> (scope.startIndex1)] = ev;
        return true;
    }

    bool push (std::uint8_t status, std::uint8_t data1, std::uint8_t data2, int sampleOffset = 0) noexcept
    {
        return push (MidiEvent { status, data1, data2, sampleOffset });
    }

    bool pop (MidiEvent& out) noexcept
    {
        const auto scope = fifo.read (1);
        if (scope.blockSize1 < 1)
            return false;
        out = events[static_cast<size_t> (scope.startIndex1)];
        return true;
    }

    int drain (juce::MidiBuffer& dest, int blockNumSamples)
    {
        const int ready = fifo.getNumReady();
        if (ready <= 0)
            return 0;
        if (blockNumSamples <= 0)
            blockNumSamples = 1;
        const auto scope = fifo.read (ready);
        int count = 0;
        const int maxOffset = blockNumSamples - 1;
        auto emitRange = [&] (int start, int size)
        {
            for (int i = 0; i < size; ++i)
            {
                const auto& ev = events[static_cast<size_t> (start + i)];
                const std::uint8_t msg[3] = { ev.status, ev.data1, ev.data2 };
                const int offset = std::clamp (ev.sampleOffset, 0, maxOffset);
                dest.addEvent (msg, 3, offset);
                ++count;
            }
        };
        if (scope.blockSize1 > 0)
            emitRange (scope.startIndex1, scope.blockSize1);
        if (scope.blockSize2 > 0)
            emitRange (scope.startIndex2, scope.blockSize2);
        return count;
    }

    int getNumReady() const noexcept { return fifo.getNumReady(); }
    void clear() noexcept { fifo.reset(); }

private:
    juce::AbstractFifo fifo { kCapacity };
    std::array<MidiEvent, kCapacity> events {};
};
}
