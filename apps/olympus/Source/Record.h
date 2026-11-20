// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 The Pantheon authors

#pragma once

#include "Edit.h"

#include <array>
#include <atomic>
#include <memory>
#include <vector>

// Recording the device's inputs into takes, and putting the takes on the timeline.
//
//   Recorder       one take per armed track: a WAV of 32-bit float, two channels, written by a background thread (a ThreadedWriter on a
//                  TimeSliceThread). push() is called from the audio thread: it copies into the writers' FIFOs and updates the meters, and
//                  it allocates nothing, takes no lock and does no file IO. start() and stop() run on the message thread.
//   beginRecording the armed tracks of a session become takes; refused, with the reason, when no track is armed.
//   finishRecording puts the finished takes on their tracks as clips: one undo step, starting `latencyCompensation` samples early.
//
// Threads: prepare() is called with the device stopped. push() may run from the audio thread only while a take is running. stop() waits
// for a push that is in flight (the `busy` flag) before it lets the writers go, so a file never receives a block after it is closed.
namespace olympus
{
class Recorder
{
public:
    static constexpr int kMaxInputs = 32; // the meters cover the first kMaxInputs device inputs

    // A take: the track it goes to, the device inputs it records (inRight -1 = mono: inLeft on both channels) and the name of its file.
    struct Take
    {
        int track = -1;
        int inLeft = 0;
        int inRight = 1;
        juce::String name;
    };

    // A take that has stopped: its track, its file and the frames in it (the frames the file holds, which is what was accepted).
    struct Finished
    {
        int track = -1;
        juce::File file;
        juce::int64 frames = 0;
    };

    Recorder() : thread ("Olympus recorder")
    {
        for (auto& m : meters)
            m.store (0.0f);
    }
    Recorder (const Recorder&) = delete;
    Recorder& operator= (const Recorder&) = delete;
    ~Recorder() { stop(); }

    // Message thread, with the device stopped: the largest block push() will be given, and the rate the buffers are sized for.
    void prepare (double rate, int maxBlock)
    {
        if (recording.load())
            return;
        blockMax = juce::jmax (1, maxBlock);
        bufferSamples = (int) juce::jmax (65536.0, rate * 6.0); // six seconds of a disk stall before a take drops audio
        silence.assign ((size_t) blockMax, 0.0f);
        prepared.store (true);
    }

    // Message thread. Starts one file per take in `folder`, at `rate`. Refused, with the reason, when a take is wrong, the folder cannot be
    // made or a file cannot be written; then no file is left behind. A file is never overwritten: a name that exists gets the next number.
    bool start (const std::vector<Take>& takes, const juce::File& folder, double rate, juce::String& error)
    {
        if (recording.load()) { error = "Already recording."; return false; }
        if (! prepared.load()) { error = "The recorder is not prepared: the audio device is not running."; return false; }
        if (takes.empty()) { error = "No take to record."; return false; }
        if (! (rate > 0.0)) { error = "The recording rate must be above zero."; return false; }
        for (const auto& t : takes)
        {
            if (t.track < 0) { error = "A take needs a track."; return false; }
            if (t.inLeft < 0 || t.inRight < -1) { error = "A take needs its inputs: the left 0 or more, the right 0 or more, or -1 for mono."; return false; }
        }
        if (! folder.createDirectory().wasOk()) { error = "Cannot make the folder " + folder.getFullPathName(); return false; }

        if (! thread.isThreadRunning())
            thread.startThread();
        std::vector<Lane> built;
        const auto abandon = [&]
        {
            for (auto& l : built) { l.writer.reset(); l.file.deleteFile(); }
            built.clear();
            if (thread.isThreadRunning()) thread.stopThread (5000);
        };
        juce::WavAudioFormat wav;
        for (const auto& t : takes)
        {
            const auto file = nextTakeFile (folder, t);
            std::unique_ptr<juce::OutputStream> stream (file.createOutputStream());
            if (stream == nullptr) { error = "Cannot write " + file.getFullPathName(); abandon(); return false; }
            auto writer = wav.createWriterFor (stream, juce::AudioFormatWriterOptions {}
                                                           .withSampleRate (rate)
                                                           .withNumChannels (2)
                                                           .withBitsPerSample (32)
                                                           .withSampleFormat (juce::AudioFormatWriterOptions::SampleFormat::floatingPoint));
            if (writer == nullptr) { error = "Cannot create a WAV writer for " + file.getFileName(); abandon(); return false; }
            Lane lane;
            lane.take = t;
            lane.file = file;
            lane.writer = std::make_unique<juce::AudioFormatWriter::ThreadedWriter> (writer.release(), thread, bufferSamples);
            built.push_back (std::move (lane));
        }

        framesPushed.store (0);
        dropped.store (0);
        lanes = std::move (built);
        recording.store (true);
        return true;
    }

    // Audio thread. The inputs of one device block: inputs[i] holds numSamples samples of device input i. Blocks longer than the prepared
    // maximum are cut. Meters are kept whether or not a take runs; takes are written only while one runs.
    void push (const float* const* inputs, int numInputs, int numSamples)
    {
        if (! prepared.load() || inputs == nullptr || numSamples <= 0)
            return;
        for (int offset = 0; offset < numSamples; offset += blockMax)
        {
            const int n = juce::jmin (blockMax, numSamples - offset);
            const int metered = juce::jmin (numInputs, kMaxInputs);
            for (int i = 0; i < metered; ++i)
            {
                const float p = peakOf (inputs[i] + offset, n);
                if (p > meters[(size_t) i].load())
                    meters[(size_t) i].store (p);
            }
            busy.store (true);
            if (recording.load())
            {
                for (auto& lane : lanes)
                {
                    const float* channels[2];
                    channels[0] = inputPointer (inputs, numInputs, lane.take.inLeft, offset);
                    channels[1] = lane.take.inRight < 0 ? channels[0] : inputPointer (inputs, numInputs, lane.take.inRight, offset);
                    if (lane.writer->write (channels, n))
                        lane.accepted += n;
                    else
                        dropped.fetch_add (n); // the writer's FIFO was full: the disk did not keep up
                }
                framesPushed.fetch_add (n);
            }
            busy.store (false);
        }
    }

    // Message thread. Ends the takes: waits for a push in flight, flushes every file, and returns them. Nothing when no take runs.
    std::vector<Finished> stop()
    {
        std::vector<Finished> done;
        recording.store (false);
        while (busy.load())
            juce::Thread::sleep (1);
        for (auto& lane : lanes)
            done.push_back ({ lane.take.track, lane.file, lane.accepted });
        lanes.clear(); // each writer writes what is left into its file when it goes
        if (thread.isThreadRunning())
            thread.stopThread (5000);
        return done;
    }

    bool isRecording() const { return recording.load(); }
    // The frames pushed since the take started (every take hears the same blocks).
    juce::int64 framesRecorded() const { return framesPushed.load(); }
    // Frames a take could not take because its disk writer fell behind. Should stay 0.
    juce::int64 samplesDropped() const { return dropped.load(); }
    // The peak of one device input since it was last read, linear (1 = full scale). Reading resets it.
    float inputPeak (int input) { return juce::isPositiveAndBelow (input, kMaxInputs) ? meters[(size_t) input].exchange (0.0f) : 0.0f; }

private:
    struct Lane
    {
        Take take;
        juce::File file;
        std::unique_ptr<juce::AudioFormatWriter::ThreadedWriter> writer;
        juce::int64 accepted = 0; // frames the writer took (touched by the audio thread while recording only)
    };

    // The file of a take: "<track name>-take-<n>.wav" with the first n that is not taken in the folder.
    static juce::File nextTakeFile (const juce::File& folder, const Take& take)
    {
        const auto base = juce::File::createLegalFileName (take.name.isNotEmpty() ? take.name : "Track " + juce::String (take.track + 1));
        for (int n = 1;; ++n)
        {
            auto file = folder.getChildFile (base + "-take-" + juce::String (n) + ".wav");
            if (! file.exists())
                return file;
        }
    }

    static float peakOf (const float* x, int n)
    {
        float p = 0.0f;
        for (int i = 0; i < n; ++i)
            p = juce::jmax (p, std::abs (x[i]));
        return p;
    }

    // The block of device input `index` at `offset`, or silence for an input the device does not have.
    const float* inputPointer (const float* const* inputs, int numInputs, int index, int offset) const
    {
        return index >= 0 && index < numInputs ? inputs[index] + offset : silence.data();
    }

    juce::TimeSliceThread thread;        // declared first: the lanes (and their writers) go before it
    std::vector<Lane> lanes;
    std::vector<float> silence;          // zeros, blockMax long
    int blockMax = 512;
    int bufferSamples = 65536;
    std::atomic<bool> prepared { false }, recording { false }, busy { false };
    std::atomic<juce::int64> framesPushed { 0 }, dropped { 0 };
    std::array<std::atomic<float>, kMaxInputs> meters;
};

// Starts a take on every armed track of the session, each on its own inputs. Refused, with the reason, when no track is armed. Takes the
// session's lock only to read the tracks, not while the files are written.
inline bool beginRecording (Recorder& recorder, Session& session, const juce::File& folder, double deviceRate, juce::String& error)
{
    std::vector<Recorder::Take> takes;
    {
        const juce::ScopedLock sl (session.lock);
        for (int i = 0; i < (int) session.tracks.size(); ++i)
        {
            const auto& t = *session.tracks[(size_t) i];
            if (t.armed)
                takes.push_back ({ i, t.inputLeft, t.inputRight, t.name });
        }
    }
    if (takes.empty())
    {
        error = "No track is armed: arm a track to record it.";
        return false;
    }
    return recorder.start (takes, folder, deviceRate, error);
}

// Puts each finished take on its track as a clip, all as one undo step. A take begins `latencyCompensation` samples before `startSample`
// (the point where recording began), so it lines up with what was heard; never before 0. When the session has no rate yet, the first clip
// read sets it to the take's rate (Session::loadPooled), as for any clip. A take with no frames is skipped (its file is kept). Returns false
// with the reason if a clip cannot be added; the clips added before it stay (the step is still one undo step).
inline bool finishRecording (const std::vector<Recorder::Finished>& takes, juce::int64 startSample, int latencyCompensation, Editor& editor,
                             juce::String& error)
{
    Editor::Gesture gesture (editor, "Record");
    const juce::int64 begin = juce::jmax ((juce::int64) 0, startSample - (juce::int64) latencyCompensation);
    for (const auto& take : takes)
    {
        if (take.frames <= 0)
            continue;
        if (! editor.addClip (take.track, take.file, begin, 0, 0, error).valid())
            return false;
    }
    return true;
}
} // namespace olympus
