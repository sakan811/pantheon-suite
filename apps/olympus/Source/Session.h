// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 The Pantheon authors

#pragma once

#include "Catalog.h"
#include "Synth.h"
#include "Tempo.h"

#include <juce_audio_formats/juce_audio_formats.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cmath>
#include <functional>
#include <iterator>
#include <limits>
#include <map>
#include <memory>
#include <thread>

// One session: a timeline of tracks, each with clips of audio, a chain of plugins and a fader, pan, mute and solo; the markers, the
// loop, the tempo and the metronome; and what is done with them (play, render stems, save). No interface in here: the window, the
// MCP server and the tests all talk to this.
//
//   Clip     a piece of an audio file placed on a track: where it starts on the timeline, where in the file it begins, how long, gain, fades
//   Track    clips summed, then the chain, then the fader and the balance pan; its meters
//   Slot     one plugin of a chain: a live instance, its settings are the instance's own state
//   Marker   a named place on the timeline
//
// Every position on the timeline is in samples at the session's rate (see Tempo.h for the tempo and the grid).
//
// Threads: everything that changes the tracks, a chain or the timeline holds `lock`; the audio callback of the window tries it and falls
// back to the lock-free playback snapshot when `lock` is held. Offline rendering holds it throughout. The audio pool is touched on the message thread only.
namespace olympus
{
inline constexpr int kProjectVersion = 3;
inline constexpr int kMonitorVersion = 1; // monitor.xml, separate from the project's version
inline constexpr int kBlockSize = 512;
inline constexpr int kMaxMonitorChannels = 8; // the most channels the monitor chain is handed (a device's outputs past this are not run)
inline constexpr juce::int64 kMaxSafeSample = std::numeric_limits<juce::int64>::max() / 4; // the largest timeline position: a rate change scales it without overflowing
inline constexpr double kMinValidRate = 8000.0, kMaxValidRate = 768000.0; // a saved sample rate outside these is not a rate Olympus can use

// Hears a plugin say its latency has changed (a linear-phase switch, say). A plugin may say so on any thread, so it only sets a flag: the
// window's timer reads it on the message thread and the session follows (Session::takeLatencyChange). One is owned by each Session.
class LatencyWatch : public juce::AudioProcessorListener
{
public:
    std::atomic<bool> changed { false };
    void audioProcessorParameterChanged (juce::AudioProcessor*, int, float) override {}
    void audioProcessorChanged (juce::AudioProcessor*, const ChangeDetails& details) override
    {
        if (details.latencyChanged)
            changed = true;
    }
};

struct Slot
{
    juce::String plugin;                                 // the plugin's name, as the catalog knows it
    bool bypass = false;
    float wet = 1.0f;                                    // 0.0 (dry) .. 1.0 (wet)
    int sidechainTrack = -1;                             // -1 = none, >= 0 = source track index for sidechain input (ch 2-3)
    juce::AudioBuffer<float> dryScratch;                 // sized to >= 2 x kBlockSize off the audio thread for wet/dry blending
    std::unique_ptr<juce::AudioPluginInstance> instance; // null while the plugin is not installed on this machine
    juce::MemoryBlock savedState;                        // what a missing plugin had, kept so a project that is saved again does not lose it
    LatencyWatch* watch = nullptr;                       // the session's listener on `instance`, removed before the instance goes

    Slot() = default;
    Slot (const Slot&) = delete;
    Slot& operator= (const Slot&) = delete;
    ~Slot() { if (instance != nullptr && watch != nullptr) instance->removeListener (watch); }

    int latency() const { return instance != nullptr && ! bypass ? juce::jmax (0, instance->getLatencySamples()) : 0; }
};

// A named place on the timeline. The session keeps them sorted by start.
struct Marker
{
    juce::String name;
    juce::int64 start = 0;
};

// A piece of an audio file on a track. Clips cut from one file share one buffer (the audio pool), and undo copies share it too.
struct Clip
{
    juce::String name;
    juce::File file;                                     // the file it came from (kept when the audio is missing)
    std::shared_ptr<const juce::AudioBuffer<float>> audio; // two channels at the session's rate; null when the file is missing
    juce::int64 start = 0;                               // where the clip begins on the timeline
    juce::int64 offset = 0;                              // the sample of the source the clip begins at
    juce::int64 length = 0;                              // how many samples it plays
    float gainDb = 0.0f;
    juce::int64 fadeIn = 0, fadeOut = 0;                 // linear, in samples: from silence at the start, to silence at the end
    bool mute = false;

    juce::int64 end() const { return start + length; }
};

struct MidiNote
{
    double startBeat = 0.0;
    double durationBeats = 1.0;
    int pitch = 60;
    int velocity = 100;
    int channel = 1;
};

struct MidiClip
{
    juce::String name;
    juce::int64 start = 0;
    juce::int64 length = 0;
    std::vector<MidiNote> notes;
    bool mute = false;

    juce::int64 end() const { return start + length; }
};

enum class AutoCurveType { Linear = 0, Step = 1, Smooth = 2 };

struct AutoPoint
{
    juce::int64 sample = 0;
    float value = 0.0f;
    AutoCurveType curve = AutoCurveType::Linear;
};

inline float autoNormToDb (float v)
{
    if (! std::isfinite (v))
        return -100.0f;
    v = juce::jlimit (0.0f, 1.0f, v);
    return -100.0f + v * 112.0f;
}

inline float dbToAutoNorm (float db)
{
    if (! std::isfinite (db))
        return 0.0f;
    return juce::jlimit (0.0f, 1.0f, (db + 100.0f) / 112.0f);
}

struct AutomationLane
{
    juce::String targetId; // "volume", "pan", or "plugin:<slot>:<param>"
    int slotIndex = -1;
    int paramIndex = -1;
    bool enabled = true;
    std::vector<AutoPoint> points; // sorted by sample

    float evaluateAt (juce::int64 sample) const
    {
        auto clampUnit = [] (float v) { return std::isfinite (v) ? juce::jlimit (0.0f, 1.0f, v) : 0.0f; };
        if (points.empty())
            return 0.0f;
        if (points.size() == 1 || sample <= points.front().sample)
            return clampUnit (points.front().value);
        if (sample >= points.back().sample)
            return clampUnit (points.back().value);

        auto it = std::upper_bound (points.begin(), points.end(), sample,
                                    [] (juce::int64 s, const AutoPoint& p) { return s < p.sample; });
        if (it == points.begin())
            return clampUnit (points.front().value);
        const auto& p0 = *std::prev (it);
        const auto& p1 = *it;
        if (p1.sample <= p0.sample)
            return clampUnit (p1.value);
        if (p0.curve == AutoCurveType::Step)
            return clampUnit (p0.value);
        if (! std::isfinite (p0.value) || ! std::isfinite (p1.value))
            return 0.0f;
        const double t = juce::jlimit (0.0, 1.0, (double) (sample - p0.sample) / (double) (p1.sample - p0.sample));
        const double mu = p0.curve == AutoCurveType::Smooth ? (t * t * (3.0 - 2.0 * t)) : t;
        const float val = (float) ((double) p0.value + ((double) p1.value - (double) p0.value) * mu);
        return clampUnit (val);
    }
};

enum class TrackKind { Audio, Midi, Bus };

struct AuxSend
{
    int targetTrack = -1;
    float levelDb = 0.0f;
    bool preFader = false;
    bool mute = false;
};

// Fixed-size, zero-allocation active MIDI note ledger (16 channels x 128 pitches = 2048 bits).
// Tracks sounding notes per track on the audio thread so transport seeks, loop wraps, and track mutes flush NoteOffs.
struct ActiveNoteMask
{
    std::array<std::uint64_t, 32> words {};

    bool any() const noexcept
    {
        for (auto w : words)
            if (w != 0)
                return true;
        return false;
    }

    bool isActive (int channel, int pitch) const noexcept
    {
        const int ch = std::clamp (channel, 1, 16) - 1;
        const int p = std::clamp (pitch, 0, 127);
        const std::size_t idx = (std::size_t) (ch * 2 + (p >> 6));
        return (words[idx] & (std::uint64_t (1) << (p & 63))) != 0;
    }

    void set (int channel, int pitch) noexcept
    {
        const int ch = std::clamp (channel, 1, 16) - 1;
        const int p = std::clamp (pitch, 0, 127);
        const std::size_t idx = (std::size_t) (ch * 2 + (p >> 6));
        words[idx] |= (std::uint64_t (1) << (p & 63));
    }

    void clear (int channel, int pitch) noexcept
    {
        const int ch = std::clamp (channel, 1, 16) - 1;
        const int p = std::clamp (pitch, 0, 127);
        const std::size_t idx = (std::size_t) (ch * 2 + (p >> 6));
        words[idx] &= ~(std::uint64_t (1) << (p & 63));
    }

    void reset() noexcept
    {
        words.fill (0);
    }

    void updateFromBuffer (const juce::MidiBuffer& buf) noexcept
    {
        for (const auto meta : buf)
        {
            const auto msg = meta.getMessage();
            if (msg.isNoteOn())
                set (msg.getChannel(), msg.getNoteNumber());
            else if (msg.isNoteOff())
                clear (msg.getChannel(), msg.getNoteNumber());
            else if (msg.isAllNotesOff() || msg.isAllSoundOff())
                reset();
        }
    }

    void appendNoteOffs (juce::MidiBuffer& dest, int sampleOffset = 0) const
    {
        for (int ch = 0; ch < 16; ++ch)
        {
            for (int half = 0; half < 2; ++half)
            {
                std::uint64_t w = words[(std::size_t) (ch * 2 + half)];
                while (w != 0)
                {
                    const int bit = std::countr_zero (w);
                    w &= ~(std::uint64_t (1) << bit);
                    const int pitch = half * 64 + bit;
                    dest.addEvent (juce::MidiMessage::noteOff (ch + 1, pitch), sampleOffset);
                }
            }
        }
    }
};

struct Track
{
    using TrackKind = ::olympus::TrackKind;
    using AuxSend = ::olympus::AuxSend;

    juce::String name;
    TrackKind kind = TrackKind::Audio;
    bool isBus() const { return kind == TrackKind::Bus; }
    int outputTrack = -1;                                // -1 = Master, >= 0 = target Bus track index
    std::vector<AuxSend> sends;
    std::vector<Clip> clips;                             // may be empty: an empty track is valid
    std::vector<MidiClip> midiClips;
    bool isMidi = false;
    std::unique_ptr<BasicSynth> synth;
    std::unique_ptr<Slot> instrument;
    float gainDb = 0.0f;                                 // the fader
    float pan = 0.0f;                                    // -1 (left) .. 1 (right), a balance: see Session::panGains
    bool mute = false, solo = false;
    // Recording (Record.h): armed tracks take the device inputs inputLeft and inputRight (-1 = mono, the left input on both sides). Session
    // state, saved in the project as attributes; not undone.
    bool armed = false;
    int inputLeft = 0, inputRight = 1;
    std::vector<std::unique_ptr<Slot>> chain;
    std::vector<AutomationLane> automation;
    bool showAutomation = false;
    juce::String activeAutoTarget { "volume" };
    bool frozen = false;
    juce::File frozenFile;
    std::shared_ptr<const juce::AudioBuffer<float>> frozenAudio;
    std::atomic<float> peakL { 0.0f }, peakR { 0.0f };   // post fader and pan; the audio thread keeps the max, the display resets them

    // The sum of the instrument and chain reported latencies (0 when frozen, since latency is baked out).
    int latency() const
    {
        if (frozen)
            return 0;
        int sum = instrument != nullptr ? instrument->latency() : 0;
        for (auto& s : chain) sum += s->latency();
        return juce::jmax (0, sum);
    }
    // The end of the last clip (0 with none).
    juce::int64 length() const
    {
        juce::int64 n = 0;
        for (auto& c : clips) n = juce::jmax (n, c.end());
        for (auto& mc : midiClips) n = juce::jmax (n, mc.end());
        if (frozen && frozenAudio != nullptr)
            n = juce::jmax (n, (juce::int64) frozenAudio->getNumSamples());
        return n;
    }

    // for the audio thread, set under the session's lock
    juce::AudioBuffer<float> scratch;
    juce::AudioBuffer<float> busInput;                   // 2 x kBlockSize, accumulated audio for Bus tracks
    juce::AudioBuffer<float> postScratch;                // 2 x kBlockSize, post-fader/pan stereo output for sends/buses/sidechains
    juce::AudioBuffer<float> delayRing;
    int delaySamples = 0, delayPos = 0;
    bool delayDirty = false;                             // the ring holds audio from a block that ran (cleared when the track is skipped)
    ActiveNoteMask activeNotes;
    bool pendingNoteFlush = false;
};

struct RenderOptions
{
    juce::File folder;
    double tailSeconds = 0.0; // after the audio ends, so a reverb or delay can ring out
    bool writeMix = true;     // mix.wav, the sum of the stems, with the master gain
    bool writeStems = true;   // the stems; off, only mix.wav is written (the mix still runs every track together)
    int bits = 32;            // 16, 24 or 32 (32 is float: it cannot clip, and stems are for editing later)
};

struct RenderResult
{
    struct Stem { juce::String track, file; double peakDb = -200.0; };
    std::vector<Stem> stems;
    juce::String mixFile;
    double mixPeakDb = -200.0;
    double seconds = 0.0;
    bool stopped = false;     // the progress callback asked for the render to stop; its files of this run were deleted
};

// What an edit changes, copied out and back for undo. Clips share their audio, so a copy is cheap.
struct EditState
{
    std::vector<std::vector<Clip>> clips; // per track
    std::vector<std::vector<MidiClip>> midiClips; // per track
    std::vector<std::vector<AutomationLane>> automation; // per track
    struct Mix
    {
        juce::String name;
        float gainDb = 0.0f, pan = 0.0f;
        bool mute = false, solo = false;
    };
    std::vector<Mix> mix; // per track
    float masterDb = 0.0f;
    TempoMap tempo;
    std::vector<Marker> markers;
    juce::int64 loopStart = 0, loopEnd = 0;
    bool loopOn = false;
    bool metronomeOn = false;
};

struct PlaybackSnapshot
{
    struct Track
    {
        std::vector<Clip> clips;
        std::vector<MidiClip> midiClips;
        std::vector<AutomationLane> automation;
        TrackKind kind = TrackKind::Audio;
        int outputTrack = -1;
        std::vector<AuxSend> sends;
        bool isMidi = false;
        bool hasMidiPlugin = false;
        float gainDb = 0.0f;
        float pan = 0.0f;
        bool mute = false;
        bool solo = false;
        bool armed = false;
        bool frozen = false;
        std::shared_ptr<const juce::AudioBuffer<float>> frozenAudio;
    };
    std::vector<Track> tracks;
    std::vector<int> topoOrder;
    std::vector<juce::AudioBuffer<float>> busInputs;
    std::vector<juce::AudioBuffer<float>> postScratches;
    std::vector<bool> audibleScratch;
    float masterDb = 0.0f;
    double sampleRate = 0.0;
    TempoMap tempo;
    bool metronomeOn = false;
};

class Session
{
public:
    LatencyWatch latencyWatch;                 // declared before the slots: they remove their listener from it (Slot::~Slot)
    Catalog catalog;
    mutable juce::CriticalSection lock;
    // true while renderStems runs: it holds `lock` for the whole render, so the message thread (the timer, the paint) must not wait for
    // it: it tries the lock or skips, and reads the plugins only when this is false.
    std::atomic<bool> renderBusy { false };
    bool renderRunning() const { return renderBusy.load(); }
    std::atomic<bool> recording { false };
    std::atomic<juce::uint64> revision { 1 };
    void bumpRevision() { revision.fetch_add (1, std::memory_order_release); }
    std::vector<std::unique_ptr<Track>> tracks;
    double sampleRate = 0.0; // 0 until the first clip is added
    juce::File projectFile;
    juce::String name { "Untitled" };
    bool dirty = false;
    juce::StringArray warnings; // what the last load could not do

    // ---- live MIDI & lock-free playback snapshot -----------------------------------------------------------------------
    MidiFifo liveMidiFifo;
    std::array<PlaybackSnapshot, 2> snapshots;
    std::atomic<int> activeSnapshot { 0 };
    std::atomic<bool> snapshotValid { false };

    void publishPlaybackSnapshotLocked()
    {
        const int cur = activeSnapshot.load (std::memory_order_relaxed);
        const int next = 1 - cur;
        while (snapshotReaders[(size_t) next].load (std::memory_order_acquire) != 0)
            std::this_thread::yield();
        auto& dst = snapshots[(size_t) next];
        dst.tracks.resize (tracks.size());
        for (size_t i = 0; i < tracks.size(); ++i)
        {
            const auto& src = *tracks[i];
            auto& t = dst.tracks[i];
            t.clips = src.clips;
            t.midiClips = src.midiClips;
            t.automation = src.automation;
            t.kind = src.kind;
            t.outputTrack = src.outputTrack;
            t.sends = src.sends;
            t.isMidi = src.isMidi;
            t.hasMidiPlugin = (src.instrument != nullptr && src.instrument->instance != nullptr && ! src.instrument->bypass);
            if (! t.hasMidiPlugin)
            {
                for (const auto& s : src.chain)
                {
                    if (s->instance != nullptr && ! s->bypass && s->instance->acceptsMidi())
                    {
                        t.hasMidiPlugin = true;
                        break;
                    }
                }
            }
            t.gainDb = src.gainDb;
            t.pan = src.pan;
            t.mute = src.mute;
            t.solo = src.solo;
            t.armed = src.armed;
            t.frozen = src.frozen;
            t.frozenAudio = src.frozenAudio;
        }
        std::vector<int> snapInLat, snapOutLat;
        computeRoutingLocked (dst.topoOrder, snapInLat, snapOutLat);
        dst.busInputs.resize (tracks.size());
        dst.postScratches.resize (tracks.size());
        for (size_t i = 0; i < tracks.size(); ++i)
        {
            dst.busInputs[i].setSize (2, kBlockSize, false, true, false);
            dst.postScratches[i].setSize (2, kBlockSize, false, true, false);
        }
        dst.audibleScratch.assign (tracks.size(), false);
        dst.masterDb = masterDb;
        dst.sampleRate = sampleRate;
        dst.tempo = tempo;
        dst.metronomeOn = metronomeOn;
        activeSnapshot.store (next, std::memory_order_release);
        snapshotValid.store (true, std::memory_order_release);
    }

    // ---- the timeline ---------------------------------------------------------------------------------------------------
    TempoMap tempo;                 // tempo.sampleRate is the session's rate whenever that is set
    float masterDb = 0.0f;          // a gain on the sum of the tracks
    std::vector<std::unique_ptr<Slot>> masterChain; // master bus FX chain (after tracks sum, before masterDb and metronome)
    std::atomic<float> masterPeakL { 0.0f }, masterPeakR { 0.0f }; // max since the display last read them
    std::vector<Marker> markers;    // sorted by start
    juce::int64 loopStart = 0, loopEnd = 0;
    bool loopOn = false;
    bool metronomeOn = false;       // a click on every beat, a higher one on the bar; not in the stems
    bool snapOn = true;             // view settings, saved with the project
    Grid grid = Grid::Quarter;

    // ---- the monitor chain ----------------------------------------------------------------------------------------------
    // The plugins the headphones hear (a correction plugin, say): run on the playback block by processMonitor, after processBlock, and
    // by nothing else. Not in the project, not in a render, not an undo step: it belongs to the headphones. Saved in its own file (saveMonitor).
    std::vector<std::unique_ptr<Slot>> monitor;
    std::atomic<bool> monitorBypass { false };   // read by the audio callback without the lock: atomic

    bool isOffline() const { return offline; }

    Session()
    {
        formats.registerBasicFormats();
        masterScratch.setSize (2, kBlockSize, false, true, false);
        monitorScratch.setSize (kMaxMonitorChannels, kBlockSize, false, true, false);
        snapshotScratch.setSize (2, kBlockSize, false, true, false);
        midiScratch.ensureSize (32768);
        snapshotMidiScratch.ensureSize (16384);
        liveMidiScratch.ensureSize (16384);
    }
    ~Session()
    {
        for (auto& t : tracks)
        {
            if (t->instrument != nullptr && t->instrument->instance != nullptr)
                t->instrument->instance->setPlayHead (nullptr);
            for (auto& s : t->chain)
                if (s->instance != nullptr) s->instance->setPlayHead (nullptr);
        }
        for (auto& s : masterChain)
            if (s->instance != nullptr) s->instance->setPlayHead (nullptr);
        for (auto& s : monitor)
            if (s->instance != nullptr) s->instance->setPlayHead (nullptr);
    }

    // ---- tracks ---------------------------------------------------------------------------------------------------------
    // A track with one clip at sample 0 covering the whole file (the first file sets the session's rate).
    bool addTrack (const juce::File& file, juce::String trackName, juce::String& error)
    {
        auto audio = loadPooled (file, error);
        if (audio == nullptr)
            return false;
        auto track = std::make_unique<Track>();
        track->name = trackName.isNotEmpty() ? trackName : file.getFileNameWithoutExtension();
        Clip clip;
        clip.name = file.getFileNameWithoutExtension();
        clip.file = file;
        clip.audio = audio;
        clip.length = audio->getNumSamples();
        track->clips.push_back (std::move (clip));
        const juce::ScopedLock sl (lock);
        makeTrackLocked (*track);
        tracks.push_back (std::move (track));
        updateLatenciesLocked();
        publishPlaybackSnapshotLocked();
        dirty = true;
        return true;
    }

    // A track with no clips. Returns its index.
    int addEmptyTrack (juce::String trackName)
    {
        auto track = std::make_unique<Track>();
        const juce::ScopedLock sl (lock);
        track->name = trackName.isNotEmpty() ? trackName : "Track " + juce::String ((int) tracks.size() + 1);
        makeTrackLocked (*track);
        tracks.push_back (std::move (track));
        updateLatenciesLocked();
        publishPlaybackSnapshotLocked();
        dirty = true;
        return (int) tracks.size() - 1;
    }

    // A MIDI track with a built-in synth. Sets sampleRate = 44100.0 if unset. Returns its index.
    int addMidiTrack (juce::String trackName)
    {
        auto track = std::make_unique<Track>();
        track->kind = TrackKind::Midi;
        track->isMidi = true;
        track->synth = std::make_unique<BasicSynth>();
        const juce::ScopedLock sl (lock);
        track->name = trackName.isNotEmpty() ? trackName : "MIDI " + juce::String ((int) tracks.size() + 1);
        const bool setInitialRate = (sampleRate <= 0.0);
        if (setInitialRate)
            setSampleRateLocked (44100.0);
        tempo.sampleRate = sampleRate;
        makeTrackLocked (*track);
        tracks.push_back (std::move (track));
        if (setInitialRate)
            prepareLocked (false);
        else
            updateLatenciesLocked();
        publishPlaybackSnapshotLocked();
        dirty = true;
        return (int) tracks.size() - 1;
    }

    // A Bus / Group track that sums routed tracks and aux sends, runs a plugin chain, and routes to Master or another Bus.
    int addBusTrack (const juce::String& busName, juce::String& error)
    {
        juce::ignoreUnused (error);
        auto track = std::make_unique<Track>();
        track->kind = TrackKind::Bus;
        track->isMidi = false;
        const juce::ScopedLock sl (lock);
        track->name = busName.isNotEmpty() ? busName : "Bus " + juce::String ((int) tracks.size() + 1);
        makeTrackLocked (*track);
        tracks.push_back (std::move (track));
        updateLatenciesLocked();
        publishPlaybackSnapshotLocked();
        dirty = true;
        return (int) tracks.size() - 1;
    }

    bool setTrackOutput (int track, int targetBus, juce::String& error)
    {
        const juce::ScopedLock sl (lock);
        if (! juce::isPositiveAndBelow (track, (int) tracks.size())) { error = "No track " + juce::String (track); return false; }
        if (targetBus < -1 || targetBus >= (int) tracks.size()) { error = "No bus track " + juce::String (targetBus); return false; }
        if (targetBus == track) { error = "A track cannot route to itself."; return false; }
        if (targetBus >= 0)
        {
            if (! tracks[(size_t) targetBus]->isBus())
            {
                error = "Target track '" + tracks[(size_t) targetBus]->name + "' is not a bus.";
                return false;
            }
            if (wouldCreateCycleLocked (track, targetBus))
            {
                error = "Routing '" + tracks[(size_t) track]->name + "' to '" + tracks[(size_t) targetBus]->name + "' would create a feedback cycle.";
                return false;
            }
        }
        auto& t = *tracks[(size_t) track];
        if (t.outputTrack == targetBus)
            return true;
        t.outputTrack = targetBus;
        updateLatenciesLocked();
        publishPlaybackSnapshotLocked();
        dirty = true;
        return true;
    }

    bool setTrackSend (int track, int targetBus, float levelDb, bool preFader, bool mute, juce::String& error)
    {
        if (! std::isfinite (levelDb))
        {
            error = "Send level must be a finite number.";
            return false;
        }
        const juce::ScopedLock sl (lock);
        if (! juce::isPositiveAndBelow (track, (int) tracks.size())) { error = "No track " + juce::String (track); return false; }
        if (! juce::isPositiveAndBelow (targetBus, (int) tracks.size())) { error = "No bus track " + juce::String (targetBus); return false; }
        if (targetBus == track) { error = "A track cannot send to itself."; return false; }
        if (! tracks[(size_t) targetBus]->isBus())
        {
            error = "Target track '" + tracks[(size_t) targetBus]->name + "' is not a bus.";
            return false;
        }
        if (wouldCreateCycleLocked (track, targetBus))
        {
            error = "Sending from '" + tracks[(size_t) track]->name + "' to '" + tracks[(size_t) targetBus]->name + "' would create a feedback cycle.";
            return false;
        }
        const float clampedDb = juce::jlimit (-100.0f, 12.0f, levelDb);
        auto& sends = tracks[(size_t) track]->sends;
        for (auto& s : sends)
        {
            if (s.targetTrack == targetBus)
            {
                s.levelDb = clampedDb;
                s.preFader = preFader;
                s.mute = mute;
                updateLatenciesLocked();
                publishPlaybackSnapshotLocked();
                dirty = true;
                return true;
            }
        }
        sends.push_back ({ targetBus, clampedDb, preFader, mute });
        updateLatenciesLocked();
        publishPlaybackSnapshotLocked();
        dirty = true;
        return true;
    }

    bool removeTrackSend (int track, int sendIndex, juce::String& error)
    {
        const juce::ScopedLock sl (lock);
        if (! juce::isPositiveAndBelow (track, (int) tracks.size())) { error = "No track " + juce::String (track); return false; }
        auto& sends = tracks[(size_t) track]->sends;
        if (! juce::isPositiveAndBelow (sendIndex, (int) sends.size()))
        {
            error = "No send " + juce::String (sendIndex) + " on track " + juce::String (track);
            return false;
        }
        sends.erase (sends.begin() + sendIndex);
        updateLatenciesLocked();
        publishPlaybackSnapshotLocked();
        dirty = true;
        return true;
    }

    bool setTrackSends (int track, const std::vector<AuxSend>& newSends, juce::String& error)
    {
        const juce::ScopedLock sl (lock);
        if (! juce::isPositiveAndBelow (track, (int) tracks.size())) { error = "No track " + juce::String (track); return false; }
        auto oldSends = tracks[(size_t) track]->sends;
        tracks[(size_t) track]->sends.clear();
        for (const auto& snd : newSends)
        {
            if (! std::isfinite (snd.levelDb))
            {
                tracks[(size_t) track]->sends = std::move (oldSends);
                error = "Send level must be a finite number.";
                return false;
            }
            if (! juce::isPositiveAndBelow (snd.targetTrack, (int) tracks.size()))
            {
                tracks[(size_t) track]->sends = std::move (oldSends);
                error = "No bus track " + juce::String (snd.targetTrack);
                return false;
            }
            if (snd.targetTrack == track)
            {
                tracks[(size_t) track]->sends = std::move (oldSends);
                error = "A track cannot send to itself.";
                return false;
            }
            if (! tracks[(size_t) snd.targetTrack]->isBus())
            {
                tracks[(size_t) track]->sends = std::move (oldSends);
                error = "Target track '" + tracks[(size_t) snd.targetTrack]->name + "' is not a bus.";
                return false;
            }
            if (wouldCreateCycleLocked (track, snd.targetTrack))
            {
                tracks[(size_t) track]->sends = std::move (oldSends);
                error = "Sending from '" + tracks[(size_t) track]->name + "' to '" + tracks[(size_t) snd.targetTrack]->name + "' would create a feedback cycle.";
                return false;
            }
            const float clampedDb = juce::jlimit (-100.0f, 12.0f, snd.levelDb);
            auto& sends = tracks[(size_t) track]->sends;
            bool updated = false;
            for (auto& existing : sends)
            {
                if (existing.targetTrack == snd.targetTrack)
                {
                    existing.levelDb = clampedDb;
                    existing.preFader = snd.preFader;
                    existing.mute = snd.mute;
                    updated = true;
                    break;
                }
            }
            if (! updated)
                sends.push_back ({ snd.targetTrack, clampedDb, snd.preFader, snd.mute });
        }
        updateLatenciesLocked();
        publishPlaybackSnapshotLocked();
        dirty = true;
        return true;
    }

    bool canSidechainTrack (int track, int sourceTrack, juce::String& error) const
    {
        const juce::ScopedLock sl (lock);
        if (! juce::isPositiveAndBelow (track, (int) tracks.size())) { error = "No track " + juce::String (track); return false; }
        if (tracks[(size_t) track]->frozen) { error = frozenTrackError (*tracks[(size_t) track]); return false; }
        if (sourceTrack < -1 || sourceTrack >= (int) tracks.size()) { error = "No track " + juce::String (sourceTrack); return false; }
        if (sourceTrack == track) { error = "A track cannot sidechain from itself."; return false; }
        if (sourceTrack >= 0 && wouldCreateCycleLocked (sourceTrack, track))
        {
            error = "Sidechaining '" + tracks[(size_t) track]->name + "' from '" + tracks[(size_t) sourceTrack]->name + "' would create a feedback cycle.";
            return false;
        }
        return true;
    }

    bool setSlotSidechain (int track, int slot, int sourceTrack, juce::String& error)
    {
        const juce::ScopedLock sl (lock);
        auto* s = slotAt (track, slot, error);
        if (s == nullptr) return false;
        if (tracks[(size_t) track]->frozen) { error = frozenTrackError (*tracks[(size_t) track]); return false; }
        if (sourceTrack < -1 || sourceTrack >= (int) tracks.size()) { error = "No track " + juce::String (sourceTrack); return false; }
        if (sourceTrack == track) { error = "A track cannot sidechain from itself."; return false; }
        if (sourceTrack >= 0 && wouldCreateCycleLocked (sourceTrack, track))
        {
            error = "Sidechaining '" + tracks[(size_t) track]->name + "' from '" + tracks[(size_t) sourceTrack]->name + "' would create a feedback cycle.";
            return false;
        }
        if (s->sidechainTrack == sourceTrack)
            return true;
        s->sidechainTrack = sourceTrack;
        updateLatenciesLocked();
        dirty = true;
        return true;
    }

    // Appends a clip of the file to a track, at `startSample` on the timeline: from `offset` in the file, for `length` samples (0 = to the
    // end of the file). The file is read into the pool BEFORE the lock is taken (the audio callback only tries it, so a decode under it
    // would silence every block); the clip is checked and inserted under the lock, so a refusal changes nothing. Returns the clip's index
    // on the track, -1 with the reason in `error`.
    int addClip (int track, const juce::File& file, juce::int64 startSample, juce::String& error, juce::int64 offset = 0, juce::int64 length = 0)
    {
        {
            const juce::ScopedLock sl (lock);
            if (! juce::isPositiveAndBelow (track, (int) tracks.size())) { error = "No track " + juce::String (track); return -1; }
        }
        auto audio = loadPooled (file, error);
        if (audio == nullptr)
            return -1;
        const juce::ScopedLock sl (lock);
        if (! juce::isPositiveAndBelow (track, (int) tracks.size())) { error = "No track " + juce::String (track); return -1; }
        const juce::int64 available = audio->getNumSamples();
        const juce::int64 play = length > 0 ? length : available - offset;
        if (offset < 0 || offset >= available || play < 1 || offset + play > available)
        {
            error = "The file is " + juce::String (available) + " samples long: a clip from sample " + juce::String (offset) + " for "
                    + juce::String (play) + " samples does not fit in it.";
            return -1;
        }
        Clip clip;
        clip.name = file.getFileNameWithoutExtension();
        clip.file = file;
        clip.audio = audio;
        clip.start = juce::jmax ((juce::int64) 0, startSample);
        clip.offset = offset;
        clip.length = play;
        auto& clips = tracks[(size_t) track]->clips;
        clips.push_back (std::move (clip));
        publishPlaybackSnapshotLocked();
        dirty = true;
        bumpRevision();
        return (int) clips.size() - 1;
    }

    // The audio of a file, shared with every clip that plays it: a file already in the pool is not read again. The first file sets the
    // session's rate (and tempo.sampleRate) and prepares the tracks' plugins and their latency compensation for it, for addTrack and
    // addClip alike. Null, with the reason, if the file cannot be read.
    std::shared_ptr<const juce::AudioBuffer<float>> loadPooled (const juce::File& file, juce::String& error)
    {
        double rate = 0.0;
        {
            const juce::ScopedLock sl (lock);
            rate = sampleRate;
        }
        auto audio = pooledAudio (file, rate, error);
        if (audio != nullptr)
        {
            const juce::ScopedLock sl (lock);
            if (sampleRate <= 0.0)
            {
                setSampleRateLocked (rate);
                prepareLocked (false); // every track's plugins at the new rate, and the latency compensation (addClip never prepares)
                publishPlaybackSnapshotLocked();
            }
        }
        return audio;
    }

    bool removeTrack (int index, juce::String& error)
    {
        const juce::ScopedLock sl (lock);
        if (! juce::isPositiveAndBelow (index, (int) tracks.size())) { error = "No track " + juce::String (index); return false; }
        if (tracks[(size_t) index]->instrument != nullptr && tracks[(size_t) index]->instrument->instance != nullptr)
            tracks[(size_t) index]->instrument->instance->setPlayHead (nullptr);
        for (auto& s : tracks[(size_t) index]->chain)
            if (s->instance != nullptr) s->instance->setPlayHead (nullptr);
        tracks.erase (tracks.begin() + index);
        for (auto& t : tracks)
        {
            if (t->outputTrack == index) t->outputTrack = -1;
            else if (t->outputTrack > index) --t->outputTrack;
            t->sends.erase (std::remove_if (t->sends.begin(), t->sends.end(),
                                            [index] (const AuxSend& snd) { return snd.targetTrack == index; }),
                            t->sends.end());
            for (auto& snd : t->sends)
                if (snd.targetTrack > index) --snd.targetTrack;
            for (auto& slot : t->chain)
            {
                if (slot->sidechainTrack == index) slot->sidechainTrack = -1;
                else if (slot->sidechainTrack > index) --slot->sidechainTrack;
            }
        }
        updateLatenciesLocked();
        publishPlaybackSnapshotLocked();
        dirty = true;
        return true;
    }

    // Renders `track` through its clips/MIDI/automation/chain with chain latency trimmed from the front into a 32-bit float WAV
    // at `targetWav`, loads it via the audio pool, and marks the track frozen so playback and renders read the baked audio directly.
    bool freezeTrack (int track, const juce::File& targetWav, juce::String& error)
    {
        if (recording.load())
        {
            error = "Stop recording before freezing a track.";
            return false;
        }
        if (renderBusy.load())
        {
            error = "A render is running: wait for it or cancel it first.";
            return false;
        }
        if (targetWav == juce::File())
        {
            error = "No target file given for frozen audio.";
            return false;
        }
        {
            const juce::ScopedLock sl (lock);
            if (! juce::isPositiveAndBelow (track, (int) tracks.size()))
            {
                error = "No track " + juce::String (track);
                return false;
            }
            auto& t = *tracks[(size_t) track];
            if (t.isBus())
            {
                error = "Cannot freeze a bus track.";
                return false;
            }
            if (t.frozen)
            {
                error = "Track '" + t.name + "' is already frozen.";
                return false;
            }
            if (sampleRate <= 0.0)
            {
                error = "Nothing to freeze: no audio has been added.";
                return false;
            }
            const juce::int64 baseLength = t.length();
            if (baseLength <= 0)
            {
                error = "Track '" + t.name + "' has nothing to freeze.";
                return false;
            }
            const juce::int64 maxTail = (juce::int64) std::llround (1.0 * sampleRate);
            const juce::int64 renderLen = baseLength + maxTail;
            if (renderLen > (juce::int64) (3.0 * 3600.0 * sampleRate)
                || renderLen > (juce::int64) std::numeric_limits<int>::max() - kBlockSize)
            {
                error = "Track '" + t.name + "' is too long to freeze.";
                return false;
            }
            if (! targetWav.getParentDirectory().createDirectory().wasOk())
            {
                error = "Cannot make the folder " + targetWav.getParentDirectory().getFullPathName();
                return false;
            }

            const int chainLat = t.latency();
            const OfflineScope offlineScope (*this);
            struct HeadStop
            {
                Session& session;
                ~HeadStop() { session.publishHead (0, false); }
            } headStop { *this };

            juce::AudioBuffer<float> rendered (2, (int) renderLen);
            rendered.clear();
            const juce::int64 totalInput = renderLen + (juce::int64) chainLat;
            juce::int64 lastNonSilent = baseLength - 1;
            constexpr float kSilenceThreshold = 1.0e-6f;

            for (juce::int64 start = 0; start < totalInput; start += kBlockSize)
            {
                const int n = (int) juce::jmin ((juce::int64) kBlockSize, totalInput - start);
                publishHead (start, true);
                runChain (t, start, n);

                const juce::int64 first = juce::jmax ((juce::int64) 0, (juce::int64) chainLat - start);
                const juce::int64 last = juce::jmin ((juce::int64) n, renderLen + (juce::int64) chainLat - start);
                if (last <= first)
                    continue;
                const int count = (int) (last - first);
                const int dstStart = (int) (start + first - (juce::int64) chainLat);
                for (int ch = 0; ch < 2; ++ch)
                {
                    const int srcCh = juce::jmin (ch, t.scratch.getNumChannels() - 1);
                    const float* src = t.scratch.getReadPointer (srcCh, (int) first);
                    float* dst = rendered.getWritePointer (ch, dstStart);
                    for (int i = 0; i < count; ++i)
                    {
                        const float v = std::isfinite (src[i]) ? src[i] : 0.0f;
                        dst[i] = v;
                        if (std::abs (v) > kSilenceThreshold)
                            lastNonSilent = juce::jmax (lastNonSilent, (juce::int64) (dstStart + i));
                    }
                }
            }

            const int finalLen = (int) juce::jlimit (baseLength, renderLen, lastNonSilent + 1);
            auto writer = makeWriter (targetWav, 32, error);
            if (writer == nullptr)
            {
                targetWav.deleteFile();
                return false;
            }
            if (! writer->writeFromAudioSampleBuffer (rendered, 0, finalLen))
            {
                writer.reset();
                targetWav.deleteFile();
                error = "Writing " + targetWav.getFileName() + " failed.";
                return false;
            }
            writer.reset();
            pool.erase (targetWav.getFullPathName());
        }

        auto pooled = loadPooled (targetWav, error);
        if (pooled == nullptr)
        {
            targetWav.deleteFile();
            return false;
        }

        const juce::ScopedLock sl (lock);
        if (! juce::isPositiveAndBelow (track, (int) tracks.size()))
        {
            error = "No track " + juce::String (track);
            return false;
        }
        auto& t = *tracks[(size_t) track];
        if (t.frozen)
        {
            error = "Track '" + t.name + "' is already frozen.";
            return false;
        }
        t.frozen = true;
        t.frozenFile = targetWav;
        t.frozenAudio = std::move (pooled);
        updateLatenciesLocked();
        clearDelayRingsLocked();
        publishPlaybackSnapshotLocked();
        dirty = true;
        bumpRevision();
        return true;
    }

    // Restores `track` to live clip/MIDI/plugin processing while keeping its clips, automation and chain intact.
    bool unfreezeTrack (int track, juce::String& error)
    {
        const juce::ScopedLock sl (lock);
        if (! juce::isPositiveAndBelow (track, (int) tracks.size()))
        {
            error = "No track " + juce::String (track);
            return false;
        }
        auto& t = *tracks[(size_t) track];
        if (! t.frozen)
            return true;
        t.frozen = false;
        t.frozenAudio.reset();
        t.frozenFile = juce::File();
        updateLatenciesLocked();
        clearDelayRingsLocked();
        publishPlaybackSnapshotLocked();
        dirty = true;
        bumpRevision();
        return true;
    }

    // The track a name or number means (a number, or a name in any case); -1 with the reason in `error`.
    int findTrack (const juce::var& which, juce::String& error) const
    {
        int i = -1;
        if (which.isString())
        {
            const auto s = which.toString().trim();
            if (s.isEmpty())
            {
                error = "Give 'track' (a track number from 0, or a track name).";
                return -1;
            }
            if (! s.containsOnly ("0123456789"))
            {
                for (int t = 0; t < (int) tracks.size(); ++t)
                    if (tracks[(size_t) t]->name.equalsIgnoreCase (s)) return t;
                error = "No track called '" + which.toString() + "'";
                return -1;
            }
            juce::int64 parsed = 0;
            for (int k = 0; k < s.length(); ++k)
            {
                const int d = (int) (s[k] - '0');
                if (parsed > (std::numeric_limits<int>::max() - d) / 10)
                {
                    error = "No track " + s + " (there are " + juce::String ((int) tracks.size()) + ", numbered from 0)";
                    return -1;
                }
                parsed = parsed * 10 + d;
            }
            i = (int) parsed;
        }
        else if (which.isInt())
        {
            i = (int) which;
        }
        else if (which.isInt64())
        {
            const juce::int64 n = (juce::int64) which;
            if (n < 0 || n > (juce::int64) std::numeric_limits<int>::max())
            {
                error = "No track " + which.toString() + " (there are " + juce::String ((int) tracks.size()) + ", numbered from 0)";
                return -1;
            }
            i = (int) n;
        }
        else if (which.isDouble())
        {
            const double d = (double) which;
            if (! std::isfinite (d) || std::trunc (d) != d)
            {
                error = "Track must be a whole number (from 0) or a track name.";
                return -1;
            }
            if (d < 0.0 || d > (double) std::numeric_limits<int>::max())
            {
                error = "No track " + which.toString() + " (there are " + juce::String ((int) tracks.size()) + ", numbered from 0)";
                return -1;
            }
            i = (int) d;
        }
        else
        {
            error = "Track must be a track number (from 0) or a track name.";
            return -1;
        }
        if (juce::isPositiveAndBelow (i, (int) tracks.size())) return i;
        error = "No track " + juce::String (i) + " (there are " + juce::String ((int) tracks.size()) + ", numbered from 0)";
        return -1;
    }

    // ---- markers --------------------------------------------------------------------------------------------------------
    // Recording set-up of a track (Record.h). Under the lock; a refusal changes nothing and says why. Saved with the project, not undone.
    bool setTrackArm (int track, bool on, juce::String& error)
    {
        const juce::ScopedLock sl (lock);
        if (! juce::isPositiveAndBelow (track, (int) tracks.size())) { error = "No track " + juce::String (track); return false; }
        auto& t = *tracks[(size_t) track];
        if (t.armed != on) { t.armed = on; publishPlaybackSnapshotLocked(); dirty = true; bumpRevision(); }
        return true;
    }

    // `right` is -1 for a mono input (the left input is taken on both channels).
    bool setTrackInput (int track, int left, int right, juce::String& error)
    {
        const juce::ScopedLock sl (lock);
        if (! juce::isPositiveAndBelow (track, (int) tracks.size())) { error = "No track " + juce::String (track); return false; }
        if (left < 0) { error = "The left input must be 0 or more."; return false; }
        if (right < -1) { error = "The right input must be 0 or more, or -1 for a mono input."; return false; }
        auto& t = *tracks[(size_t) track];
        if (t.inputLeft != left || t.inputRight != right) { t.inputLeft = left; t.inputRight = right; dirty = true; bumpRevision(); }
        return true;
    }

    // Adds a marker, kept sorted. Returns its index.
    int addMarker (const juce::String& markerName, juce::int64 sample)
    {
        const juce::ScopedLock sl (lock);
        Marker m { markerName, juce::jmax ((juce::int64) 0, sample) };
        auto at = std::upper_bound (markers.begin(), markers.end(), m.start, [] (juce::int64 s, const Marker& o) { return s < o.start; });
        const int index = (int) (at - markers.begin());
        markers.insert (at, std::move (m));
        dirty = true;
        bumpRevision();
        return index;
    }

    bool removeMarker (int index, juce::String& error)
    {
        const juce::ScopedLock sl (lock);
        if (! juce::isPositiveAndBelow (index, (int) markers.size())) { error = "No marker " + juce::String (index); return false; }
        markers.erase (markers.begin() + index);
        dirty = true;
        bumpRevision();
        return true;
    }

    bool moveMarker (int index, juce::int64 sample, juce::String& error)
    {
        const juce::ScopedLock sl (lock);
        if (! juce::isPositiveAndBelow (index, (int) markers.size())) { error = "No marker " + juce::String (index); return false; }
        Marker m = markers[(size_t) index];
        markers.erase (markers.begin() + index);
        m.start = juce::jmax ((juce::int64) 0, sample);
        auto at = std::upper_bound (markers.begin(), markers.end(), m.start, [] (juce::int64 s, const Marker& o) { return s < o.start; });
        markers.insert (at, std::move (m));
        dirty = true;
        bumpRevision();
        return true;
    }

    static juce::String frozenTrackError (const Track& t)
    {
        return "Track '" + t.name + "' is frozen: unfreeze track to edit plugins.";
    }

    // ---- chains ---------------------------------------------------------------------------------------------------------
    // Puts a new plugin (its own defaults) in the chain at `position` (-1 = the end).
    bool addPlugin (int track, const juce::String& query, int position, juce::String& error)
    {
        double snapRate = 0.0;
        {
            const juce::ScopedLock sl (lock);
            if (! juce::isPositiveAndBelow (track, (int) tracks.size())) { error = "No track " + juce::String (track); return false; }
            if (tracks[(size_t) track]->frozen) { error = frozenTrackError (*tracks[(size_t) track]); return false; }
            snapRate = sampleRate;
        }
        const auto exact = catalog.resolve (query, error);
        if (exact.isEmpty()) return false;
        auto slot = makeSlot (exact, nullptr, error, snapRate);
        if (slot == nullptr) return false;
        const juce::ScopedLock sl (lock);
        if (! juce::isPositiveAndBelow (track, (int) tracks.size()))
        {
            if (slot->instance != nullptr) { slot->instance->releaseResources(); slot->instance->setPlayHead (nullptr); }
            error = "No track " + juce::String (track);
            return false;
        }
        if (tracks[(size_t) track]->frozen)
        {
            if (slot->instance != nullptr) { slot->instance->releaseResources(); slot->instance->setPlayHead (nullptr); }
            error = frozenTrackError (*tracks[(size_t) track]);
            return false;
        }
        auto& chain = tracks[(size_t) track]->chain;
        position = position < 0 || position > (int) chain.size() ? (int) chain.size() : position;
        chain.insert (chain.begin() + position, std::move (slot));
        updateLatenciesLocked();
        dirty = true;
        return true;
    }

    bool removePlugin (int track, int slot, juce::String& error)
    {
        const juce::ScopedLock sl (lock);
        if (! juce::isPositiveAndBelow (track, (int) tracks.size())) { error = "No track " + juce::String (track); return false; }
        auto& trk = *tracks[(size_t) track];
        if (trk.frozen) { error = frozenTrackError (trk); return false; }
        auto& chain = trk.chain;
        if (! juce::isPositiveAndBelow (slot, (int) chain.size())) { error = "No plugin " + juce::String (slot) + " on that track"; return false; }
        if (chain[(size_t) slot]->instance != nullptr) chain[(size_t) slot]->instance->setPlayHead (nullptr);
        chain.erase (chain.begin() + slot);
        remapAutomationOnRemove (trk, slot);
        updateLatenciesLocked();
        publishPlaybackSnapshotLocked();
        dirty = true;
        return true;
    }

    bool movePlugin (int track, int from, int to, juce::String& error)
    {
        const juce::ScopedLock sl (lock);
        if (! juce::isPositiveAndBelow (track, (int) tracks.size())) { error = "No track " + juce::String (track); return false; }
        auto& trk = *tracks[(size_t) track];
        if (trk.frozen) { error = frozenTrackError (trk); return false; }
        auto& chain = trk.chain;
        if (! juce::isPositiveAndBelow (from, (int) chain.size()) || ! juce::isPositiveAndBelow (to, (int) chain.size())) { error = "No such place in the chain"; return false; }
        auto moved = std::move (chain[(size_t) from]);
        chain.erase (chain.begin() + from);
        chain.insert (chain.begin() + to, std::move (moved));
        remapAutomationOnMove (trk, from, to);
        publishPlaybackSnapshotLocked();
        dirty = true;
        bumpRevision();
        return true;
    }

    bool setBypass (int track, int slot, bool bypass, juce::String& error)
    {
        const juce::ScopedLock sl (lock);
        if (auto* s = slotAt (track, slot, error))
        {
            if (tracks[(size_t) track]->frozen) { error = frozenTrackError (*tracks[(size_t) track]); return false; }
            s->bypass = bypass;
            updateLatenciesLocked();
            dirty = true;
            return true;
        }
        return false;
    }

    // Sets per-slot wet/dry mix (0.0 = dry, 1.0 = wet).
    // track >= 0: track chain; track == -1: monitor chain (does not dirty project); track == -2: master chain.
    bool setPluginWet (int track, int slot, float wet, juce::String& error)
    {
        if (! std::isfinite (wet))
        {
            error = "Wet must be a finite number (0..1).";
            return false;
        }
        wet = juce::jlimit (0.0f, 1.0f, wet);
        const juce::ScopedLock sl (lock);
        if (track == -1)
        {
            if (auto* s = monitorSlotAt (slot, error))
            {
                s->wet = wet;
                bumpRevision();
                return true;
            }
            return false;
        }
        if (track == -2)
        {
            if (auto* s = masterSlot (slot, error))
            {
                s->wet = wet;
                dirty = true;
                bumpRevision();
                return true;
            }
            return false;
        }
        if (auto* s = slotAt (track, slot, error))
        {
            if (tracks[(size_t) track]->frozen) { error = frozenTrackError (*tracks[(size_t) track]); return false; }
            s->wet = wet;
            dirty = true;
            bumpRevision();
            return true;
        }
        return false;
    }

    // Replaces the chain with new plugins (their defaults), in this order. All names are checked first: a wrong one changes nothing.
    bool setChain (int track, const juce::StringArray& plugins, juce::String& error)
    {
        double snapRate = 0.0;
        {
            const juce::ScopedLock sl (lock);
            if (! juce::isPositiveAndBelow (track, (int) tracks.size())) { error = "No track " + juce::String (track); return false; }
            if (tracks[(size_t) track]->frozen) { error = frozenTrackError (*tracks[(size_t) track]); return false; }
            snapRate = sampleRate;
        }
        std::vector<std::unique_ptr<Slot>> fresh;
        auto releaseFresh = [&fresh]
        {
            for (auto& s : fresh)
                if (s->instance != nullptr) { s->instance->releaseResources(); s->instance->setPlayHead (nullptr); }
        };
        for (const auto& p : plugins)
        {
            const auto exact = catalog.resolve (p, error);
            if (exact.isEmpty()) { releaseFresh(); return false; }
            auto slot = makeSlot (exact, nullptr, error, snapRate);
            if (slot == nullptr) { releaseFresh(); return false; }
            fresh.push_back (std::move (slot));
        }
        const juce::ScopedLock sl (lock);
        if (! juce::isPositiveAndBelow (track, (int) tracks.size()))
        {
            releaseFresh();
            error = "No track " + juce::String (track);
            return false;
        }
        if (tracks[(size_t) track]->frozen)
        {
            releaseFresh();
            error = frozenTrackError (*tracks[(size_t) track]);
            return false;
        }
        auto& trk = *tracks[(size_t) track];
        auto& chain = trk.chain;
        for (auto& s : chain)
            if (s->instance != nullptr) s->instance->setPlayHead (nullptr);
        chain = std::move (fresh);
        clearPluginAutomation (trk);
        updateLatenciesLocked();
        publishPlaybackSnapshotLocked();
        dirty = true;
        return true;
    }

    // Gives each of `to` the same chain as `from`, with the same settings (a copy of every plugin's state).
    bool copyChain (int from, const std::vector<int>& to, juce::String& error)
    {
        struct Snap { juce::String plugin; bool bypass = false; float wet = 1.0f; juce::MemoryBlock state; };
        std::vector<Snap> chainSnap;
        std::vector<int> targets;
        double snapRate = 0.0;
        {
            const juce::ScopedLock sl (lock);
            if (! juce::isPositiveAndBelow (from, (int) tracks.size())) { error = "No track " + juce::String (from); return false; }
            for (int t : to)
            {
                if (! juce::isPositiveAndBelow (t, (int) tracks.size())) { error = "No track " + juce::String (t); return false; }
                if (t != from && tracks[(size_t) t]->frozen) { error = frozenTrackError (*tracks[(size_t) t]); return false; }
                if (t != from && std::find (targets.begin(), targets.end(), t) == targets.end())
                    targets.push_back (t);
            }
            snapRate = sampleRate;
            chainSnap.reserve (tracks[(size_t) from]->chain.size());
            for (auto& s : tracks[(size_t) from]->chain)
                chainSnap.push_back ({ s->plugin, s->bypass, s->wet, stateOf (*s) });
        }

        // Every new chain is built outside `lock` so plugin creation does not silence the audio callback:
        // a plugin that cannot be made leaves all the targets as they were.
        std::vector<std::pair<int, std::vector<std::unique_ptr<Slot>>>> built;
        auto releaseBuilt = [] (std::vector<std::unique_ptr<Slot>>& slots)
        {
            for (auto& s : slots)
                if (s->instance != nullptr) { s->instance->releaseResources(); s->instance->setPlayHead (nullptr); }
        };
        for (int t : targets)
        {
            std::vector<std::unique_ptr<Slot>> fresh;
            fresh.reserve (chainSnap.size());
            for (const auto& item : chainSnap)
            {
                auto slot = makeSlot (item.plugin, &item.state, error, snapRate);
                if (slot == nullptr)
                {
                    releaseBuilt (fresh);
                    for (auto& b : built)
                        releaseBuilt (b.second);
                    return false;
                }
                slot->bypass = item.bypass;
                slot->wet = item.wet;
                fresh.push_back (std::move (slot));
            }
            built.emplace_back (t, std::move (fresh));
        }

        const juce::ScopedLock sl (lock);
        for (auto& b : built)
        {
            if (! juce::isPositiveAndBelow (b.first, (int) tracks.size()))
            {
                for (auto& item : built)
                    releaseBuilt (item.second);
                error = "No track " + juce::String (b.first);
                return false;
            }
            if (tracks[(size_t) b.first]->frozen)
            {
                for (auto& item : built)
                    releaseBuilt (item.second);
                error = frozenTrackError (*tracks[(size_t) b.first]);
                return false;
            }
        }
        for (auto& b : built)
        {
            auto& trk = *tracks[(size_t) b.first];
            releaseBuilt (trk.chain);
            trk.chain = std::move (b.second);
            clearPluginAutomation (trk);
        }
        updateLatenciesLocked();
        publishPlaybackSnapshotLocked();
        dirty = true;
        return true;
    }

    Slot* slotAt (int track, int slot, juce::String& error) const
    {
        if (! juce::isPositiveAndBelow (track, (int) tracks.size())) { error = "No track " + juce::String (track); return nullptr; }
        auto& chain = tracks[(size_t) track]->chain;
        if (! juce::isPositiveAndBelow (slot, (int) chain.size())) { error = "No plugin " + juce::String (slot) + " on track " + juce::String (track); return nullptr; }
        return chain[(size_t) slot].get();
    }

    Slot* instrumentSlot (int track, juce::String& error) const
    {
        if (! juce::isPositiveAndBelow (track, (int) tracks.size())) { error = "No track " + juce::String (track); return nullptr; }
        auto* inst = tracks[(size_t) track]->instrument.get();
        if (inst == nullptr) { error = "No instrument on track " + juce::String (track); return nullptr; }
        return inst;
    }

    bool setTrackInstrument (int track, const juce::String& query, juce::String& error,
                             const std::function<void()>& onBeforeReplace = {})
    {
        double snapRate = 0.0;
        {
            const juce::ScopedLock sl (lock);
            if (! juce::isPositiveAndBelow (track, (int) tracks.size())) { error = "No track " + juce::String (track); return false; }
            auto& trk = *tracks[(size_t) track];
            if (trk.isBus()) { error = "Cannot set an instrument on a bus track."; return false; }
            if (trk.frozen) { error = frozenTrackError (trk); return false; }
            snapRate = sampleRate;
        }
        const auto exact = catalog.resolve (query, error);
        if (exact.isEmpty()) return false;
        auto slot = makeSlot (exact, nullptr, error, snapRate);
        if (slot == nullptr) return false;
        if (onBeforeReplace)
            onBeforeReplace();
        const juce::ScopedLock sl (lock);
        if (! juce::isPositiveAndBelow (track, (int) tracks.size()))
        {
            if (slot->instance != nullptr) { slot->instance->releaseResources(); slot->instance->setPlayHead (nullptr); }
            error = "No track " + juce::String (track);
            return false;
        }
        auto& trk = *tracks[(size_t) track];
        if (trk.isBus())
        {
            if (slot->instance != nullptr) { slot->instance->releaseResources(); slot->instance->setPlayHead (nullptr); }
            error = "Cannot set an instrument on a bus track.";
            return false;
        }
        if (trk.frozen)
        {
            if (slot->instance != nullptr) { slot->instance->releaseResources(); slot->instance->setPlayHead (nullptr); }
            error = frozenTrackError (trk);
            return false;
        }
        if (trk.instrument != nullptr && trk.instrument->instance != nullptr)
            trk.instrument->instance->setPlayHead (nullptr);
        trk.instrument = std::move (slot);
        updateLatenciesLocked();
        publishPlaybackSnapshotLocked();
        dirty = true;
        return true;
    }

    bool removeTrackInstrument (int track, juce::String& error)
    {
        const juce::ScopedLock sl (lock);
        if (! juce::isPositiveAndBelow (track, (int) tracks.size())) { error = "No track " + juce::String (track); return false; }
        auto& trk = *tracks[(size_t) track];
        if (trk.frozen) { error = frozenTrackError (trk); return false; }
        if (trk.instrument == nullptr) { error = "No instrument on track " + juce::String (track); return false; }
        if (trk.instrument->instance != nullptr)
            trk.instrument->instance->setPlayHead (nullptr);
        trk.instrument.reset();
        updateLatenciesLocked();
        publishPlaybackSnapshotLocked();
        dirty = true;
        return true;
    }

    bool bypassTrackInstrument (int track, bool bypass, juce::String& error)
    {
        const juce::ScopedLock sl (lock);
        if (! juce::isPositiveAndBelow (track, (int) tracks.size())) { error = "No track " + juce::String (track); return false; }
        auto& trk = *tracks[(size_t) track];
        if (trk.frozen) { error = frozenTrackError (trk); return false; }
        if (trk.instrument == nullptr) { error = "No instrument on track " + juce::String (track); return false; }
        trk.instrument->bypass = bypass;
        updateLatenciesLocked();
        publishPlaybackSnapshotLocked();
        dirty = true;
        return true;
    }

    // ---- master bus FX chain --------------------------------------------------------------------------------------------
    bool addMasterPlugin (const juce::String& query, int position, juce::String& error)
    {
        double snapRate = 0.0;
        {
            const juce::ScopedLock sl (lock);
            snapRate = sampleRate;
        }
        const auto exact = catalog.resolve (query, error);
        if (exact.isEmpty()) return false;
        auto slot = makeSlot (exact, nullptr, error, snapRate);
        if (slot == nullptr) return false;
        const juce::ScopedLock sl (lock);
        position = position < 0 || position > (int) masterChain.size() ? (int) masterChain.size() : position;
        masterChain.insert (masterChain.begin() + position, std::move (slot));
        updateLatenciesLocked();
        dirty = true;
        return true;
    }

    bool removeMasterPlugin (int index, juce::String& error)
    {
        const juce::ScopedLock sl (lock);
        if (! juce::isPositiveAndBelow (index, (int) masterChain.size())) { error = "No master plugin " + juce::String (index); return false; }
        if (masterChain[(size_t) index]->instance != nullptr) masterChain[(size_t) index]->instance->setPlayHead (nullptr);
        masterChain.erase (masterChain.begin() + index);
        updateLatenciesLocked();
        dirty = true;
        return true;
    }

    bool moveMasterPlugin (int from, int to, juce::String& error)
    {
        const juce::ScopedLock sl (lock);
        if (! juce::isPositiveAndBelow (from, (int) masterChain.size()) || ! juce::isPositiveAndBelow (to, (int) masterChain.size()))
        { error = "No master plugin " + juce::String (juce::isPositiveAndBelow (from, (int) masterChain.size()) ? to : from); return false; }
        auto slot = std::move (masterChain[(size_t) from]);
        masterChain.erase (masterChain.begin() + from);
        masterChain.insert (masterChain.begin() + to, std::move (slot));
        dirty = true;
        bumpRevision();
        return true;
    }

    bool bypassMasterPlugin (int index, bool bypass, juce::String& error)
    {
        const juce::ScopedLock sl (lock);
        if (! juce::isPositiveAndBelow (index, (int) masterChain.size())) { error = "No master plugin " + juce::String (index); return false; }
        masterChain[(size_t) index]->bypass = bypass;
        dirty = true;
        bumpRevision();
        return true;
    }

    bool setMasterChain (const juce::StringArray& plugins, juce::String& error)
    {
        double snapRate = 0.0;
        {
            const juce::ScopedLock sl (lock);
            snapRate = sampleRate;
        }
        std::vector<std::unique_ptr<Slot>> fresh;
        auto releaseFresh = [&fresh]
        {
            for (auto& s : fresh)
                if (s->instance != nullptr) { s->instance->releaseResources(); s->instance->setPlayHead (nullptr); }
        };
        for (const auto& p : plugins)
        {
            const auto exact = catalog.resolve (p, error);
            if (exact.isEmpty()) { releaseFresh(); return false; }
            auto slot = makeSlot (exact, nullptr, error, snapRate);
            if (slot == nullptr) { releaseFresh(); return false; }
            fresh.push_back (std::move (slot));
        }
        const juce::ScopedLock sl (lock);
        for (auto& s : masterChain)
            if (s->instance != nullptr) s->instance->setPlayHead (nullptr);
        masterChain = std::move (fresh);
        updateLatenciesLocked();
        dirty = true;
        return true;
    }

    Slot* masterSlot (int index) const
    {
        if (! juce::isPositiveAndBelow (index, (int) masterChain.size())) return nullptr;
        return masterChain[(size_t) index].get();
    }

    Slot* masterSlot (int index, juce::String& error) const
    {
        if (! juce::isPositiveAndBelow (index, (int) masterChain.size())) { error = "No master plugin " + juce::String (index); return nullptr; }
        return masterChain[(size_t) index].get();
    }

    int masterLatencyLocked() const
    {
        int sum = 0;
        for (const auto& s : masterChain) sum += s->latency();
        return juce::jmax (0, sum);
    }

    int masterLatency() const
    {
        const juce::ScopedLock sl (lock);
        return masterLatencyLocked();
    }

    // ---- time -----------------------------------------------------------------------------------------------------------
    // ---- the monitor chain ----------------------------------------------------------------------------------------------
    // Puts a plugin in the monitor chain at `position` (-1 = the end). Message thread only: the plugin is made here.
    bool addMonitorPlugin (const juce::String& query, int position, juce::String& error)
    {
        const auto exact = catalog.resolve (query, error);
        if (exact.isEmpty()) return false;
        auto slot = makeSlot (exact, nullptr, error, 0.0, false);
        if (slot == nullptr) return false;
        const juce::ScopedLock sl (lock);
        position = position < 0 || position > (int) monitor.size() ? (int) monitor.size() : position;
        monitor.insert (monitor.begin() + position, std::move (slot));
        bumpRevision();
        return true;
    }

    bool removeMonitorPlugin (int index, juce::String& error)
    {
        const juce::ScopedLock sl (lock);
        if (! juce::isPositiveAndBelow (index, (int) monitor.size())) { error = "No monitor plugin " + juce::String (index); return false; }
        if (monitor[(size_t) index]->instance != nullptr) monitor[(size_t) index]->instance->setPlayHead (nullptr);
        monitor.erase (monitor.begin() + index);
        bumpRevision();
        return true;
    }

    // The monitor plugin at `index` (message thread: the chain changes only there).
    Slot* monitorSlotAt (int index, juce::String& error) const
    {
        if (! juce::isPositiveAndBelow (index, (int) monitor.size())) { error = "No monitor plugin " + juce::String (index); return nullptr; }
        return monitor[(size_t) index].get();
    }

    // Moves a plugin of the monitor chain to another place in it.
    bool moveMonitorPlugin (int from, int to, juce::String& error)
    {
        const juce::ScopedLock sl (lock);
        if (! juce::isPositiveAndBelow (from, (int) monitor.size()) || ! juce::isPositiveAndBelow (to, (int) monitor.size()))
        { error = "No monitor plugin " + juce::String (juce::isPositiveAndBelow (from, (int) monitor.size()) ? to : from); return false; }
        auto slot = std::move (monitor[(size_t) from]);
        monitor.erase (monitor.begin() + from);
        monitor.insert (monitor.begin() + to, std::move (slot));
        bumpRevision();
        return true;
    }

    // Bypasses one plugin of the monitor chain (its own flag, saved in monitor.xml).
    bool setMonitorPluginBypass (int index, bool bypass, juce::String& error)
    {
        const juce::ScopedLock sl (lock);
        if (! juce::isPositiveAndBelow (index, (int) monitor.size())) { error = "No monitor plugin " + juce::String (index); return false; }
        monitor[(size_t) index]->bypass = bypass;
        bumpRevision();
        return true;
    }

    // Bypasses the whole monitor chain. It does not dirty the project: the chain is not part of it.
    void setMonitorBypass (bool bypass)
    {
        monitorBypass = bypass;
        bumpRevision();
    }

    // The latency the monitor path adds (a bypassed plugin or chain adds none). The takes are not moved by it: see Record.h.
    int monitorLatency() const
    {
        const juce::ScopedLock sl (lock);
        if (monitorBypass) return 0;
        int sum = 0;
        for (auto& s : monitor) sum += s->latency();
        return sum;
    }

    // Runs the monitor chain in place over one playback block of any length (cut into kBlockSize pieces). The audio callback calls it after
    // processBlock and nowhere else. If the lock is busy the block is left as it is; a bypassed or empty chain does nothing.
    // The plugins are never handed the device's buffer: each block is copied into monitorScratch (two channels at least, so a stereo plugin
    // always has its two, and zeros where the device has none) and copied back for the channels the device has.
    void processMonitor (juce::AudioBuffer<float>& out)
    {
        const int n = out.getNumSamples();
        const int copied = juce::jmin (out.getNumChannels(), kMaxMonitorChannels);
        if (monitorBypass || n <= 0 || copied <= 0)
            return;
        const juce::ScopedTryLock sl (lock);
        if (! sl.isLocked())
            return;
        bool active = false;
        for (auto& s : monitor)
            active = active || (s->instance != nullptr && ! s->bypass);
        if (! active)
            return;
        const int viewChannels = juce::jmax (2, copied);
        for (int start = 0; start < n; start += kBlockSize)
        {
            const int count = juce::jmin (kBlockSize, n - start);
            for (int ch = 0; ch < viewChannels; ++ch)
            {
                if (ch < copied) monitorScratch.copyFrom (ch, 0, out, ch, start, count);
                else monitorScratch.clear (ch, 0, count);
            }
            juce::AudioBuffer<float> view (monitorScratch.getArrayOfWritePointers(), viewChannels, count);
            midiScratch.clear();
            for (auto& s : monitor)
                processSlot (*s, view, midiScratch);
            for (int ch = 0; ch < copied; ++ch)
                out.copyFrom (ch, start, monitorScratch, ch, 0, count);
        }
    }

    // The monitor chain in its own file: the plugins' names, their bypass flags and each one's settings (a plugin that is not installed keeps
    // its saved settings). The chain's own bypass is saved too. Message thread.
    bool saveMonitor (const juce::File& file, juce::String& error)
    {
        auto xml = std::make_unique<juce::XmlElement> ("OlympusMonitor");
        xml->setAttribute ("version", kMonitorVersion);
        xml->setAttribute ("bypass", monitorBypass.load());
        {
            // every plugin's state is read here: not while processBlock runs beside it
            const juce::ScopedLock sl (lock);
            for (auto& s : monitor)
            {
                auto* e = xml->createNewChildElement ("Plugin");
                e->setAttribute ("plugin", s->plugin);
                e->setAttribute ("bypass", s->bypass);
                if (s->wet < 0.9999f)
                    e->setAttribute ("wet", (double) s->wet);
                e->setAttribute ("state", stateOf (*s).toBase64Encoding());
            }
        }
        file.getParentDirectory().createDirectory();
        juce::TemporaryFile temp (file);
        if (! xml->writeTo (temp.getFile()) || ! temp.overwriteTargetFileWithTemporary())
        {
            error = "Cannot write " + file.getFullPathName();
            return false;
        }
        return true;
    }

    // Replaces the monitor chain with the one in `file`. A plugin that cannot be made stays as a slot with its saved settings and a line in
    // `problems`. A missing file means an empty chain. Returns false (the chain unchanged) when the file is not a monitor chain, or is newer.
    bool loadMonitor (const juce::File& file, juce::StringArray& problems)
    {
        if (! file.existsAsFile())
        {
            const juce::ScopedLock sl (lock);
            for (auto& s : monitor)
                if (s->instance != nullptr) s->instance->setPlayHead (nullptr);
            monitor.clear();
            bumpRevision();
            return true;
        }
        const auto xml = juce::parseXML (file);
        if (xml == nullptr || ! xml->hasTagName ("OlympusMonitor")) { problems.add (file.getFileName() + " is not an Olympus monitor chain."); return false; }
        if (xml->getIntAttribute ("version", 1) > kMonitorVersion) { problems.add ("This monitor chain is from a newer Olympus."); return false; }

        std::vector<std::unique_ptr<Slot>> fresh;
        for (auto* e : xml->getChildWithTagNameIterator ("Plugin"))
        {
            juce::MemoryBlock state;
            state.fromBase64Encoding (e->getStringAttribute ("state"));
            const auto pluginName = e->getStringAttribute ("plugin");
            juce::String problem;
            auto slot = makeSlot (pluginName, &state, problem, 0.0, false);
            if (slot == nullptr)
            {
                problems.add ("Monitor '" + pluginName + "': " + problem);
                slot = std::make_unique<Slot>();
                slot->plugin = pluginName;
                slot->savedState = state;
            }
            slot->bypass = e->getBoolAttribute ("bypass");
            const double rawWet = e->getDoubleAttribute ("wet", 1.0);
            slot->wet = std::isfinite (rawWet) ? juce::jlimit (0.0f, 1.0f, (float) rawWet) : 1.0f;
            fresh.push_back (std::move (slot));
        }
        const juce::ScopedLock sl (lock);
        for (auto& s : monitor)
            if (s->instance != nullptr) s->instance->setPlayHead (nullptr);
        monitor = std::move (fresh);
        monitorBypass = xml->getBoolAttribute ("bypass");
        bumpRevision();
        return true;
    }

    // The end of the last clip of any track (and of the tails the chains do not count): what the window plays to.
    juce::int64 length() const
    {
        juce::int64 n = 0;
        for (auto& t : tracks) n = juce::jmax (n, t->length());
        return n;
    }
    juce::int64 endOfAudio() const { return length(); }
    int maxLatency() const
    {
        std::vector<int> order, inLat, outLat;
        computeRoutingLocked (order, inLat, outLat);
        int m = 0;
        for (int lat : outLat) m = juce::jmax (m, lat);
        return m;
    }

    // Whether the track is heard: not muted, and (when anything is soloed) soloed.
    static bool audible (const Track& t, bool anySolo)
    {
        return ! t.mute && (! anySolo || t.solo);
    }
    bool audible (const Track& t) const
    {
        bool anySolo = false;
        for (auto& o : tracks) anySolo = anySolo || o->solo;
        return audible (t, anySolo);
    }

    // The gains of a balance pan: the far side is attenuated, the near side is untouched (so the centre is unity and a track is never
    // louder for being panned). pan -1 is hard left: nothing in the right.
    static void panGains (float pan, float& left, float& right)
    {
        pan = juce::jlimit (-1.0f, 1.0f, pan);
        left = pan > 0.0f ? 1.0f - pan : 1.0f;
        right = pan < 0.0f ? 1.0f + pan : 1.0f;
    }

    // The clips of a track on [start, start + n), summed into `dest` (cleared first; n <= dest's length). Muted clips and clips without
    // audio are silent. A clip is gain, then the linear fades at its start and end, inside the clip only. Used by the real time path and
    // the render alike, so both hear the same thing.
    static void mixClipList (const std::vector<Clip>& clips, juce::int64 start, int n, juce::AudioBuffer<float>& dest)
    {
        dest.clear (0, n);
        const int channels = juce::jmin (2, dest.getNumChannels());
        if (channels <= 0 || n <= 0)
            return;
        for (const auto& c : clips)
        {
            if (c.mute || c.audio == nullptr || c.length <= 0)
                continue;
            const juce::int64 from = juce::jmax (start, c.start), to = juce::jmin (start + n, c.end());
            if (from >= to)
                continue;
            const auto& src = *c.audio;
            const int srcChannels = src.getNumChannels();
            const juce::int64 srcSamples = (juce::int64) src.getNumSamples();
            if (srcChannels <= 0 || srcSamples <= 0)
                continue;
            const juce::int64 sFrom = juce::jmax (from, c.start - c.offset);
            const juce::int64 sTo = juce::jmin (to, c.start - c.offset + srcSamples);
            if (sFrom >= sTo)
                continue;
            const float gain = juce::Decibels::decibelsToGain (c.gainDb, -100.0f);
            const juce::int64 flatStart = c.fadeIn > 0 ? juce::jlimit (sFrom, sTo, c.start + c.fadeIn) : sFrom;
            const juce::int64 flatEnd = c.fadeOut > 0 ? juce::jlimit (flatStart, sTo, c.end() - c.fadeOut + 1) : sTo;

            auto addFadeSample = [&] (juce::int64 s)
            {
                const juce::int64 local = s - c.start;
                const int index = (int) (c.offset + local);
                float env = gain;
                if (local < c.fadeIn)
                    env *= (float) local / (float) c.fadeIn;
                const juce::int64 remaining = c.length - local;
                if (remaining < c.fadeOut)
                    env *= (float) remaining / (float) c.fadeOut;
                const int destIdx = (int) (s - start);
                for (int ch = 0; ch < channels; ++ch)
                    dest.getWritePointer (ch)[destIdx] += src.getReadPointer (juce::jmin (ch, srcChannels - 1))[index] * env;
            };

            for (juce::int64 s = sFrom; s < flatStart; ++s)
                addFadeSample (s);

            if (flatEnd > flatStart)
            {
                const int count = (int) (flatEnd - flatStart);
                const int destStart = (int) (flatStart - start);
                const int srcStart = (int) (c.offset + (flatStart - c.start));
                for (int ch = 0; ch < channels; ++ch)
                {
                    const int srcCh = juce::jmin (ch, srcChannels - 1);
                    juce::FloatVectorOperations::addWithMultiply (dest.getWritePointer (ch, destStart),
                                                                  src.getReadPointer (srcCh, srcStart),
                                                                  gain, count);
                }
            }

            for (juce::int64 s = flatEnd; s < sTo; ++s)
                addFadeSample (s);
        }
    }

    static void mixClips (const Track& t, juce::int64 start, int n, juce::AudioBuffer<float>& dest)
    {
        mixClipList (t.clips, start, n, dest);
    }

    static void collectMidiEvents (const std::vector<MidiClip>& midiClips, const TempoMap& map,
                                   juce::int64 start, int n, juce::MidiBuffer& dest,
                                   bool triggerSpanning = false, const BasicSynth* activeSynth = nullptr)
    {
        if (n <= 0 || ! std::isfinite (map.sampleRate) || map.sampleRate <= 0.0)
            return;

        auto forEachValidNote = [&] (auto&& fn)
        {
            for (const auto& mc : midiClips)
            {
                if (mc.mute || mc.length <= 0)
                    continue;
                const double clipStartBeats = map.samplesToBeats ((double) mc.start);
                const juce::int64 clipEnd = mc.end();
                for (const auto& note : mc.notes)
                {
                    if (! std::isfinite (note.startBeat) || ! std::isfinite (note.durationBeats)
                        || note.durationBeats <= 0.0 || note.velocity <= 0)
                        continue;
                    const double onS = map.beatsToSamples (clipStartBeats + note.startBeat);
                    const double offS = map.beatsToSamples (clipStartBeats + note.startBeat + note.durationBeats);
                    if (! std::isfinite (onS) || ! std::isfinite (offS) || onS >= (double) clipEnd || offS <= (double) mc.start)
                        continue;
                    const juce::int64 onSample = onS <= (double) mc.start ? mc.start : (juce::int64) std::llround (onS);
                    const juce::int64 offSample = offS >= (double) clipEnd ? clipEnd : (juce::int64) std::llround (offS);
                    if (onSample >= clipEnd || offSample <= onSample)
                        continue;
                    const int ch = juce::jlimit (1, 16, note.channel);
                    const int pitch = juce::jlimit (0, 127, note.pitch);
                    const juce::uint8 vel = (juce::uint8) juce::jlimit (1, 127, note.velocity);
                    fn (onSample, offSample, ch, pitch, vel);
                }
            }
        };

        // Pass 1: emit all noteOff events in [start, start + n) before any noteOn at the same sample offset.
        forEachValidNote ([&] (juce::int64, juce::int64 offSample, int ch, int pitch, juce::uint8)
        {
            if (offSample >= start && offSample < start + n)
                dest.addEvent (juce::MidiMessage::noteOff (ch, pitch), (int) (offSample - start));
        });

        // Pass 2: emit all noteOn (and spanning) events in [start, start + n).
        forEachValidNote ([&] (juce::int64 onSample, juce::int64 offSample, int ch, int pitch, juce::uint8 vel)
        {
            if (onSample >= start && onSample < start + n)
                dest.addEvent (juce::MidiMessage::noteOn (ch, pitch, vel), (int) (onSample - start));
            else if (triggerSpanning && onSample < start && offSample > start
                     && (activeSynth == nullptr || ! activeSynth->isNoteActive (ch, pitch)))
                dest.addEvent (juce::MidiMessage::noteOn (ch, pitch, vel), 0);
        });
    }

    static void appendLiveMidiSlice (const juce::MidiBuffer& live, int offset, int count, juce::MidiBuffer& dest)
    {
        if (live.isEmpty() || count <= 0)
            return;
        for (const auto metadata : live)
        {
            if (metadata.samplePosition >= offset && metadata.samplePosition < offset + count)
                dest.addEvent (metadata.data, metadata.numBytes, metadata.samplePosition - offset);
        }
    }

    // Makes `rate` the session's rate (the device's, see the window). Message thread. Without clip audio it only sets the rate. With audio, every
    // buffer of the pool is resampled once (a buffer shared by clips stays shared), and every clip, marker and the loop is scaled to the new
    // rate in samples (so it keeps its seconds; a clip keeps at least one sample). Then every plugin of every track and the monitor chain
    // is set up again for the new rate. The undo steps are not converted: the window clears them. A rate that is the same changes nothing.
    // A rate whose resampled length would overflow int is refused whole, before anything changes.
    bool adoptRate (double rate, juce::String& error)
    {
        if (! (rate > 0.0) || ! std::isfinite (rate)) { error = "The sample rate must be above zero."; return false; }
        const juce::ScopedLock sl (lock);
        if (std::abs (rate - sampleRate) < 0.5)
            return true;
        const double old = sampleRate;
        const double k = old > 0.0 ? rate / old : 1.0;

        // pre-flight: every clip's buffer must fit the resampler's length guard before anything changes, so a refused rate leaves the
        // whole session as it was (a buffer kept at the old rate under a new one would be resampled down by the next adoptRate)
        if (old > 0.0)
            for (auto& t : tracks)
            {
                for (auto& c : t->clips)
                {
                    if (c.audio == nullptr)
                        continue;
                    const double wanted = std::ceil ((double) c.audio->getNumSamples() / (old / rate)); // the length resampled() would want
                    if (! std::isfinite (wanted) || wanted > (double) (std::numeric_limits<int>::max() - 16))
                    {
                        error = "Clip '" + c.name + "' on track '" + t->name + "' cannot be resampled to " + juce::String (rate)
                                + " Hz; the rate is unchanged.";
                        return false;
                    }
                }
                if (t->frozen && t->frozenAudio != nullptr)
                {
                    const double wanted = std::ceil ((double) t->frozenAudio->getNumSamples() / (old / rate));
                    if (! std::isfinite (wanted) || wanted > (double) (std::numeric_limits<int>::max() - 16))
                    {
                        error = "Frozen audio on track '" + t->name + "' cannot be resampled to " + juce::String (rate)
                                + " Hz; the rate is unchanged.";
                        return false;
                    }
                }
            }

        // every distinct buffer of the clips, converted once; the old ones are kept alive until the clips are changed
        std::map<const juce::AudioBuffer<float>*, std::shared_ptr<const juce::AudioBuffer<float>>> converted;
        std::vector<std::shared_ptr<const juce::AudioBuffer<float>>> keepAlive;
        for (auto& t : tracks)
        {
            for (auto& c : t->clips)
            {
                if (c.audio == nullptr || old <= 0.0)
                    continue;
                keepAlive.push_back (c.audio);
                auto found = converted.find (c.audio.get());
                if (found == converted.end())
                    found = converted.emplace (c.audio.get(), std::make_shared<const juce::AudioBuffer<float>> (resampled (*c.audio, old, rate))).first;
                c.audio = found->second;
            }
            if (t->frozen && t->frozenAudio != nullptr && old > 0.0)
            {
                keepAlive.push_back (t->frozenAudio);
                auto found = converted.find (t->frozenAudio.get());
                if (found == converted.end())
                    found = converted.emplace (t->frozenAudio.get(), std::make_shared<const juce::AudioBuffer<float>> (resampled (*t->frozenAudio, old, rate))).first;
                t->frozenAudio = found->second;
            }
        }
        for (auto it = pool.begin(); it != pool.end();)
        {
            const auto alive = it->second.lock();
            const auto found = alive != nullptr ? converted.find (alive.get()) : converted.end();
            if (found != converted.end()) { it->second = found->second; ++it; }
            else it = pool.erase (it);
        }

        // bounded: a saved position at the limit times a large k (8 kHz to 48 kHz) would not fit in int64, so it lands on the limit
        const auto scale = [k] (juce::int64 v)
        {
            const double scaled = (double) v * k;
            if (! (scaled > 0.0)) return (juce::int64) 0;
            if (scaled >= (double) kMaxSafeSample) return kMaxSafeSample;
            return (juce::int64) std::llround (scaled);
        };
        for (auto& t : tracks)
        {
            for (auto& c : t->clips)
            {
                c.start = scale (c.start);
                c.offset = scale (c.offset);
                c.length = juce::jmax ((juce::int64) 1, scale (c.length));
                if (c.audio != nullptr)
                {
                    const juce::int64 n = c.audio->getNumSamples();
                    c.offset = juce::jmin (c.offset, juce::jmax ((juce::int64) 0, n - 1));
                    c.length = juce::jmax ((juce::int64) 1, juce::jmin (c.length, n - c.offset));
                }
                c.fadeIn = juce::jmin (scale (c.fadeIn), c.length);
                c.fadeOut = juce::jmin (scale (c.fadeOut), c.length - c.fadeIn); // the two fades never overlap (Edit.h keeps the same rule)
            }
            if (old > 0.0)
                for (auto& mc : t->midiClips)
                {
                    mc.start = scale (mc.start);
                    mc.length = juce::jmax ((juce::int64) 1, scale (mc.length));
                }
            if (old > 0.0)
                for (auto& lane : t->automation)
                {
                    for (auto& pt : lane.points)
                        pt.sample = scale (pt.sample);
                    std::stable_sort (lane.points.begin(), lane.points.end(),
                                      [] (const AutoPoint& a, const AutoPoint& b) { return a.sample < b.sample; });
                }
        }
        for (auto& m : markers)
            m.start = scale (m.start);
        loopStart = scale (loopStart);
        loopEnd = scale (loopEnd);
        loopOn = loopOn && loopEnd > loopStart; // a narrow loop can scale to equal ends: off then, as load() and Editor::setLoop keep it

        setSampleRateLocked (rate);  // also the tempo's rate, and the monitor chain
        prepareLocked (false);       // every plugin of every track
        publishPlaybackSnapshotLocked();
        if (old > 0.0) dirty = true;
        return true;
    }

    // ---- playing --------------------------------------------------------------------------------------------------------
    // Sets every plugin up for `sampleRate` and the block size, for real time or (nonRealtime) for rendering.
    void prepare (bool nonRealtime)
    {
        const juce::ScopedLock sl (lock);
        offline = nonRealtime;
        prepareLocked (nonRealtime);
    }

    // Clears the real-time latency compensation delay rings of every track (for example after a seek so stale audio is not heard).
    void clearDelayRings()
    {
        const juce::ScopedLock sl (lock);
        clearDelayRingsLocked();
    }

    // `out` is overwritten with [start, start + n) of the session (two channels, cut into kBlockSize pieces so every plugin and track
    // scratch stays within kBlockSize; n <= 0 does nothing). No loop here: the caller splits blocks at the loop's end. Audio thread.
    void processBlock (juce::AudioBuffer<float>& out, juce::int64 start)
    {
        const int n = out.getNumSamples();
        if (n <= 0)
            return;
        out.clear();
        liveMidiScratch.clear();
        liveMidiFifo.drain (liveMidiScratch, n);
        const juce::ScopedTryLock sl (lock);
        if (! sl.isLocked())
        {
            if (snapshotValid.load (std::memory_order_acquire))
                processBlockFromSnapshot (out, start);
            return;
        }
        const bool wasFallback = lastBlockWasFallback;
        lastBlockWasFallback = false;
        const bool discontinuous = ! rendering() && lastProcessedEnd >= 0 && start != lastProcessedEnd;
        if (wasFallback || discontinuous)
        {
            for (auto& t : tracks)
            {
                if (t->synth != nullptr)
                    t->synth->reset();
                if (t->activeNotes.any())
                    t->pendingNoteFlush = true;
            }
        }
        computeTrackAudibilityLocked (cachedAudibleScratch);
        const int channels = juce::jmin (2, out.getNumChannels());
        const juce::int64 maxLat = (juce::int64) maxLatency();
        bool hasMasterPlugin = false;
        for (const auto& s : masterChain)
            if (s->instance != nullptr && ! s->bypass) { hasMasterPlugin = true; break; }
        for (int done = 0; done < n; done += kBlockSize)
        {
            const int count = juce::jmin (kBlockSize, n - done);
            const juce::int64 blockStart = start + done;
            publishHead (blockStart, true);
            for (auto& t : tracks)
                if (t->isBus())
                    t->busInput.clear (0, count);

            for (int ti : cachedTopoOrder)
            {
                auto& t = *tracks[(size_t) ti];
                const bool isAudible = (size_t) ti < cachedAudibleScratch.size() && cachedAudibleScratch[(size_t) ti];
                if (! isAudible || (! t.isBus() && ! (t.frozen && t.frozenAudio != nullptr) && t.clips.empty() && t.midiClips.empty() && ! t.isMidi))
                {
                    if (t.synth != nullptr)
                        t.synth->reset();
                    if (t.activeNotes.any())
                    {
                        if (t.scratch.getNumChannels() >= 2)
                        {
                            midiScratch.clear();
                            t.activeNotes.appendNoteOffs (midiScratch, 0);
                            t.scratch.clear (0, count);
                            juce::AudioBuffer<float> instView (t.scratch.getArrayOfWritePointers(), t.scratch.getNumChannels(), count);
                            if (t.instrument != nullptr && t.instrument->instance != nullptr && ! t.instrument->bypass)
                                processSlot (*t.instrument, instView, midiScratch);
                            for (auto& s : t.chain)
                                if (s->instance != nullptr && ! s->bypass && s->instance->acceptsMidi())
                                    processSlot (*s, instView, midiScratch);
                        }
                        t.activeNotes.reset();
                    }
                    t.pendingNoteFlush = false;
                    // skipped: its delay must not come back with stale audio when it plays again
                    if (t.delayDirty)
                    {
                        t.delayRing.clear();
                        t.delayPos = 0;
                        t.delayDirty = false;
                    }
                    t.scratch.clear (0, count);
                    t.postScratch.clear (0, count);
                    continue;
                }
                const juce::MidiBuffer* live = (! t.isBus() && t.isMidi && t.armed) ? &liveMidiScratch : nullptr;
                runChain (t, blockStart, count, live, done);

                for (const auto& snd : t.sends)
                {
                    if (! snd.preFader || snd.mute)
                        continue;
                    if (juce::isPositiveAndBelow (snd.targetTrack, (int) tracks.size()) && tracks[(size_t) snd.targetTrack]->isBus())
                    {
                        const float sg = juce::Decibels::decibelsToGain (snd.levelDb, -100.0f);
                        auto& dst = tracks[(size_t) snd.targetTrack]->busInput;
                        for (int ch = 0; ch < 2; ++ch)
                            dst.addFrom (ch, 0, t.scratch, ch, 0, count, sg);
                    }
                }

                t.postScratch.clear (0, count);
                float pL = 0.0f, pR = 0.0f;
                const juce::int64 autoOffset = ((size_t) ti < cachedOutLat.size())
                                                   ? (juce::int64) (cachedOutLat[(size_t) ti] + t.delaySamples)
                                                   : maxLat;
                mixTrackBlock (t.scratch, t.postScratch, 0, count, blockStart - autoOffset, t.gainDb, t.pan,
                               findActiveLane (t.automation, "volume"), findActiveLane (t.automation, "pan"),
                               &pL, &pR);
                if (std::isfinite (pL)) t.peakL = juce::jmax (t.peakL.load(), pL);
                if (std::isfinite (pR)) t.peakR = juce::jmax (t.peakR.load(), pR);

                for (const auto& snd : t.sends)
                {
                    if (snd.preFader || snd.mute)
                        continue;
                    if (juce::isPositiveAndBelow (snd.targetTrack, (int) tracks.size()) && tracks[(size_t) snd.targetTrack]->isBus())
                    {
                        const float sg = juce::Decibels::decibelsToGain (snd.levelDb, -100.0f);
                        auto& dst = tracks[(size_t) snd.targetTrack]->busInput;
                        for (int ch = 0; ch < 2; ++ch)
                            dst.addFrom (ch, 0, t.postScratch, ch, 0, count, sg);
                    }
                }

                if (juce::isPositiveAndBelow (t.outputTrack, (int) tracks.size()) && tracks[(size_t) t.outputTrack]->isBus())
                {
                    auto& dst = tracks[(size_t) t.outputTrack]->busInput;
                    for (int ch = 0; ch < 2; ++ch)
                        dst.addFrom (ch, 0, t.postScratch, ch, 0, count, 1.0f);
                }
                else
                {
                    for (int ch = 0; ch < channels; ++ch)
                        out.addFrom (ch, done, t.postScratch, ch, 0, count, 1.0f);
                }
            }
            if (hasMasterPlugin && channels > 0 && masterScratch.getNumChannels() >= 2)
            {
                const int viewChannels = masterScratch.getNumChannels();
                for (int ch = 0; ch < viewChannels; ++ch)
                {
                    if (ch < channels) masterScratch.copyFrom (ch, 0, out, ch, done, count);
                    else masterScratch.clear (ch, 0, count);
                }
                juce::AudioBuffer<float> view (masterScratch.getArrayOfWritePointers(), viewChannels, count);
                midiScratch.clear();
                for (auto& s : masterChain)
                    processSlot (*s, view, midiScratch);
                for (int ch = 0; ch < channels; ++ch)
                    out.copyFrom (ch, done, masterScratch, ch, 0, count);
            }
        }
        addMetronome (out, start);
        out.applyGain (juce::Decibels::decibelsToGain (masterDb, -100.0f));
        if (channels > 0)
        {
            const float mL = out.getMagnitude (0, 0, n);
            if (std::isfinite (mL)) masterPeakL = juce::jmax (masterPeakL.load(), mL);
        }
        if (channels > 1)
        {
            const float mR = out.getMagnitude (1, 0, n);
            if (std::isfinite (mR)) masterPeakR = juce::jmax (masterPeakR.load(), mR);
        }
        lastProcessedEnd = start + n;
    }

    // ---- rendering ------------------------------------------------------------------------------------------------------
    // Writes every audible track as a WAV of its own (named `NN-track.wav`, after its clips, chain, fader and pan, aligned to the audio:
    // the chain's latency is taken off the front; no master gain), and the mix (the stems summed, with the master gain). All tracks run
    // together, block by block, which is what a plugin that listens to the others (Hestia's bus) needs. `progress` gets 0..1 and returns
    // false to stop. Covers [0, length() + tail).
    bool renderStems (const RenderOptions& options, RenderResult& result, juce::String& error, const std::function<bool (float)>& progress = {})
    {
        struct Busy { std::atomic<bool>& flag; explicit Busy (std::atomic<bool>& f) : flag (f) { flag = true; } ~Busy() { flag = false; } };
        const Busy busy (renderBusy);   // set before the lock is taken: the message thread sees it while this waits for the lock too
        const juce::ScopedLock sl (lock);
        if (tracks.empty()) { error = "There are no tracks."; return false; }
        if (sampleRate <= 0.0) { error = "Nothing to render: no audio has been added."; return false; }
        if (! options.writeStems && ! options.writeMix) { error = "Nothing to write: stems and mix are both off."; return false; }
        // The tail is at most a minute: a reverb or delay ringing out, not a way to write hours of silence.
        const double tail = std::isfinite (options.tailSeconds) ? juce::jlimit (0.0, 60.0, options.tailSeconds) : 0.0;
        const juce::int64 stemLength = length() + (juce::int64) std::llround (tail * sampleRate);
        if (stemLength > (juce::int64) (3.0 * 3600.0 * sampleRate)) { error = "The render would be longer than three hours."; return false; }
        if (! options.folder.createDirectory().wasOk()) { error = "Cannot make the folder " + options.folder.getFullPathName(); return false; }
        const OfflineScope offlineScope (*this); // plugins in render mode for this function; back to real time on every way out

        struct Out { int trackIndex = -1; Track* track = nullptr; std::unique_ptr<juce::AudioFormatWriter> writer; juce::File file; int latency = 0; float left = 1.0f, right = 1.0f; juce::int64 written = 0; float peak = 0.0f; };
        std::vector<Out> outs;
        const auto mixFile = options.folder.getChildFile ("mix.wav");
        std::unique_ptr<juce::AudioFormatWriter> mixWriter;
        // an error after files were made: this run's stems go, as a cancel's do, so no partial folder is left (the caller reports the error)
        auto discardRun = [&outs, &options, &mixWriter, &mixFile]
        {
            for (auto& o : outs) { o.writer.reset(); if (options.writeStems) o.file.deleteFile(); }
            mixWriter.reset();
            if (options.writeMix) mixFile.deleteFile();
        };
        std::vector<bool> trackAudible;
        computeTrackAudibilityLocked (trackAudible);
        std::vector<int> topoOrder, inLat, outLat;
        computeRoutingLocked (topoOrder, inLat, outLat);
        std::vector<int> trackToOut (tracks.size(), -1);
        for (int ti = 0; ti < (int) tracks.size(); ++ti)
        {
            if (! trackAudible[(size_t) ti]) continue;
            auto& t = tracks[(size_t) ti];
            Out o;
            o.trackIndex = ti;
            o.track = t.get();
            o.latency = outLat[(size_t) ti];
            trackGains (*o.track, o.left, o.right);
            o.file = options.folder.getChildFile (juce::String (ti + 1).paddedLeft ('0', 2) + "-" + juce::File::createLegalFileName (t->name) + ".wav");
            if (options.writeStems)
            {
                o.writer = makeWriter (o.file, options.bits, error);
                if (o.writer == nullptr) { o.file.deleteFile(); discardRun(); return false; }
            }
            trackToOut[(size_t) ti] = (int) outs.size();
            outs.push_back (std::move (o));
        }
        if (outs.empty()) { error = "No track is audible (all muted, or others soloed)."; return false; }

        if (options.writeMix)
        {
            mixWriter = makeWriter (mixFile, options.bits, error);
            if (mixWriter == nullptr)
            {
                if (error.isEmpty()) error = "Writing mix.wav failed.";
                mixFile.deleteFile();
                discardRun();
                return false;
            }
        }

        struct EdgeDelay
        {
            int dstBus = -1;
            bool preFader = false;
            float gain = 1.0f;
            int delaySamples = 0;
            int pos = 0;
            juce::AudioBuffer<float> ring;
            void pushAndAdd (const juce::AudioBuffer<float>& src, int n, juce::AudioBuffer<float>& dst)
            {
                if (delaySamples <= 0)
                {
                    for (int ch = 0; ch < 2; ++ch)
                        dst.addFrom (ch, 0, src, ch, 0, n, gain);
                    return;
                }
                for (int i = 0; i < n; ++i)
                {
                    for (int ch = 0; ch < 2; ++ch)
                    {
                        auto* r = ring.getWritePointer (ch);
                        const float delayed = r[pos];
                        r[pos] = src.getSample (ch, i);
                        dst.addSample (ch, i, delayed * gain);
                    }
                    if (++pos >= delaySamples) pos = 0;
                }
            }
        };
        std::vector<std::vector<EdgeDelay>> edgeDelays (tracks.size());
        for (int ti = 0; ti < (int) tracks.size(); ++ti)
        {
            if (! trackAudible[(size_t) ti]) continue;
            const auto& t = *tracks[(size_t) ti];
            auto addEdge = [&] (int targetBus, bool preFader, float gain)
            {
                if (! juce::isPositiveAndBelow (targetBus, (int) tracks.size()) || ! tracks[(size_t) targetBus]->isBus())
                    return;
                EdgeDelay ed;
                ed.dstBus = targetBus;
                ed.preFader = preFader;
                ed.gain = gain;
                ed.delaySamples = juce::jmax (0, inLat[(size_t) targetBus] - outLat[(size_t) ti]);
                if (ed.delaySamples > 0)
                {
                    ed.ring.setSize (2, ed.delaySamples, false, true, false);
                    ed.ring.clear();
                }
                edgeDelays[(size_t) ti].push_back (std::move (ed));
            };
            for (const auto& snd : t.sends)
                if (! snd.mute)
                    addEdge (snd.targetTrack, snd.preFader, juce::Decibels::decibelsToGain (snd.levelDb, -100.0f));
            if (t.outputTrack >= 0)
                addEdge (t.outputTrack, false, 1.0f);
        }

        int totalLatency = 0;
        for (auto& o : outs) totalLatency = juce::jmax (totalLatency, o.latency);
        const int masterLat = options.writeMix ? masterLatencyLocked() : 0;
        int masterChannels = 2;
        bool hasMasterPlugin = false;
        if (options.writeMix)
        {
            for (const auto& s : masterChain)
            {
                if (s->instance != nullptr && ! s->bypass)
                {
                    hasMasterPlugin = true;
                    masterChannels = juce::jmax (masterChannels, s->instance->getTotalNumInputChannels(), s->instance->getTotalNumOutputChannels());
                }
            }
        }
        const int mixRingSize = totalLatency + kBlockSize;
        juce::AudioBuffer<float> mixRing (2, options.writeMix ? mixRingSize : 0);
        mixRing.clear();
        juce::AudioBuffer<float> masterBlock (masterChannels, (options.writeMix && hasMasterPlugin) ? kBlockSize : 0);
        const float masterGain = juce::Decibels::decibelsToGain (masterDb, -100.0f);
        float mixPeak = 0.0f;
        juce::int64 mixFlushed = 0;

        const juce::int64 mixTotalInput = stemLength + masterLat;
        const juce::int64 end = stemLength + totalLatency + masterLat;
        juce::AudioBuffer<float> block (2, kBlockSize);
        struct HeadStop
        {
            Session& session;
            juce::int64 pos = 0;
            ~HeadStop() { session.publishHead (pos, false); }
        } headStop { *this, 0 };

        for (juce::int64 start = 0; start < end; start += kBlockSize)
        {
            const int n = (int) juce::jmin ((juce::int64) kBlockSize, end - start);
            headStop.pos = start;
            publishHead (start, true);
            for (auto& t : tracks)
                if (t->isBus())
                    t->busInput.clear (0, n);

            for (int ti : topoOrder)
            {
                auto& t = *tracks[(size_t) ti];
                if (! trackAudible[(size_t) ti])
                {
                    t.scratch.clear (0, n);
                    t.postScratch.clear (0, n);
                    continue;
                }
                runChain (t, start, n);
                for (int ch = 0; ch < 2; ++ch)
                {
                    auto* sDst = t.scratch.getWritePointer (ch);
                    for (int i = 0; i < n; ++i)
                        if (! std::isfinite (sDst[i]))
                            sDst[i] = 0.0f;
                }
                const auto* volLane = findActiveLane (t.automation, "volume");
                const auto* panLane = findActiveLane (t.automation, "pan");
                const int trackLat = outLat[(size_t) ti];
                if (volLane == nullptr && panLane == nullptr)
                {
                    float gL = 1.0f, gR = 1.0f;
                    trackGains (t, gL, gR);
                    for (int ch = 0; ch < 2; ++ch)
                    {
                        t.postScratch.copyFrom (ch, 0, t.scratch, ch, 0, n);
                        t.postScratch.applyGain (ch, 0, n, ch == 0 ? gL : gR);
                    }
                }
                else
                {
                    const float* src0 = t.scratch.getReadPointer (0);
                    const float* src1 = t.scratch.getReadPointer (1);
                    float* dst0 = t.postScratch.getWritePointer (0);
                    float* dst1 = t.postScratch.getWritePointer (1);
                    for (int i = 0; i < n; ++i)
                    {
                        const juce::int64 stemPos = start + i - trackLat;
                        const float gainDb = volLane != nullptr ? autoNormToDb (volLane->evaluateAt (stemPos)) : t.gainDb;
                        const float panVal = panLane != nullptr ? juce::jlimit (-1.0f, 1.0f, panLane->evaluateAt (stemPos) * 2.0f - 1.0f) : t.pan;
                        const float g = juce::Decibels::decibelsToGain (gainDb, -100.0f);
                        float pL = 1.0f, pR = 1.0f;
                        panGains (panVal, pL, pR);
                        dst0[i] = src0[i] * (g * pL);
                        dst1[i] = src1[i] * (g * pR);
                    }
                }
                for (int ch = 0; ch < 2; ++ch)
                {
                    auto* dst = t.postScratch.getWritePointer (ch);
                    for (int i = 0; i < n; ++i)
                        if (! std::isfinite (dst[i]))
                            dst[i] = 0.0f;
                }
                for (auto& ed : edgeDelays[(size_t) ti])
                    ed.pushAndAdd (ed.preFader ? t.scratch : t.postScratch, n, tracks[(size_t) ed.dstBus]->busInput);

                const int outIdx = trackToOut[(size_t) ti];
                if (outIdx < 0) continue;
                auto& o = outs[(size_t) outIdx];
                // this block holds output samples [start, start + n): sample j of it is stem sample j - latency
                const juce::int64 first = juce::jmax ((juce::int64) 0, (juce::int64) o.latency - start);
                const juce::int64 last = juce::jmin ((juce::int64) n, stemLength + o.latency - start);
                if (last <= first) continue;
                const int count = (int) (last - first);
                block.setSize (2, count, false, false, true);
                for (int ch = 0; ch < 2; ++ch)
                {
                    block.copyFrom (ch, 0, t.postScratch, ch, (int) first, count);
                    const float mag = block.getMagnitude (ch, 0, count);
                    if (std::isfinite (mag))
                        o.peak = juce::jmax (o.peak, mag);
                }
                if (options.writeStems && o.writer != nullptr && ! o.writer->writeFromAudioSampleBuffer (block, 0, count))
                {
                    error = "Writing " + o.file.getFileName() + " failed.";
                    discardRun();
                    return false;
                }
                if (options.writeMix && t.outputTrack < 0)
                {
                    const juce::int64 dest = start + first - o.latency;
                    const int r0 = (int) (dest % mixRingSize);
                    const int s1 = juce::jmin (count, mixRingSize - r0);
                    const int s2 = count - s1;
                    for (int ch = 0; ch < 2; ++ch)
                    {
                        mixRing.addFrom (ch, r0, block, ch, 0, s1, 1.0f);
                        if (s2 > 0)
                            mixRing.addFrom (ch, 0, block, ch, s1, s2, 1.0f);
                    }
                }
            }
            if (options.writeMix)
            {
                const juce::int64 readyEnd = juce::jmin (mixTotalInput, start + n - totalLatency);
                while (mixFlushed < readyEnd)
                {
                    const int r0 = (int) (mixFlushed % mixRingSize);
                    const int chunk = (int) juce::jmin (readyEnd - mixFlushed, (juce::int64) (mixRingSize - r0), (juce::int64) kBlockSize);
                    const juce::int64 outStart = mixFlushed;
                    const juce::int64 outEnd = mixFlushed + chunk;
                    const juce::int64 keepFrom = juce::jmax (outStart, (juce::int64) masterLat);
                    const juce::int64 keepTo = juce::jmin (outEnd, (juce::int64) masterLat + stemLength);
                    const int keepCount = keepTo > keepFrom ? (int) (keepTo - keepFrom) : 0;
                    const int keepOffset = keepCount > 0 ? (int) (keepFrom - outStart) : 0;

                    if (hasMasterPlugin)
                    {
                        for (int ch = 0; ch < masterChannels; ++ch)
                        {
                            if (ch < 2) masterBlock.copyFrom (ch, 0, mixRing, ch, r0, chunk);
                            else masterBlock.clear (ch, 0, chunk);
                        }
                        mixRing.clear (r0, chunk);
                        juce::AudioBuffer<float> masterView (masterBlock.getArrayOfWritePointers(), masterChannels, chunk);
                        midiScratch.clear();
                        for (auto& s : masterChain)
                            processSlot (*s, masterView, midiScratch);
                        if (keepCount > 0)
                        {
                            for (int ch = 0; ch < 2; ++ch)
                            {
                                masterBlock.applyGain (ch, keepOffset, keepCount, masterGain);
                                auto* dst = masterBlock.getWritePointer (ch, keepOffset);
                                for (int i = 0; i < keepCount; ++i)
                                    if (! std::isfinite (dst[i]))
                                        dst[i] = 0.0f;
                                const float mag = masterBlock.getMagnitude (ch, keepOffset, keepCount);
                                if (std::isfinite (mag))
                                    mixPeak = juce::jmax (mixPeak, mag);
                            }
                            juce::AudioBuffer<float> stereoOut (masterBlock.getArrayOfWritePointers(), 2, keepOffset + keepCount);
                            if (! mixWriter->writeFromAudioSampleBuffer (stereoOut, keepOffset, keepCount))
                            {
                                error = "Writing mix.wav failed.";
                                discardRun();
                                return false;
                            }
                        }
                    }
                    else
                    {
                        if (keepCount > 0)
                        {
                            for (int ch = 0; ch < 2; ++ch)
                            {
                                mixRing.applyGain (ch, r0 + keepOffset, keepCount, masterGain);
                                auto* dst = mixRing.getWritePointer (ch, r0 + keepOffset);
                                for (int i = 0; i < keepCount; ++i)
                                    if (! std::isfinite (dst[i]))
                                        dst[i] = 0.0f;
                                const float mag = mixRing.getMagnitude (ch, r0 + keepOffset, keepCount);
                                if (std::isfinite (mag))
                                    mixPeak = juce::jmax (mixPeak, mag);
                            }
                            if (! mixWriter->writeFromAudioSampleBuffer (mixRing, r0 + keepOffset, keepCount))
                            {
                                error = "Writing mix.wav failed.";
                                discardRun();
                                return false;
                            }
                        }
                        mixRing.clear (r0, chunk);
                    }
                    mixFlushed += chunk;
                }
            }
            if (progress && ! progress ((float) ((double) (start + n) / (double) end)))
            {
                // a cancelled render leaves no stem of its run behind: the files it wrote are closed and deleted, and the caller says nothing
                if (options.writeStems)
                    for (auto& o : outs) { o.writer.reset(); o.file.deleteFile(); }
                if (options.writeMix)
                {
                    mixWriter.reset();
                    mixFile.deleteFile();
                }
                result.stopped = true;
                error = "Stopped.";
                return false;
            }
        }
        for (auto& o : outs)
        {
            o.writer.reset();
            if (options.writeStems)
                result.stems.push_back ({ o.track->name, o.file.getFullPathName(), o.peak > 0.0f ? (double) juce::Decibels::gainToDecibels (o.peak) : -200.0 });
        }
        if (options.writeMix)
        {
            mixWriter.reset();
            result.mixFile = mixFile.getFullPathName();
            result.mixPeakDb = (double) juce::Decibels::gainToDecibels (mixPeak, -200.0f);
        }
        result.seconds = (double) stemLength / sampleRate;
        return true;
    }

    // ---- the project file -----------------------------------------------------------------------------------------------
    // Every slot's state is read from its plugin at this moment. A clip keeps its file even when the audio is missing.
    std::unique_ptr<juce::XmlElement> toXml (const juce::File& relativeTo) const
    {
        auto xml = std::make_unique<juce::XmlElement> ("OlympusProject");
        xml->setAttribute ("version", kProjectVersion);
        xml->setAttribute ("name", name);
        xml->setAttribute ("sampleRate", sampleRate);
        xml->setAttribute ("masterDb", (double) masterDb);
        xml->setAttribute ("loopStart", juce::String (loopStart));
        xml->setAttribute ("loopEnd", juce::String (loopEnd));
        xml->setAttribute ("loopOn", loopOn);
        xml->setAttribute ("metronome", metronomeOn);
        xml->setAttribute ("snap", snapOn);
        xml->setAttribute ("grid", (int) grid);
        xml->setAttribute ("tsNum", tempo.numerator);
        xml->setAttribute ("tsDen", tempo.denominator);
        for (auto& p : tempo.points)
        {
            auto* e = xml->createNewChildElement ("Tempo");
            e->setAttribute ("beat", p.beat);
            e->setAttribute ("bpm", p.bpm);
        }
        for (auto& m : markers)
        {
            auto* e = xml->createNewChildElement ("Marker");
            e->setAttribute ("name", m.name);
            e->setAttribute ("sample", juce::String (m.start));
        }
        for (auto& t : tracks)
        {
            auto* e = xml->createNewChildElement ("Track");
            e->setAttribute ("name", t->name);
            e->setAttribute ("kind", t->isBus() ? "bus" : (t->kind == TrackKind::Midi ? "midi" : "audio"));
            e->setAttribute ("outputTrack", t->outputTrack);
            e->setAttribute ("pan", (double) t->pan);
            e->setAttribute ("gainDb", (double) t->gainDb);
            e->setAttribute ("mute", t->mute);
            e->setAttribute ("solo", t->solo);
            e->setAttribute ("armed", t->armed);
            e->setAttribute ("inputLeft", t->inputLeft);
            e->setAttribute ("inputRight", t->inputRight);
            e->setAttribute ("isMidi", t->isMidi);
            e->setAttribute ("showAutomation", t->showAutomation);
            e->setAttribute ("activeAutoTarget", t->activeAutoTarget);
            e->setAttribute ("frozen", t->frozen);
            if (t->frozen && t->frozenFile != juce::File())
            {
                e->setAttribute ("frozenFile", relativeTo != juce::File()
                                                   ? t->frozenFile.getRelativePathFrom (relativeTo.getParentDirectory())
                                                   : t->frozenFile.getFullPathName());
                e->setAttribute ("frozenPath", t->frozenFile.getFullPathName());
            }
            for (const auto& snd : t->sends)
            {
                auto* se = e->createNewChildElement ("Send");
                se->setAttribute ("targetTrack", snd.targetTrack);
                se->setAttribute ("levelDb", (double) snd.levelDb);
                se->setAttribute ("preFader", snd.preFader);
                se->setAttribute ("mute", snd.mute);
            }
            for (auto& c : t->clips)
            {
                auto* k = e->createNewChildElement ("Clip");
                k->setAttribute ("name", c.name);
                k->setAttribute ("file", c.file != juce::File() && relativeTo != juce::File()
                                              ? c.file.getRelativePathFrom (relativeTo.getParentDirectory())
                                              : c.file.getFullPathName());
                k->setAttribute ("path", c.file.getFullPathName());
                k->setAttribute ("start", juce::String (c.start));
                k->setAttribute ("offset", juce::String (c.offset));
                k->setAttribute ("length", juce::String (c.length));
                k->setAttribute ("gainDb", (double) c.gainDb);
                k->setAttribute ("fadeIn", juce::String (c.fadeIn));
                k->setAttribute ("fadeOut", juce::String (c.fadeOut));
                k->setAttribute ("mute", c.mute);
            }
            for (auto& mc : t->midiClips)
            {
                auto* k = e->createNewChildElement ("MidiClip");
                k->setAttribute ("name", mc.name);
                k->setAttribute ("start", juce::String (mc.start));
                k->setAttribute ("length", juce::String (mc.length));
                k->setAttribute ("mute", mc.mute);
                for (auto& note : mc.notes)
                {
                    auto* ne = k->createNewChildElement ("Note");
                    ne->setAttribute ("startBeat", note.startBeat);
                    ne->setAttribute ("durationBeats", note.durationBeats);
                    ne->setAttribute ("pitch", note.pitch);
                    ne->setAttribute ("velocity", note.velocity);
                    ne->setAttribute ("channel", note.channel);
                }
            }
            if (t->instrument != nullptr)
                writeSlotXml (*e, "Instrument", *t->instrument);
            for (auto& s : t->chain)
                writeSlotXml (*e, "Slot", *s);
            for (const auto& lane : t->automation)
            {
                auto* ae = e->createNewChildElement ("AutomationLane");
                ae->setAttribute ("targetId", lane.targetId);
                ae->setAttribute ("slotIndex", lane.slotIndex);
                ae->setAttribute ("paramIndex", lane.paramIndex);
                ae->setAttribute ("enabled", lane.enabled);
                for (const auto& pt : lane.points)
                {
                    auto* pe = ae->createNewChildElement ("Point");
                    pe->setAttribute ("sample", juce::String (pt.sample));
                    pe->setAttribute ("value", (double) pt.value);
                    pe->setAttribute ("curve", (int) pt.curve);
                }
            }
        }
        auto* mc = xml->createNewChildElement ("MasterChain");
        for (auto& s : masterChain)
            writeSlotXml (*mc, "Plugin", *s);
        return xml;
    }

    bool save (const juce::File& file, juce::String& error)
    {
        const auto nextName = file.getFileNameWithoutExtension(); // the project is named after its file
        std::unique_ptr<juce::XmlElement> xml;
        {
            // every plugin's state is read here: not while processBlock runs beside it
            const juce::ScopedLock sl (lock);
            const auto previousName = name;
            name = nextName;
            xml = toXml (file);
            name = previousName;
        }
        file.getParentDirectory().createDirectory();
        juce::TemporaryFile temp (file);
        if (! xml->writeTo (temp.getFile()) || ! temp.overwriteTargetFileWithTemporary())
        {
            error = "Cannot write " + file.getFullPathName();
            return false;
        }
        const juce::ScopedLock sl (lock);
        name = nextName;
        projectFile = file;
        dirty = false;
        bumpRevision();
        return true;
    }

    // Writes the project to `file` and leaves the session as it is: the name, the project file and the dirty flag do not change (the
    // autosave, Olympus' MainComponent). Holds the lock while the XML is made, like save(); the file is written beside and moved over.
    bool saveCopy (const juce::File& file, juce::String& error)
    {
        std::unique_ptr<juce::XmlElement> xml;
        {
            const juce::ScopedLock sl (lock);
            xml = toXml (file);
        }
        file.getParentDirectory().createDirectory();
        juce::TemporaryFile temp (file);
        if (! xml->writeTo (temp.getFile()) || ! temp.overwriteTargetFileWithTemporary())
        {
            error = "Cannot write " + file.getFullPathName();
            return false;
        }
        return true;
    }

    // Reads a project. A clip whose audio is gone stays (silent, with its length) and a plugin that is not installed stays (passed
    // through), both with their settings, so saving again loses nothing; each is a line in `warnings`. A version 1 track (one file, no
    // clips) becomes one clip at sample 0 covering the whole file.
    bool load (const juce::File& file, juce::String& error)
    {
        const auto xml = juce::parseXML (file);
        if (xml == nullptr || ! xml->hasTagName ("OlympusProject")) { error = file.getFileName() + " is not an Olympus project."; return false; }
        if (xml->getIntAttribute ("version", 1) > kProjectVersion) { error = "This project is from a newer Olympus."; return false; }

        pool.clear(); // the rate of the project may differ from the one the pool was read at
        const auto savedRateText = xml->getStringAttribute ("sampleRate");
        const double savedRate = xml->getDoubleAttribute ("sampleRate", 0.0);
        // a saved 0 is the rate the app itself does not know yet and stays unknown in silence, and so does no attribute at all; anything
        // else that is not a rate in the band (a NaN, a negative, one no device runs at, or text that is not a number and reads as 0.0)
        // becomes unknown too, and is said with the text as it was saved. An unknown rate is set by the device or by a file.
        const bool rateIsUsable = std::isfinite (savedRate) && savedRate >= kMinValidRate && savedRate <= kMaxValidRate;
        const double initialRate = rateIsUsable ? savedRate : 0.0;
        juce::String initialProblem;
        if (! (initialRate > 0.0) && savedRateText.isNotEmpty() && savedRateText != "0")
            initialProblem = "The saved sample rate '" + savedRateText + "' is not a rate Olympus can use, so it is unknown until the device sets one.";

        auto parsed = parseXml (*xml, file.getParentDirectory(), initialRate, initialProblem);

        const juce::ScopedLock sl (lock);
        clearProjectPlayheadsLocked();
        tracks = std::move (parsed.tracks);
        masterChain = std::move (parsed.masterChain);
        setSampleRateLocked (parsed.rate);
        attachProjectPlayheadsLocked();
        tempo = parsed.tempo;
        if (sampleRate > 0.0) tempo.sampleRate = sampleRate;
        markers = std::move (parsed.markers);
        masterDb = parsed.masterDb;
        loopStart = parsed.loopStart;
        loopEnd = parsed.loopEnd;
        loopOn = parsed.loopOn;
        metronomeOn = parsed.metronomeOn;
        snapOn = parsed.snapOn;
        grid = parsed.grid;
        projectFile = file;
        name = xml->getStringAttribute ("name", file.getFileNameWithoutExtension());
        warnings = parsed.problems;
        const double finalRate = sampleRate > 0.0 ? sampleRate : 48000.0;
        const bool needsPrepare = std::any_of (parsed.slotPreparedRates.begin(), parsed.slotPreparedRates.end(),
                                               [finalRate] (double r) { return std::abs (r - finalRate) > 0.5; });
        if (needsPrepare)
            prepareLocked (false);
        else
        {
            for (auto& t : tracks)
                makeTrackLocked (*t);
            monitorScratch.setSize (kMaxMonitorChannels, kBlockSize, false, true, false);
            updateLatenciesLocked();
            clearDelayRingsLocked();
        }
        publishPlaybackSnapshotLocked();
        dirty = false;
        bumpRevision();
        return true;
    }

    // Restores tracks, master chain and timeline state from an XML snapshot (structural undo/redo) without touching projectFile, monitor or undo.
    bool restoreEditXml (const juce::XmlElement& xml, juce::StringArray* warningsOut = nullptr)
    {
        if (! xml.hasTagName ("OlympusProject"))
            return false;
        if (xml.getIntAttribute ("version", 1) > kProjectVersion)
            return false;

        juce::File baseDir;
        double currentRate = 0.0;
        {
            const juce::ScopedLock sl (lock);
            baseDir = projectFile != juce::File() ? projectFile.getParentDirectory() : juce::File();
            currentRate = sampleRate;
        }
        const double xmlRate = xml.getDoubleAttribute ("sampleRate", 0.0);
        const double usableXmlRate = (std::isfinite (xmlRate) && xmlRate >= kMinValidRate && xmlRate <= kMaxValidRate) ? xmlRate : 0.0;
        const double parseRate = currentRate > 0.0 ? currentRate : usableXmlRate;

        auto parsed = parseXml (xml, baseDir, parseRate);

        const juce::ScopedLock sl (lock);
        clearProjectPlayheadsLocked();
        tracks = std::move (parsed.tracks);
        masterChain = std::move (parsed.masterChain);
        attachProjectPlayheadsLocked();
        masterDb = parsed.masterDb;
        tempo = parsed.tempo;
        if (sampleRate > 0.0)
            tempo.sampleRate = sampleRate;
        markers = std::move (parsed.markers);
        loopStart = parsed.loopStart;
        loopEnd = parsed.loopEnd;
        loopOn = parsed.loopOn;
        metronomeOn = parsed.metronomeOn;
        grid = parsed.grid;
        snapOn = parsed.snapOn;
        if (warningsOut != nullptr)
            *warningsOut = parsed.problems;

        prepareLocked (offline);
        updateLatenciesLocked();
        publishPlaybackSnapshotLocked();
        dirty = true;
        bumpRevision();
        return true;
    }

    // The settings of one plugin as it is now.
    juce::MemoryBlock stateOf (const Slot& s) const
    {
        juce::MemoryBlock mb;
        if (s.instance != nullptr) s.instance->getStateInformation (mb);
        else mb = s.savedState;
        return mb;
    }

    // Everything an edit can change (tracks' clips and mix, master, timeline), copied out for undo.
    EditState captureEdit() const
    {
        const juce::ScopedLock sl (lock);
        EditState e;
        for (auto& t : tracks)
        {
            e.clips.push_back (t->clips);
            e.midiClips.push_back (t->midiClips);
            e.automation.push_back (t->automation);
            EditState::Mix mix;
            mix.name = t->name;
            mix.gainDb = t->gainDb;
            mix.pan = t->pan;
            mix.mute = t->mute;
            mix.solo = t->solo;
            e.mix.push_back (mix);
        }
        e.masterDb = masterDb;
        e.tempo = tempo;
        e.markers = markers;
        e.loopStart = loopStart;
        e.loopEnd = loopEnd;
        e.loopOn = loopOn;
        e.metronomeOn = metronomeOn;
        return e;
    }

    // Puts an edit back. Ignored when the number of tracks is not the one the edit was taken with (tracks are not undone).
    void restoreEdit (const EditState& e)
    {
        const juce::ScopedLock sl (lock);
        if (e.clips.size() != tracks.size() || e.midiClips.size() != tracks.size() || e.mix.size() != tracks.size())
            return;
        for (size_t i = 0; i < tracks.size(); ++i)
        {
            auto& t = *tracks[i];
            t.clips = e.clips[i];
            t.midiClips = e.midiClips[i];
            if (e.automation.size() == tracks.size())
                t.automation = e.automation[i];
            t.name = e.mix[i].name;
            t.gainDb = e.mix[i].gainDb;
            t.pan = e.mix[i].pan;
            t.mute = e.mix[i].mute;
            t.solo = e.mix[i].solo;
        }
        masterDb = e.masterDb;
        const double rate = tempo.sampleRate;
        tempo = e.tempo;
        tempo.sampleRate = sampleRate > 0.0 ? sampleRate : rate;
        markers = e.markers;
        loopStart = e.loopStart;
        loopEnd = e.loopEnd;
        loopOn = e.loopOn;
        metronomeOn = e.metronomeOn;
        publishPlaybackSnapshotLocked();
        dirty = true;
        bumpRevision();
    }

    // An empty session under a name: no tracks, no audio pool, the timeline at its defaults. Plugins stop listening to the playhead.
    void newProject (const juce::String& projectName)
    {
        const juce::ScopedLock sl (lock);
        for (auto& t : tracks)
        {
            if (t->instrument != nullptr && t->instrument->instance != nullptr)
                t->instrument->instance->setPlayHead (nullptr);
            for (auto& s : t->chain)
                if (s->instance != nullptr) s->instance->setPlayHead (nullptr);
        }
        tracks.clear();
        for (auto& s : masterChain)
            if (s->instance != nullptr) s->instance->setPlayHead (nullptr);
        masterChain.clear();
        pool.clear();
        setSampleRateLocked (0.0);
        tempo = TempoMap();
        masterDb = 0.0f;
        markers.clear();
        loopStart = loopEnd = 0;
        loopOn = false;
        metronomeOn = false;
        snapOn = true;
        grid = Grid::Quarter;
        projectFile = juce::File();
        name = projectName.isNotEmpty() ? projectName : "Untitled";
        publishPlaybackSnapshotLocked();
        dirty = false;
        warnings.clear();
        bumpRevision();
    }

    // What the session holds, as the MCP server and the tests read it.
    juce::var describe() const
    {
        const auto seconds = [this] (juce::int64 samples) { return sampleRate > 0.0 ? (double) samples / sampleRate : 0.0; };
        auto* root = new juce::DynamicObject();
        root->setProperty ("name", name);
        root->setProperty ("project_file", projectFile.getFullPathName());
        root->setProperty ("sample_rate", sampleRate);
        root->setProperty ("unsaved_changes", dirty);
        root->setProperty ("master_db", (double) masterDb);

        juce::Array<juce::var> tempoList;
        for (auto& p : tempo.points)
        {
            auto* o = new juce::DynamicObject();
            o->setProperty ("beat", p.beat);
            o->setProperty ("bpm", p.bpm);
            tempoList.add (juce::var (o));
        }
        root->setProperty ("tempo", tempoList);
        root->setProperty ("time_signature", juce::String (tempo.numerator) + "/" + juce::String (tempo.denominator));

        juce::Array<juce::var> markerList;
        int m = 0;
        for (auto& mk : markers)
        {
            auto* o = new juce::DynamicObject();
            o->setProperty ("index", m++);
            o->setProperty ("name", mk.name);
            o->setProperty ("sample", juce::String (mk.start));
            o->setProperty ("seconds", seconds (mk.start));
            markerList.add (juce::var (o));
        }
        root->setProperty ("markers", markerList);

        auto* loop = new juce::DynamicObject();
        loop->setProperty ("start_seconds", seconds (loopStart));
        loop->setProperty ("end_seconds", seconds (loopEnd));
        loop->setProperty ("on", loopOn);
        root->setProperty ("loop", juce::var (loop));
        root->setProperty ("metronome", metronomeOn);
        root->setProperty ("snap", snapOn);
        root->setProperty ("grid", juce::String (gridName (grid)));

        juce::Array<juce::var> list;
        int i = 0;
        for (auto& t : tracks)
        {
            auto* o = new juce::DynamicObject();
            o->setProperty ("index", i++);
            o->setProperty ("name", t->name);
            o->setProperty ("kind", t->isBus() ? "bus" : (t->kind == TrackKind::Midi ? "midi" : "audio"));
            o->setProperty ("outputTrack", t->outputTrack);
            o->setProperty ("is_midi", t->isMidi);
            o->setProperty ("audio_found", ! t->clips.empty() && std::all_of (t->clips.begin(), t->clips.end(), [] (const Clip& c) { return c.audio != nullptr; }));
            o->setProperty ("seconds", seconds (t->length()));
            o->setProperty ("pan", (double) t->pan);
            o->setProperty ("gain_db", (double) t->gainDb);
            o->setProperty ("mute", t->mute);
            o->setProperty ("solo", t->solo);
            o->setProperty ("latency_samples", t->latency());
            o->setProperty ("frozen", t->frozen);
            o->setProperty ("armed", t->armed);
            o->setProperty ("input_left", t->inputLeft);
            o->setProperty ("input_right", t->inputRight);

            juce::Array<juce::var> sendsList;
            for (int sIdx = 0; sIdx < (int) t->sends.size(); ++sIdx)
            {
                const auto& snd = t->sends[(size_t) sIdx];
                auto* so = new juce::DynamicObject();
                so->setProperty ("index", sIdx);
                so->setProperty ("targetTrack", snd.targetTrack);
                so->setProperty ("levelDb", (double) snd.levelDb);
                so->setProperty ("preFader", snd.preFader);
                so->setProperty ("mute", snd.mute);
                sendsList.add (juce::var (so));
            }
            o->setProperty ("sends", sendsList);

            juce::Array<juce::var> clips;
            int k = 0;
            for (auto& c : t->clips)
            {
                auto* co = new juce::DynamicObject();
                co->setProperty ("index", k++);
                co->setProperty ("name", c.name);
                co->setProperty ("file", c.file.getFullPathName());
                co->setProperty ("start_seconds", seconds (c.start));
                co->setProperty ("offset_seconds", seconds (c.offset));
                co->setProperty ("length_seconds", seconds (c.length));
                co->setProperty ("gain_db", (double) c.gainDb);
                co->setProperty ("fade_in_seconds", seconds (c.fadeIn));
                co->setProperty ("fade_out_seconds", seconds (c.fadeOut));
                co->setProperty ("mute", c.mute);
                clips.add (juce::var (co));
            }
            o->setProperty ("clips", clips);

            juce::Array<juce::var> midiClips;
            int mkIdx = 0;
            for (auto& mc : t->midiClips)
            {
                auto* mco = new juce::DynamicObject();
                mco->setProperty ("index", mkIdx++);
                mco->setProperty ("name", mc.name);
                mco->setProperty ("start_seconds", seconds (mc.start));
                mco->setProperty ("length_seconds", seconds (mc.length));
                mco->setProperty ("mute", mc.mute);
                mco->setProperty ("note_count", (int) mc.notes.size());
                midiClips.add (juce::var (mco));
            }
            o->setProperty ("midi_clips", midiClips);

            if (t->instrument != nullptr)
            {
                auto* inst = new juce::DynamicObject();
                inst->setProperty ("plugin", t->instrument->plugin);
                inst->setProperty ("bypass", t->instrument->bypass);
                inst->setProperty ("missing", t->instrument->instance == nullptr);
                inst->setProperty ("latencySamples", t->instrument->latency());
                inst->setProperty ("maker", t->instrument->instance != nullptr
                                                ? t->instrument->instance->getPluginDescription().manufacturerName
                                                : juce::String());
                if (t->instrument->instance == nullptr)
                    inst->setProperty ("reason", slotProblem (*t->instrument));
                o->setProperty ("instrument", juce::var (inst));
            }

            juce::Array<juce::var> chain;
            int j = 0;
            for (auto& s : t->chain)
            {
                auto* c = new juce::DynamicObject();
                c->setProperty ("index", j++);
                c->setProperty ("plugin", s->plugin);
                c->setProperty ("installed", s->instance != nullptr);
                if (s->instance == nullptr) c->setProperty ("reason", slotProblem (*s));
                c->setProperty ("bypass", s->bypass);
                c->setProperty ("wet", (double) s->wet);
                c->setProperty ("sidechainTrack", s->sidechainTrack);
                chain.add (juce::var (c));
            }
            o->setProperty ("chain", chain);
            list.add (juce::var (o));
        }
        root->setProperty ("tracks", list);

        juce::Array<juce::var> masterList;
        int masterIdx = 0;
        for (auto& s : masterChain)
        {
            auto* entry = new juce::DynamicObject();
            entry->setProperty ("index", masterIdx++);
            entry->setProperty ("name", s->plugin);
            entry->setProperty ("maker", s->instance != nullptr ? s->instance->getPluginDescription().manufacturerName : juce::String());
            entry->setProperty ("bypass", s->bypass);
            entry->setProperty ("wet", (double) s->wet);
            entry->setProperty ("latencySamples", s->latency());
            entry->setProperty ("loaded", s->instance != nullptr);
            if (s->instance == nullptr) entry->setProperty ("reason", slotProblem (*s));
            masterList.add (juce::var (entry));
        }
        root->setProperty ("masterChain", masterList);

        // the monitor chain: what the headphones hear, not part of the project
        juce::Array<juce::var> monitorList;
        for (auto& s : monitor)
        {
            auto* entry = new juce::DynamicObject();
            entry->setProperty ("plugin", s->plugin);
            entry->setProperty ("bypass", s->bypass);
            entry->setProperty ("wet", (double) s->wet);
            entry->setProperty ("latency_samples", s->latency());
            entry->setProperty ("installed", s->instance != nullptr);
            if (s->instance == nullptr) entry->setProperty ("reason", slotProblem (*s));
            monitorList.add (juce::var (entry));
        }
        root->setProperty ("monitor", monitorList);
        root->setProperty ("monitor_bypass", monitorBypass.load());
        return juce::var (root);
    }

    // Why a slot holds no instance, for anything that describes it: a plugin the catalog here does have was refused by the stereo check
    // with the settings it saved; one the catalog does not have is not installed. catalog.has is the very lookup create() used, so the
    // two can never disagree about what is installed.
    juce::String slotProblem (const Slot& s) const
    {
        return catalog.has (s.plugin) ? "cannot run in stereo with its saved settings" : "not installed on this machine";
    }

    // Makes a slot for a plugin. With `state`, the plugin is given those settings. Null, with the reason, if the plugin cannot be made.
    std::unique_ptr<Slot> makeSlot (const juce::String& pluginName, const juce::MemoryBlock* state, juce::String& error, double rate = 0.0, bool allowSidechain = true)
    {
        auto instance = catalog.create (pluginName, rate > 0.0 ? rate : (sampleRate > 0.0 ? sampleRate : 48000.0), kBlockSize, error);
        if (instance == nullptr)
        {
            if (error.isEmpty()) error = "Cannot create " + pluginName;
            return nullptr;
        }
        const double useRate = rate > 0.0 ? rate : (sampleRate > 0.0 ? sampleRate : 48000.0);
        // the state goes on before the plugin is set up: a state that changes its buses must be there when the stereo check runs
        if (state != nullptr && state->getSize() > 0)
            instance->setStateInformation (state->getData(), (int) state->getSize());
        const juce::String supports = layoutWords (*instance); // before configure: a refused layout leaves the buses empty
        if (! configure (*instance, useRate, false, allowSidechain))
        {
            instance->releaseResources(); // configure set it up and this drops it: as everywhere else, a prepared instance goes released, not straight destroyed
            error = pluginName + " cannot run on a stereo track (it supports " + supports + ")";
            return nullptr;
        }
        auto slot = std::make_unique<Slot>();
        slot->plugin = pluginName;
        slot->dryScratch.setSize (2, kBlockSize, false, true, false);
        instance->setPlayHead (&head);
        slot->instance = std::move (instance);
        slot->watch = &latencyWatch;
        slot->instance->addListener (&latencyWatch);
        return slot;
    }

    // Message thread (the window's timer): true once after a plugin reported a change of its latency. The caller then calls updateLatencies(),
    // so the tracks' delays and the take compensation follow the new latency.
    bool takeLatencyChange() { return latencyWatch.changed.exchange (false); }

    // The playhead the plugins read (a plugin that listens to the transport sees this one). The audio thread sets it in processBlock, so
    // a plugin never reads the tempo map itself.
    struct Head : juce::AudioPlayHead
    {
        juce::Optional<PositionInfo> getPosition() const override
        {
            PositionInfo info;
            info.setTimeInSamples (position.load());
            info.setIsPlaying (playing.load());
            info.setBpm (bpm.load());
            TimeSignature signature;
            signature.numerator = numerator.load();
            signature.denominator = denominator.load();
            info.setTimeSignature (signature);
            info.setPpqPosition (ppq.load());
            info.setIsLooping (looping.load());
            if (looping.load())
            {
                LoopPoints points;
                points.ppqStart = loopStart.load();
                points.ppqEnd = loopEnd.load();
                info.setLoopPoints (points);
            }
            return info;
        }
        void set (juce::int64 p, bool isPlaying) { position = p; playing = isPlaying; }
        void set (juce::int64 p, bool isPlaying, double bpmNow, double ppqNow) { set (p, isPlaying); bpm = bpmNow; ppq = ppqNow; }
        void setTimeSignature (int num, int den) { numerator = num; denominator = den; }
        void setIsLooping (bool on) { looping = on; }
        void setLoopPoints (double startPpq, double endPpq) { loopStart = startPpq; loopEnd = endPpq; }

        std::atomic<juce::int64> position { 0 };
        std::atomic<bool> playing { false }, looping { false };
        std::atomic<double> bpm { 120.0 }, ppq { 0.0 }, loopStart { 0.0 }, loopEnd { 0.0 };
        std::atomic<int> numerator { 4 }, denominator { 4 };
    };
    Head head;

    void updateLatencies() { const juce::ScopedLock sl (lock); updateLatenciesLocked(); }

    // Message thread, under the lock: every missing plugin of the tracks and of the monitor chain gets another try, since a scan may have found
    // it since. One that now loads takes its slot and is given the saved state the slot kept. Returns how many came back.
    int retryMissing()
    {
        const juce::ScopedLock sl (lock);
        int back = 0;
        const auto retry = [this] (Slot& slot, bool allowSidechain, int& count)
        {
            if (slot.instance != nullptr)
                return;
            juce::String error;
            auto fresh = makeSlot (slot.plugin, &slot.savedState, error, 0.0, allowSidechain);
            if (fresh == nullptr)
                return;
            slot.instance = std::move (fresh->instance);   // fresh keeps no instance, so its destructor removes nothing
            slot.watch = fresh->watch;
            ++count;
        };
        for (auto& t : tracks)
        {
            if (t->instrument != nullptr)
                retry (*t->instrument, true, back);
            for (auto& s : t->chain)
                retry (*s, true, back);
        }
        for (auto& s : masterChain)
            retry (*s, false, back);
        for (auto& s : monitor)
            retry (*s, false, back);
        if (back > 0)
        {
            updateLatenciesLocked();
            publishPlaybackSnapshotLocked();
        }
        return back;
    }

    bool hasMissingAudio() const
    {
        const juce::ScopedLock sl (lock);
        for (const auto& t : tracks)
            for (const auto& c : t->clips)
                if (c.audio == nullptr)
                    return true;
        return false;
    }

    static juce::File findAudioFileByName (const juce::String& fileName, const juce::File& searchRoot, int maxDepth = 3)
    {
        if (fileName.isEmpty() || ! searchRoot.isDirectory() || maxDepth < 0)
            return {};
        for (const auto& f : searchRoot.findChildFiles (juce::File::findFiles, false))
        {
            if (f.existsAsFile() && f.getFileName() == fileName)
                return f;
        }
        if (maxDepth > 0)
        {
            for (const auto& d : searchRoot.findChildFiles (juce::File::findDirectories, false))
            {
                if (d.getFileName().startsWithChar ('.'))
                    continue;
                auto found = findAudioFileByName (fileName, d, maxDepth - 1);
                if (found != juce::File())
                    return found;
            }
        }
        return {};
    }

    std::shared_ptr<const juce::AudioBuffer<float>> loadPooledLocked (const juce::File& file, juce::String& error)
    {
        double targetRate = sampleRate > 0.0 ? sampleRate : 0.0;
        auto buf = pooledAudio (file, targetRate, error);
        if (buf != nullptr && sampleRate <= 0.0 && targetRate > 0.0)
        {
            setSampleRateLocked (targetRate);
            prepareLocked (false);
        }
        return buf;
    }

    void finishRelinkLocked()
    {
        updateLatenciesLocked();
    }

    static void relinkClipLocked (Clip& clip, const juce::File& newFile, std::shared_ptr<const juce::AudioBuffer<float>> buf)
    {
        if (buf == nullptr)
            return;
        const juce::int64 source = (juce::int64) buf->getNumSamples();
        clip.file = newFile;
        clip.audio = std::move (buf);
        clip.offset = juce::jlimit ((juce::int64) 0, source, clip.offset);
        clip.length = juce::jlimit ((juce::int64) 0, source - clip.offset, clip.length);
        clip.fadeIn = juce::jlimit ((juce::int64) 0, clip.length, clip.fadeIn);
        clip.fadeOut = juce::jmin (juce::jlimit ((juce::int64) 0, clip.length, clip.fadeOut), clip.length - clip.fadeIn);
    }

    int rescanMissingAudioLocked (const juce::File& searchRoot, juce::String& error)
    {
        if (! searchRoot.isDirectory())
        {
            error = "Folder does not exist.";
            return 0;
        }
        int relinked = 0;
        std::map<juce::String, juce::File> foundByName;
        for (auto& t : tracks)
        {
            for (auto& c : t->clips)
            {
                if (c.audio != nullptr)
                    continue;
                const auto fileName = c.file.getFileName();
                if (fileName.isEmpty())
                    continue;
                auto it = foundByName.find (fileName);
                if (it == foundByName.end())
                    it = foundByName.emplace (fileName, findAudioFileByName (fileName, searchRoot)).first;
                if (it->second == juce::File())
                    continue;
                juce::String loadErr;
                auto buf = loadPooledLocked (it->second, loadErr);
                if (buf == nullptr)
                {
                    if (error.isEmpty())
                        error = loadErr;
                    continue;
                }
                relinkClipLocked (c, it->second, std::move (buf));
                ++relinked;
            }
        }
        if (relinked > 0)
        {
            updateLatenciesLocked();
            publishPlaybackSnapshotLocked();
            bumpRevision();
        }
        return relinked;
    }

    int rescanMissingAudio (const juce::File& searchRoot, juce::String& error)
    {
        const juce::ScopedLock sl (lock);
        const int relinked = rescanMissingAudioLocked (searchRoot, error);
        if (relinked > 0)
            pruneResolvedAudioWarningsLocked();
        return relinked;
    }

    void pruneResolvedAudioWarningsLocked()
    {
        juce::StringArray stillMissingNames;
        for (const auto& t : tracks)
            for (const auto& c : t->clips)
                if (c.audio == nullptr && c.file.getFileName().isNotEmpty())
                    stillMissingNames.addIfNotAlreadyThere (c.file.getFileName());

        static const char* const kAudioErrorSuffixes[] = {
            " contains no audio.",
            " has an invalid sample rate.",
            " is too long to load.",
            " is longer than two hours.",
            " is truncated or corrupt.",
            " is too long to resample."
        };
        static const juce::String kCannotReadPrefix ("Cannot read the audio file ");

        for (int i = warnings.size() - 1; i >= 0; --i)
        {
            const auto& w = warnings[i];
            if (! w.startsWith ("Track '"))
                continue;
            const int sep = w.indexOf (7, "': ");
            if (sep < 0)
                continue;
            const auto problem = w.substring (sep + 3);
            juce::String warningFileName;
            if (problem.startsWith (kCannotReadPrefix))
            {
                const auto path = problem.substring (kCannotReadPrefix.length());
                warningFileName = path.fromLastOccurrenceOf ("/", false, false).fromLastOccurrenceOf ("\\", false, false);
            }
            else
            {
                for (const char* suffix : kAudioErrorSuffixes)
                {
                    if (problem.endsWith (suffix))
                    {
                        warningFileName = problem.dropLastCharacters ((int) std::strlen (suffix));
                        break;
                    }
                }
            }
            if (warningFileName.isEmpty())
                continue;
            if (! stillMissingNames.contains (warningFileName))
                warnings.remove (i);
        }
    }

private:
    juce::AudioFormatManager formats;
    juce::AudioBuffer<float> masterScratch;  // masterChain's view of a block (channels x kBlockSize), sized here, never in the callback
    juce::AudioBuffer<float> monitorScratch; // the monitor chain's view of a block (kMaxMonitorChannels x kBlockSize), sized here, never in the callback
    juce::AudioBuffer<float> snapshotScratch;
    juce::MidiBuffer midiScratch;
    juce::MidiBuffer snapshotMidiScratch;
    juce::MidiBuffer liveMidiScratch;
    std::array<std::atomic<int>, 2> snapshotReaders {};
    std::array<BasicSynth, 64> fallbackSynths {};
    bool lastBlockWasFallback = false;
    size_t lastFallbackTrackCount = 0;
    juce::int64 lastProcessedEnd = -1;
    std::vector<int> cachedTopoOrder;
    std::vector<int> cachedInLat;
    std::vector<int> cachedOutLat;
    std::vector<bool> cachedAudibleScratch;

    // The audio pool: a file's buffer, shared by the clips that play it. Held weakly, so a buffer goes when its last clip does.
    // Message thread only; the audio thread never reads it.
    using Pool = std::map<juce::String, std::weak_ptr<const juce::AudioBuffer<float>>>;
    Pool pool;

    // The buffer of a file from the pool, or read now at `rate` (0 = the file's own rate, which is then written to `rate`).
    std::shared_ptr<const juce::AudioBuffer<float>> pooledAudio (const juce::File& file, double& rate, juce::String& error)
    {
        if (! std::isfinite (rate) || rate < 0.0)
            rate = 0.0;
        const auto key = file.getFullPathName();
        if (auto it = pool.find (key); it != pool.end())
            if (auto found = it->second.lock())
                return found;
        auto buffer = std::make_shared<juce::AudioBuffer<float>>();
        double got = 0.0;
        if (! readAudio (file, rate, *buffer, got, error))
            return nullptr;
        if (rate <= 0.0)
            rate = got;
        for (auto it = pool.begin(); it != pool.end();)
            it = it->second.expired() ? pool.erase (it) : std::next (it);
        pool[key] = buffer;
        return buffer;
    }

    // Reads a file as two channels at `wantedRate` (0 = keep the file's rate); `rateOut` is the rate of the result.
    bool readAudio (const juce::File& file, double wantedRate, juce::AudioBuffer<float>& out, double& rateOut, juce::String& error)
    {
        const juce::int64 fileSize = file.getSize();
        if (fileSize <= 0) { error = file.getFileName() + " contains no audio."; return false; }
        std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (file));
        if (reader == nullptr) { error = "Cannot read the audio file " + file.getFullPathName(); return false; }
        if (! std::isfinite (reader->sampleRate) || reader->sampleRate <= 0.0) { error = file.getFileName() + " has an invalid sample rate."; return false; }
        if (reader->numChannels == 0 || reader->lengthInSamples <= 0) { error = file.getFileName() + " contains no audio."; return false; }
        if (reader->lengthInSamples > (juce::int64) std::numeric_limits<int>::max()) { error = file.getFileName() + " is too long to load."; return false; }
        if ((double) reader->lengthInSamples > reader->sampleRate * 3600.0 * 2.0) { error = file.getFileName() + " is longer than two hours."; return false; }

        const bool isWav = reader->getFormatName() == "WAV file";
        const bool isAiff = reader->getFormatName() == "AIFF file";
        if (isWav || isAiff)
        {
            const juce::int64 minHeader = isWav ? 44 : 54;
            const juce::int64 bytesPerSample = (juce::int64) (reader->bitsPerSample / 8);
            if (reader->bitsPerSample < 8 || (reader->bitsPerSample % 8) != 0 || bytesPerSample <= 0
                || fileSize <= minHeader
                || (juce::int64) reader->numChannels > (fileSize - minHeader) / bytesPerSample)
            {
                error = file.getFileName() + " is truncated or corrupt.";
                return false;
            }
            const juce::int64 frameBytes = (juce::int64) reader->numChannels * bytesPerSample;
            if (reader->lengthInSamples > (fileSize - minHeader) / frameBytes)
            {
                error = file.getFileName() + " is truncated or corrupt.";
                return false;
            }
        }
        else if (reader->lengthInSamples / 8192 > fileSize)
        {
            error = file.getFileName() + " is truncated or corrupt.";
            return false;
        }

        const int len = (int) reader->lengthInSamples;
        juce::AudioBuffer<float> raw (2, len);
        raw.clear();
        if (! reader->read (&raw, 0, len, 0, true, true))
        {
            error = file.getFileName() + " is truncated or corrupt.";
            return false;
        }
        if (reader->numChannels == 1)
            raw.copyFrom (1, 0, raw, 0, 0, len);
        const double rate = (std::isfinite (wantedRate) && wantedRate > 0.0) ? wantedRate : reader->sampleRate;
        rateOut = rate;
        if (std::abs (rate - reader->sampleRate) < 0.5)
        {
            out = std::move (raw);
            return true;
        }
        const double resampledLen = std::ceil ((double) len * (rate / reader->sampleRate));
        if (! std::isfinite (resampledLen) || resampledLen > (double) std::numeric_limits<int>::max())
        {
            error = file.getFileName() + " is too long to resample.";
            return false;
        }
        out = resampled (raw, reader->sampleRate, rate);
        return true;
    }

    // Two channels of `raw` at `fromRate`, as two channels at `toRate` (Lagrange interpolation; the one resampler of the session).
    static juce::AudioBuffer<float> resampled (const juce::AudioBuffer<float>& raw, double fromRate, double toRate)
    {
        const int len = raw.getNumSamples();
        if (len <= 0 || ! (fromRate > 0.0) || ! (toRate > 0.0) || ! std::isfinite (fromRate) || ! std::isfinite (toRate))
            return {};
        const double ratio = fromRate / toRate;
        const double wanted = std::ceil ((double) len / ratio);
        if (! std::isfinite (wanted) || wanted > (double) (std::numeric_limits<int>::max() - 16))
            return {};
        const int outLen = (int) wanted;
        juce::AudioBuffer<float> out (2, outLen);
        for (int ch = 0; ch < 2; ++ch)
        {
            juce::LagrangeInterpolator resampler;
            resampler.process (ratio, raw.getReadPointer (ch), out.getWritePointer (ch), outLen, len, 0);
        }
        return out;
    }

    static juce::File resolveSavedFile (const juce::String& relAttr, const juce::String& pathAttr, const juce::File& base)
    {
        const juce::File relFile = relAttr.isEmpty() ? juce::File()
                                                     : (juce::File::isAbsolutePath (relAttr)
                                                        ? juce::File (relAttr)
                                                        : (base != juce::File() ? base.getChildFile (relAttr) : juce::File()));
        const juce::File absFile = pathAttr.isNotEmpty() && juce::File::isAbsolutePath (pathAttr) ? juce::File (pathAttr) : juce::File();
        if (relFile.existsAsFile())
            return relFile;
        if (absFile.existsAsFile())
            return absFile;
        return absFile != juce::File() ? absFile : relFile;
    }

    // One clip from a project element. A version 1 track is read as its own clip: the whole file at sample 0.
    Clip readClip (const juce::XmlElement& e, const juce::File& base, double& rate, bool legacy, const juce::String& trackName, juce::StringArray& problems)
    {
        Clip c;
        c.file = resolveSavedFile (e.getStringAttribute ("file").trim(), e.getStringAttribute ("path").trim(), base);

        c.name = legacy ? c.file.getFileNameWithoutExtension() : e.getStringAttribute ("name", c.file.getFileNameWithoutExtension());
        c.start = juce::jlimit ((juce::int64) 0, kMaxSafeSample, e.getStringAttribute ("start", "0").getLargeIntValue());
        c.offset = juce::jlimit ((juce::int64) 0, kMaxSafeSample, e.getStringAttribute ("offset", "0").getLargeIntValue());
        const juce::int64 savedLength = e.getStringAttribute ("length", "-1").getLargeIntValue();
        const double rawGainDb = e.getDoubleAttribute ("gainDb", 0.0);
        c.gainDb = std::isfinite (rawGainDb) ? (float) juce::jlimit (-100.0, 24.0, rawGainDb) : 0.0f;
        c.fadeIn = juce::jmax ((juce::int64) 0, e.getStringAttribute ("fadeIn", "0").getLargeIntValue());
        c.fadeOut = juce::jmax ((juce::int64) 0, e.getStringAttribute ("fadeOut", "0").getLargeIntValue());
        c.mute = e.getBoolAttribute ("mute");

        juce::String problem;
        c.audio = pooledAudio (c.file, rate, problem);
        if (c.audio != nullptr)
        {
            const juce::int64 source = c.audio->getNumSamples();
            c.offset = juce::jmin (c.offset, source);
            c.length = savedLength < 0 || legacy ? source - c.offset : juce::jlimit ((juce::int64) 0, source - c.offset, savedLength);
        }
        else
        {
            // no audio to take the length from: keep the saved one, capped once the session rate is known
            if (std::isfinite (rate) && rate > 0.0)
            {
                const double maxSamplesD = std::min (rate * 3600.0 * 2.0, (double) std::numeric_limits<juce::int64>::max() / 2.0);
                c.length = juce::jlimit ((juce::int64) 0, (juce::int64) maxSamplesD, savedLength);
            }
            else
            {
                c.length = juce::jmax ((juce::int64) 0, savedLength);
            }
            problems.add ("Track '" + trackName + "': " + problem);
        }
        c.fadeIn = juce::jlimit ((juce::int64) 0, c.length, c.fadeIn);
        c.fadeOut = juce::jmin (juce::jlimit ((juce::int64) 0, c.length, c.fadeOut), c.length - c.fadeIn);
        return c;
    }

    void setSampleRateLocked (double r)
    {
        const bool changed = std::abs (r - sampleRate) > 0.5;
        sampleRate = r;
        if (r > 0.0)
            tempo.sampleRate = r;
        // the monitor chain is made before the rate may be known: it is set up again for the new one (never by a render, which leaves it alone)
        if (changed && r > 0.0)
            for (auto& s : monitor)
                if (s->instance != nullptr)
                {
                    s->instance->releaseResources();
                    configure (*s->instance, r, false, false);
                    s->instance->reset();
                }
        if (changed)
            bumpRevision();
    }

    // Stereo main buses, optional stereo 2nd input bus (sidechain) kept enabled when stereo-compatible (unless disabled),
    // every other bus disabled, then the rate and the block, then the verdict. Returns false when the plugin does not run stereo
    // as set up here (see runsStereo): it is set up all the same, so a slot that is kept is never handed a buffer after releaseResources().
    bool configure (juce::AudioPluginInstance& p, double rate, bool nonRealtime, bool allowSidechain = true)
    {
        auto base = p.getBusesLayout();
        for (int i = 0; i < base.inputBuses.size(); ++i)
            base.inputBuses.getReference (i) = i == 0 ? juce::AudioChannelSet::stereo() : juce::AudioChannelSet::disabled();
        for (int i = 0; i < base.outputBuses.size(); ++i)
            base.outputBuses.getReference (i) = i == 0 ? juce::AudioChannelSet::stereo() : juce::AudioChannelSet::disabled();
        bool applied = false;
        if (allowSidechain && base.inputBuses.size() > 1)
        {
            auto withSc = base;
            withSc.inputBuses.getReference (1) = juce::AudioChannelSet::stereo();
            applied = p.setBusesLayout (withSc);
        }
        if (! applied)
            p.setBusesLayout (base); // a refusal is caught by runsStereo, which looks at the layout the plugin ended with
        p.setNonRealtime (nonRealtime);
        p.prepareToPlay (rate, kBlockSize);
        return runsStereo (p);
    }

    // Whether the plugin takes two channels in and out on its main buses (or 0 in for an instrument),
    // with an optional stereo 2nd input bus (sidechain) and no other auxiliary bus enabled.
    static bool runsStereo (const juce::AudioPluginInstance& p)
    {
        const int mainIn = p.getBusCount (true) > 0 ? p.getChannelCountOfBus (true, 0) : 0;
        const int scIn = p.getBusCount (true) > 1 ? p.getChannelCountOfBus (true, 1) : 0;
        const int outputs = p.getTotalNumOutputChannels();
        bool auxOff = true;
        for (int i = 2; i < p.getBusCount (true); ++i) auxOff = auxOff && p.getChannelCountOfBus (true, i) == 0;
        for (int i = 1; i < p.getBusCount (false); ++i) auxOff = auxOff && p.getChannelCountOfBus (false, i) == 0;
        const bool validMainIn = (mainIn == 0 || mainIn == 2);
        const bool validScIn = (scIn == 0) || (mainIn == 2 && scIn == 2);
        return validMainIn && validScIn && outputs == 2 && auxOff;
    }

    // The channels the plugin has, for the refusal's words: "1 in, 1 out".
    static juce::String layoutWords (const juce::AudioPluginInstance& p)
    {
        return juce::String (p.getTotalNumInputChannels()) + " in, " + juce::String (p.getTotalNumOutputChannels()) + " out";
    }

    // A track's buffers before it plays: two channels at the block size (the chain may need more; prepareLocked grows them).
    void makeTrackLocked (Track& t)
    {
        t.scratch.setSize (juce::jmax (2, t.scratch.getNumChannels()), kBlockSize, false, true, false);
        t.busInput.setSize (2, kBlockSize, false, true, false);
        t.postScratch.setSize (2, kBlockSize, false, true, false);
        if (t.isMidi && t.synth == nullptr)
            t.synth = std::make_unique<BasicSynth>();
        if (t.synth != nullptr && sampleRate > 0.0)
            t.synth->prepare (sampleRate);
    }

    void clearDelayRingsLocked()
    {
        for (auto& t : tracks)
        {
            t->delayRing.clear();
            t->delayPos = 0;
            t->delayDirty = false;
            if (t->synth != nullptr)
                t->synth->reset();
        }
    }

    void writeSlotXml (juce::XmlElement& parent, const juce::String& tagName, const Slot& s) const
    {
        auto* c = parent.createNewChildElement (tagName);
        c->setAttribute ("plugin", s.plugin);
        c->setAttribute ("bypass", s.bypass);
        if (s.wet < 0.9999f)
            c->setAttribute ("wet", (double) s.wet);
        if (s.sidechainTrack >= 0)
            c->setAttribute ("sidechainTrack", s.sidechainTrack);
        c->setAttribute ("state", stateOf (s).toBase64Encoding());
    }

    std::unique_ptr<Slot> readSlotXml (const juce::XmlElement& c, double rate, const juce::String& ownerLabel,
                                       juce::StringArray& problems, std::vector<double>& slotPreparedRates)
    {
        juce::MemoryBlock state;
        state.fromBase64Encoding (c.getStringAttribute ("state"));
        juce::String problem;
        const double slotRate = rate > 0.0 ? rate : (sampleRate > 0.0 ? sampleRate : 48000.0);
        auto slot = makeSlot (c.getStringAttribute ("plugin"), &state, problem, rate);
        if (slot == nullptr)
        {
            problems.add (ownerLabel + ": " + problem);
            slot = std::make_unique<Slot>();
            slot->plugin = c.getStringAttribute ("plugin");
            slot->savedState = state;
        }
        else
        {
            slotPreparedRates.push_back (slotRate);
        }
        slot->bypass = c.getBoolAttribute ("bypass");
        const double rawWet = c.getDoubleAttribute ("wet", 1.0);
        slot->wet = std::isfinite (rawWet) ? juce::jlimit (0.0f, 1.0f, (float) rawWet) : 1.0f;
        slot->sidechainTrack = juce::jmax (-1, c.getIntAttribute ("sidechainTrack", -1));
        return slot;
    }

    struct ParsedXml
    {
        double rate = 0.0;
        std::vector<double> slotPreparedRates;
        std::vector<std::unique_ptr<Track>> tracks;
        std::vector<std::unique_ptr<Slot>> masterChain;
        TempoMap tempo;
        std::vector<Marker> markers;
        float masterDb = 0.0f;
        juce::int64 loopStart = 0;
        juce::int64 loopEnd = 0;
        bool loopOn = false;
        bool metronomeOn = false;
        bool snapOn = true;
        Grid grid = Grid::Quarter;
        juce::StringArray problems;
    };

    ParsedXml parseXml (const juce::XmlElement& xml, const juce::File& baseDir,
                        double initialRate, const juce::String& initialProblem = {})
    {
        ParsedXml out;
        out.rate = initialRate;
        if (initialProblem.isNotEmpty())
            out.problems.add (initialProblem);

        for (auto* e : xml.getChildWithTagNameIterator ("Track"))
        {
            auto t = std::make_unique<Track>();
            t->name = e->getStringAttribute ("name");
            const double trackGain = e->getDoubleAttribute ("gainDb", 0.0);
            const double trackPan = e->getDoubleAttribute ("pan", 0.0);
            // the same range as the fader and Editor::setTrackGain (-100 is silence, +12 the top)
            if (std::isfinite (trackGain) && trackGain > 12.0)
                out.problems.add ("Track '" + t->name + "': the gain was above +12 dB and is set to +12 dB.");
            t->gainDb = std::isfinite (trackGain) ? (float) juce::jlimit (-100.0, 12.0, trackGain) : 0.0f;
            t->pan = std::isfinite (trackPan) ? (float) juce::jlimit (-1.0, 1.0, trackPan) : 0.0f;
            t->mute = e->getBoolAttribute ("mute");
            t->solo = e->getBoolAttribute ("solo");
            t->armed = e->getBoolAttribute ("armed");
            t->inputLeft = juce::jmax (0, e->getIntAttribute ("inputLeft", 0));
            t->inputRight = juce::jmax (-1, e->getIntAttribute ("inputRight", 1));
            const auto kindStr = e->getStringAttribute ("kind").trim().toLowerCase();
            if (kindStr == "bus")
            {
                t->kind = TrackKind::Bus;
                t->isMidi = false;
            }
            else if (kindStr == "midi")
            {
                t->kind = TrackKind::Midi;
                t->isMidi = true;
            }
            else if (kindStr == "audio")
            {
                t->kind = TrackKind::Audio;
                t->isMidi = false;
            }
            else
            {
                t->isMidi = e->getBoolAttribute ("isMidi", false);
                t->kind = t->isMidi ? TrackKind::Midi : TrackKind::Audio;
            }
            t->outputTrack = juce::jmax (-1, e->getIntAttribute ("outputTrack", -1));
            for (auto* se : e->getChildWithTagNameIterator ("Send"))
            {
                AuxSend snd;
                snd.targetTrack = se->getIntAttribute ("targetTrack", -1);
                const double rawLvl = se->getDoubleAttribute ("levelDb", 0.0);
                snd.levelDb = std::isfinite (rawLvl) ? (float) juce::jlimit (-100.0, 12.0, rawLvl) : 0.0f;
                snd.preFader = se->getBoolAttribute ("preFader", false);
                snd.mute = se->getBoolAttribute ("mute", false);
                t->sends.push_back (snd);
            }
            t->showAutomation = e->getBoolAttribute ("showAutomation", false);
            t->activeAutoTarget = e->getStringAttribute ("activeAutoTarget", "volume");
            if (t->activeAutoTarget.isEmpty()) t->activeAutoTarget = "volume";
            if (e->getBoolAttribute ("frozen", false))
            {
                const auto resolved = resolveSavedFile (e->getStringAttribute ("frozenFile").trim(),
                                                        e->getStringAttribute ("frozenPath").trim(), baseDir);
                juce::String problem;
                auto audio = resolved != juce::File() ? pooledAudio (resolved, out.rate, problem) : nullptr;
                if (audio != nullptr)
                {
                    t->frozen = true;
                    t->frozenFile = resolved;
                    t->frozenAudio = std::move (audio);
                }
                else
                {
                    out.problems.add ("Track '" + t->name + "': frozen audio missing"
                                      + (problem.isNotEmpty() ? " (" + problem + ")" : juce::String()) + "; unfrozen.");
                }
            }
            if (t->isMidi)
                t->synth = std::make_unique<BasicSynth>();
            const bool legacy = e->getChildByName ("Clip") == nullptr && (e->hasAttribute ("file") || e->hasAttribute ("path"));
            if (legacy)
                t->clips.push_back (readClip (*e, baseDir, out.rate, true, t->name, out.problems));
            else
                for (auto* c : e->getChildWithTagNameIterator ("Clip"))
                    t->clips.push_back (readClip (*c, baseDir, out.rate, false, t->name, out.problems));

            for (auto* mc : e->getChildWithTagNameIterator ("MidiClip"))
            {
                MidiClip m;
                m.name = mc->getStringAttribute ("name");
                m.start = juce::jlimit ((juce::int64) 0, kMaxSafeSample, mc->getStringAttribute ("start", "0").getLargeIntValue());
                m.length = juce::jlimit ((juce::int64) 0, kMaxSafeSample, mc->getStringAttribute ("length", "0").getLargeIntValue());
                m.mute = mc->getBoolAttribute ("mute");
                for (auto* ne : mc->getChildWithTagNameIterator ("Note"))
                {
                    MidiNote note;
                    const double sb = ne->getDoubleAttribute ("startBeat", 0.0);
                    const double db = ne->getDoubleAttribute ("durationBeats", 1.0);
                    note.startBeat = std::isfinite (sb) ? juce::jlimit (0.0, 1e7, sb) : 0.0;
                    note.durationBeats = (std::isfinite (db) && db > 0.0) ? juce::jlimit (1.0 / 64.0, 1e6, db) : 1.0;
                    note.pitch = juce::jlimit (0, 127, ne->getIntAttribute ("pitch", 60));
                    note.velocity = juce::jlimit (1, 127, ne->getIntAttribute ("velocity", 100));
                    note.channel = juce::jlimit (1, 16, ne->getIntAttribute ("channel", 1));
                    m.notes.push_back (note);
                }
                t->midiClips.push_back (std::move (m));
            }

            if (auto* instEl = e->getChildByName ("Instrument"))
                if (! t->isBus())
                    t->instrument = readSlotXml (*instEl, out.rate, "Track '" + t->name + "'", out.problems, out.slotPreparedRates);
            for (auto* c : e->getChildWithTagNameIterator ("Slot"))
                t->chain.push_back (readSlotXml (*c, out.rate, "Track '" + t->name + "'", out.problems, out.slotPreparedRates));
            for (auto* ae : e->getChildWithTagNameIterator ("AutomationLane"))
            {
                AutomationLane lane;
                lane.targetId = ae->getStringAttribute ("targetId");
                if (lane.targetId.isEmpty()) continue;
                lane.slotIndex = ae->getIntAttribute ("slotIndex", -1);
                lane.paramIndex = ae->getIntAttribute ("paramIndex", -1);
                lane.enabled = ae->getBoolAttribute ("enabled", true);
                for (auto* pe : ae->getChildWithTagNameIterator ("Point"))
                {
                    AutoPoint pt;
                    pt.sample = juce::jlimit ((juce::int64) 0, kMaxSafeSample, pe->getStringAttribute ("sample", "0").getLargeIntValue());
                    const double rawVal = pe->getDoubleAttribute ("value", 0.0);
                    pt.value = std::isfinite (rawVal) ? (float) juce::jlimit (0.0, 1.0, rawVal) : 0.0f;
                    pt.curve = (AutoCurveType) juce::jlimit (0, 2, pe->getIntAttribute ("curve", 0));
                    lane.points.push_back (pt);
                }
                std::stable_sort (lane.points.begin(), lane.points.end(),
                                  [] (const AutoPoint& a, const AutoPoint& b) { return a.sample < b.sample; });
                t->automation.push_back (std::move (lane));
            }
            out.tracks.push_back (std::move (t));
        }

        if (auto* mc = xml.getChildByName ("MasterChain"))
            for (auto* c : mc->getChildWithTagNameIterator ("Plugin"))
                out.masterChain.push_back (readSlotXml (*c, out.rate, "Master", out.problems, out.slotPreparedRates));

        const int numTracks = (int) out.tracks.size();
        for (int ti = 0; ti < numTracks; ++ti)
        {
            auto& t = *out.tracks[(size_t) ti];
            if (t.outputTrack >= 0)
            {
                if (t.outputTrack == ti || ! juce::isPositiveAndBelow (t.outputTrack, numTracks) || ! out.tracks[(size_t) t.outputTrack]->isBus())
                {
                    out.problems.add ("Track '" + t.name + "': invalid outputTrack reset to Master.");
                    t.outputTrack = -1;
                }
            }
            std::vector<AuxSend> validSends;
            for (const auto& snd : t.sends)
            {
                if (snd.targetTrack == ti || ! juce::isPositiveAndBelow (snd.targetTrack, numTracks) || ! out.tracks[(size_t) snd.targetTrack]->isBus())
                {
                    out.problems.add ("Track '" + t.name + "': invalid aux send ignored.");
                    continue;
                }
                validSends.push_back (snd);
            }
            t.sends = std::move (validSends);
            for (auto& slot : t.chain)
            {
                if (slot->sidechainTrack >= 0)
                {
                    if (slot->sidechainTrack == ti || ! juce::isPositiveAndBelow (slot->sidechainTrack, numTracks))
                    {
                        out.problems.add ("Track '" + t.name + "': invalid sidechainTrack cleared.");
                        slot->sidechainTrack = -1;
                    }
                }
            }
        }
        // Break any cycles in malformed XML
        for (int ti = 0; ti < numTracks; ++ti)
        {
            auto& t = *out.tracks[(size_t) ti];
            if (t.outputTrack >= 0 && wouldCreateCycleIn (out.tracks, ti, t.outputTrack))
            {
                out.problems.add ("Track '" + t.name + "': routing cycle broken.");
                t.outputTrack = -1;
            }
            for (auto it = t.sends.begin(); it != t.sends.end();)
            {
                const int target = it->targetTrack;
                it->targetTrack = -1; // temporarily unlink to test if adding it creates a cycle
                if (wouldCreateCycleIn (out.tracks, ti, target))
                {
                    out.problems.add ("Track '" + t.name + "': aux send cycle broken.");
                    it = t.sends.erase (it);
                }
                else
                {
                    it->targetTrack = target;
                    ++it;
                }
            }
            for (auto& slot : t.chain)
            {
                if (slot->sidechainTrack >= 0)
                {
                    const int sc = slot->sidechainTrack;
                    slot->sidechainTrack = -1;
                    if (wouldCreateCycleIn (out.tracks, sc, ti))
                        out.problems.add ("Track '" + t.name + "': sidechain cycle broken.");
                    else
                        slot->sidechainTrack = sc;
                }
            }
        }

        const double capRate = (std::isfinite (out.rate) && out.rate > 0.0) ? out.rate : 48000.0;
        const juce::int64 maxMissingSamples = (juce::int64) std::min (capRate * 3600.0 * 2.0, (double) std::numeric_limits<juce::int64>::max() / 2.0);
        for (auto& t : out.tracks)
            for (auto& c : t->clips)
                if (c.audio == nullptr)
                {
                    c.length = juce::jlimit ((juce::int64) 0, maxMissingSamples, c.length);
                    c.fadeIn = juce::jlimit ((juce::int64) 0, c.length, c.fadeIn);
                    c.fadeOut = juce::jmin (juce::jlimit ((juce::int64) 0, c.length, c.fadeOut), c.length - c.fadeIn);
                }

        std::vector<TempoPoint> points;
        for (auto* p : xml.getChildWithTagNameIterator ("Tempo"))
        {
            const double rawBeat = p->getDoubleAttribute ("beat", 0.0);
            const double rawBpm = p->getDoubleAttribute ("bpm", 120.0);
            if (std::isfinite (rawBeat) && std::isfinite (rawBpm))
                points.push_back ({ juce::jmax (0.0, rawBeat), juce::jlimit (TempoMap::kMinBpm, TempoMap::kMaxBpm, rawBpm) });
        }
        std::stable_sort (points.begin(), points.end(), [] (const TempoPoint& a, const TempoPoint& b) { return a.beat < b.beat; });
        if (points.empty() || points.front().beat > TempoMap::kBeatEps)
            points.insert (points.begin(), { 0.0, points.empty() ? 120.0 : points.front().bpm });
        out.tempo.points.clear();
        for (const auto& p : points)
            if (out.tempo.points.empty() || p.beat > out.tempo.points.back().beat + TempoMap::kBeatEps)
                out.tempo.points.push_back (p);
        out.tempo.setSignature (juce::jlimit (1, 32, xml.getIntAttribute ("tsNum", 4)), xml.getIntAttribute ("tsDen", 4));

        for (auto* m : xml.getChildWithTagNameIterator ("Marker"))
            out.markers.push_back ({ m->getStringAttribute ("name"),
                                     juce::jlimit ((juce::int64) 0, kMaxSafeSample, m->getStringAttribute ("sample", "0").getLargeIntValue()) });
        std::stable_sort (out.markers.begin(), out.markers.end(), [] (const Marker& a, const Marker& b) { return a.start < b.start; });

        // the same range as the fader and Editor::setMasterDb (-100 is silence, +12 the top)
        const double savedMasterDb = xml.getDoubleAttribute ("masterDb", 0.0);
        if (! std::isfinite (savedMasterDb))
        {
            out.masterDb = 0.0f;
        }
        else
        {
            if (savedMasterDb > 12.0)
                out.problems.add ("The master gain was above +12 dB and is set to +12 dB.");
            out.masterDb = (float) juce::jlimit (-100.0, 12.0, savedMasterDb);
        }
        out.loopStart = juce::jlimit ((juce::int64) 0, kMaxSafeSample, xml.getStringAttribute ("loopStart", "0").getLargeIntValue());
        out.loopEnd = juce::jlimit ((juce::int64) 0, kMaxSafeSample, xml.getStringAttribute ("loopEnd", "0").getLargeIntValue());
        out.loopOn = xml.getBoolAttribute ("loopOn") && out.loopEnd > out.loopStart;
        out.metronomeOn = xml.getBoolAttribute ("metronome");
        out.snapOn = xml.getBoolAttribute ("snap", true);
        out.grid = (Grid) juce::jlimit (0, 5, xml.getIntAttribute ("grid", (int) Grid::Quarter));
        return out;
    }

    void clearProjectPlayheadsLocked()
    {
        for (auto& t : tracks)
        {
            if (t->instrument != nullptr && t->instrument->instance != nullptr)
                t->instrument->instance->setPlayHead (nullptr);
            for (auto& s : t->chain)
                if (s->instance != nullptr) s->instance->setPlayHead (nullptr);
        }
        for (auto& s : masterChain)
            if (s->instance != nullptr) s->instance->setPlayHead (nullptr);
    }

    void attachProjectPlayheadsLocked()
    {
        for (auto& t : tracks)
        {
            if (t->instrument != nullptr && t->instrument->instance != nullptr)
                t->instrument->instance->setPlayHead (&head);
            for (auto& s : t->chain)
                if (s->instance != nullptr) s->instance->setPlayHead (&head);
        }
        for (auto& s : masterChain)
            if (s->instance != nullptr) s->instance->setPlayHead (&head);
    }

    static void ensureSlotDryScratch (Slot& s)
    {
        if (s.dryScratch.getNumChannels() < 2 || s.dryScratch.getNumSamples() < kBlockSize)
            s.dryScratch.setSize (2, kBlockSize, false, true, false);
    }

    static bool wouldCreateCycleIn (const std::vector<std::unique_ptr<Track>>& trackList, int fromTrack, int toTrack)
    {
        const int n = (int) trackList.size();
        if (fromTrack == toTrack) return true;
        if (! juce::isPositiveAndBelow (fromTrack, n) || ! juce::isPositiveAndBelow (toTrack, n)) return false;
        std::vector<bool> visited ((size_t) n, false);
        std::vector<int> stack { toTrack };
        visited[(size_t) toTrack] = true;
        while (! stack.empty())
        {
            const int u = stack.back();
            stack.pop_back();
            if (u == fromTrack) return true;
            const auto& t = *trackList[(size_t) u];
            auto push = [&] (int next)
            {
                if (juce::isPositiveAndBelow (next, n) && ! visited[(size_t) next])
                {
                    visited[(size_t) next] = true;
                    stack.push_back (next);
                }
            };
            if (t.outputTrack >= 0) push (t.outputTrack);
            for (const auto& snd : t.sends)
                if (snd.targetTrack >= 0) push (snd.targetTrack);
            for (int v = 0; v < n; ++v)
                for (const auto& s : trackList[(size_t) v]->chain)
                    if (s->sidechainTrack == u) push (v);
        }
        return false;
    }

    bool wouldCreateCycleLocked (int fromTrack, int toTrack) const
    {
        return wouldCreateCycleIn (tracks, fromTrack, toTrack);
    }

    void computeRoutingLocked (std::vector<int>& order, std::vector<int>& inLat, std::vector<int>& outLat) const
    {
        const int n = (int) tracks.size();
        order.clear();
        inLat.assign ((size_t) n, 0);
        outLat.assign ((size_t) n, 0);
        if (n == 0)
            return;

        std::vector<int> inDegree ((size_t) n, 0);
        std::vector<std::vector<int>> adj ((size_t) n);
        auto addEdge = [&] (int u, int v)
        {
            if (u >= 0 && u < n && v >= 0 && v < n && u != v)
            {
                adj[(size_t) u].push_back (v);
                ++inDegree[(size_t) v];
            }
        };
        for (int i = 0; i < n; ++i)
        {
            const auto& t = *tracks[(size_t) i];
            if (t.outputTrack >= 0)
                addEdge (i, t.outputTrack);
            for (const auto& s : t.sends)
                if (s.targetTrack >= 0)
                    addEdge (i, s.targetTrack);
            for (const auto& slot : t.chain)
                if (slot->sidechainTrack >= 0)
                    addEdge (slot->sidechainTrack, i);
        }
        std::vector<int> queue;
        queue.reserve ((size_t) n);
        for (int i = 0; i < n; ++i)
            if (inDegree[(size_t) i] == 0)
                queue.push_back (i);
        for (size_t headIdx = 0; headIdx < queue.size(); ++headIdx)
        {
            const int u = queue[headIdx];
            order.push_back (u);
            for (int v : adj[(size_t) u])
                if (--inDegree[(size_t) v] == 0)
                    queue.push_back (v);
        }
        if ((int) order.size() < n)
        {
            std::vector<bool> seen ((size_t) n, false);
            for (int u : order) seen[(size_t) u] = true;
            for (int i = 0; i < n; ++i)
                if (! seen[(size_t) i])
                    order.push_back (i);
        }

        for (int u : order)
        {
            const auto& t = *tracks[(size_t) u];
            outLat[(size_t) u] = inLat[(size_t) u] + t.latency();
            if (t.outputTrack >= 0 && t.outputTrack < n)
                inLat[(size_t) t.outputTrack] = juce::jmax (inLat[(size_t) t.outputTrack], outLat[(size_t) u]);
            for (const auto& s : t.sends)
                if (s.targetTrack >= 0 && s.targetTrack < n)
                    inLat[(size_t) s.targetTrack] = juce::jmax (inLat[(size_t) s.targetTrack], outLat[(size_t) u]);
        }
    }

    template <typename TrackVec, typename OrderVec>
    static void computeAudibility (const TrackVec& trackList, const OrderVec& topoOrder, std::vector<bool>& outAudible)
    {
        const int n = (int) trackList.size();
        if ((int) outAudible.size() != n)
            outAudible.assign ((size_t) n, false);
        else
            std::fill (outAudible.begin(), outAudible.end(), false);
        if (n == 0)
            return;

        auto getTrack = [&trackList] (int idx) -> const auto&
        {
            if constexpr (std::is_same_v<std::decay_t<decltype (trackList[(size_t) idx])>, std::unique_ptr<Track>>)
                return *trackList[(size_t) idx];
            else
                return trackList[(size_t) idx];
        };

        bool anySolo = false;
        for (int i = 0; i < n; ++i)
            if (getTrack (i).solo) { anySolo = true; break; }

        if (! anySolo)
        {
            for (int i = 0; i < n; ++i)
                outAudible[(size_t) i] = ! getTrack (i).mute;
            return;
        }

        for (int i = 0; i < n; ++i)
        {
            const auto& t = getTrack (i);
            outAudible[(size_t) i] = ! t.mute && t.solo;
        }
        // Upstream propagation: if a soloed bus is active, unmuted tracks feeding it become audible
        for (int idx = (int) topoOrder.size() - 1; idx >= 0; --idx)
        {
            const int u = topoOrder[(size_t) idx];
            if (! juce::isPositiveAndBelow (u, n)) continue;
            const auto& t = getTrack (u);
            if (t.mute || outAudible[(size_t) u]) continue;
            bool feedsActiveBus = false;
            if (juce::isPositiveAndBelow (t.outputTrack, n) && outAudible[(size_t) t.outputTrack])
                feedsActiveBus = true;
            if (! feedsActiveBus)
            {
                for (const auto& snd : t.sends)
                    if (! snd.mute && juce::isPositiveAndBelow (snd.targetTrack, n) && outAudible[(size_t) snd.targetTrack])
                        { feedsActiveBus = true; break; }
            }
            if (feedsActiveBus)
                outAudible[(size_t) u] = true;
        }
        // Downstream propagation: if an active track routes/sends to an unmuted bus, that bus becomes audible
        for (int u : topoOrder)
        {
            if (! juce::isPositiveAndBelow (u, n) || ! outAudible[(size_t) u]) continue;
            const auto& t = getTrack (u);
            if (juce::isPositiveAndBelow (t.outputTrack, n) && ! getTrack (t.outputTrack).mute)
                outAudible[(size_t) t.outputTrack] = true;
            for (const auto& snd : t.sends)
                if (! snd.mute && juce::isPositiveAndBelow (snd.targetTrack, n) && ! getTrack (snd.targetTrack).mute)
                    outAudible[(size_t) snd.targetTrack] = true;
        }
    }

    void computeTrackAudibilityLocked (std::vector<bool>& outAudible) const
    {
        if (cachedTopoOrder.size() == tracks.size())
        {
            computeAudibility (tracks, cachedTopoOrder, outAudible);
            return;
        }
        std::vector<int> order, inLat, outLat;
        computeRoutingLocked (order, inLat, outLat);
        computeAudibility (tracks, order, outAudible);
    }

    void prepareLocked (bool nonRealtime)
    {
        const double rate = sampleRate > 0.0 ? sampleRate : 48000.0;
        for (auto& t : tracks)
        {
            t->busInput.setSize (2, kBlockSize, false, true, false);
            t->postScratch.setSize (2, kBlockSize, false, true, false);
            if (t->isMidi && t->synth == nullptr)
                t->synth = std::make_unique<BasicSynth>();
            if (t->synth != nullptr && sampleRate > 0.0)
            {
                t->synth->prepare (sampleRate);
                t->synth->reset();
            }
            t->activeNotes.reset();
            t->pendingNoteFlush = false;
            if (t->instrument != nullptr)
            {
                ensureSlotDryScratch (*t->instrument);
                if (t->instrument->instance != nullptr)
                {
                    t->instrument->instance->releaseResources();
                    configure (*t->instrument->instance, rate, nonRealtime, true);
                    t->instrument->instance->reset();
                }
            }
            for (auto& s : t->chain)
            {
                ensureSlotDryScratch (*s);
                if (s->instance != nullptr)
                {
                    s->instance->releaseResources();
                    configure (*s->instance, rate, nonRealtime, true);
                    s->instance->reset();
                }
            }
        }
        for (auto& s : masterChain)
        {
            ensureSlotDryScratch (*s);
            if (s->instance != nullptr)
            {
                s->instance->releaseResources();
                configure (*s->instance, rate, nonRealtime, false);
                s->instance->reset();
                s->instance->setPlayHead (&head);
            }
        }
        for (auto& s : monitor)
            ensureSlotDryScratch (*s);
        masterScratch.setSize (2, kBlockSize, false, true, false);
        monitorScratch.setSize (kMaxMonitorChannels, kBlockSize, false, true, false);
        updateLatenciesLocked();
        clearDelayRingsLocked();
    }

    // How long each track is delayed in real time so the tracks stay aligned with each other (the one with the most latency is not).
    void updateLatenciesLocked()
    {
        computeRoutingLocked (cachedTopoOrder, cachedInLat, cachedOutLat);
        cachedAudibleScratch.assign (tracks.size(), false);
        int most = 0;
        for (int lat : cachedOutLat) most = juce::jmax (most, lat);

        for (size_t i = 0; i < tracks.size(); ++i)
        {
            auto& t = tracks[i];
            t->busInput.setSize (2, kBlockSize, false, true, false);
            t->postScratch.setSize (2, kBlockSize, false, true, false);
            // the buffer the chain runs in has the channels of its widest plugin (two at least), so no plugin is handed more than it owns
            int channels = 2;
            if (t->instrument != nullptr)
            {
                ensureSlotDryScratch (*t->instrument);
                if (t->instrument->instance != nullptr)
                    channels = juce::jmax (channels, t->instrument->instance->getTotalNumInputChannels(), t->instrument->instance->getTotalNumOutputChannels());
            }
            for (auto& s : t->chain)
            {
                ensureSlotDryScratch (*s);
                if (s->instance != nullptr)
                    channels = juce::jmax (channels, s->instance->getTotalNumInputChannels(), s->instance->getTotalNumOutputChannels());
            }
            if (channels != t->scratch.getNumChannels())
                t->scratch.setSize (channels, kBlockSize, false, true, false);
            const int targetLat = (t->outputTrack >= 0 && t->outputTrack < (int) cachedInLat.size())
                                      ? cachedInLat[(size_t) t->outputTrack]
                                      : most;
            const int wanted = juce::jmax (0, targetLat - cachedOutLat[i]);
            if (wanted != t->delaySamples)
            {
                t->delaySamples = wanted;
                t->delayRing.setSize (2, juce::jmax (1, wanted), false, true, false);
                t->delayPos = 0;
            }
        }
        int masterChannels = 2;
        for (auto& s : masterChain)
        {
            ensureSlotDryScratch (*s);
            if (s->instance != nullptr)
                masterChannels = juce::jmax (masterChannels, s->instance->getTotalNumInputChannels(), s->instance->getTotalNumOutputChannels());
        }
        for (auto& s : monitor)
            ensureSlotDryScratch (*s);
        if (masterChannels != masterScratch.getNumChannels() || masterScratch.getNumSamples() != kBlockSize)
            masterScratch.setSize (masterChannels, kBlockSize, false, true, false);
        bumpRevision();
    }

    // The fader and the balance of a track as the two channel gains.
    static void trackGains (const Track& t, float& left, float& right)
    {
        const float gain = juce::Decibels::decibelsToGain (t.gainDb, -100.0f);
        float l = 1.0f, r = 1.0f;
        panGains (t.pan, l, r);
        left = gain * l;
        right = gain * r;
    }

    static bool parsePluginTarget (const juce::String& targetId, int& slotIndex, int& paramIndex)
    {
        if (! targetId.startsWith ("plugin:"))
            return false;
        const auto rest = targetId.substring (7);
        const int colon = rest.indexOfChar (':');
        if (colon <= 0 || colon + 1 >= rest.length())
            return false;
        const auto sStr = rest.substring (0, colon);
        const auto pStr = rest.substring (colon + 1);
        if (! sStr.containsOnly ("0123456789") || ! pStr.containsOnly ("0123456789"))
            return false;
        slotIndex = sStr.getIntValue();
        paramIndex = pStr.getIntValue();
        return slotIndex >= 0 && paramIndex >= 0;
    }

    static juce::String pluginTargetId (int slot, int param)
    {
        return "plugin:" + juce::String (slot) + ":" + juce::String (param);
    }

    static void remapAutomationOnRemove (Track& t, int removedSlot)
    {
        t.automation.erase (std::remove_if (t.automation.begin(), t.automation.end(),
                                            [removedSlot] (const AutomationLane& l) { return l.slotIndex == removedSlot; }),
                            t.automation.end());
        for (auto& lane : t.automation)
        {
            if (lane.slotIndex > removedSlot)
            {
                --lane.slotIndex;
                lane.targetId = pluginTargetId (lane.slotIndex, lane.paramIndex);
            }
        }
        int s = -1, p = -1;
        if (parsePluginTarget (t.activeAutoTarget, s, p))
        {
            if (s == removedSlot)
                t.activeAutoTarget = "volume";
            else if (s > removedSlot)
                t.activeAutoTarget = pluginTargetId (s - 1, p);
        }
    }

    static int remapMovedSlot (int s, int from, int to)
    {
        if (from == to || s < 0) return s;
        if (s == from) return to;
        if (from < to && s > from && s <= to) return s - 1;
        if (from > to && s >= to && s < from) return s + 1;
        return s;
    }

    static void remapAutomationOnMove (Track& t, int from, int to)
    {
        if (from == to) return;
        for (auto& lane : t.automation)
        {
            if (lane.slotIndex >= 0)
            {
                const int mapped = remapMovedSlot (lane.slotIndex, from, to);
                if (mapped != lane.slotIndex)
                {
                    lane.slotIndex = mapped;
                    lane.targetId = pluginTargetId (lane.slotIndex, lane.paramIndex);
                }
            }
        }
        int s = -1, p = -1;
        if (parsePluginTarget (t.activeAutoTarget, s, p))
        {
            const int mapped = remapMovedSlot (s, from, to);
            if (mapped != s)
                t.activeAutoTarget = pluginTargetId (mapped, p);
        }
    }

    static void clearPluginAutomation (Track& t)
    {
        t.automation.erase (std::remove_if (t.automation.begin(), t.automation.end(),
                                            [] (const AutomationLane& l) { return l.slotIndex >= 0; }),
                            t.automation.end());
        if (t.activeAutoTarget.startsWith ("plugin:"))
            t.activeAutoTarget = "volume";
    }

    static const AutomationLane* findActiveLane (const std::vector<AutomationLane>& lanes, juce::StringRef targetId)
    {
        for (const auto& lane : lanes)
            if (lane.enabled && ! lane.points.empty() && lane.targetId == targetId)
                return &lane;
        return nullptr;
    }

    static void mixTrackBlock (const juce::AudioBuffer<float>& src, juce::AudioBuffer<float>& out,
                               int outOffset, int count, juce::int64 blockStart,
                               float baseGainDb, float basePan,
                               const AutomationLane* volLane, const AutomationLane* panLane,
                               float* outPeakL = nullptr, float* outPeakR = nullptr)
    {
        const int channels = juce::jmin (2, out.getNumChannels());
        if (volLane == nullptr && panLane == nullptr)
        {
            const float gain = juce::Decibels::decibelsToGain (baseGainDb, -100.0f);
            float l = 1.0f, r = 1.0f;
            panGains (basePan, l, r);
            const float left = gain * l, right = gain * r;
            for (int ch = 0; ch < channels; ++ch)
                out.addFrom (ch, outOffset, src, ch, 0, count, ch == 0 ? left : right);
            if (outPeakL != nullptr)
                *outPeakL = src.getNumChannels() > 0 ? src.getMagnitude (0, 0, count) * left : 0.0f;
            if (outPeakR != nullptr)
                *outPeakR = src.getNumChannels() > 1 ? src.getMagnitude (1, 0, count) * right : 0.0f;
            return;
        }

        const float* srcL = src.getNumChannels() > 0 ? src.getReadPointer (0) : nullptr;
        const float* srcR = src.getNumChannels() > 1 ? src.getReadPointer (1) : srcL;
        float* dstL = channels > 0 ? out.getWritePointer (0, outOffset) : nullptr;
        float* dstR = channels > 1 ? out.getWritePointer (1, outOffset) : nullptr;
        float pL = 0.0f, pR = 0.0f;
        for (int s = 0; s < count; ++s)
        {
            const juce::int64 pos = blockStart + s;
            const float gainDb = volLane != nullptr ? autoNormToDb (volLane->evaluateAt (pos)) : baseGainDb;
            const float panVal = panLane != nullptr ? juce::jlimit (-1.0f, 1.0f, panLane->evaluateAt (pos) * 2.0f - 1.0f) : basePan;
            const float gain = juce::Decibels::decibelsToGain (gainDb, -100.0f);
            float panL = 1.0f, panR = 1.0f;
            panGains (panVal, panL, panR);
            const float vL = (srcL != nullptr ? srcL[s] : 0.0f) * (gain * panL);
            const float vR = (srcR != nullptr ? srcR[s] : 0.0f) * (gain * panR);
            if (dstL != nullptr) dstL[s] += vL;
            if (dstR != nullptr) dstR[s] += vR;
            if (std::isfinite (vL)) pL = juce::jmax (pL, std::abs (vL));
            if (std::isfinite (vR)) pR = juce::jmax (pR, std::abs (vR));
        }
        if (outPeakL != nullptr) *outPeakL = pL;
        if (outPeakR != nullptr) *outPeakR = pR;
    }

    // Sets the playhead for the block that starts at `start`: the position, the tempo and the time signature, and the loop.
    void publishHead (juce::int64 start, bool playing)
    {
        const double beat = tempo.samplesToBeats ((double) start);
        head.set (start, playing, tempo.bpmAtBeat (beat), beat);
        head.setTimeSignature (tempo.numerator, tempo.denominator);
        head.setIsLooping (loopOn);
        if (loopOn)
            head.setLoopPoints (tempo.samplesToBeats ((double) loopStart), tempo.samplesToBeats ((double) loopEnd));
    }

    // The metronome: a click of 15 ms at every beat that starts in [start - click, start + n). It is computed from the distance since
    // each beat, so a block that starts in the middle of a click gets the rest of it, and nothing is kept between blocks.
    static void addMetronomeWith (juce::AudioBuffer<float>& out, juce::int64 start, const TempoMap& map, bool on)
    {
        if (! on)
            return;
        const int n = out.getNumSamples();
        const double sr = map.sampleRate;
        const juce::int64 click = (juce::int64) std::llround (0.015 * sr);
        if (click <= 0 || sr <= 0.0)
            return;
        const double beatsFrom = map.samplesToBeats ((double) (start - click));
        const double beatsTo = map.samplesToBeats ((double) (start + n));
        for (juce::int64 k = juce::jmax ((juce::int64) 0, (juce::int64) std::floor (beatsFrom)); (double) k <= beatsTo; ++k)
        {
            const double beatSample = map.beatsToSamples ((double) k);
            const juce::int64 onset = (juce::int64) std::llround (beatSample);
            const juce::int64 from = juce::jmax (start, onset), to = juce::jmin (start + n, onset + click);
            if (to <= from)
                continue;
            const bool bar = map.barBeatAt (beatSample).beat == 1;
            const double frequency = bar ? 1500.0 : 1000.0;
            const float amplitude = bar ? 0.35f : 0.25f;
            for (juce::int64 s = from; s < to; ++s)
            {
                const double t = (double) (s - onset);
                const float v = amplitude * (float) (std::exp (-6.0 * t / (double) click) * std::sin (2.0 * juce::MathConstants<double>::pi * frequency * t / sr));
                for (int ch = 0; ch < juce::jmin (2, out.getNumChannels()); ++ch)
                    out.addSample (ch, (int) (s - start), v);
            }
        }
    }

    void addMetronome (juce::AudioBuffer<float>& out, juce::int64 start) const
    {
        addMetronomeWith (out, start, tempo, metronomeOn);
    }

    void processBlockFromSnapshot (juce::AudioBuffer<float>& out, juce::int64 start)
    {
        int snapIdx = 0;
        for (;;)
        {
            snapIdx = activeSnapshot.load (std::memory_order_acquire);
            snapshotReaders[(size_t) snapIdx].fetch_add (1, std::memory_order_acq_rel);
            if (activeSnapshot.load (std::memory_order_acquire) == snapIdx)
                break;
            snapshotReaders[(size_t) snapIdx].fetch_sub (1, std::memory_order_release);
        }
        struct ReaderGuard
        {
            std::atomic<int>& counter;
            ~ReaderGuard() { counter.fetch_sub (1, std::memory_order_release); }
        } guard { snapshotReaders[(size_t) snapIdx] };

        auto& snap = snapshots[(size_t) snapIdx];
        const bool continuing = lastBlockWasFallback && snap.tracks.size() == lastFallbackTrackCount;
        lastBlockWasFallback = true;
        lastFallbackTrackCount = snap.tracks.size();
        if (! continuing)
            for (auto& fs : fallbackSynths)
                fs.reset();

        computeAudibility (snap.tracks, snap.topoOrder, snap.audibleScratch);

        const int n = out.getNumSamples();
        const int channels = juce::jmin (2, out.getNumChannels());
        for (int done = 0; done < n; done += kBlockSize)
        {
            const int count = juce::jmin (kBlockSize, n - done);
            const juce::int64 blockStart = start + done;
            for (size_t i = 0; i < snap.tracks.size(); ++i)
                if (snap.tracks[i].kind == TrackKind::Bus && i < snap.busInputs.size())
                    snap.busInputs[i].clear (0, count);

            for (int tiInt : snap.topoOrder)
            {
                if (! juce::isPositiveAndBelow (tiInt, (int) snap.tracks.size()))
                    continue;
                const size_t ti = (size_t) tiInt;
                const auto& st = snap.tracks[ti];
                const bool isBus = (st.kind == TrackKind::Bus);
                const bool isAudible = ti < snap.audibleScratch.size() && snap.audibleScratch[ti];
                if (! isAudible || (! isBus && ! (st.frozen && st.frozenAudio != nullptr) && st.clips.empty() && st.midiClips.empty() && ! st.isMidi))
                {
                    if (ti < fallbackSynths.size())
                        fallbackSynths[ti].reset();
                    continue;
                }
                if (isBus)
                {
                    snapshotScratch.clear (0, count);
                    if (ti < snap.busInputs.size())
                        for (int ch = 0; ch < 2; ++ch)
                            snapshotScratch.copyFrom (ch, 0, snap.busInputs[ti], ch, 0, count);
                    if (ti < fallbackSynths.size())
                        fallbackSynths[ti].reset();
                }
                else if (st.frozen && st.frozenAudio != nullptr)
                {
                    copyFrozenSlice (*st.frozenAudio, blockStart, count, snapshotScratch);
                    if (ti < fallbackSynths.size())
                        fallbackSynths[ti].reset();
                }
                else
                {
                    mixClipList (st.clips, blockStart, count, snapshotScratch);
                    if (st.isMidi && ! st.hasMidiPlugin && ti < fallbackSynths.size())
                    {
                        auto& synth = fallbackSynths[ti];
                        if (snap.sampleRate > 0.0 && std::abs (synth.getSampleRate() - snap.sampleRate) > 0.5)
                            synth.prepare (snap.sampleRate);
                        snapshotMidiScratch.clear();
                        collectMidiEvents (st.midiClips, snap.tempo, blockStart, count, snapshotMidiScratch, ! continuing && done == 0, &synth);
                        if (st.isMidi && st.armed)
                            appendLiveMidiSlice (liveMidiScratch, done, count, snapshotMidiScratch);
                        juce::AudioBuffer<float> view (snapshotScratch.getArrayOfWritePointers(), 2, count);
                        synth.process (view, snapshotMidiScratch);
                    }
                    else if (ti < fallbackSynths.size())
                    {
                        fallbackSynths[ti].reset();
                    }
                }
                for (const auto& snd : st.sends)
                {
                    if (! snd.preFader || snd.mute) continue;
                    if (juce::isPositiveAndBelow (snd.targetTrack, (int) snap.tracks.size())
                        && snap.tracks[(size_t) snd.targetTrack].kind == TrackKind::Bus
                        && (size_t) snd.targetTrack < snap.busInputs.size())
                    {
                        const float sg = juce::Decibels::decibelsToGain (snd.levelDb, -100.0f);
                        for (int ch = 0; ch < 2; ++ch)
                            snap.busInputs[(size_t) snd.targetTrack].addFrom (ch, 0, snapshotScratch, ch, 0, count, sg);
                    }
                }
                if (ti < snap.postScratches.size())
                {
                    snap.postScratches[ti].clear (0, count);
                    mixTrackBlock (snapshotScratch, snap.postScratches[ti], 0, count, blockStart, st.gainDb, st.pan,
                                   findActiveLane (st.automation, "volume"), findActiveLane (st.automation, "pan"));
                    for (const auto& snd : st.sends)
                    {
                        if (snd.preFader || snd.mute) continue;
                        if (juce::isPositiveAndBelow (snd.targetTrack, (int) snap.tracks.size())
                            && snap.tracks[(size_t) snd.targetTrack].kind == TrackKind::Bus
                            && (size_t) snd.targetTrack < snap.busInputs.size())
                        {
                            const float sg = juce::Decibels::decibelsToGain (snd.levelDb, -100.0f);
                            for (int ch = 0; ch < 2; ++ch)
                                snap.busInputs[(size_t) snd.targetTrack].addFrom (ch, 0, snap.postScratches[ti], ch, 0, count, sg);
                        }
                    }
                    if (juce::isPositiveAndBelow (st.outputTrack, (int) snap.tracks.size())
                        && snap.tracks[(size_t) st.outputTrack].kind == TrackKind::Bus
                        && (size_t) st.outputTrack < snap.busInputs.size())
                    {
                        for (int ch = 0; ch < 2; ++ch)
                            snap.busInputs[(size_t) st.outputTrack].addFrom (ch, 0, snap.postScratches[ti], ch, 0, count, 1.0f);
                    }
                    else
                    {
                        for (int ch = 0; ch < channels; ++ch)
                            out.addFrom (ch, done, snap.postScratches[ti], ch, 0, count, 1.0f);
                    }
                }
                else
                {
                    mixTrackBlock (snapshotScratch, out, done, count, blockStart, st.gainDb, st.pan,
                                   findActiveLane (st.automation, "volume"), findActiveLane (st.automation, "pan"));
                }
            }
        }
        addMetronomeWith (out, start, snap.tempo, snap.metronomeOn);
        out.applyGain (juce::Decibels::decibelsToGain (snap.masterDb, -100.0f));
        if (channels > 0)
        {
            const float mL = out.getMagnitude (0, 0, n);
            if (std::isfinite (mL)) masterPeakL = juce::jmax (masterPeakL.load(), mL);
        }
        if (channels > 1)
        {
            const float mR = out.getMagnitude (1, 0, n);
            if (std::isfinite (mR)) masterPeakR = juce::jmax (masterPeakR.load(), mR);
        }
    }

    // Processes one chain slot in place, blending dry and wet when wet < 0.9999f. Real-time safe (uses pre-allocated dryScratch).
    static void processSlot (Slot& s, juce::AudioBuffer<float>& buf, juce::MidiBuffer& midi)
    {
        if (s.bypass || s.instance == nullptr)
            return;
        const float w = juce::jlimit (0.0f, 1.0f, s.wet);
        if (w >= 0.9999f)
        {
            s.instance->processBlock (buf, midi);
            return;
        }
        const int n = buf.getNumSamples();
        const int dryCh = juce::jmin (2, buf.getNumChannels(), s.dryScratch.getNumChannels());
        if (n <= 0 || dryCh <= 0 || s.dryScratch.getNumSamples() < n)
        {
            s.instance->processBlock (buf, midi);
            return;
        }
        for (int ch = 0; ch < dryCh; ++ch)
            s.dryScratch.copyFrom (ch, 0, buf, ch, 0, n);
        s.instance->processBlock (buf, midi);
        const float dryGain = 1.0f - w;
        for (int ch = 0; ch < dryCh; ++ch)
        {
            const float* dry = s.dryScratch.getReadPointer (ch);
            float* dst = buf.getWritePointer (ch);
            for (int i = 0; i < n; ++i)
                dst[i] = dry[i] * dryGain + dst[i] * w;
        }
    }

    // Copies [start, start + n) of `frozenAudio` into `dest` (cleared first, clamped to buffer length). Audio-thread safe.
    static void copyFrozenSlice (const juce::AudioBuffer<float>& frozenAudio, juce::int64 start, int n, juce::AudioBuffer<float>& dest)
    {
        const int clearN = juce::jmin (n, dest.getNumSamples());
        if (clearN <= 0)
            return;
        dest.clear (0, clearN);
        const int audioLen = frozenAudio.getNumSamples();
        const int dstCh = dest.getNumChannels();
        const int srcCh = frozenAudio.getNumChannels();
        if (audioLen <= 0 || dstCh <= 0 || srcCh <= 0 || start >= (juce::int64) audioLen || start + clearN <= 0)
            return;
        const juce::int64 srcStart = juce::jmax ((juce::int64) 0, start);
        const juce::int64 srcEnd = juce::jmin ((juce::int64) audioLen, start + clearN);
        const int count = (int) (srcEnd - srcStart);
        const int dstOffset = (int) (srcStart - start);
        if (count <= 0)
            return;
        const int chs = juce::jmin (2, dstCh, srcCh);
        for (int ch = 0; ch < chs; ++ch)
            dest.copyFrom (ch, dstOffset, frozenAudio, ch, (int) srcStart, count);
        if (chs == 1 && dstCh >= 2)
            dest.copyFrom (1, dstOffset, dest, 0, dstOffset, count);
    }

    // The block [start, start + n) of the track through its clips and its chain, left in track.scratch. Silent outside the audio.
    void runChain (Track& t, juce::int64 start, int n, const juce::MidiBuffer* liveMidi = nullptr, int liveOffset = 0)
    {
        if (n <= 0)
            return;
        if (t.frozen && t.frozenAudio != nullptr)
        {
            copyFrozenSlice (*t.frozenAudio, start, n, t.scratch);
        }
        else
        {
            midiScratch.clear();
            if (t.isBus())
            {
                t.scratch.clear (0, n);
                for (int ch = 0; ch < 2; ++ch)
                    t.scratch.copyFrom (ch, 0, t.busInput, ch, 0, n);
            }
            else
            {
                mixClips (t, start, n, t.scratch);
                if (! rendering() && t.pendingNoteFlush)
                {
                    t.activeNotes.appendNoteOffs (midiScratch, 0);
                    t.activeNotes.reset();
                    t.pendingNoteFlush = false;
                }
                collectMidiEvents (t.midiClips, tempo, start, n, midiScratch);
                if (liveMidi != nullptr)
                    appendLiveMidiSlice (*liveMidi, liveOffset, n, midiScratch);
                if (! rendering())
                    t.activeNotes.updateFromBuffer (midiScratch);
                if (t.instrument != nullptr && t.instrument->instance != nullptr && ! t.instrument->bypass)
                {
                    if (t.scratch.getNumChannels() >= 4)
                    {
                        t.scratch.clear (2, 0, n);
                        t.scratch.clear (3, 0, n);
                    }
                    juce::AudioBuffer<float> instView (t.scratch.getArrayOfWritePointers(), t.scratch.getNumChannels(), n);
                    processSlot (*t.instrument, instView, midiScratch);
                }
                else
                {
                    bool pluginAcceptsMidi = false;
                    for (auto& s : t.chain)
                    {
                        if (s->instance != nullptr && ! s->bypass && s->instance->acceptsMidi())
                        {
                            pluginAcceptsMidi = true;
                            break;
                        }
                    }
                    if (t.isMidi && t.synth != nullptr && ! pluginAcceptsMidi)
                    {
                        juce::AudioBuffer<float> synthView (t.scratch.getArrayOfWritePointers(), t.scratch.getNumChannels(), n);
                        t.synth->process (synthView, midiScratch);
                    }
                }
            }
            juce::AudioBuffer<float> view (t.scratch.getArrayOfWritePointers(), t.scratch.getNumChannels(), n);
            for (const auto& lane : t.automation)
            {
                if (! lane.enabled || lane.points.empty() || lane.slotIndex < 0 || lane.paramIndex < 0)
                    continue;
                if (lane.slotIndex < (int) t.chain.size())
                {
                    auto* inst = t.chain[(size_t) lane.slotIndex]->instance.get();
                    if (inst != nullptr)
                    {
                        const auto& params = inst->getParameters();
                        if (juce::isPositiveAndBelow (lane.paramIndex, params.size()) && params[lane.paramIndex] != nullptr)
                            params[lane.paramIndex]->setValue (lane.evaluateAt (start));
                    }
                }
            }
            for (auto& s : t.chain)
            {
                if (t.scratch.getNumChannels() >= 4)
                {
                    const int sc = s->sidechainTrack;
                    if (s->instance != nullptr && s->instance->getTotalNumInputChannels() >= 4
                        && juce::isPositiveAndBelow (sc, (int) tracks.size()))
                    {
                        const auto& scBuf = tracks[(size_t) sc]->postScratch;
                        const int scCh = juce::jmin (2, scBuf.getNumChannels());
                        for (int ch = 0; ch < scCh; ++ch)
                            t.scratch.copyFrom (2 + ch, 0, scBuf, ch, 0, n);
                        for (int ch = scCh; ch < 2; ++ch)
                            t.scratch.clear (2 + ch, 0, n);
                    }
                    else
                    {
                        t.scratch.clear (2, 0, n);
                        t.scratch.clear (3, 0, n);
                    }
                }
                processSlot (*s, view, midiScratch);
            }
        }
        if (t.delaySamples > 0 && t.delayRing.getNumSamples() == t.delaySamples && ! rendering() && n > 0)
        {
            juce::AudioBuffer<float> view (t.scratch.getArrayOfWritePointers(), t.scratch.getNumChannels(), n);
            const int d = t.delaySamples;
            t.delayDirty = true;
            if (n <= kBlockSize)
            {
                std::array<float, kBlockSize> tmp;
                const int pos = t.delayPos;
                if (n <= d)
                {
                    const int first = juce::jmin (n, d - pos);
                    const int second = n - first;
                    for (int ch = 0; ch < 2; ++ch)
                    {
                        auto* data = view.getWritePointer (ch);
                        auto* ring = t.delayRing.getWritePointer (ch);
                        juce::FloatVectorOperations::copy (tmp.data(), data, n);
                        juce::FloatVectorOperations::copy (data, ring + pos, first);
                        if (second > 0)
                            juce::FloatVectorOperations::copy (data + first, ring, second);
                        juce::FloatVectorOperations::copy (ring + pos, tmp.data(), first);
                        if (second > 0)
                            juce::FloatVectorOperations::copy (ring, tmp.data() + first, second);
                    }
                    const int next = pos + n;
                    t.delayPos = next >= d ? next - d : next;
                }
                else
                {
                    const int firstOut = d - pos;
                    const int newPos = (pos + n) % d;
                    const int firstRing = d - newPos;
                    const int tailStart = n - d;
                    for (int ch = 0; ch < 2; ++ch)
                    {
                        auto* data = view.getWritePointer (ch);
                        auto* ring = t.delayRing.getWritePointer (ch);
                        juce::FloatVectorOperations::copy (tmp.data(), data, n);
                        juce::FloatVectorOperations::copy (data, ring + pos, firstOut);
                        if (pos > 0)
                            juce::FloatVectorOperations::copy (data + firstOut, ring, pos);
                        juce::FloatVectorOperations::copy (data + d, tmp.data(), tailStart);
                        juce::FloatVectorOperations::copy (ring + newPos, tmp.data() + tailStart, firstRing);
                        if (newPos > 0)
                            juce::FloatVectorOperations::copy (ring, tmp.data() + tailStart + firstRing, newPos);
                    }
                    t.delayPos = newPos;
                }
            }
            else
            {
                for (int i = 0; i < n; ++i)
                {
                    for (int ch = 0; ch < 2; ++ch)
                    {
                        auto* ring = t.delayRing.getWritePointer (ch);
                        const float in = view.getSample (ch, i);
                        view.setSample (ch, i, ring[t.delayPos]);
                        ring[t.delayPos] = in;
                    }
                    t.delayPos = (t.delayPos + 1) % d;
                }
            }
        }
    }

    bool rendering() const { return offline; }
    bool offline = false;
    struct OfflineScope
    {
        explicit OfflineScope (Session& s) : session (s), ownsScope (! s.offline)
        {
            if (ownsScope) { session.offline = true; session.prepareLocked (true); }
        }
        ~OfflineScope()
        {
            if (ownsScope) { session.offline = false; session.prepareLocked (false); }
        }
        Session& session;
        bool ownsScope = false;
    };

    std::unique_ptr<juce::AudioFormatWriter> makeWriter (const juce::File& file, int bits, juce::String& error)
    {
        file.deleteFile();
        std::unique_ptr<juce::OutputStream> stream (file.createOutputStream());
        if (stream == nullptr) { error = "Cannot write " + file.getFullPathName(); return nullptr; }
        juce::WavAudioFormat wav;
        auto writer = wav.createWriterFor (stream, juce::AudioFormatWriter::Options {}.withSampleRate (sampleRate).withNumChannels (2).withBitsPerSample (bits == 16 || bits == 24 ? bits : 32));
        if (writer == nullptr) error = "Cannot create a WAV writer for " + file.getFileName();
        return writer;
    }
};
}
