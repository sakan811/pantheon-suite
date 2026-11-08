// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 The Pantheon authors

#pragma once

#include "Dsp/Edits.h"
#include "Dsp/PitchAnalysis.h"
#include "Dsp/Plan.h"
#include "Dsp/Psola.h"
#include "Dsp/Segmenter.h"

#include <juce_audio_utils/juce_audio_utils.h>

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
};

class Session
{
public:
    struct Analysis
    {
        Contour contour;
        std::vector<double> marks;
        std::vector<Note> notes;
        Key key;
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
    Plan plan;
    std::atomic<int> version { 0 };     // counts changes; the editor repaints when it moves

    static constexpr int kPeakBlock = 128;

    bool isLoaded() const noexcept { return source.getNumSamples() > 0; }
    bool isAnalysed() const noexcept { return contour.numFrames() > 0; }
    int numSamples() const noexcept { return source.getNumSamples(); }
    double seconds() const noexcept { return sampleRate > 0 ? numSamples() / sampleRate : 0.0; }

    // Reads an audio file (anything JUCE can decode). Resets the session.
    bool load (const juce::File& f, juce::String& error)
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
        juce::AudioBuffer<float> in (channels, n);
        reader->read (&in, 0, n, 0, true, channels > 1);
        adoptAudio (std::move (in), reader->sampleRate, f);
        return true;
    }

    // For tests and for anything that already has samples.
    void adoptAudio (juce::AudioBuffer<float>&& audio, double rate, const juce::File& from = {})
    {
        const int n = audio.getNumSamples(), channels = audio.getNumChannels();
        std::vector<float> mixed ((size_t) n, 0.0f);
        for (int ch = 0; ch < channels; ++ch)
            juce::FloatVectorOperations::addWithMultiply (mixed.data(), audio.getReadPointer (ch), 1.0f / (float) channels, n);
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
        {
            // the player reads source and output on the audio thread: swap them under its lock
            const juce::SpinLock::ScopedLockType lock (outputLock);
            source = std::move (audio);
            output.makeCopyOf (source);
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
        mono = std::move (mixed);
        peakMin = std::move (lows);
        peakMax = std::move (highs);
        ++version;
    }

    // ---- analysis ---------------------------------------------------------------------------------------------------
    // The heavy part, a pure function of the samples: safe to run on another thread. `progress` returns false to cancel.
    static Analysis analyse (const std::vector<float>& monoSamples, double rate, const std::function<bool (float)>& progress = {})
    {
        Analysis a;
        a.contour = PitchAnalyzer::analyse (monoSamples.data(), (int) monoSamples.size(), rate, {}, progress);
        if (a.contour.numFrames() == 0)
            return a;
        a.marks = Psola::findMarks (monoSamples.data(), (int) monoSamples.size(), a.contour);
        for (const auto& span : Segmenter::segment (a.contour))
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
        undoStack.clear();
        redoStack.clear();
        changedAll();
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

    // ---- files ------------------------------------------------------------------------------------------------------
    // The corrected audio as a 24-bit WAV at the rate of the source.
    bool exportWav (const juce::File& to, juce::String& error)
    {
        juce::WavAudioFormat wav;
        to.deleteFile();
        std::unique_ptr<juce::OutputStream> stream (to.createOutputStream());
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
        const juce::SpinLock::ScopedLockType lock (outputLock);
        if (! writer->writeFromAudioSampleBuffer (output, 0, output.getNumSamples()))
        {
            error = "Writing failed.";
            return false;
        }
        return true;
    }

    // The edit, as XML: the notes and the key. The audio is not in it, only where to find it.
    std::unique_ptr<juce::XmlElement> toXml() const
    {
        auto xml = std::make_unique<juce::XmlElement> ("Orpheus");
        xml->setAttribute ("version", 1);
        xml->setAttribute ("source", file.getFullPathName());
        xml->setAttribute ("frames", contour.numFrames());
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

    // Puts a saved edit onto the analysed recording. The notes are the saved ones (their centres are measured again from the
    // contour, so a target that was an exact pitch stays one). Returns false if the project is not for this recording.
    bool applyXml (const juce::XmlElement& xml)
    {
        if (! xml.hasTagName ("Orpheus") || xml.getIntAttribute ("frames") != contour.numFrames())
            return false;
        std::vector<Note> loaded;
        for (const auto* e : xml.getChildWithTagNameIterator ("Note"))
        {
            const int s = e->getIntAttribute ("start"), t = e->getIntAttribute ("end");
            if (s < 0 || t <= s || t > contour.numFrames())
                return false;
            Note n = makeNote (contour, s, t);
            n.target = (float) e->getDoubleAttribute ("target", n.centre);
            n.drift = (float) e->getDoubleAttribute ("drift", 0.0);
            n.vibrato = (float) e->getDoubleAttribute ("vibrato", 1.0);
            n.formant = (float) e->getDoubleAttribute ("formant", 0.0);
            n.gainDb = (float) e->getDoubleAttribute ("gain", 0.0);
            loaded.push_back (n);
        }
        notes = std::move (loaded);
        key.root = juce::jlimit (0, 11, xml.getIntAttribute ("root"));
        key.scale = juce::jlimit (0, kNumScales - 1, xml.getIntAttribute ("scale"));
        key.a4Hz = juce::jlimit (400.0, 480.0, xml.getDoubleAttribute ("a4", 440.0));
        glideMs = juce::jlimit (0.0, 200.0, xml.getDoubleAttribute ("glide", 30.0));
        undoStack.clear();
        redoStack.clear();
        changedAll();
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
};
} // namespace orpheus
