// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 The Pantheon authors

#pragma once

#include "Catalog.h"

#include <juce_audio_formats/juce_audio_formats.h>

#include <atomic>
#include <cmath>
#include <memory>

// One session: tracks of audio, a chain of plugins on each, and what is done with them (play, render stems, save). No interface in
// here: the window, the MCP server and the tests all talk to this.
//
//   Track   an audio file read into memory at the session's sample rate (always two channels), a fader, mute, solo, and a chain
//   Slot    one plugin of a chain: a live instance, its settings are the instance's own state
//
// Threads: everything that changes the tracks or a chain holds `lock`; the audio callback of the window tries it and plays silence
// for that block if it cannot have it. Offline rendering holds it throughout.
namespace olympus
{
inline constexpr int kProjectVersion = 1;
inline constexpr int kBlockSize = 512;

struct Slot
{
    juce::String plugin;                                 // the plugin's name, as the catalog knows it
    bool bypass = false;
    std::unique_ptr<juce::AudioPluginInstance> instance; // null while the plugin is not installed on this machine
    juce::MemoryBlock savedState;                        // what a missing plugin had, kept so a project that is saved again does not lose it

    int latency() const { return instance != nullptr && ! bypass ? instance->getLatencySamples() : 0; }
};

struct Track
{
    juce::String name;
    juce::File file;
    juce::AudioBuffer<float> audio; // two channels at the session's rate; empty when the file is missing
    float gainDb = 0.0f;
    bool mute = false, solo = false;
    std::vector<std::unique_ptr<Slot>> chain;

    // The sum of the chain's reported latencies.
    int latency() const
    {
        int sum = 0;
        for (auto& s : chain) sum += s->latency();
        return sum;
    }
    int length() const { return audio.getNumSamples(); }

    // for the audio thread, set under the session's lock
    juce::AudioBuffer<float> scratch;
    juce::AudioBuffer<float> delayRing;
    int delaySamples = 0, delayPos = 0;
};

struct RenderOptions
{
    juce::File folder;
    double tailSeconds = 0.0; // after the audio ends, so a reverb or delay can ring out
    bool writeMix = true;     // mix.wav, the sum of the stems
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

class Session
{
public:
    Catalog catalog;
    juce::CriticalSection lock;
    std::vector<std::unique_ptr<Track>> tracks;
    double sampleRate = 0.0; // 0 until the first track is added
    juce::File projectFile;
    juce::String name { "Untitled" };
    bool dirty = false;
    juce::StringArray warnings; // what the last load could not do

    Session() { formats.registerBasicFormats(); }
    ~Session()
    {
        for (auto& t : tracks)
            for (auto& s : t->chain)
                if (s->instance != nullptr) s->instance->setPlayHead (nullptr);
    }

    // ---- tracks ---------------------------------------------------------------------------------------------------------
    bool addTrack (const juce::File& file, juce::String trackName, juce::String& error)
    {
        auto track = std::make_unique<Track>();
        track->file = file;
        track->name = trackName.isNotEmpty() ? trackName : file.getFileNameWithoutExtension();
        if (! loadAudio (*track, file, sampleRate > 0.0 ? sampleRate : 0.0, error))
            return false;
        const juce::ScopedLock sl (lock);
        if (sampleRate <= 0.0)
        {
            sampleRate = lastLoadedRate;
            prepareLocked (false);
        }
        tracks.push_back (std::move (track));
        updateLatenciesLocked();
        dirty = true;
        return true;
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
            const juce::ScopedLock sl (lock);
            for (auto& s : tracks[(size_t) t]->chain)
                if (s->instance != nullptr) s->instance->setPlayHead (nullptr);
            tracks[(size_t) t]->chain = std::move (fresh);
        }
        const juce::ScopedLock sl (lock);
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
    juce::int64 length() const
    {
        juce::int64 n = 0;
        for (auto& t : tracks) n = juce::jmax (n, (juce::int64) t->length());
        return n;
    }
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

    // ---- playing --------------------------------------------------------------------------------------------------------
    // Sets every plugin up for `sampleRate` and the block size, for real time or (nonRealtime) for rendering.
    void prepare (bool nonRealtime)
    {
        const juce::ScopedLock sl (lock);
        prepareLocked (nonRealtime);
    }

    // `out` is overwritten with the sum of the audible tracks for [start, start + n): n <= kBlockSize, two channels. Used on the audio thread.
    void processBlock (juce::AudioBuffer<float>& out, juce::int64 start)
    {
        const int n = out.getNumSamples();
        out.clear();
        const juce::ScopedTryLock sl (lock);
        if (! sl.isLocked())
            return;
        head.set (start, true);
        for (auto& t : tracks)
        {
            if (! audible (*t) || t->audio.getNumSamples() == 0)
            {
                continue;
            }
            runChain (*t, start, n);
            const auto gain = juce::Decibels::decibelsToGain (t->gainDb);
            for (int ch = 0; ch < juce::jmin (2, out.getNumChannels()); ++ch)
                out.addFrom (ch, 0, t->scratch, ch, 0, n, gain);
        }
    }

    // ---- rendering ------------------------------------------------------------------------------------------------------
    // Writes every audible track as a WAV of its own (named `NN-track.wav`, after its chain and fader, aligned to the audio: the chain's
    // latency is taken off the front), and the mix. All tracks run together, block by block, which is what a plugin that listens to the
    // others (Hestia's bus) needs. `progress` gets 0..1 and returns false to stop.
    bool renderStems (const RenderOptions& options, RenderResult& result, juce::String& error, const std::function<bool (float)>& progress = {})
    {
        if (tracks.empty()) { error = "There are no tracks."; return false; }
        if (! options.folder.createDirectory().wasOk()) { error = "Cannot make the folder " + options.folder.getFullPathName(); return false; }
        const juce::ScopedLock sl (lock);
        const OfflineScope offlineScope (*this); // plugins in render mode for this function; back to real time on every way out

        struct Out { Track* track; std::unique_ptr<juce::AudioFormatWriter> writer; juce::File file; int latency; juce::int64 written = 0; float peak = 0.0f; };
        std::vector<Out> outs;
        const juce::int64 stemLength = length() + (juce::int64) std::llround (options.tailSeconds * sampleRate);
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
            head.set (start, true);
            for (auto& o : outs)
            {
                runChain (*o.track, start, n);
                // this block holds output samples [start, start + n): sample j of it is stem sample j - latency
                const juce::int64 first = juce::jmax ((juce::int64) 0, (juce::int64) o.latency - start);
                const juce::int64 last = juce::jmin ((juce::int64) n, stemLength + o.latency - start);
                if (last <= first) continue;
                const int count = (int) (last - first);
                const auto gain = juce::Decibels::decibelsToGain (o.track->gainDb);
                block.setSize (2, count, false, false, true);
                for (int ch = 0; ch < 2; ++ch)
                {
                    block.copyFrom (ch, 0, o.track->scratch, ch, (int) first, count);
                    block.applyGain (ch, 0, count, gain);
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
    // Every slot's state is read from its plugin at this moment.
    std::unique_ptr<juce::XmlElement> toXml (const juce::File& relativeTo) const
    {
        auto xml = std::make_unique<juce::XmlElement> ("OlympusProject");
        xml->setAttribute ("version", kProjectVersion);
        xml->setAttribute ("name", name);
        xml->setAttribute ("sampleRate", sampleRate);
        for (auto& t : tracks)
        {
            auto* e = xml->createNewChildElement ("Track");
            e->setAttribute ("name", t->name);
            e->setAttribute ("file", t->file.getRelativePathFrom (relativeTo.getParentDirectory()));
            e->setAttribute ("path", t->file.getFullPathName());
            e->setAttribute ("gainDb", (double) t->gainDb);
            e->setAttribute ("mute", t->mute);
            e->setAttribute ("solo", t->solo);
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
        const auto xml = toXml (file);
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

    // Reads a project. A track whose audio is gone stays (silent) and a plugin that is not installed stays (passed through), both with
    // their settings, so saving again loses nothing; each is a line in `warnings`.
    bool load (const juce::File& file, juce::String& error)
    {
        const auto xml = juce::parseXML (file);
        if (xml == nullptr || ! xml->hasTagName ("OlympusProject")) { error = file.getFileName() + " is not an Olympus project."; return false; }
        if (xml->getIntAttribute ("version", 1) > kProjectVersion) { error = "This project is from a newer Olympus."; return false; }
        std::vector<std::unique_ptr<Track>> loaded;
        warnings.clear();
        double rate = xml->getDoubleAttribute ("sampleRate", 0.0);
        for (auto* e : xml->getChildWithTagNameIterator ("Track"))
        {
            auto t = std::make_unique<Track>();
            t->name = e->getStringAttribute ("name");
            t->file = file.getParentDirectory().getChildFile (e->getStringAttribute ("file"));
            if (! t->file.existsAsFile())
                t->file = juce::File (e->getStringAttribute ("path"));
            t->gainDb = (float) e->getDoubleAttribute ("gainDb", 0.0);
            t->mute = e->getBoolAttribute ("mute");
            t->solo = e->getBoolAttribute ("solo");
            juce::String problem;
            if (! loadAudio (*t, t->file, rate, problem))
            {
                warnings.add ("Track '" + t->name + "': " + problem);
                t->audio.setSize (2, 0);
            }
            else if (rate <= 0.0)
                rate = lastLoadedRate;
            for (auto* c : e->getChildWithTagNameIterator ("Slot"))
            {
                juce::MemoryBlock state;
                state.fromBase64Encoding (c->getStringAttribute ("state"));
                juce::String problem2;
                auto slot = makeSlot (c->getStringAttribute ("plugin"), &state, problem2, rate);
                if (slot == nullptr)
                {
                    warnings.add ("Track '" + t->name + "': " + problem2);
                    slot = std::make_unique<Slot>();
                    slot->plugin = c->getStringAttribute ("plugin");
                    slot->savedState = state;
                }
                slot->bypass = c->getBoolAttribute ("bypass");
                t->chain.push_back (std::move (slot));
            }
            loaded.push_back (std::move (t));
        }
        const juce::ScopedLock sl (lock);
        for (auto& t : tracks)
            for (auto& s : t->chain)
                if (s->instance != nullptr) s->instance->setPlayHead (nullptr);
        tracks = std::move (loaded);
        sampleRate = rate;
        for (auto& t : tracks)
            for (auto& s : t->chain)
                if (s->instance != nullptr) s->instance->setPlayHead (&head);
        projectFile = file;
        name = xml->getStringAttribute ("name", file.getFileNameWithoutExtension());
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

    // What the session holds, as the MCP server and the tests read it.
    juce::var describe() const
    {
        auto* root = new juce::DynamicObject();
        root->setProperty ("name", name);
        root->setProperty ("project_file", projectFile.getFullPathName());
        root->setProperty ("sample_rate", sampleRate);
        root->setProperty ("unsaved_changes", dirty);
        juce::Array<juce::var> list;
        int i = 0;
        for (auto& t : tracks)
        {
            auto* o = new juce::DynamicObject();
            o->setProperty ("index", i++);
            o->setProperty ("name", t->name);
            o->setProperty ("file", t->file.getFullPathName());
            o->setProperty ("audio_found", t->length() > 0);
            o->setProperty ("seconds", sampleRate > 0 ? (double) t->length() / sampleRate : 0.0);
            o->setProperty ("gain_db", (double) t->gainDb);
            o->setProperty ("mute", t->mute);
            o->setProperty ("solo", t->solo);
            o->setProperty ("latency_samples", t->latency());
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

    // The playhead the plugins read (a plugin that listens to the transport sees this one).
    struct Head : juce::AudioPlayHead
    {
        juce::Optional<PositionInfo> getPosition() const override
        {
            PositionInfo info;
            info.setTimeInSamples (position.load());
            info.setIsPlaying (playing.load());
            info.setBpm (120.0);
            return info;
        }
        void set (juce::int64 p, bool isPlaying) { position = p; playing = isPlaying; }
        std::atomic<juce::int64> position { 0 };
        std::atomic<bool> playing { false };
    };
    Head head;

    void updateLatencies() { const juce::ScopedLock sl (lock); updateLatenciesLocked(); }

private:
    juce::AudioFormatManager formats;
    double lastLoadedRate = 0.0;

    // Reads a file into the track at `wantedRate` (0 = keep the file's rate, reported in lastLoadedRate), two channels.
    bool loadAudio (Track& t, const juce::File& file, double wantedRate, juce::String& error)
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
        lastLoadedRate = rate;
        if (std::abs (rate - reader->sampleRate) < 0.5)
        {
            t.audio = std::move (raw);
            return true;
        }
        const double ratio = reader->sampleRate / rate;
        const int outLen = (int) std::ceil ((double) len / ratio);
        t.audio.setSize (2, outLen);
        for (int ch = 0; ch < 2; ++ch)
        {
            juce::LagrangeInterpolator resampler;
            resampler.process (ratio, raw.getReadPointer (ch), t.audio.getWritePointer (ch), outLen, len, 0);
        }
        return true;
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

    // The block [start, start + n) of the track through its chain, left in track.scratch. Silent outside the audio.
    void runChain (Track& t, juce::int64 start, int n)
    {
        t.scratch.setSize (t.scratch.getNumChannels(), kBlockSize, false, false, true);
        t.scratch.clear();
        const int have = (int) juce::jlimit ((juce::int64) 0, (juce::int64) n, (juce::int64) t.audio.getNumSamples() - start);
        for (int ch = 0; ch < 2 && have > 0; ++ch)
            t.scratch.copyFrom (ch, 0, t.audio, ch, (int) start, have);
        juce::AudioBuffer<float> view (t.scratch.getArrayOfWritePointers(), t.scratch.getNumChannels(), n);
        juce::MidiBuffer midi;
        for (auto& s : t.chain)
            if (s->instance != nullptr && ! s->bypass)
                s->instance->processBlock (view, midi);
        if (t.delaySamples > 0 && t.delayRing.getNumSamples() == t.delaySamples && ! rendering())
        {
            const int d = t.delaySamples;
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
