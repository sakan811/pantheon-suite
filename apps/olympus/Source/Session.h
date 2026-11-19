// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 The Pantheon authors

#pragma once

#include "Catalog.h"
#include "Tempo.h"

#include <juce_audio_formats/juce_audio_formats.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <functional>
#include <iterator>
#include <map>
#include <memory>

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
// Threads: everything that changes the tracks, a chain or the timeline holds `lock`; the audio callback of the window tries it and plays
// silence for that block if it cannot have it. Offline rendering holds it throughout. The audio pool is touched on the message thread only.
namespace olympus
{
inline constexpr int kProjectVersion = 2;
inline constexpr int kBlockSize = 512;

struct Slot
{
    juce::String plugin;                                 // the plugin's name, as the catalog knows it
    bool bypass = false;
    std::unique_ptr<juce::AudioPluginInstance> instance; // null while the plugin is not installed on this machine
    juce::MemoryBlock savedState;                        // what a missing plugin had, kept so a project that is saved again does not lose it

    int latency() const { return instance != nullptr && ! bypass ? instance->getLatencySamples() : 0; }
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

struct Track
{
    juce::String name;
    std::vector<Clip> clips;                             // may be empty: an empty track is valid
    float gainDb = 0.0f;                                 // the fader
    float pan = 0.0f;                                    // -1 (left) .. 1 (right), a balance: see Session::panGains
    bool mute = false, solo = false;
    std::vector<std::unique_ptr<Slot>> chain;
    std::atomic<float> peakL { 0.0f }, peakR { 0.0f };   // post fader and pan; the audio thread keeps the max, the display resets them

    // The sum of the chain's reported latencies.
    int latency() const
    {
        int sum = 0;
        for (auto& s : chain) sum += s->latency();
        return sum;
    }
    // The end of the last clip (0 with none).
    juce::int64 length() const
    {
        juce::int64 n = 0;
        for (auto& c : clips) n = juce::jmax (n, c.end());
        return n;
    }

    // for the audio thread, set under the session's lock
    juce::AudioBuffer<float> scratch;
    juce::AudioBuffer<float> delayRing;
    int delaySamples = 0, delayPos = 0;
    bool delayDirty = false;                             // the ring holds audio from a block that ran (cleared when the track is skipped)
};

struct RenderOptions
{
    juce::File folder;
    double tailSeconds = 0.0; // after the audio ends, so a reverb or delay can ring out
    bool writeMix = true;     // mix.wav, the sum of the stems, with the master gain
    int bits = 32;            // 16, 24 or 32 (32 is float: it cannot clip, and stems are for editing later)
};

struct RenderResult
{
    struct Stem { juce::String track, file; double peakDb = -200.0; };
    std::vector<Stem> stems;
    juce::String mixFile;
    double mixPeakDb = -200.0;
    double seconds = 0.0;
};

// What an edit changes, copied out and back for undo. Clips share their audio, so a copy is cheap.
struct EditState
{
    std::vector<std::vector<Clip>> clips; // per track
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
};

class Session
{
public:
    Catalog catalog;
    mutable juce::CriticalSection lock;
    std::vector<std::unique_ptr<Track>> tracks;
    double sampleRate = 0.0; // 0 until the first clip is added
    juce::File projectFile;
    juce::String name { "Untitled" };
    bool dirty = false;
    juce::StringArray warnings; // what the last load could not do

    // ---- the timeline ---------------------------------------------------------------------------------------------------
    TempoMap tempo;                 // tempo.sampleRate is the session's rate whenever that is set
    float masterDb = 0.0f;          // a gain on the sum of the tracks
    std::atomic<float> masterPeakL { 0.0f }, masterPeakR { 0.0f }; // max since the display last read them
    std::vector<Marker> markers;    // sorted by start
    juce::int64 loopStart = 0, loopEnd = 0;
    bool loopOn = false;
    bool metronomeOn = false;       // a click on every beat, a higher one on the bar; not in the stems
    bool snapOn = true;             // view settings, saved with the project
    Grid grid = Grid::Quarter;

    Session() { formats.registerBasicFormats(); }
    ~Session()
    {
        for (auto& t : tracks)
            for (auto& s : t->chain)
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
        dirty = true;
        return (int) tracks.size() - 1;
    }

    // Appends a clip of the file to a track, starting at `startSample` on the timeline; the whole file, at its own place in the pool.
    // Returns the clip's index on the track, -1 with the reason in `error`.
    int addClip (int track, const juce::File& file, juce::int64 startSample, juce::String& error)
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
        Clip clip;
        clip.name = file.getFileNameWithoutExtension();
        clip.file = file;
        clip.audio = audio;
        clip.start = juce::jmax ((juce::int64) 0, startSample);
        clip.length = audio->getNumSamples();
        auto& clips = tracks[(size_t) track]->clips;
        clips.push_back (std::move (clip));
        dirty = true;
        return (int) clips.size() - 1;
    }

    // The audio of a file, shared with every clip that plays it: a file already in the pool is not read again. The first file sets the
    // session's rate (and tempo.sampleRate). Null, with the reason, if the file cannot be read.
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
                setSampleRateLocked (rate);
        }
        return audio;
    }

    bool removeTrack (int index, juce::String& error)
    {
        const juce::ScopedLock sl (lock);
        if (! juce::isPositiveAndBelow (index, (int) tracks.size())) { error = "No track " + juce::String (index); return false; }
        for (auto& s : tracks[(size_t) index]->chain)
            if (s->instance != nullptr) s->instance->setPlayHead (nullptr);
        tracks.erase (tracks.begin() + index);
        updateLatenciesLocked();
        dirty = true;
        return true;
    }

    // The track a name or number means (a number, or a name in any case); -1 with the reason in `error`.
    int findTrack (const juce::var& which, juce::String& error) const
    {
        if (which.isString() && which.toString().containsOnly ("0123456789") == false)
        {
            for (int i = 0; i < (int) tracks.size(); ++i)
                if (tracks[(size_t) i]->name.equalsIgnoreCase (which.toString().trim())) return i;
            error = "No track called '" + which.toString() + "'";
            return -1;
        }
        const int i = which.isString() ? which.toString().getIntValue() : (int) which;
        if (juce::isPositiveAndBelow (i, (int) tracks.size())) return i;
        error = "No track " + juce::String (i) + " (there are " + juce::String ((int) tracks.size()) + ", numbered from 0)";
        return -1;
    }

    // ---- markers --------------------------------------------------------------------------------------------------------
    // Adds a marker, kept sorted. Returns its index.
    int addMarker (const juce::String& markerName, juce::int64 sample)
    {
        const juce::ScopedLock sl (lock);
        Marker m { markerName, juce::jmax ((juce::int64) 0, sample) };
        auto at = std::upper_bound (markers.begin(), markers.end(), m.start, [] (juce::int64 s, const Marker& o) { return s < o.start; });
        const int index = (int) (at - markers.begin());
        markers.insert (at, std::move (m));
        dirty = true;
        return index;
    }

    bool removeMarker (int index, juce::String& error)
    {
        const juce::ScopedLock sl (lock);
        if (! juce::isPositiveAndBelow (index, (int) markers.size())) { error = "No marker " + juce::String (index); return false; }
        markers.erase (markers.begin() + index);
        dirty = true;
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
        return true;
    }

    // ---- chains ---------------------------------------------------------------------------------------------------------
    // Puts a new plugin (its own defaults) in the chain at `position` (-1 = the end).
    bool addPlugin (int track, const juce::String& query, int position, juce::String& error)
    {
        if (! juce::isPositiveAndBelow (track, (int) tracks.size())) { error = "No track " + juce::String (track); return false; }
        const auto exact = catalog.resolve (query, error);
        if (exact.isEmpty()) return false;
        auto slot = makeSlot (exact, nullptr, error);
        if (slot == nullptr) return false;
        const juce::ScopedLock sl (lock);
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
        auto& chain = tracks[(size_t) track]->chain;
        if (! juce::isPositiveAndBelow (slot, (int) chain.size())) { error = "No plugin " + juce::String (slot) + " on that track"; return false; }
        if (chain[(size_t) slot]->instance != nullptr) chain[(size_t) slot]->instance->setPlayHead (nullptr);
        chain.erase (chain.begin() + slot);
        updateLatenciesLocked();
        dirty = true;
        return true;
    }

    bool movePlugin (int track, int from, int to, juce::String& error)
    {
        const juce::ScopedLock sl (lock);
        if (! juce::isPositiveAndBelow (track, (int) tracks.size())) { error = "No track " + juce::String (track); return false; }
        auto& chain = tracks[(size_t) track]->chain;
        if (! juce::isPositiveAndBelow (from, (int) chain.size()) || ! juce::isPositiveAndBelow (to, (int) chain.size())) { error = "No such place in the chain"; return false; }
        auto moved = std::move (chain[(size_t) from]);
        chain.erase (chain.begin() + from);
        chain.insert (chain.begin() + to, std::move (moved));
        dirty = true;
        return true;
    }

    bool setBypass (int track, int slot, bool bypass, juce::String& error)
    {
        const juce::ScopedLock sl (lock);
        if (auto* s = slotAt (track, slot, error)) { s->bypass = bypass; updateLatenciesLocked(); dirty = true; return true; }
        return false;
    }

    // Replaces the chain with new plugins (their defaults), in this order. All names are checked first: a wrong one changes nothing.
    bool setChain (int track, const juce::StringArray& plugins, juce::String& error)
    {
        if (! juce::isPositiveAndBelow (track, (int) tracks.size())) { error = "No track " + juce::String (track); return false; }
        std::vector<std::unique_ptr<Slot>> fresh;
        for (const auto& p : plugins)
        {
            const auto exact = catalog.resolve (p, error);
            if (exact.isEmpty()) return false;
            auto slot = makeSlot (exact, nullptr, error);
            if (slot == nullptr) return false;
            fresh.push_back (std::move (slot));
        }
        const juce::ScopedLock sl (lock);
        auto& chain = tracks[(size_t) track]->chain;
        for (auto& s : chain)
            if (s->instance != nullptr) s->instance->setPlayHead (nullptr);
        chain = std::move (fresh);
        updateLatenciesLocked();
        dirty = true;
        return true;
    }

    // Gives each of `to` the same chain as `from`, with the same settings (a copy of every plugin's state).
    bool copyChain (int from, const std::vector<int>& to, juce::String& error)
    {
        if (! juce::isPositiveAndBelow (from, (int) tracks.size())) { error = "No track " + juce::String (from); return false; }
        for (int t : to)
            if (! juce::isPositiveAndBelow (t, (int) tracks.size())) { error = "No track " + juce::String (t); return false; }
        // Every new chain is built first: a plugin that cannot be made leaves all the targets as they were.
        std::vector<std::pair<int, std::vector<std::unique_ptr<Slot>>>> built;
        for (int t : to)
        {
            if (t == from) continue;
            std::vector<std::unique_ptr<Slot>> fresh;
            for (auto& s : tracks[(size_t) from]->chain)
            {
                juce::MemoryBlock state = stateOf (*s);
                auto slot = makeSlot (s->plugin, &state, error);
                if (slot == nullptr) return false;
                slot->bypass = s->bypass;
                fresh.push_back (std::move (slot));
            }
            built.emplace_back (t, std::move (fresh));
        }
        const juce::ScopedLock sl (lock);
        for (auto& b : built)
        {
            for (auto& s : tracks[(size_t) b.first]->chain)
                if (s->instance != nullptr) s->instance->setPlayHead (nullptr);
            tracks[(size_t) b.first]->chain = std::move (b.second);
        }
        updateLatenciesLocked();
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

    // ---- time -----------------------------------------------------------------------------------------------------------
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
        int m = 0;
        for (auto& t : tracks) m = juce::jmax (m, t->latency());
        return m;
    }

    // Whether the track is heard: not muted, and (when anything is soloed) soloed.
    bool audible (const Track& t) const
    {
        bool anySolo = false;
        for (auto& o : tracks) anySolo = anySolo || o->solo;
        return ! t.mute && (! anySolo || t.solo);
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
    static void mixClips (const Track& t, juce::int64 start, int n, juce::AudioBuffer<float>& dest)
    {
        dest.clear (0, n);
        const int channels = juce::jmin (2, dest.getNumChannels());
        for (const auto& c : t.clips)
        {
            if (c.mute || c.audio == nullptr || c.length <= 0)
                continue;
            const juce::int64 from = juce::jmax (start, c.start), to = juce::jmin (start + n, c.end());
            const auto& src = *c.audio;
            const float gain = juce::Decibels::decibelsToGain (c.gainDb, -100.0f);
            for (juce::int64 s = from; s < to; ++s)
            {
                const juce::int64 local = s - c.start;
                const juce::int64 index = c.offset + local;
                if (index < 0 || index >= src.getNumSamples())
                    continue;
                float env = gain;
                if (local < c.fadeIn)
                    env *= (float) local / (float) c.fadeIn;
                const juce::int64 remaining = c.length - local;
                if (remaining < c.fadeOut)
                    env *= (float) remaining / (float) c.fadeOut;
                for (int ch = 0; ch < channels; ++ch)
                    dest.addSample (ch, (int) (s - start), src.getSample (juce::jmin (ch, src.getNumChannels() - 1), (int) index) * env);
            }
        }
    }

    // ---- playing --------------------------------------------------------------------------------------------------------
    // Sets every plugin up for `sampleRate` and the block size, for real time or (nonRealtime) for rendering.
    void prepare (bool nonRealtime)
    {
        const juce::ScopedLock sl (lock);
        prepareLocked (nonRealtime);
    }

    // `out` is overwritten with one block [start, start + n) of the session: n <= kBlockSize, two channels. No loop here: the caller splits
    // blocks at the loop's end. Used on the audio thread.
    void processBlock (juce::AudioBuffer<float>& out, juce::int64 start)
    {
        const int n = out.getNumSamples();
        out.clear();
        const juce::ScopedTryLock sl (lock);
        if (! sl.isLocked())
            return;
        publishHead (start, true);
        const int channels = juce::jmin (2, out.getNumChannels());
        for (auto& t : tracks)
        {
            if (! audible (*t) || t->clips.empty())
            {
                // skipped: its delay must not come back with stale audio when it plays again
                if (t->delayDirty)
                {
                    t->delayRing.clear();
                    t->delayPos = 0;
                    t->delayDirty = false;
                }
                continue;
            }
            runChain (*t, start, n);
            float left = 0.0f, right = 0.0f;
            trackGains (*t, left, right);
            for (int ch = 0; ch < channels; ++ch)
                out.addFrom (ch, 0, t->scratch, ch, 0, n, ch == 0 ? left : right);
            t->peakL = juce::jmax (t->peakL.load(), t->scratch.getMagnitude (0, 0, n) * left);
            t->peakR = juce::jmax (t->peakR.load(), t->scratch.getMagnitude (1, 0, n) * right);
        }
        addMetronome (out, start);
        out.applyGain (juce::Decibels::decibelsToGain (masterDb, -100.0f));
        if (channels > 0) masterPeakL = juce::jmax (masterPeakL.load(), out.getMagnitude (0, 0, n));
        if (channels > 1) masterPeakR = juce::jmax (masterPeakR.load(), out.getMagnitude (1, 0, n));
    }

    // ---- rendering ------------------------------------------------------------------------------------------------------
    // Writes every audible track as a WAV of its own (named `NN-track.wav`, after its clips, chain, fader and pan, aligned to the audio:
    // the chain's latency is taken off the front; no master gain), and the mix (the stems summed, with the master gain). All tracks run
    // together, block by block, which is what a plugin that listens to the others (Hestia's bus) needs. `progress` gets 0..1 and returns
    // false to stop. Covers [0, length() + tail).
    bool renderStems (const RenderOptions& options, RenderResult& result, juce::String& error, const std::function<bool (float)>& progress = {})
    {
        const juce::ScopedLock sl (lock);
        if (tracks.empty()) { error = "There are no tracks."; return false; }
        if (sampleRate <= 0.0) { error = "Nothing to render: no audio has been added."; return false; }
        // The tail is at most a minute: a reverb or delay ringing out, not a way to write hours of silence.
        const double tail = std::isfinite (options.tailSeconds) ? juce::jlimit (0.0, 60.0, options.tailSeconds) : 0.0;
        const juce::int64 stemLength = length() + (juce::int64) std::llround (tail * sampleRate);
        if (stemLength > (juce::int64) (3.0 * 3600.0 * sampleRate)) { error = "The render would be longer than three hours."; return false; }
        if (! options.folder.createDirectory().wasOk()) { error = "Cannot make the folder " + options.folder.getFullPathName(); return false; }
        const OfflineScope offlineScope (*this); // plugins in render mode for this function; back to real time on every way out

        struct Out { Track* track; std::unique_ptr<juce::AudioFormatWriter> writer; juce::File file; int latency; juce::int64 written = 0; float peak = 0.0f; };
        std::vector<Out> outs;
        int index = 0;
        for (auto& t : tracks)
        {
            ++index;
            if (! audible (*t)) continue;
            Out o;
            o.track = t.get();
            o.latency = t->latency();
            o.file = options.folder.getChildFile (juce::String (index).paddedLeft ('0', 2) + "-" + juce::File::createLegalFileName (t->name) + ".wav");
            o.writer = makeWriter (o.file, options.bits, error);
            if (o.writer == nullptr) { return false; }
            outs.push_back (std::move (o));
        }
        if (outs.empty()) { error = "No track is audible (all muted, or others soloed)."; return false; }

        juce::AudioBuffer<float> mix (2, options.writeMix ? (int) stemLength : 0);
        mix.clear();
        int totalLatency = 0;
        for (auto& o : outs) totalLatency = juce::jmax (totalLatency, o.latency);
        const juce::int64 end = stemLength + totalLatency;
        juce::AudioBuffer<float> block (2, kBlockSize);

        for (juce::int64 start = 0; start < end; start += kBlockSize)
        {
            const int n = (int) juce::jmin ((juce::int64) kBlockSize, end - start);
            publishHead (start, true);
            for (auto& o : outs)
            {
                runChain (*o.track, start, n);
                // this block holds output samples [start, start + n): sample j of it is stem sample j - latency
                const juce::int64 first = juce::jmax ((juce::int64) 0, (juce::int64) o.latency - start);
                const juce::int64 last = juce::jmin ((juce::int64) n, stemLength + o.latency - start);
                if (last <= first) continue;
                const int count = (int) (last - first);
                float left = 0.0f, right = 0.0f;
                trackGains (*o.track, left, right);
                block.setSize (2, count, false, false, true);
                for (int ch = 0; ch < 2; ++ch)
                {
                    block.copyFrom (ch, 0, o.track->scratch, ch, (int) first, count);
                    block.applyGain (ch, 0, count, ch == 0 ? left : right);
                    o.peak = juce::jmax (o.peak, block.getMagnitude (ch, 0, count));
                }
                if (! o.writer->writeFromAudioSampleBuffer (block, 0, count)) { error = "Writing " + o.file.getFileName() + " failed."; return false; }
                if (options.writeMix)
                {
                    const int dest = (int) (start + first - o.latency);
                    for (int ch = 0; ch < 2; ++ch)
                        mix.addFrom (ch, dest, block, ch, 0, count);
                }
            }
            if (progress && ! progress ((float) ((double) (start + n) / (double) end))) { error = "Stopped."; return false; }
        }
        for (auto& o : outs)
        {
            o.writer.reset();
            result.stems.push_back ({ o.track->name, o.file.getFullPathName(), o.peak > 0.0f ? (double) juce::Decibels::gainToDecibels (o.peak) : -200.0 });
        }
        if (options.writeMix)
        {
            mix.applyGain (juce::Decibels::decibelsToGain (masterDb, -100.0f));
            const auto file = options.folder.getChildFile ("mix.wav");
            auto writer = makeWriter (file, options.bits, error);
            if (writer == nullptr || ! writer->writeFromAudioSampleBuffer (mix, 0, mix.getNumSamples())) { if (error.isEmpty()) error = "Writing mix.wav failed."; return false; }
            writer.reset();
            result.mixFile = file.getFullPathName();
            result.mixPeakDb = (double) juce::Decibels::gainToDecibels (mix.getMagnitude (0, mix.getNumSamples()) > mix.getMagnitude (1, 0, mix.getNumSamples())
                                                                           ? mix.getMagnitude (0, mix.getNumSamples()) : mix.getMagnitude (1, 0, mix.getNumSamples()), -200.0f);
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
            e->setAttribute ("pan", (double) t->pan);
            e->setAttribute ("gainDb", (double) t->gainDb);
            e->setAttribute ("mute", t->mute);
            e->setAttribute ("solo", t->solo);
            for (auto& c : t->clips)
            {
                auto* k = e->createNewChildElement ("Clip");
                k->setAttribute ("name", c.name);
                k->setAttribute ("file", c.file.getRelativePathFrom (relativeTo.getParentDirectory()));
                k->setAttribute ("path", c.file.getFullPathName());
                k->setAttribute ("start", juce::String (c.start));
                k->setAttribute ("offset", juce::String (c.offset));
                k->setAttribute ("length", juce::String (c.length));
                k->setAttribute ("gainDb", (double) c.gainDb);
                k->setAttribute ("fadeIn", juce::String (c.fadeIn));
                k->setAttribute ("fadeOut", juce::String (c.fadeOut));
                k->setAttribute ("mute", c.mute);
            }
            for (auto& s : t->chain)
            {
                auto* c = e->createNewChildElement ("Slot");
                c->setAttribute ("plugin", s->plugin);
                c->setAttribute ("bypass", s->bypass);
                c->setAttribute ("state", stateOf (*s).toBase64Encoding());
            }
        }
        return xml;
    }

    bool save (const juce::File& file, juce::String& error)
    {
        const auto previousName = name;
        name = file.getFileNameWithoutExtension(); // the project is named after its file
        std::unique_ptr<juce::XmlElement> xml;
        {
            // every plugin's state is read here: not while processBlock runs beside it
            const juce::ScopedLock sl (lock);
            xml = toXml (file);
        }
        juce::TemporaryFile temp (file);
        if (! xml->writeTo (temp.getFile()) || ! temp.overwriteTargetFileWithTemporary())
        {
            name = previousName;
            error = "Cannot write " + file.getFullPathName();
            return false;
        }
        projectFile = file;
        dirty = false;
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

        juce::StringArray problems;
        pool.clear(); // the rate of the project may differ from the one the pool was read at
        double rate = xml->getDoubleAttribute ("sampleRate", 0.0);
        std::vector<std::unique_ptr<Track>> loaded;
        for (auto* e : xml->getChildWithTagNameIterator ("Track"))
        {
            auto t = std::make_unique<Track>();
            t->name = e->getStringAttribute ("name");
            t->gainDb = (float) juce::jlimit (-100.0, 24.0, e->getDoubleAttribute ("gainDb", 0.0));
            t->pan = (float) juce::jlimit (-1.0, 1.0, e->getDoubleAttribute ("pan", 0.0));
            t->mute = e->getBoolAttribute ("mute");
            t->solo = e->getBoolAttribute ("solo");
            const bool legacy = e->getChildByName ("Clip") == nullptr && (e->hasAttribute ("file") || e->hasAttribute ("path"));
            if (legacy)
                t->clips.push_back (readClip (*e, file.getParentDirectory(), rate, true, t->name, problems));
            else
                for (auto* c : e->getChildWithTagNameIterator ("Clip"))
                    t->clips.push_back (readClip (*c, file.getParentDirectory(), rate, false, t->name, problems));

            for (auto* c : e->getChildWithTagNameIterator ("Slot"))
            {
                juce::MemoryBlock state;
                state.fromBase64Encoding (c->getStringAttribute ("state"));
                juce::String problem2;
                auto slot = makeSlot (c->getStringAttribute ("plugin"), &state, problem2, rate);
                if (slot == nullptr)
                {
                    problems.add ("Track '" + t->name + "': " + problem2);
                    slot = std::make_unique<Slot>();
                    slot->plugin = c->getStringAttribute ("plugin");
                    slot->savedState = state;
                }
                slot->bypass = c->getBoolAttribute ("bypass");
                t->chain.push_back (std::move (slot));
            }
            loaded.push_back (std::move (t));
        }

        // the timeline
        std::vector<TempoPoint> points;
        for (auto* p : xml->getChildWithTagNameIterator ("Tempo"))
        {
            const double beat = juce::jmax (0.0, p->getDoubleAttribute ("beat", 0.0));
            const double bpm = juce::jlimit (TempoMap::kMinBpm, TempoMap::kMaxBpm, p->getDoubleAttribute ("bpm", 120.0));
            if (std::isfinite (beat) && std::isfinite (bpm))
                points.push_back ({ beat, bpm });
        }
        std::stable_sort (points.begin(), points.end(), [] (const TempoPoint& a, const TempoPoint& b) { return a.beat < b.beat; });
        if (points.empty() || points.front().beat > TempoMap::kBeatEps)
            points.insert (points.begin(), { 0.0, points.empty() ? 120.0 : points.front().bpm });
        TempoMap map;
        map.points.clear();
        for (const auto& p : points)
            if (map.points.empty() || p.beat > map.points.back().beat + TempoMap::kBeatEps)
                map.points.push_back (p);
        map.setSignature (juce::jlimit (1, 32, xml->getIntAttribute ("tsNum", 4)), xml->getIntAttribute ("tsDen", 4));

        std::vector<Marker> markerList;
        for (auto* m : xml->getChildWithTagNameIterator ("Marker"))
            markerList.push_back ({ m->getStringAttribute ("name"), juce::jmax ((juce::int64) 0, m->getStringAttribute ("sample", "0").getLargeIntValue()) });
        std::stable_sort (markerList.begin(), markerList.end(), [] (const Marker& a, const Marker& b) { return a.start < b.start; });

        const juce::ScopedLock sl (lock);
        for (auto& t : tracks)
            for (auto& s : t->chain)
                if (s->instance != nullptr) s->instance->setPlayHead (nullptr);
        tracks = std::move (loaded);
        setSampleRateLocked (rate);
        for (auto& t : tracks)
            for (auto& s : t->chain)
                if (s->instance != nullptr) s->instance->setPlayHead (&head);
        tempo = map;
        if (sampleRate > 0.0) tempo.sampleRate = sampleRate;
        markers = std::move (markerList);
        masterDb = (float) juce::jlimit (-100.0, 24.0, xml->getDoubleAttribute ("masterDb", 0.0));
        loopStart = juce::jmax ((juce::int64) 0, xml->getStringAttribute ("loopStart", "0").getLargeIntValue());
        loopEnd = juce::jmax ((juce::int64) 0, xml->getStringAttribute ("loopEnd", "0").getLargeIntValue());
        loopOn = xml->getBoolAttribute ("loopOn");
        metronomeOn = xml->getBoolAttribute ("metronome");
        snapOn = xml->getBoolAttribute ("snap", true);
        grid = (Grid) juce::jlimit (0, 5, xml->getIntAttribute ("grid", (int) Grid::Quarter));
        projectFile = file;
        name = xml->getStringAttribute ("name", file.getFileNameWithoutExtension());
        warnings = problems;
        prepareLocked (false);
        dirty = false;
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
        return e;
    }

    // Puts an edit back. Ignored when the number of tracks is not the one the edit was taken with (tracks are not undone).
    void restoreEdit (const EditState& e)
    {
        const juce::ScopedLock sl (lock);
        if (e.clips.size() != tracks.size() || e.mix.size() != tracks.size())
            return;
        for (size_t i = 0; i < tracks.size(); ++i)
        {
            auto& t = *tracks[i];
            t.clips = e.clips[i];
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
        dirty = true;
    }

    // An empty session under a name: no tracks, no audio pool, the timeline at its defaults. Plugins stop listening to the playhead.
    void newProject (const juce::String& projectName)
    {
        const juce::ScopedLock sl (lock);
        for (auto& t : tracks)
            for (auto& s : t->chain)
                if (s->instance != nullptr) s->instance->setPlayHead (nullptr);
        tracks.clear();
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
        dirty = false;
        warnings.clear();
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
            o->setProperty ("audio_found", ! t->clips.empty() && std::all_of (t->clips.begin(), t->clips.end(), [] (const Clip& c) { return c.audio != nullptr; }));
            o->setProperty ("seconds", seconds (t->length()));
            o->setProperty ("pan", (double) t->pan);
            o->setProperty ("gain_db", (double) t->gainDb);
            o->setProperty ("mute", t->mute);
            o->setProperty ("solo", t->solo);
            o->setProperty ("latency_samples", t->latency());

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

            juce::Array<juce::var> chain;
            int j = 0;
            for (auto& s : t->chain)
            {
                auto* c = new juce::DynamicObject();
                c->setProperty ("index", j++);
                c->setProperty ("plugin", s->plugin);
                c->setProperty ("installed", s->instance != nullptr);
                c->setProperty ("bypass", s->bypass);
                chain.add (juce::var (c));
            }
            o->setProperty ("chain", chain);
            list.add (juce::var (o));
        }
        root->setProperty ("tracks", list);
        return juce::var (root);
    }

    // Makes a slot for a plugin. With `state`, the plugin is given those settings. Null, with the reason, if the plugin cannot be made.
    std::unique_ptr<Slot> makeSlot (const juce::String& pluginName, const juce::MemoryBlock* state, juce::String& error, double rate = 0.0)
    {
        auto instance = catalog.create (pluginName, rate > 0.0 ? rate : (sampleRate > 0.0 ? sampleRate : 48000.0), kBlockSize, error);
        if (instance == nullptr)
        {
            if (error.isEmpty()) error = "Cannot create " + pluginName;
            return nullptr;
        }
        auto slot = std::make_unique<Slot>();
        slot->plugin = pluginName;
        configure (*instance, rate > 0.0 ? rate : (sampleRate > 0.0 ? sampleRate : 48000.0), false);
        if (state != nullptr && state->getSize() > 0)
            instance->setStateInformation (state->getData(), (int) state->getSize());
        instance->setPlayHead (&head);
        slot->instance = std::move (instance);
        return slot;
    }

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

private:
    juce::AudioFormatManager formats;

    // The audio pool: a file's buffer, shared by the clips that play it. Held weakly, so a buffer goes when its last clip does.
    // Message thread only; the audio thread never reads it.
    using Pool = std::map<juce::String, std::weak_ptr<const juce::AudioBuffer<float>>>;
    Pool pool;

    // The buffer of a file from the pool, or read now at `rate` (0 = the file's own rate, which is then written to `rate`).
    std::shared_ptr<const juce::AudioBuffer<float>> pooledAudio (const juce::File& file, double& rate, juce::String& error)
    {
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
        std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (file));
        if (reader == nullptr) { error = "Cannot read the audio file " + file.getFullPathName(); return false; }
        if (reader->lengthInSamples > (juce::int64) reader->sampleRate * 3600 * 2) { error = file.getFileName() + " is longer than two hours."; return false; }
        const int len = (int) reader->lengthInSamples;
        juce::AudioBuffer<float> raw (2, len);
        raw.clear();
        reader->read (&raw, 0, len, 0, true, true);
        if (reader->numChannels == 1)
            raw.copyFrom (1, 0, raw, 0, 0, len);
        const double rate = wantedRate > 0.0 ? wantedRate : reader->sampleRate;
        rateOut = rate;
        if (std::abs (rate - reader->sampleRate) < 0.5)
        {
            out = std::move (raw);
            return true;
        }
        const double ratio = reader->sampleRate / rate;
        const int outLen = (int) std::ceil ((double) len / ratio);
        out.setSize (2, outLen);
        for (int ch = 0; ch < 2; ++ch)
        {
            juce::LagrangeInterpolator resampler;
            resampler.process (ratio, raw.getReadPointer (ch), out.getWritePointer (ch), outLen, len, 0);
        }
        return true;
    }

    // One clip from a project element. A version 1 track is read as its own clip: the whole file at sample 0.
    Clip readClip (const juce::XmlElement& e, const juce::File& base, double& rate, bool legacy, const juce::String& trackName, juce::StringArray& problems)
    {
        Clip c;
        c.file = base.getChildFile (e.getStringAttribute ("file"));
        if (! c.file.existsAsFile())
            c.file = juce::File (e.getStringAttribute ("path"));
        c.name = legacy ? c.file.getFileNameWithoutExtension() : e.getStringAttribute ("name", c.file.getFileNameWithoutExtension());
        c.start = juce::jmax ((juce::int64) 0, e.getStringAttribute ("start", "0").getLargeIntValue());
        c.offset = juce::jmax ((juce::int64) 0, e.getStringAttribute ("offset", "0").getLargeIntValue());
        const juce::int64 savedLength = e.getStringAttribute ("length", "-1").getLargeIntValue();
        c.gainDb = (float) juce::jlimit (-100.0, 24.0, e.getDoubleAttribute ("gainDb", 0.0));
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
            // no audio to take the length from: keep the saved one, but no longer than the limit readAudio applies
            c.length = juce::jlimit ((juce::int64) 0, (juce::int64) (rate * 3600 * 2), savedLength);
            problems.add ("Track '" + trackName + "': " + problem);
        }
        return c;
    }

    void setSampleRateLocked (double r)
    {
        sampleRate = r;
        if (r > 0.0)
            tempo.sampleRate = r;
    }

    // Stereo in and out, the rate and block, the playhead; for a plugin that has been made or that must start from a clean state.
    void configure (juce::AudioPluginInstance& p, double rate, bool nonRealtime)
    {
        auto layout = p.getBusesLayout();
        if (p.getBusCount (true) > 0) layout.getMainInputChannelSet() = juce::AudioChannelSet::stereo();
        if (p.getBusCount (false) > 0) layout.getMainOutputChannelSet() = juce::AudioChannelSet::stereo();
        p.setBusesLayout (layout);
        p.setNonRealtime (nonRealtime);
        p.prepareToPlay (rate, kBlockSize);
    }

    // A track's buffers before it plays: two channels at the block size (the chain may need more; prepareLocked grows them).
    void makeTrackLocked (Track& t)
    {
        t.scratch.setSize (2, kBlockSize, false, true, false);
    }

    void prepareLocked (bool nonRealtime)
    {
        const double rate = sampleRate > 0.0 ? sampleRate : 48000.0;
        for (auto& t : tracks)
        {
            int channels = 2;
            for (auto& s : t->chain)
                if (s->instance != nullptr)
                {
                    s->instance->releaseResources();
                    configure (*s->instance, rate, nonRealtime);
                    s->instance->reset();
                    channels = juce::jmax (channels, s->instance->getTotalNumInputChannels(), s->instance->getTotalNumOutputChannels());
                }
            t->scratch.setSize (channels, kBlockSize, false, true, false);
        }
        updateLatenciesLocked();
    }

    // How long each track is delayed in real time so the tracks stay aligned with each other (the one with the most latency is not).
    void updateLatenciesLocked()
    {
        const int most = maxLatency();
        for (auto& t : tracks)
        {
            const int wanted = most - t->latency();
            if (wanted != t->delaySamples)
            {
                t->delaySamples = wanted;
                t->delayRing.setSize (2, juce::jmax (1, wanted), false, true, false);
                t->delayPos = 0;
            }
        }
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
    void addMetronome (juce::AudioBuffer<float>& out, juce::int64 start) const
    {
        if (! metronomeOn)
            return;
        const int n = out.getNumSamples();
        const double sr = tempo.sampleRate;
        const juce::int64 click = (juce::int64) std::llround (0.015 * sr);
        if (click <= 0)
            return;
        const int beatsInBar = juce::jmax (1, (int) std::lround (tempo.beatsPerBar()));
        const double beatsFrom = tempo.samplesToBeats ((double) (start - click));
        const double beatsTo = tempo.samplesToBeats ((double) (start + n));
        for (juce::int64 k = juce::jmax ((juce::int64) 0, (juce::int64) std::floor (beatsFrom)); (double) k <= beatsTo; ++k)
        {
            const juce::int64 onset = (juce::int64) std::llround (tempo.beatsToSamples ((double) k));
            const juce::int64 from = juce::jmax (start, onset), to = juce::jmin (start + n, onset + click);
            if (to <= from)
                continue;
            const bool bar = k % beatsInBar == 0;
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

    // The block [start, start + n) of the track through its clips and its chain, left in track.scratch. Silent outside the audio.
    void runChain (Track& t, juce::int64 start, int n)
    {
        mixClips (t, start, n, t.scratch);
        juce::AudioBuffer<float> view (t.scratch.getArrayOfWritePointers(), t.scratch.getNumChannels(), n);
        juce::MidiBuffer midi;
        for (auto& s : t.chain)
            if (s->instance != nullptr && ! s->bypass)
                s->instance->processBlock (view, midi);
        if (t.delaySamples > 0 && t.delayRing.getNumSamples() == t.delaySamples && ! rendering())
        {
            const int d = t.delaySamples;
            t.delayDirty = true;
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

    bool rendering() const { return offline; }
    bool offline = false;
    struct OfflineScope
    {
        explicit OfflineScope (Session& s) : session (s) { session.offline = true; session.prepareLocked (true); }
        ~OfflineScope() { session.offline = false; session.prepareLocked (false); }
        Session& session;
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
