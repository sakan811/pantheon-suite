// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 The Pantheon authors

// Developer tool (not part of the shipped plug-in):
//   HarmoniaDevTool selftest              drive the real HarmoniaProcessor with a synthetic vocal
//   HarmoniaDevTool snapshot out.png [seconds [in.wav start]]
//                                         render the editor (after feeding it a phrase, or `seconds` of a WAV from `start`) to a PNG
//   HarmoniaDevTool process in.wav out.wav [key 0-11] [scale index] [speed ms] [natural %] [strength %]
//                                         run a WAV file through the real HarmoniaProcessor (delay-compensated)

#include "../Source/Advice.h"
#include "../Source/PluginEditor.h"
#include "../Source/PluginProcessor.h"
#include "TestSignals.h"
#include "WavIO.h"

#include <cstdio>
#include <cstdlib>

static void setParam (HarmoniaProcessor& p, const juce::String& id, float plain)
{
    auto* param = p.apvts.getParameter (id);
    param->setValueNotifyingHost (param->convertTo0to1 (plain));
}

static std::vector<float> run (HarmoniaProcessor& p, const std::vector<float>& in, int block = 512)
{
    std::vector<float> out (in.size());
    juce::AudioBuffer<float> buf (2, block);
    juce::MidiBuffer midi;
    for (size_t pos = 0; pos < in.size(); pos += (size_t) block)
    {
        const int n = (int) std::min<size_t> ((size_t) block, in.size() - pos);
        buf.setSize (2, n, false, false, true);
        for (int i = 0; i < n; ++i)
        {
            buf.setSample (0, i, in[pos + (size_t) i]);
            buf.setSample (1, i, in[pos + (size_t) i]);
        }
        p.processBlock (buf, midi);
        for (int i = 0; i < n; ++i)
            out[pos + (size_t) i] = buf.getSample (0, i);
    }
    return out;
}

static int selftest()
{
    constexpr double fs = 48000.0;
    using testsig::check;
    using testsig::failures;

    HarmoniaProcessor proc;
    proc.setPlayConfigDetails (2, 2, fs, 512);
    proc.prepareToPlay (fs, 512);

    // A major, sing C#4 (61) 30 cents flat, then E4 (64) 25 cents sharp... but E is in A major, C# too.
    // Use A major (key = 9, scale = Major) and sing D#4 (63), which is NOT in the scale: it must land on D4 or E4.
    setParam (proc, ids::key, 9);
    setParam (proc, ids::scale, (float) harmonia::kMajorScale);
    setParam (proc, ids::speed, 0.0f);
    setParam (proc, ids::natural, 0.0f);

    std::printf ("Processor + parameters\n");
    check (proc.getName() == juce::String (words::kProjectWord) + " " + words::kPluginWord, "the reported name is the nameplate's two words", 0.0);
    check (proc.currentNoteMask() == harmonia::scaleMask (9, harmonia::kMajorScale), "A major mask from parameters", proc.currentNoteMask());

    auto voice = [&] (double midi) {
        return testsig::makeVoice (fs, 2.5, [midi] (double) { return testsig::midiToHz (midi); });
    };
    auto measure = [&] (const std::vector<float>& x, double expected) {
        return testsig::stats (testsig::centsVs (testsig::measurePitch (x, fs), 1.2 + proc.getLatencySamples() / fs, 2.3, expected));
    };

    {
        auto out = run (proc, voice (69.0 - 0.33)); // A4, 33 cents flat, in key
        auto s = measure (out, 69.0);
        check (std::fabs (s.mean) < 2.0 && s.stddev < 2.0, "A4 33c flat -> A4 (in key)", s.mean);
    }
    {
        proc.prepareToPlay (fs, 512);
        auto out = run (proc, voice (63.45)); // D#4 + 45c: nearest in-key note is E4 (64)
        auto s = measure (out, 64.0);
        check (std::fabs (s.mean) < 2.0 && s.stddev < 2.0, "out-of-key D#4 +45c -> E4", s.mean);
    }
    {
        // Custom scale: only allow C and G.
        proc.prepareToPlay (fs, 512);
        setParam (proc, ids::scale, (float) harmonia::kCustomScale);
        for (int i = 0; i < 12; ++i)
            setParam (proc, ids::note (i), (i == 0 || i == 7) ? 1.0f : 0.0f);
        check (proc.currentNoteMask() == ((1u << 0) | (1u << 7)), "custom mask = C + G", proc.currentNoteMask());
        auto out = run (proc, voice (65.2)); // F4 + 20c -> between C and G; nearest allowed is G4 (67)? F4=65: C5(72) 7 away, G4(67) 2 away
        auto s = measure (out, 67.0);
        check (std::fabs (s.mean) < 2.0 && s.stddev < 2.0, "custom scale {C,G}: F4 -> G4", s.mean);
    }
    {
        // Bypass: output equals the delayed input.
        proc.prepareToPlay (fs, 512);
        setParam (proc, ids::bypass, 1.0f);
        auto in = voice (63.45);
        auto out = run (proc, in);
        const int lat = proc.getLatencySamples();
        double maxDiff = 0;
        for (size_t i = (size_t) (fs * 1.0); i < in.size(); ++i)
            maxDiff = std::max (maxDiff, (double) std::fabs (in[i - (size_t) lat] - out[i]));
        check (maxDiff < 1e-4, "bypass = input delayed by reported latency", maxDiff);
        setParam (proc, ids::bypass, 0.0f);
    }
    {
        // Mono-sized blocks / tiny blocks must not crash.
        proc.prepareToPlay (fs, 64);
        auto out = run (proc, voice (60.0), 7);
        bool finite = true;
        for (float s : out)
            finite = finite && std::isfinite (s);
        check (finite, "tiny odd block sizes stay finite", finite ? 1 : 0);
    }
    {
        // State round trip.
        setParam (proc, ids::speed, 123.0f);
        juce::MemoryBlock mb;
        proc.getStateInformation (mb);
        setParam (proc, ids::speed, 5.0f);
        proc.setStateInformation (mb.getData(), (int) mb.getSize());
        check (std::fabs (proc.apvts.getRawParameterValue (ids::speed)->load() - 123.0f) < 0.5f, "state save/restore", proc.apvts.getRawParameterValue (ids::speed)->load());
    }
    {
        // Key detection: sing a C major arpeggio-ish melody and ask for the key.
        proc.prepareToPlay (fs, 512);
        setParam (proc, ids::bypass, 0.0f);
        const double notes[] = { 60, 64, 67, 64, 62, 65, 69, 65, 60, 67, 64, 60 };
        auto in = testsig::makeVoice (fs, 8.0, [&] (double t) {
            const int k = (int) (t / 0.6);
            return k >= 12 ? 0.0 : testsig::midiToHz (notes[k] + 0.12);
        });
        proc.startKeyLearn();
        auto chunk = run (proc, std::vector<float> (in.begin(), in.begin() + 1024)); // let the command be picked up
        (void) chunk;
        run (proc, std::vector<float> (in.begin() + 1024, in.end()));
        proc.finishKeyLearn();
        run (proc, std::vector<float> (4096, 0.0f));
        HarmoniaProcessor::KeyResult r;
        const bool got = proc.takeKeyResult (r);
        check (got && r.root == 0 && ! r.minor, "key detection finds C major", got ? r.root + (r.minor ? 100 : 0) : -1);
    }

    {
        // Auto key through the processor: the chosen key (F# minor) is overruled by what is sung (C major),
        // and the editor-facing mask follows. Turning it off hands control back.
        proc.prepareToPlay (fs, 512);
        setParam (proc, ids::key, 6);
        setParam (proc, ids::scale, (float) harmonia::kMinorScale);
        setParam (proc, ids::autoKey, 1.0f);
        const int tune[] = { 0, 2, 4, 2, 4, 5, 7, 5, 4, 2, 0, 2, 4, 7, 4, 2, 0, -1, 0, 2, 4, 5, 4, 2, 0 };
        auto in = testsig::makeVoice (fs, 22.0, [&] (double t) { return testsig::midiToHz (60.0 + tune[(int) (t / 0.35) % 25]); });
        run (proc, in);
        int root = -1;
        bool minor = true;
        const bool following = proc.followedKey (root, minor);
        check (following && proc.currentNoteMask() == harmonia::scaleMask (0, harmonia::kMajorScale), "auto key follows what is sung", proc.currentNoteMask());
        setParam (proc, ids::autoKey, 0.0f);
        check (! proc.followedKey (root, minor) && proc.currentNoteMask() == harmonia::scaleMask (6, harmonia::kMinorScale), "auto key off: the chosen key is back", proc.currentNoteMask());
    }

    std::printf ("\nAdvice: the notes that keep a pitch corrector from being used for the wrong job\n");
    {
        using ui::Severity;
        const auto has = [] (const std::vector<ui::Note>& n, const char* w, Severity s) { return ui::hasNote (n, w, s); };
        harmonia::AdviceInput fine;
        fine.speedMs = 40.0;
        check (harmonia::adviceFor (fine).empty(), "a normal setting: nothing to say", 0);
        auto in = fine; in.bypass = true;
        check (has (harmonia::adviceFor (in), "BYPASS", Severity::warning), "BYPASS on: a warning", 0);
        in = fine; in.allowedNotes = 0;
        check (has (harmonia::adviceFor (in), "No note is allowed", Severity::warning), "no note allowed: nothing can be corrected", 0);
        in = fine; in.allowedNotes = 3;
        check (has (harmonia::adviceFor (in), "Only 3 notes", Severity::warning), "three notes allowed: a warning", 0);
        in = fine; in.tuningHz = 432.0;
        check (has (harmonia::adviceFor (in), "432 Hz", Severity::warning), "TUNING away from 440: a warning", 0);
        in = fine; in.speedMs = 0.0;
        check (has (harmonia::adviceFor (in), "robotic", Severity::info), "SPEED at 0: the robotic effect", 0);
        in = fine; in.holdMs = 0.0;
        check (has (harmonia::adviceFor (in), "NOTE HOLD is 0", Severity::info), "NOTE HOLD 0: it can flicker", 0);
        in = fine; in.autoKey = true; in.formantSemitones = 2.0; in.voiceRangeFixed = true;
        check (has (harmonia::adviceFor (in), "AUTO KEY", Severity::info) && has (harmonia::adviceFor (in), "FORMANT", Severity::info)
                   && has (harmonia::adviceFor (in), "VOICE", Severity::info), "AUTO KEY, FORMANT and VOICE: each named", 0);

        size_t longest = 0;
        for (int mask = 0; mask < 256; ++mask)
        {
            harmonia::AdviceInput all;
            all.bypass = mask & 1; all.autoKey = mask & 2; all.speedMs = (mask & 4) ? 0.0 : 40.0; all.formantSemitones = (mask & 8) ? 3.0 : 0.0;
            all.tuningHz = (mask & 16) ? 466.0 : 440.0; all.holdMs = (mask & 32) ? 0.0 : 70.0; all.allowedNotes = (mask & 64) ? 3 : 12; all.voiceRangeFixed = mask & 128;
            for (auto& n : harmonia::adviceFor (all))
                longest = std::max (longest, n.text.size());
        }
        check ((int) longest <= ui::kMaxNoteChars, "the longest note fits a line", (double) longest);

        HarmoniaEditor editor (proc);
        setParam (proc, ids::bypass, 1.0f);
        check (has (editor.notes(), "BYPASS", Severity::warning), "BYPASS in the plugin reaches the editor's notes", 0);
        setParam (proc, ids::bypass, 0.0f);
        check (editor.guide().isVisible() && std::strlen (harmonia::guideText()) > 400 && std::string (harmonia::guideText()).find ("NOT FOR") != std::string::npos,
               "the GUIDE button is there and its text says what the plugin is not for", 0);
    }

    std::printf ("\n%s\n", failures == 0 ? "PROCESSOR TESTS PASSED" : "PROCESSOR TESTS FAILED");
    return failures == 0 ? 0 : 1;
}

static int snapshot (const char* path, double feedSeconds, const char* wavPath = nullptr, double wavStart = 0.0)
{
    constexpr double fs = 48000.0;
    HarmoniaProcessor proc;
    proc.setPlayConfigDetails (2, 2, fs, 512);
    proc.prepareToPlay (fs, 512);

    setParam (proc, ids::key, 0);
    setParam (proc, ids::scale, (float) harmonia::kMajorScale);
    setParam (proc, ids::speed, 40.0f);
    setParam (proc, ids::natural, 70.0f);

    // a few seconds of an off-key, vibrato-laden phrase
    const double notes[] = { 60.0 - 0.35, 62.0 + 0.30, 64.0 - 0.20, 65.0 + 0.40, 67.0 - 0.30, 64.0 + 0.2 };
    auto in = testsig::makeVoice (fs, 4.2, [&] (double t) {
        const int k = (int) (t / 0.68);
        if (k >= 6 || std::fmod (t, 0.68) > 0.62)
            return 0.0;
        return testsig::midiToHz (notes[k] + 0.22 * std::sin (2.0 * testsig::kPi * 5.6 * t));
    });
    if (std::getenv ("HARMONIA_AUTOKEY")) // demo: wrong key chosen, Auto key on, enough singing for it to settle
    {
        setParam (proc, ids::key, 6);
        setParam (proc, ids::scale, (float) harmonia::kMinorScale);
        setParam (proc, ids::autoKey, 1.0f);
        const auto phrase = in;
        in.clear();
        for (int i = 0; i < 8; ++i)
            in.insert (in.end(), phrase.begin(), phrase.end());
        feedSeconds = (double) in.size() / fs;
    }
    in.resize ((size_t) (feedSeconds * fs));
    if (wavPath)
    {
        wavio::Wav w;
        if (! wavio::read (wavPath, w) || w.sampleRate != (int) fs)
        {
            std::fprintf (stderr, "need a %d Hz WAV: %s\n", (int) fs, wavPath);
            return 1;
        }
        const size_t from = (size_t) (wavStart * fs);
        for (size_t i = 0; i < in.size() && from + i < w.data[0].size(); ++i)
            in[i] = w.data[0][from + i];
    }
    run (proc, in);

    auto* editor = new HarmoniaEditor (proc);
    editor->setSize (2 * HarmoniaEditor::kDesignW, 2 * HarmoniaEditor::kDesignH);
    juce::MessageManager::getInstance()->runDispatchLoopUntil (400);
    auto img = editor->createComponentSnapshot (editor->getLocalBounds(), true, 1.0f);
    juce::File f (path);
    f.deleteFile();
    juce::FileOutputStream out (f);
    juce::PNGImageFormat().writeImageToStream (img, out);
    delete editor;
    std::printf ("wrote %s (%d x %d)\n", path, img.getWidth(), img.getHeight());
    return 0;
}

// Streams a WAV file through the processor in host-sized blocks, the way a DAW would, and writes the
// result shifted back by the reported latency so that it lines up with the input.
static int processFile (int argc, char** argv)
{
    wavio::Wav in;
    if (! wavio::read (argv[2], in))
    {
        std::fprintf (stderr, "cannot read %s\n", argv[2]);
        return 1;
    }
    const int nch = std::min (in.channels, 2);
    const size_t frames = in.data[0].size();

    HarmoniaProcessor proc;
    proc.setPlayConfigDetails (nch, nch, (double) in.sampleRate, 512);
    proc.prepareToPlay ((double) in.sampleRate, 512);
    setParam (proc, ids::bypass, 0.0f);
    setParam (proc, ids::key, argc > 4 ? (float) std::atof (argv[4]) : 0.0f);
    setParam (proc, ids::scale, argc > 5 ? (float) std::atof (argv[5]) : (float) harmonia::kChromaticScale);
    if (argc > 6)
        setParam (proc, ids::speed, (float) std::atof (argv[6]));
    if (argc > 7)
        setParam (proc, ids::natural, (float) std::atof (argv[7]));
    if (argc > 8)
        setParam (proc, ids::strength, (float) std::atof (argv[8]));

    const int latency = proc.getLatencySamples();
    const size_t total = frames + (size_t) latency;
    wavio::Wav out;
    out.sampleRate = in.sampleRate;
    out.channels = nch;
    out.data.assign ((size_t) nch, std::vector<float> (frames));

    juce::AudioBuffer<float> buf (nch, 512);
    juce::MidiBuffer midi;
    for (size_t pos = 0; pos < total; pos += 512)
    {
        const int n = (int) std::min<size_t> (512, total - pos);
        buf.setSize (nch, n, false, false, true);
        for (int c = 0; c < nch; ++c)
            for (int i = 0; i < n; ++i)
                buf.setSample (c, i, pos + (size_t) i < frames ? in.data[(size_t) c][pos + (size_t) i] : 0.0f);
        proc.processBlock (buf, midi);
        for (int c = 0; c < nch; ++c)
            for (int i = 0; i < n; ++i)
            {
                const size_t at = pos + (size_t) i;
                if (at >= (size_t) latency)
                    out.data[(size_t) c][at - (size_t) latency] = buf.getSample (c, i);
            }
    }
    if (! wavio::write (argv[3], out))
        return 1;
    std::printf ("processed %zu frames @ %d Hz through HarmoniaProcessor (latency %d samples)\n", frames, in.sampleRate, latency);
    return 0;
}

int main (int argc, char** argv)
{
    juce::ScopedJuceInitialiser_GUI init;
    if (argc >= 2 && std::string (argv[1]) == "selftest")
        return selftest();
    if (argc >= 3 && std::string (argv[1]) == "snapshot")
        return snapshot (argv[2], argc >= 4 ? std::atof (argv[3]) : 4.2, argc >= 6 ? argv[4] : nullptr, argc >= 6 ? std::atof (argv[5]) : 0.0);
    if (argc >= 4 && std::string (argv[1]) == "process")
        return processFile (argc, argv);
    std::printf ("usage: HarmoniaDevTool selftest | snapshot out.png | process in.wav out.wav [key scale speed natural strength]\n");
    return 1;
}
