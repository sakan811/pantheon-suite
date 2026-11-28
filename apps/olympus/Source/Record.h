// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 The Pantheon authors

#pragma once

#include "Edit.h"

#include <array>
#include <atomic>
#include <memory>
#include <utility>
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

    // Audio thread: the device block of absolute input `absolute` (its position in the callback's `in`), or nullptr when the input is off or
    // not in this block. Used by DeviceCallback::deviceBlock to hand the session the inputs a monitoring track plays.
    const float* inputFor (const float* const* in, int numIn, int absolute) const
    {
        const int at = positionOf (absolute);
        return (in != nullptr && at >= 0 && at < numIn) ? in[at] : nullptr;
    }

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

        // a session of MIDI tracks only records no file, so it makes no folder either
        if (! takes.empty() && ! folder.createDirectory().wasOk()) { error = "Cannot make the folder " + folder.getFullPathName(); return false; }

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

    // Audio thread. The meters of one device block, as push() updates them, and nothing else: no take is written, no frame is counted, no
    // latch is set. The count-in (F5) gives the meters the blocks the takes do not get; the same try-lock as push().
    void meterOnly (const float* const* inputs, int numInputs, int numSamples)
    {
        const juce::SpinLock::ScopedTryLockType sl (prepareLock);
        if (! sl.isLocked() || ! prepared.load() || inputs == nullptr || numSamples <= 0)
            return;
        for (int offset = 0; offset < numSamples; offset += blockMax)
        {
            const int n = juce::jmin (blockMax, numSamples - offset);
            for (int k = 0; k < numActive; ++k)
            {
                const int input = activeList[(size_t) k];
                const int at = positionOf (input);
                if (at < 0 || at >= numInputs)
                    continue;
                const float p = peakOf (inputs[at] + offset, n);
                if (p > meters[(size_t) input].load())
                    meters[(size_t) input].store (p);
            }
        }
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

// Starts a take on every armed audio track of the session, each on its own inputs, and the MIDI capture when a MIDI track is armed. A MIDI
// track records no file: its input is kept as notes (Session::midiRecorder) and put on the timeline at the stop. Refused, with the reason,
// when no track is armed. Takes the session's lock only to read the tracks, not while the files are written. `opening` is passed to
// Recorder::start (its guard for the blocks that arrive while the files open).
inline bool beginRecording (Recorder& recorder, Session& session, const juce::File& folder, double deviceRate, juce::String& error,
                            std::atomic<bool>* opening = nullptr)
{
    std::vector<Recorder::Take> takes;
    bool midiArmed = false;
    {
        const juce::ScopedLock sl (session.lock);
        for (int i = 0; i < (int) session.tracks.size(); ++i)
        {
            const auto& t = *session.tracks[(size_t) i];
            if (! t.armed || t.isBus())
                continue;
            if (t.isMidi)
                midiArmed = true;
            else
                takes.push_back ({ i, t.inputLeft, t.inputRight, t.name });
        }
    }
    if (takes.empty() && ! midiArmed)
    {
        error = "No track is armed: arm a track to record it.";
        return false;
    }
    // a take with no audio takes is valid: the recorder runs (its counters, the take's start) and writes nothing to disk
    if (! recorder.start (takes, folder, deviceRate, error, opening))
        return false;
    if (midiArmed)
        session.midiRecorder.start();
    return true;
}

// A MIDI take's notes (F7): the events the take kept, paired by channel and pitch (a note on of velocity 0 is a note off), moved earlier by
// `latency` samples (the playback latency, so a note sits where it was heard) and placed on the tempo map. A note still held at the stop ends
// at `stopSample`, or at its last event when that is later. A note whose shifted start falls before 0 is dropped, not moved to 0. The sustain
// pedal (CC 64, down at 64 or more) holds the notes that are released while it is down: they end at the pedal up, at the same key played
// again, or at the stop. With `punch` (the take's range on the timeline) only the notes that start in [punch.first, punch.second) are kept, and
// each ends at punch.second at the latest. The clip starts at the bar before the first note's bar (at 0 at the earliest) and
// ends at the bar after the last note-off's bar; its notes' beats count from its start. No note, no clip.
struct RecordedMidi
{
    juce::int64 start = 0;          // the clip's start on the timeline, in samples
    juce::int64 length = 0;         // the clip's length, in samples
    std::vector<MidiNote> notes;    // beats from the clip's start
};

// `wrapAt` (F8) is the loop's end when the take was a loop recording: the transport wraps there, so the positions are not in order. The events
// are paired in the order they arrived (the vector's order), and a note whose off position is before its on position was held across the
// wrap: it ends at `wrapAt`. Without a loop the events are sorted by position first, as they always were (the take's events are in arrival
// order already, and the sort keeps a list written out of order paired as it was meant). -1 for a take without a loop.
inline RecordedMidi buildRecordedMidi (std::vector<MidiRecorder::Event> events, juce::int64 stopSample, juce::int64 latency, const TempoMap& map,
                                       const std::pair<juce::int64, juce::int64>* punch = nullptr, juce::int64 wrapAt = -1)
{
    RecordedMidi out;
    if (wrapAt < 0)
        std::stable_sort (events.begin(), events.end(), [] (const MidiRecorder::Event& a, const MidiRecorder::Event& b) { return a.position < b.position; });
    struct Open { juce::int64 on = -1; int pitch = 0, velocity = 0, channel = 1; bool released = false; };
    struct Span { juce::int64 on = 0, off = 0; int pitch = 0, velocity = 0, channel = 1; };
    std::array<Open, 16 * 128> open {};   // by channel (0-15) and pitch
    std::array<bool, 16> pedal {};        // the sustain pedal of each channel: down at 64 or more
    std::vector<Span> spans;
    const auto close = [&] (int key, juce::int64 at)
    {
        const auto o = open[(size_t) key];
        open[(size_t) key] = Open {};
        // the note's shift. Shifted before 0 it started before the take, so it is dropped rather than moved to 0 (the rest of the take keeps its time)
        const juce::int64 on = o.on - latency;
        if (on < 0)
            return;
        // at least one sample long, so a note cut at once is still a note
        // a position before the note's own start is the other side of the wrap: the note ran to the loop's end
        const juce::int64 end = (wrapAt >= 0 && at < o.on) ? wrapAt : at;
        juce::int64 off = juce::jmax (on + 1, end - latency);
        if (punch != nullptr)
        {
            // a note that starts outside the punch range is not in the take; one that runs past the punch-out is cut there
            if (on < punch->first || on >= punch->second)
                return;
            off = juce::jmin (off, punch->second);
        }
        spans.push_back ({ on, off, o.pitch, o.velocity, o.channel });
    };

    juce::int64 lastPosition = 0;
    for (const auto& e : events)
    {
        lastPosition = juce::jmax (lastPosition, e.position);
        if (e.size < 2)
            continue;
        const int kind = e.bytes[0] & 0xF0;
        const int channel = e.bytes[0] & 0x0F;
        if (kind == 0xB0 && e.size == 3 && e.bytes[1] == 64)   // the sustain pedal: up ends the notes it was holding
        {
            if (e.bytes[2] >= 64)
                pedal[(size_t) channel] = true;
            else
            {
                pedal[(size_t) channel] = false;
                for (int p = 0; p < 128; ++p)
                    if (open[(size_t) (channel * 128 + p)].released)
                        close (channel * 128 + p, e.position);
            }
            continue;
        }
        const int pitch = e.bytes[1] & 0x7F;
        const int key = channel * 128 + pitch;
        if (kind == 0x90 && e.size == 3 && e.bytes[2] > 0)
        {
            if (open[(size_t) key].on >= 0)
                close (key, e.position);   // the same key again: the held note ends where the new one starts
            open[(size_t) key] = Open { e.position, pitch, (int) (e.bytes[2] & 0x7F), channel + 1 };
        }
        else if (kind == 0x80 || (kind == 0x90 && e.size == 3))   // a note off, or a note on of velocity 0
        {
            if (open[(size_t) key].on >= 0)
            {
                if (pedal[(size_t) channel])
                    open[(size_t) key].released = true;   // the pedal holds it: it ends at the pedal up, or where the same key is played again
                else
                    close (key, e.position);
            }
        }
    }
    const juce::int64 heldEnd = juce::jmax (stopSample, lastPosition);
    for (int key = 0; key < (int) open.size(); ++key)
        if (open[(size_t) key].on >= 0)
            close (key, heldEnd);

    if (spans.empty())
        return out;
    juce::int64 first = spans.front().on, last = spans.front().off;
    for (const auto& s : spans)
    {
        first = juce::jmin (first, s.on);
        last = juce::jmax (last, s.off);
    }
    const double bar = map.beatsPerBar();
    const double firstBeat = map.samplesToBeats ((double) first), lastBeat = map.samplesToBeats ((double) last);
    const double startBeat = juce::jmax (0.0, (std::floor (firstBeat / bar) - 1.0) * bar);
    const double endBeat = (std::floor (lastBeat / bar) + 1.0) * bar;
    out.start = (juce::int64) std::llround (map.beatsToSamples (startBeat));
    out.length = juce::jmax ((juce::int64) 1, (juce::int64) std::llround (map.beatsToSamples (endBeat)) - out.start);

    const double clipBeat = map.samplesToBeats ((double) out.start);
    for (const auto& s : spans)
    {
        MidiNote note;
        note.startBeat = map.samplesToBeats ((double) s.on) - clipBeat;
        note.durationBeats = map.samplesToBeats ((double) s.off) - map.samplesToBeats ((double) s.on);
        note.pitch = s.pitch;
        note.velocity = juce::jlimit (1, 127, s.velocity);
        note.channel = s.channel;
        out.notes.push_back (note);
    }
    return out;
}

// Puts the notes of a MIDI take on each of `tracks` (the armed MIDI tracks) as one clip apiece. Called inside the caller's Editor::Gesture, so the
// clips are the same undo step as the audio takes. No note, no clip. A track that cannot take its clip is skipped and the reason is returned.
inline bool finishMidiRecording (const std::vector<MidiRecorder::Event>& events, juce::int64 stopSample, juce::int64 latency,
                                 const std::vector<int>& tracks, const TempoMap& map, Editor& editor, juce::String& error,
                                 std::vector<MidiClipRef>* placed = nullptr, const std::pair<juce::int64, juce::int64>* punch = nullptr,
                                 juce::int64 wrapAt = -1)
{
    const auto notes = buildRecordedMidi (events, stopSample, latency, map, punch, wrapAt);
    if (notes.notes.empty())
        return true;
    bool ok = true;
    for (const int track : tracks)
    {
        juce::String clipError;
        const auto ref = editor.addMidiClip (track, notes.start, notes.length, {}, clipError);
        if (! ref.valid())
        {
            if (error.isEmpty()) error = clipError.isNotEmpty() ? clipError : "A MIDI take could not be placed on track " + juce::String (track + 1) + ".";
            ok = false;
            continue;
        }
        for (const auto& n : notes.notes)
        {
            juce::String noteError;
            if (editor.addMidiNote (ref.track, ref.clip, n.startBeat, n.durationBeats, n.pitch, n.velocity, n.channel, noteError) < 0 && error.isEmpty())
                error = noteError;
        }
        if (placed != nullptr)
            placed->push_back (ref);
    }
    return ok;
}

// The passes of a take recorded with the loop on (F8). The file runs on through the loop: file sample i was played at the elapsed time i - latency
// after the take's start `takeStart`, and the transport wrapped at `loopEnd` (back to `loopStart`). Pass 0 is the first run from the take's start
// to the loop's end, placed at takeStart - latency (the caller skips what falls before 0, as for any take). Pass k >= 1 is one full run of the
// loop, placed at loopStart; the last one is cut to what was recorded. A pass shorter than `minLength` is dropped (the file keeps going past it).
// With L = loopEnd - loopStart and first = latency + (loopEnd - takeStart), pass 0 covers [0, first) and pass k covers [first + (k - 1) L, ...).
struct LoopPass
{
    juce::int64 fileOffset = 0;
    juce::int64 length = 0;
    juce::int64 timelineStart = 0;
};

inline std::vector<LoopPass> loopPasses (juce::int64 frames, juce::int64 takeStart, juce::int64 latency, juce::int64 loopStart, juce::int64 loopEnd,
                                         juce::int64 minLength)
{
    std::vector<LoopPass> out;
    const juce::int64 loop = loopEnd - loopStart;
    if (frames <= 0 || loop <= 0)
        return out;
    const juce::int64 first = latency + (loopEnd - takeStart);
    const juce::int64 keep = juce::jmax ((juce::int64) 1, minLength);
    const LoopPass head { 0, juce::jmin (frames, first), takeStart - latency };
    if (head.length >= keep)
        out.push_back (head);
    for (juce::int64 offset = first; offset < frames; offset += loop)
    {
        const juce::int64 length = juce::jmin (loop, frames - offset);
        if (length >= keep)
            out.push_back ({ offset, length, loopStart });
    }
    return out;
}

// Puts each finished take on its track as a clip, all as one undo step. A take begins `latencyCompensation` samples before `startSample`
// (the point where recording began), so it lines up with what was heard. Where that is before 0, the clip starts at 0 and the head of the
// file that falls before 0 is skipped (its offset), so the rest still plays on time. When the session has no rate yet, the first clip
// read sets it to the take's rate (Session::loadPooled), as for any clip. A take with no frames is skipped (its file is kept); a take
// that is all before 0 is skipped too. A take that cannot be placed is skipped and the rest are still placed; the first reason is returned
// in `error` (false), and the files stay on disk. The clips added stay (the step is still one undo step). `placed`, when given, collects
// the clips that were placed, in the order of the takes: the window selects what the takes actually became (see MainComponent's
// finishRecordingTakes), not any clip that happens to start at the same sample.
// With `punch` (F5) each take is trimmed to the range [punch.first, punch.second) on the timeline after its place is worked out: the clip starts at
// the punch-in, its offset moves on by what was cut before it and its length stops at the punch-out. A take whose span does not meet the range
// is not placed, and the note says which side it lies on (it ended before the punch-in or started after the punch-out).
// With `loop` (F8: the take was a loop recording, `loop` is its loop's start and end) each take is put down as its passes (loopPasses): one clip per
// pass, added in the order of the passes, so each one's layer is above the last and the last pass plays. The clips of one take share a new
// takeGroup and are numbered by takeIndex from 0. `minPassLength` drops a pass shorter than it (100 ms at the rate, from the caller). Without
// `loop` a take is one clip in a new takeGroup, with takeIndex 0.
inline bool finishRecording (const std::vector<Recorder::Finished>& takes, juce::int64 startSample, int latencyCompensation, Editor& editor,
                             juce::String& error, std::vector<ClipRef>* placed = nullptr,
                             const std::pair<juce::int64, juce::int64>* punch = nullptr,
                             const std::pair<juce::int64, juce::int64>* loop = nullptr, juce::int64 minPassLength = 0)
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
        if (loop != nullptr)
        {
            // each pass is a clip: the head before 0 is skipped on the first one, as the take's own head is
            const auto passes = loopPasses (take.frames, startSample, latency, loop->first, loop->second, minPassLength);
            if (passes.empty())
            {
                note ("A loop take on track " + juce::String (take.track + 1) + " had no pass to place: " + take.file.getFileName() + " is kept.");
                continue;
            }
            const int group = editor.newTakeGroup();
            int index = 0;
            for (const auto& pass : passes)
            {
                juce::int64 at = pass.timelineStart, offset = pass.fileOffset, length = pass.length;
                if (at < 0)
                {
                    offset += -at;
                    length += at;
                    at = 0;
                }
                if (length < 1)
                    continue;
                juce::String clipError;
                const auto ref = editor.addClip (take.track, take.file, at, offset, length, clipError);
                if (! ref.valid())
                {
                    note (clipError.isNotEmpty() ? clipError : "A pass of a loop take on track " + juce::String (take.track + 1) + " could not be placed: "
                          + take.file.getFileName() + " is kept.");
                    continue;
                }
                if (! editor.setTakeOf (ref, group, index++, clipError))
                    note (clipError);
                if (placed != nullptr)
                    placed->push_back (ref);
            }
            continue;
        }
        if (take.frames - skip < 1)
        {
            note ("A take on track " + juce::String (take.track + 1) + " was shorter than the latency and was not placed: "
                  + take.file.getFileName() + " is kept.");
            continue;
        }
        juce::int64 clipStart = begin, clipOffset = skip, clipLength = take.frames - skip;
        if (punch != nullptr)
        {
            const juce::int64 spanEnd = begin + take.frames - skip;   // the take's span on the timeline: [begin, spanEnd)
            if (spanEnd <= punch->first)
            {
                note ("A take on track " + juce::String (take.track + 1) + " ended before the punch-in and was not placed: "
                      + take.file.getFileName() + " is kept.");
                continue;
            }
            if (begin >= punch->second)
            {
                note ("A take on track " + juce::String (take.track + 1) + " started after the punch-out and was not placed: "
                      + take.file.getFileName() + " is kept.");
                continue;
            }
            // the intersection is never empty here: both ends of the span and of the range are ordered as the tests above say
            clipStart = juce::jmax (begin, punch->first);
            clipOffset = skip + (clipStart - begin);
            clipLength = juce::jmin (spanEnd, punch->second) - clipStart;
        }
        juce::String clipError;
        const auto ref = editor.addClip (take.track, take.file, clipStart, clipOffset, clipLength, clipError);
        if (! ref.valid())
        {
            note (clipError.isNotEmpty() ? clipError : "A take on track " + juce::String (take.track + 1) + " could not be placed: " + take.file.getFileName() + " is kept.");
            continue;
        }
        if (! editor.setTakeOf (ref, editor.newTakeGroup(), 0, clipError))
            note (clipError);
        if (placed != nullptr)
            placed->push_back (ref);
    }
    if (firstProblem.isNotEmpty())
        error = firstProblem;
    return firstProblem.isEmpty();
}
} // namespace olympus
