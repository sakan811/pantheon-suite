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
// Threads: prepare() is called with the device stopped. push() runs from the audio thread on every device block the opening and start
// guards pass: the meters always, the takes only while one runs (a block the guards hold is counted as skipped, not pushed). stop() waits
// for a push that is in flight (the `busy` flag) before it lets the writers go, so a file never receives a block after it is closed.
namespace olympus
{
// The take's file writer: JUCE's ThreadedWriter ignores the result of the writes it makes on its thread, so this one records a failed write in
// `failed` (set on the writer thread, read at stop). It passes every block on to the WAV writer it holds.
class CheckedWriter : public juce::AudioFormatWriter
{
public:
    CheckedWriter (juce::AudioFormatWriter* inner, std::shared_ptr<std::atomic<bool>> failedFlag)
        : AudioFormatWriter (nullptr, "checked", inner->getSampleRate(), (unsigned int) inner->getNumChannels(), 32),
          writer (inner), failed (std::move (failedFlag))
    {
        usesFloatingPointData = writer->isFloatingPoint(); // writeFromFloatArrays converts to ints unless this is set
    }

    bool write (const int** samples, int numSamples) override
    {
        const bool ok = writer->write (samples, numSamples);
        if (! ok) failed->store (true);
        return ok;
    }

private:
    std::unique_ptr<juce::AudioFormatWriter> writer;
    std::shared_ptr<std::atomic<bool>> failed;
};

class Recorder
{
public:
    static constexpr int kMaxInputs = 64; // the meters, the takes and the AUDIO dialog stop at the first kMaxInputs device inputs
    static constexpr juce::int64 kNoBlockStart = -1; // push()'s `blockStart` when the caller has no playhead to give it (a check)

    // A take: the track it goes to, the device inputs it records (inRight -1 = mono: inLeft on both channels) and the name of its file.
    struct Take
    {
        int track = -1;
        int inLeft = 0;
        int inRight = 1;
        juce::String name;
    };

    // A take that has stopped: its track, its file and the frames in it (the frames the file holds, which is what was accepted). `lost` is the
    // frames the disk could not take (not written, so the file is shorter by that much); `failed` says the file's writes failed at some point.
    struct Finished
    {
        int track = -1;
        juce::File file;
        juce::int64 frames = 0;
        juce::int64 lost = 0;
        bool failed = false;
    };

    Recorder() : thread ("Olympus recorder")
    {
        for (auto& m : meters)
            m.store (0.0f);
        for (auto& p : position)
            p.store (-1);
    }
    Recorder (const Recorder&) = delete;
    Recorder& operator= (const Recorder&) = delete;
    ~Recorder() { stop(); }

    // Message thread, with the device stopped: the largest block push() will be given, the rate the buffers are sized for, and which device
    // inputs are switched on (by absolute number). The device's callback gets only the active inputs, in order, so input i is at the number
    // of active inputs below it; an input that is not active maps to -1 (silence, and REC refuses it).
    void prepare (double rate, int maxBlock, const juce::BigInteger& activeInputs = allInputs())
    {
        const juce::SpinLock::ScopedLockType sl (prepareLock);
        if (recording.load())
            return;
        blockMax = juce::jmax (1, maxBlock);
        bufferSamples = (int) juce::jmax (65536.0, rate * 6.0); // six seconds of a disk stall before a take drops audio
        silence.assign ((size_t) blockMax, 0.0f);
        int active = 0;
        for (int i = 0; i < kMaxInputs; ++i)
        {
            const bool on = activeInputs[i];
            position[(size_t) i].store (on ? active : -1);
            if (on)
            {
                activeList[(size_t) active] = i;
                ++active;
            }
        }
        numActive = active;
        prepared.store (true);
    }

    // Message thread. Starts one file per take in `folder`, at `rate`. Refused, with the reason, when a take is wrong, the folder cannot be
    // made or a file cannot be written; then no file is left behind. A file is never overwritten: a name that exists gets the next number.
    // `opening`, when given, is the caller's "the files are still opening" guard (the window's `recordStarting`, whose callback then
    // counts a block instead of pushing it): this clears the take's counters first and raises the guard only after, before the slow work of
    // the folder and the headers, so a block that was already counted is never erased by the clear. The caller lets go of it, when the take
    // began or was refused.
    bool start (const std::vector<Take>& takes, const juce::File& folder, double rate, juce::String& error,
                std::atomic<bool>* opening = nullptr)
    {
        if (recording.load()) { error = "Already recording."; return false; }
        if (! prepared.load()) { error = "The recorder is not prepared: the audio device is not running."; return false; }
        if (takes.empty()) { error = "No take to record."; return false; }
        if (! (rate > 0.0)) { error = "The recording rate must be above zero."; return false; }
        // The counters of the take that begins now, cleared before anything slow happens (the blocks that arrive while the files open are
        // counted as skipped, and that count must not be cleared by the count itself). What the last take left is read by no one again.
        framesPushed.store (0);
        dropped.store (0);
        takeStart.store (-1);
        headSkipped.store (0);
        if (opening != nullptr)
            opening->store (true);   // from here every block the callback gets is counted as skipped; the caller lets go of the guard
        for (const auto& t : takes)
        {
            if (t.track < 0) { error = "A take needs a track."; return false; }
            if (t.inLeft < 0 || t.inRight < -1) { error = "A take needs its inputs: the left 0 or more, the right 0 or more, or -1 for mono."; return false; }
            for (const int input : { t.inLeft, t.inRight })
            {
                // an input past what the recorder meters at all: its own reason, not the one for an input that is switched off
                if (input >= kMaxInputs)
                {
                    error = (t.name.isNotEmpty() ? t.name : "Track " + juce::String (t.track + 1)) + " records input " + juce::String (input + 1)
                            + ", past the recorder's " + juce::String (kMaxInputs) + "-input limit";
                    return false;
                }
                if (input >= 0 && positionOf (input) < 0)
                {
                    error = (t.name.isNotEmpty() ? t.name : "Track " + juce::String (t.track + 1)) + " records input " + juce::String (input + 1)
                            + ", which is not switched on in AUDIO";
                    return false;
                }
            }
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
            if (writer == nullptr)
            {
                stream.reset();      // the file was created by createOutputStream: it goes too, so no empty take is left behind
                file.deleteFile();
                error = "Cannot create a WAV writer for " + file.getFileName();
                abandon();
                return false;
            }
            Lane lane;
            lane.take = t;
            lane.file = file;
            lane.writer = std::make_unique<juce::AudioFormatWriter::ThreadedWriter> (new CheckedWriter (writer.release(), lane.failed), thread, bufferSamples);
            built.push_back (std::move (lane));
        }

        lanes = std::move (built);
        recording.store (true);
        return true;
    }

    // Audio thread. The inputs of one device block: inputs[k] holds numSamples samples of the k-th ACTIVE device input (the callback's order,
    // see prepare); takes and meters name inputs by their absolute number. Blocks longer than the prepared maximum are cut. Meters are kept
    // whether or not a take runs; takes are written only while one runs. Returns true when this is the take's first block.
    // `blockStart` is the playhead at this block's first frame, which the caller reads before the block plays (the recorder does not own the
    // transport); the first block of the take latches it in `takeStart` with one compare-exchange, even if the writers refuse its frames
    // (those are counted lost), inside the busy scope stop() waits for, so the take's start and its first block are one event that a stop
    // cannot come between (DeviceCallback::deviceBlock). Callers with no playhead omit it, and `takeStart` stays as it is.
    bool push (const float* const* inputs, int numInputs, int numSamples, juce::int64 blockStart = kNoBlockStart)
    {
        const juce::SpinLock::ScopedTryLockType sl (prepareLock);
        if (! sl.isLocked() || ! prepared.load() || inputs == nullptr || numSamples <= 0)
            return false;
        struct BusyScope
        {
            std::atomic<bool>& flag;
            explicit BusyScope (std::atomic<bool>& f) : flag (f) { flag.store (true); }
            ~BusyScope() { flag.store (false); }
        };
        const BusyScope busyScope (busy);
        const bool rec = recording.load();
        const bool first = rec;
        if (rec && blockStart >= 0)
        {
            juce::int64 none = -1;
            takeStart.compare_exchange_strong (none, blockStart);   // only the first block of the take wins, refused frames or not
        }
        for (int offset = 0; offset < numSamples; offset += blockMax)
        {
            const int n = juce::jmin (blockMax, numSamples - offset);
            for (int k = 0; k < numActive; ++k)   // the active inputs only, not all kMaxInputs slots: prepare builds the list under this lock
            {
                const int input = activeList[(size_t) k];
                const int at = positionOf (input);
                if (at < 0 || at >= numInputs)
                    continue;
                const float p = peakOf (inputs[at] + offset, n);
                if (p > meters[(size_t) input].load())
                    meters[(size_t) input].store (p);
            }
            if (rec)
            {
                for (auto& lane : lanes)
                {
                    const float* channels[2];
                    channels[0] = inputPointer (inputs, numInputs, lane.take.inLeft, offset);
                    channels[1] = lane.take.inRight < 0 ? channels[0] : inputPointer (inputs, numInputs, lane.take.inRight, offset);
                    if (lane.writer->write (channels, n))
                        lane.accepted += n;
                    else
                    {
                        lane.lost += n;        // the writer's FIFO was full: the disk did not keep up
                        dropped.fetch_add (n);
                    }
                }
                framesPushed.fetch_add (n);
            }
        }
        return first;
    }

    // Message thread. Ends the takes: waits for a push in flight, flushes every file, and returns them. Nothing when no take runs.
    std::vector<Finished> stop()
    {
        std::vector<Finished> done;
        recording.store (false);
        while (busy.load())
            juce::Thread::sleep (1);
        std::vector<std::shared_ptr<std::atomic<bool>>> flags;
        for (auto& lane : lanes)
        {
            done.push_back ({ lane.take.track, lane.file, lane.accepted, lane.lost, false });
            flags.push_back (lane.failed);
        }
        lanes.clear(); // each writer writes what is left into its file when it goes: a write that fails there sets its flag
        for (size_t i = 0; i < done.size(); ++i)
            done[i].failed = flags[i]->load();
        if (thread.isThreadRunning())
            thread.stopThread (5000);
        return done;
    }

    // Checks only (the message thread, no take running): counts `frames` lost on lane `lane` as push() would when the FIFO was full, or marks
    // its file as failed as a write that failed would. Lets the checks test the window's report without a slow disk.
    void lostForTest (int lane, juce::int64 frames)
    {
        if (! juce::isPositiveAndBelow (lane, (int) lanes.size())) return;
        lanes[(size_t) lane].lost += frames;
        dropped.fetch_add (frames);
    }
    void failedForTest (int lane)
    {
        if (juce::isPositiveAndBelow (lane, (int) lanes.size())) lanes[(size_t) lane].failed->store (true);
    }

    bool isRecording() const { return recording.load(); }
    // The frames pushed since the take started (every take hears the same blocks).
    juce::int64 framesRecorded() const { return framesPushed.load(); }
    // Frames a take could not take because its disk writer fell behind. Should stay 0.
    juce::int64 samplesDropped() const { return dropped.load(); }
    // Where the take's first block began (the playhead the caller gave push()), -1 when no block ran; the first block latches it even if its
    // frames were refused. Decided inside push(), under the busy flag stop() waits for, so a stop at the first block cannot consume it
    // before it lands. start() clears it.
    juce::int64 takeStartSample() const { return takeStart.load(); }
    // Frames of input that arrived while the take's files were opening and were not pushed (start() and the headers are the message
    // thread's work, push() is the audio thread's). The caller counts them with countSkippedWhileOpening(); the window reports them at stop
    // with the lost frames, so a take whose head was never written says so. start() clears it.
    juce::int64 samplesSkippedWhileOpening() const { return headSkipped.load(); }
    // Audio thread: one block's frames that arrived before the takes could be written. An atomic add, nothing else.
    void countSkippedWhileOpening (int frames) { if (frames > 0) headSkipped.fetch_add (frames); }
    // The peak of one device input since it was last read, linear (1 = full scale). Reading resets it.
    float inputPeak (int input) { return juce::isPositiveAndBelow (input, kMaxInputs) ? meters[(size_t) input].exchange (0.0f) : 0.0f; }

private:
    struct Lane
    {
        Take take;
        juce::File file;
        std::unique_ptr<juce::AudioFormatWriter::ThreadedWriter> writer;
        juce::int64 accepted = 0; // frames the writer took (touched by the audio thread while recording only)
        juce::int64 lost = 0;     // frames the writer's FIFO refused (the audio thread only, while recording)
        std::shared_ptr<std::atomic<bool>> failed = std::make_shared<std::atomic<bool>> (false); // a write to the file failed (CheckedWriter)
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

    static juce::BigInteger allInputs()
    {
        juce::BigInteger all;
        all.setRange (0, kMaxInputs, true);
        return all;
    }

    // The callback's position of device input `input` (by absolute number), or -1 when it is not switched on.
    int positionOf (int input) const
    {
        return juce::isPositiveAndBelow (input, kMaxInputs) ? position[(size_t) input].load() : -1;
    }

    // The block of device input `input` (absolute) at `offset`, or silence for an input that is not active or not in this block.
    const float* inputPointer (const float* const* inputs, int numInputs, int input, int offset) const
    {
        const int at = positionOf (input);
        return at >= 0 && at < numInputs ? inputs[at] + offset : silence.data();
    }

    juce::TimeSliceThread thread;        // declared first: the lanes (and their writers) go before it
    juce::SpinLock prepareLock;          // guards prepare() against push() when audioDeviceAboutToStart marshals via callAsync
    std::vector<Lane> lanes;
    std::vector<float> silence;          // zeros, blockMax long
    int blockMax = 512;
    int bufferSamples = 65536;
    std::atomic<bool> prepared { false }, recording { false }, busy { false };
    std::atomic<juce::int64> framesPushed { 0 }, dropped { 0 };
    std::atomic<juce::int64> takeStart { -1 }, headSkipped { 0 }; // where the take's first block began; frames it never got
    std::array<std::atomic<float>, kMaxInputs> meters;
    std::array<std::atomic<int>, kMaxInputs> position;   // per absolute input: its position in the callback, or -1
    std::array<int, kMaxInputs> activeList {};           // the active absolute inputs, compact: what push()'s meters walk
    int numActive = 0;                                   // (both written by prepare and read by push under prepareLock)
};

// Starts a take on every armed track of the session, each on its own inputs. Refused, with the reason, when no track is armed. Takes the
// session's lock only to read the tracks, not while the files are written. `opening` is passed to Recorder::start (its guard for the blocks
// that arrive while the files open).
inline bool beginRecording (Recorder& recorder, Session& session, const juce::File& folder, double deviceRate, juce::String& error,
                            std::atomic<bool>* opening = nullptr)
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
    return recorder.start (takes, folder, deviceRate, error, opening);
}

// Puts each finished take on its track as a clip, all as one undo step. A take begins `latencyCompensation` samples before `startSample`
// (the point where recording began), so it lines up with what was heard. Where that is before 0, the clip starts at 0 and the head of the
// file that falls before 0 is skipped (its offset), so the rest still plays on time. When the session has no rate yet, the first clip
// read sets it to the take's rate (Session::loadPooled), as for any clip. A take with no frames is skipped (its file is kept); a take
// that is all before 0 is skipped too. A take that cannot be placed is skipped and the rest are still placed; the first reason is returned
// in `error` (false), and the files stay on disk. The clips added stay (the step is still one undo step). `placed`, when given, collects
// the clips that were placed, in the order of the takes: the window selects what the takes actually became (see MainComponent's
// finishRecordingTakes), not any clip that happens to start at the same sample.
inline bool finishRecording (const std::vector<Recorder::Finished>& takes, juce::int64 startSample, int latencyCompensation, Editor& editor,
                             juce::String& error, std::vector<ClipRef>* placed = nullptr)
{
    Editor::Gesture gesture (editor, "Record");
    const juce::int64 latency = juce::jmax ((juce::int64) 0, (juce::int64) latencyCompensation);
    const juce::int64 begin = juce::jmax ((juce::int64) 0, startSample - latency);
    const juce::int64 skip = juce::jmax ((juce::int64) 0, latency - startSample); // samples of each take's head before the timeline's 0
    // a take that cannot be placed does not stop the others: the first reason is reported after the loop, the files stay on disk
    juce::String firstProblem;
    const auto note = [&firstProblem] (const juce::String& problem) { if (firstProblem.isEmpty()) firstProblem = problem; };
    for (const auto& take : takes)
    {
        if (take.frames <= 0)
            continue;
        if (take.frames - skip < 1)
        {
            note ("A take on track " + juce::String (take.track + 1) + " was shorter than the latency and was not placed: "
                  + take.file.getFileName() + " is kept.");
            continue;
        }
        juce::String clipError;
        const auto ref = editor.addClip (take.track, take.file, begin, skip, take.frames - skip, clipError);
        if (! ref.valid())
        {
            note (clipError.isNotEmpty() ? clipError : "A take on track " + juce::String (take.track + 1) + " could not be placed: " + take.file.getFileName() + " is kept.");
            continue;
        }
        if (placed != nullptr)
            placed->push_back (ref);
    }
    if (firstProblem.isNotEmpty())
        error = firstProblem;
    return firstProblem.isEmpty();
}
} // namespace olympus
