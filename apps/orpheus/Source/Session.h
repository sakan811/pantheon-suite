// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 The Pantheon authors

#pragma once

#include "Dsp/Edits.h"
#include "Dsp/PitchAnalysis.h"
#include "Dsp/Plan.h"
#include "Dsp/Psola.h"
#include "Dsp/Segmenter.h"

#include <juce_audio_utils/juce_audio_utils.h>

#include <cstdint>
#include <functional>

// One recording and everything done to it. No interface in here: the editor, the tests and the player all talk to this.
//
//   source    the audio as loaded, never changed
//   contour   the detected pitch of every 5 ms frame, found once (analyse)
//   notes     the cut of the contour into notes, and what has been done to each: the only thing an edit changes
//   output    source with the edited stretches replaced; rebuilt for the part an edit touches (changed), so a drag is cheap
//
// Everything is used from the message thread except `output`, which the player reads on the audio thread under `outputLock`.
namespace orpheus
{
struct EditState
{
    std::vector<Note> notes;
    Key key;
    double glideMs = 30.0;

    // The same notes, key and glide (what "unsaved" means: undoing back to the saved state is clean again).
    bool operator== (const EditState& o) const noexcept
    {
        const auto same = [] (double a, double b) { return ! (a < b) && ! (b < a); };
        if (notes.size() != o.notes.size() || key.root != o.key.root || key.scale != o.key.scale || ! same (key.a4Hz, o.key.a4Hz) || ! same (glideMs, o.glideMs))
            return false;
        for (size_t i = 0; i < notes.size(); ++i)
            if (! notes[i].identicalTo (o.notes[i]))
                return false;
        return true;
    }
};

// Which voices the pitch tracker looks for. A narrower range cannot jump an octave to a pitch outside it, which is what a low voice
// with a strong second harmonic, or a high one with a weak fundamental, does with the full range.
inline constexpr int kNumVoiceRanges = 4;
inline constexpr const char* kVoiceRangeNames[kNumVoiceRanges] = { "wide", "low", "mid", "high" };

inline AnalysisSettings analysisSettingsFor (int range)
{
    AnalysisSettings s; // wide: the tracker's own 65 to 1100 Hz
    switch (range)
    {
        case 1: s.minHz = 65.0;  s.maxHz = 400.0;  break;  // bass, baritone, a low speaking voice
        case 2: s.minHz = 100.0; s.maxHz = 700.0;  break;  // tenor, alto, most singers
        case 3: s.minHz = 200.0; s.maxHz = 1100.0; break;  // soprano, a child
        default: break;
    }
    return s;
}

class Session
{
public:
    struct Analysis
    {
        Contour contour;
        std::vector<double> marks;
        std::vector<Note> notes;
        Key key;
        int range = 0;                  // the voice range it was found with
    };

    // ---- the recording --------------------------------------------------------------------------------------------------
    juce::AudioBuffer<float> source, output;
    juce::SpinLock outputLock;
    double sampleRate = 48000.0;
    juce::File file;
    std::vector<float> mono;            // the channels averaged: what is analysed
    std::vector<float> peakMin, peakMax; // the waveform for drawing, one pair per kPeakBlock samples

    Contour contour;
    std::vector<double> marks;
    std::vector<Note> notes;
    Key key;
    double glideMs = 30.0;
    int voiceRange = 0;                 // the range the next analysis uses (kVoiceRangeNames)
    int analysedRange = 0;              // the range the current contour was found with
    Plan plan;
    std::atomic<int> version { 0 };     // counts changes; the editor repaints when it moves

    static constexpr int kPeakBlock = 128;

    bool isLoaded() const noexcept { return source.getNumSamples() > 0; }
    bool isAnalysed() const noexcept { return contour.numFrames() > 0; }
    int numSamples() const noexcept { return source.getNumSamples(); }
    double seconds() const noexcept { return sampleRate > 0 ? numSamples() / sampleRate : 0.0; }

    // Reads an audio file (anything JUCE can decode) into `audio`, touching nothing of the session: a file that cannot be read leaves
    // the open recording, its analysis and its edits as they were.
    static bool read (const juce::File& f, juce::AudioBuffer<float>& audio, double& rate, juce::String& error)
    {
        juce::AudioFormatManager formats;
        formats.registerBasicFormats();
        std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (f));
        if (reader == nullptr || reader->lengthInSamples <= 0 || reader->numChannels == 0)
        {
            error = "Cannot read " + f.getFileName() + " as audio.";
            return false;
        }
        if (reader->lengthInSamples > (juce::int64) 1 << 28)
        {
            error = f.getFileName() + " is too long.";
            return false;
        }
        const int channels = (int) std::min<unsigned> (reader->numChannels, 2), n = (int) reader->lengthInSamples;
        audio.setSize (channels, n);
        reader->read (&audio, 0, n, 0, true, channels > 1);
        rate = reader->sampleRate;
        return true;
    }

    // read, then adoptAudio. Resets the session (only if the file could be read).
    bool load (const juce::File& f, juce::String& error)
    {
        juce::AudioBuffer<float> in;
        double rate = 0.0;
        if (! read (f, in, rate, error))
            return false;
        adoptAudio (std::move (in), rate, f);
        return true;
    }

    // The channels averaged: what is analysed.
    static std::vector<float> mixDown (const juce::AudioBuffer<float>& audio)
    {
        const int n = audio.getNumSamples(), channels = audio.getNumChannels();
        std::vector<float> mixed ((size_t) n, 0.0f);
        for (int ch = 0; ch < channels; ++ch)
            juce::FloatVectorOperations::addWithMultiply (mixed.data(), audio.getReadPointer (ch), 1.0f / (float) channels, n);
        return mixed;
    }

    // A cheap identity of a recording, written into the project: the length and 4096 samples spread evenly over it, hashed (FNV-1a). A
    // project made for another take, or for the same file after it was edited, does not match; reading the file again gives the same text.
    static juce::String fingerprintOf (const std::vector<float>& monoSamples)
    {
        std::uint64_t h = 1469598103934665603ull;
        const auto mix = [&h] (std::uint64_t v)
        {
            for (int k = 0; k < 8; ++k)
            {
                h ^= (v >> (8 * k)) & 0xffu;
                h *= 1099511628211ull;
            }
        };
        const std::uint64_t n = monoSamples.size();
        mix (n);
        constexpr std::uint64_t probes = 4096;
        for (std::uint64_t i = 0; n > 0 && i < probes; ++i)
            mix ((std::uint64_t) (std::int64_t) std::llround ((double) monoSamples[(size_t) (n * i / probes)] * 1048576.0));
        return juce::String::toHexString ((juce::int64) h);
    }

    // For tests and for anything that already has samples.
    void adoptAudio (juce::AudioBuffer<float>&& audio, double rate, const juce::File& from = {})
    {
        const int n = audio.getNumSamples();
        auto mixed = mixDown (audio);
        const int blocks = (n + kPeakBlock - 1) / kPeakBlock;
        std::vector<float> lows ((size_t) blocks, 0.0f), highs ((size_t) blocks, 0.0f);
        for (int b = 0; b < blocks; ++b)
        {
            const int begin = b * kPeakBlock, end = std::min (n, begin + kPeakBlock);
            float lo = 0.0f, hi = 0.0f;
            for (int i = begin; i < end; ++i)
            {
                lo = std::min (lo, mixed[(size_t) i]);
                hi = std::max (hi, mixed[(size_t) i]);
            }
            lows[(size_t) b] = lo;
            highs[(size_t) b] = hi;
        }
        juce::AudioBuffer<float> copy; // made before the lock: a whole-file copy would hold the audio thread up
        copy.makeCopyOf (audio);
        {
            // the player reads source and output on the audio thread: only the swap is done under its lock
            const juce::SpinLock::ScopedLockType lock (outputLock);
            source = std::move (audio);
            output = std::move (copy);
        }
        sampleRate = rate;
        file = from;
        contour = {};
        marks.clear();
        notes.clear();
        plan = {};
        undoStack.clear();
        redoStack.clear();
        key = {};
        glideMs = 30.0;
        markSaved();
        mono = std::move (mixed);
        peakMin = std::move (lows);
        peakMax = std::move (highs);
        ++version;
    }

    // ---- analysis ---------------------------------------------------------------------------------------------------
    // The heavy part, a pure function of the samples: safe to run on another thread. `progress` returns false to cancel.
    static Analysis analyse (const std::vector<float>& monoSamples, double rate, const std::function<bool (float)>& progress = {}, int range = 0)
    {
        Analysis a;
        a.range = range;
        a.contour = PitchAnalyzer::analyse (monoSamples.data(), (int) monoSamples.size(), rate, analysisSettingsFor (range), progress);
        if (a.contour.numFrames() == 0)
            return a;
        a.marks = Psola::findMarks (monoSamples.data(), (int) monoSamples.size(), a.contour);
        // the fit is quadratic in the length of a voiced stretch: it checks for a cancel as well (progress returns false to cancel)
        const Segmenter::Cancel cancelled = progress ? Segmenter::Cancel ([&progress] { return ! progress (1.0f); }) : Segmenter::Cancel();
        for (const auto& span : Segmenter::segment (a.contour, {}, cancelled))
            a.notes.push_back (makeNote (a.contour, span.start, span.end));
        const auto guess = detectKeyAuto (a.notes);
        a.key.root = guess.root;
        a.key.scale = guess.minor ? kMinor : kMajor;
        a.key.a4Hz = 440.0; // detected, not applied: the singer decides whether the song is tuned to 440
        return a;
    }

    void adopt (Analysis&& a)
    {
        contour = std::move (a.contour);
        marks = std::move (a.marks);
        notes = std::move (a.notes);
        key = a.key;
        glideMs = 30.0;
        analysedRange = a.range;
        undoStack.clear();
        redoStack.clear();
        changedAll();
        markSaved();
    }

    // ---- editing ----------------------------------------------------------------------------------------------------
    EditState state() const { return { notes, key, glideMs }; }

    // Call before changing anything: what undo returns to. A change in several steps (a drag) calls it once, at the start.
    void pushUndo()
    {
        undoStack.push_back (state());
        if (undoStack.size() > 200)
            undoStack.erase (undoStack.begin());
        redoStack.clear();
    }

    // "Unsaved": the notes, key or glide differ from what was last saved or loaded (markSaved). Undoing back to it is clean again.
    void markSaved() { saved = state(); }
    void markUnsaved() { saved.glideMs = -1.0; } // no real state has this glide: the session counts as changed until the next markSaved
    bool dirty() const { return isAnalysed() && ! (state() == saved); }

    bool canUndo() const noexcept { return ! undoStack.empty(); }
    bool canRedo() const noexcept { return ! redoStack.empty(); }

    bool undo() { return step (undoStack, redoStack); }
    bool redo() { return step (redoStack, undoStack); }

    // The notes (or the key) have been changed by the caller: rebuild the plan and the audio for frames [first, last].
    void changed (int firstFrame, int lastFrame)
    {
        if (! isAnalysed())
            return;
        plan = Plan::build (contour, notes, glideMs);
        const int margin = (int) std::lround (glideMs * 0.001 * sampleRate / contour.hop / 2.0) + 3;
        const double hop = contour.hop;
        const int a = (int) std::max (0.0, (firstFrame - margin) * hop - hop), b = (int) std::min ((double) numSamples(), (lastFrame + margin) * hop + hop);
        renderRange (a, b);
        ++version;
    }

    void changedAll() { changed (0, contour.numFrames()); }

    // Frames touched by notes[i], for `changed`.
    void changedNote (size_t i) { changed (notes[i].start, notes[i].end); }

    // Applies amount (0..1) of the pull towards the scale to every note (or the given ones), keeping the rest of what was done to them.
    void correct (float amount, const std::vector<size_t>& which = {})
    {
        pushUndo();
        if (which.empty())
            for (auto& n : notes)
                edit::snap (n, key, amount);
        else
            for (const auto i : which)
                edit::snap (notes[i], key, amount);
        changedAll();
    }

    void resetNotes (const std::vector<size_t>& which = {})
    {
        pushUndo();
        if (which.empty())
            for (auto& n : notes)
                n.reset();
        else
            for (const auto i : which)
                notes[i].reset();
        changedAll();
    }

    // The frame range in which two note lists differ (both lists' notes), or false if they are equal.
    static bool differingFrames (const std::vector<Note>& a, const std::vector<Note>& b, int& first, int& last)
    {
        const auto same = [] (const Note& x, const Note& y) { return x.identicalTo (y); };
        size_t head = 0;
        while (head < a.size() && head < b.size() && same (a[head], b[head]))
            ++head;
        if (head == a.size() && head == b.size())
            return false;
        size_t tail = 0;
        while (tail < a.size() - head && tail < b.size() - head && same (a[a.size() - 1 - tail], b[b.size() - 1 - tail]))
            ++tail;
        first = std::numeric_limits<int>::max();
        last = 0;
        for (const auto* list : { &a, &b })
            for (size_t i = head; i + tail < list->size(); ++i)
            {
                first = std::min (first, (*list)[i].start);
                last = std::max (last, (*list)[i].end);
            }
        if (first > last)
        {
            first = last = (head < a.size() ? a[head].start : (a.empty() ? 0 : a.back().end));
        }
        return true;
    }

    // ---- output -----------------------------------------------------------------------------------------------------
    // Rebuilds the stretches of the output that overlap [a, b) samples from the plan.
    void renderRange (int a, int b)
    {
        if (! isAnalysed())
            return;
        std::vector<const float*> src;
        for (int ch = 0; ch < source.getNumChannels(); ++ch)
            src.push_back (source.getReadPointer (ch));
        auto r = Psola::render (src, numSamples(), contour, marks, plan, a, b);
        const juce::SpinLock::ScopedLockType lock (outputLock);
        for (int ch = 0; ch < output.getNumChannels(); ++ch)
            std::copy (r.data[(size_t) ch].begin(), r.data[(size_t) ch].end(), output.getWritePointer (ch) + r.start);
    }

    // The highest sample of the corrected audio, linear (1.0 = 0 dBFS). Gain up to +12 dB and the re-spaced grains can take it past full scale;
    // a WAV cannot hold that. Cached until the session changes: the window asks on every repaint. Message thread only.
    float outputPeak() const
    {
        const int v = version.load();
        if (peakVersion != v)
        {
            float peak = 0.0f;
            for (int ch = 0; ch < output.getNumChannels(); ++ch)
            {
                const auto range = juce::FloatVectorOperations::findMinAndMax (output.getReadPointer (ch), output.getNumSamples());
                peak = std::max ({ peak, std::fabs (range.getStart()), std::fabs (range.getEnd()) });
            }
            peakCache = peak;
            peakVersion = v;
        }
        return peakCache;
    }

    // ---- files ------------------------------------------------------------------------------------------------------
    // The corrected audio as a 24-bit WAV at the rate of the source. It is written to a temporary file next to the target and moved
    // over it when complete, so a failure (a full disk, a lost volume) leaves an existing file as it was. `output` is read without
    // `outputLock`: only this thread writes it, and the audio thread only reads, so there is nothing to exclude and the audio is not held up.
    bool exportWav (const juce::File& to, juce::String& error)
    {
        juce::TemporaryFile temp (to);
        {
            juce::WavAudioFormat wav;
            std::unique_ptr<juce::OutputStream> stream (temp.getFile().createOutputStream());
            if (stream == nullptr)
            {
                error = "Cannot write " + to.getFullPathName();
                return false;
            }
            auto writer = wav.createWriterFor (stream, juce::AudioFormatWriter::Options {}.withSampleRate (sampleRate).withNumChannels (output.getNumChannels()).withBitsPerSample (24));
            if (writer == nullptr)
            {
                error = "Cannot create a WAV writer.";
                return false;
            }
            if (! writer->writeFromAudioSampleBuffer (output, 0, output.getNumSamples()))
            {
                error = "Writing failed.";
                return false;
            }
        } // the writer is gone here: the file is complete
        if (! temp.overwriteTargetFileWithTemporary())
        {
            error = "Cannot replace " + to.getFullPathName();
            return false;
        }
        return true;
    }

    // The edit, as XML: the notes and the key. The audio is not in it, only where to find it (the absolute path, the path relative to the
    // project's folder when `projectFile` is given, since a project and its audio are usually moved together) and a fingerprint of it.
    std::unique_ptr<juce::XmlElement> toXml (const juce::File& projectFile = {}) const
    {
        auto xml = std::make_unique<juce::XmlElement> ("Orpheus");
        xml->setAttribute ("version", 1);
        xml->setAttribute ("source", file.getFullPathName());
        if (projectFile != juce::File())
            xml->setAttribute ("sourceRelative", file.getRelativePathFrom (projectFile.getParentDirectory()));
        xml->setAttribute ("fingerprint", fingerprintOf (mono));
        xml->setAttribute ("frames", contour.numFrames());
        xml->setAttribute ("range", analysedRange);
        xml->setAttribute ("root", key.root);
        xml->setAttribute ("scale", key.scale);
        xml->setAttribute ("a4", key.a4Hz);
        xml->setAttribute ("glide", glideMs);
        for (const auto& n : notes)
        {
            auto* e = xml->createNewChildElement ("Note");
            e->setAttribute ("start", n.start);
            e->setAttribute ("end", n.end);
            e->setAttribute ("target", (double) n.target);
            e->setAttribute ("drift", (double) n.drift);
            e->setAttribute ("vibrato", (double) n.vibrato);
            e->setAttribute ("formant", (double) n.formant);
            e->setAttribute ("gain", (double) n.gainDb);
        }
        return xml;
    }

    // Where the audio of a project is: next to the project (by the relative path) if it is there, else where it was.
    static juce::File sourceOf (const juce::XmlElement& xml, const juce::File& projectFile)
    {
        const auto relative = xml.getStringAttribute ("sourceRelative");
        if (relative.isNotEmpty() && projectFile != juce::File())
            if (const auto moved = projectFile.getParentDirectory().getChildFile (relative); moved.existsAsFile())
                return moved;
        return juce::File (xml.getStringAttribute ("source"));
    }

    // Puts a saved edit onto the analysed recording. The notes are the saved ones (their centres are measured again from the
    // contour, so a target that was an exact pitch stays one). Returns false if the project is not for this recording: another
    // frame count, or another fingerprint (a project from before the fingerprint has none and is checked by the frame count alone).
    // Notes are put in order and a note that overlaps the one before it is dropped; values are held to what the editor can make.
    bool applyXml (const juce::XmlElement& xml)
    {
        if (! xml.hasTagName ("Orpheus") || xml.getIntAttribute ("frames") != contour.numFrames())
            return false;
        if (const auto fingerprint = xml.getStringAttribute ("fingerprint"); fingerprint.isNotEmpty() && fingerprint != fingerprintOf (mono))
            return false;
        const auto number = [] (const juce::XmlElement& e, const char* name, double fallback, double lo, double hi)
        {
            const double v = e.getDoubleAttribute (name, fallback);
            return std::isfinite (v) ? juce::jlimit (lo, hi, v) : fallback;
        };
        std::vector<Note> loaded;
        for (const auto* e : xml.getChildWithTagNameIterator ("Note"))
        {
            const int s = e->getIntAttribute ("start"), t = e->getIntAttribute ("end");
            if (s < 0 || t <= s || t > contour.numFrames())
                return false;
            Note n = makeNote (contour, s, t);
            n.target = (float) number (*e, "target", n.centre, -1.0e6, 1.0e6);
            n.clampTarget();
            n.drift = (float) number (*e, "drift", 0.0, 0.0, 1.0);
            n.vibrato = (float) number (*e, "vibrato", 1.0, 0.0, 2.0);
            n.formant = (float) number (*e, "formant", 0.0, -12.0, 12.0);
            n.gainDb = (float) number (*e, "gain", 0.0, -12.0, 12.0);
            loaded.push_back (n);
        }
        std::stable_sort (loaded.begin(), loaded.end(), [] (const Note& a, const Note& b) { return a.start < b.start; });
        loaded.erase (std::remove_if (loaded.begin(), loaded.end(), [last = 0] (const Note& n) mutable
                                      {
                                          if (n.start < last)
                                              return true;
                                          last = n.end;
                                          return false;
                                      }),
                      loaded.end());
        notes = std::move (loaded);
        key.root = juce::jlimit (0, 11, xml.getIntAttribute ("root"));
        key.scale = juce::jlimit (0, kNumScales - 1, xml.getIntAttribute ("scale"));
        key.a4Hz = number (xml, "a4", 440.0, kMinA4Hz, kMaxA4Hz);
        glideMs = number (xml, "glide", 30.0, 0.0, 200.0);
        undoStack.clear();
        redoStack.clear();
        changedAll();
        markSaved();
        return true;
    }

private:
    bool step (std::vector<EditState>& from, std::vector<EditState>& to)
    {
        if (from.empty())
            return false;
        to.push_back (state());
        const EditState target = std::move (from.back());
        from.pop_back();
        int first = 0, last = 0;
        const bool differ = differingFrames (notes, target.notes, first, last);
        notes = target.notes;
        key = target.key;
        const bool glideMoved = std::fabs (glideMs - target.glideMs) > 1.0e-9;
        glideMs = target.glideMs;
        if (differ || glideMoved)
        {
            if (glideMoved)
                changedAll();
            else
                changed (first, last);
        }
        else
            ++version;
        return true;
    }

    std::vector<EditState> undoStack, redoStack;
    EditState saved;                    // what the notes were when last saved or loaded
    mutable float peakCache = 0.0f;
    mutable int peakVersion = -1;
};
} // namespace orpheus
