// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 The Pantheon authors

// Headless smoke test for the JUCE layer: runs the real processor + editor without a host or a display.
//   ThemisSmoke [snapshot.png [graphs.png]]
// The optional second image shows every tab of the two history graphs.
#include "../plugin/Source/PluginEditor.h"
#include "../plugin/Source/PluginProcessor.h"

#include <cstdio>
#include <random>
#include <set>

static int failures = 0, checks = 0;
#define CHECK(cond, ...)                                       \
    do                                                         \
    {                                                          \
        ++checks;                                              \
        if (!(cond))                                           \
        {                                                      \
            ++failures;                                        \
            std::printf ("  FAIL line %d: %s  ", __LINE__, #cond); \
            std::printf ("" __VA_ARGS__);                      \
            std::printf ("\n");                                \
        }                                                      \
    } while (0)

static std::vector<float> pinkNoise (size_t n, unsigned seed, float amp)
{
    std::mt19937 rng (seed);
    std::uniform_real_distribution<float> d (-1.0f, 1.0f);
    std::vector<float> v (n);
    double b0 = 0, b1 = 0, b2 = 0, b3 = 0, b4 = 0, b5 = 0, b6 = 0;
    for (auto& x : v)
    {
        const double white = d (rng);
        b0 = 0.99886 * b0 + white * 0.0555179;
        b1 = 0.99332 * b1 + white * 0.0750759;
        b2 = 0.96900 * b2 + white * 0.1538520;
        b3 = 0.86650 * b3 + white * 0.3104856;
        b4 = 0.55000 * b4 + white * 0.5329522;
        b5 = -0.7616 * b5 - white * 0.0168980;
        x = static_cast<float> ((b0 + b1 + b2 + b3 + b4 + b5 + b6 + white * 0.5362) * 0.11 * amp);
        b6 = white * 0.115926;
    }
    return v;
}

// The analysis runs on a worker thread that polls every ~15 ms; give it time to drain what runAudio() queued.
static void settle() { juce::Thread::sleep (150); }

// Feeds a stereo signal through the processor in blocks, as a host would.
static void runBlocks (ThemisProcessor& proc, const float* l, const float* r, size_t total, int block)
{
    juce::AudioBuffer<float> buf (2, block);
    juce::MidiBuffer midi;
    for (size_t pos = 0; pos < total; pos += static_cast<size_t> (block))
    {
        const int n = static_cast<int> (std::min<size_t> (static_cast<size_t> (block), total - pos));
        buf.setSize (2, n, false, false, true);
        buf.copyFrom (0, 0, l + pos, n);
        buf.copyFrom (1, 0, r + pos, n);
        proc.processBlock (buf, midi);
    }
}

// `coherence` mixes the left channel into the right one: 0 = independent channels, 1 = mono.
static void runAudio (ThemisProcessor& proc, double seconds, double fs, int block, float amp, unsigned seed, float coherence = 0.0f)
{
    const size_t total = static_cast<size_t> (seconds * fs);
    const auto l = pinkNoise (total, seed, amp);
    auto r = pinkNoise (total, seed + 1, amp);
    for (size_t i = 0; i < total; ++i)
        r[i] += coherence * (l[i] - r[i]);
    runBlocks (proc, l.data(), r.data(), total, block);
}

// A short programme for the screenshot: a four-note bass line (fundamental + two harmonics, centred, with gaps)
// over pink noise whose right channel is `delaySeconds` late and 1 dB quieter, with a loud "chorus" driven into a hard
// clipper at -1 dBFS, so the phase check, the mono fold, the fundamental, the left/right balance, the clipping check
// and the level graph all have something to show.
static constexpr float kAirGain = 0.02f; // the widened top of the demo programme
static void runDemoProgramme (ThemisProcessor& proc, double seconds, double fs, int block, double delaySeconds)
{
    const size_t total = static_cast<size_t> (seconds * fs), delay = static_cast<size_t> (delaySeconds * fs);
    const auto noise = pinkNoise (total, 9, 0.05f);
    const auto air = pinkNoise (total, 21, kAirGain); // differenced twice below, so it sits in the top ranges; opposite on the two sides, so it is all side
    constexpr double notes[] = { 49.0, 55.0, 41.2, 61.7 }, kNoteSeconds = 5.0, kSoundingSeconds = 4.2;
    constexpr double kChorusStart = 33.0, kChorusEnd = 39.0;
    constexpr float kChorusGain = 2.6f, kCeiling = 0.891f /* -1 dBFS */, kRightTrim = 0.891f /* -1 dB */;
    std::vector<float> l (total), r (total);
    for (size_t i = 0; i < total; ++i)
    {
        const double t = static_cast<double> (i) / fs, phase = 2.0 * juce::MathConstants<double>::pi * notes[static_cast<size_t> (t / kNoteSeconds) % 4] * t;
        const bool sounding = std::fmod (t, kNoteSeconds) < kSoundingSeconds;
        const float bass = sounding ? static_cast<float> (0.30 * std::sin (phase) + 0.12 * std::sin (2.0 * phase) + 0.05 * std::sin (3.0 * phase)) : 0.0f;
        const float gain = t >= kChorusStart && t < kChorusEnd ? kChorusGain : 1.0f;
        const float wide = i > 1 ? air[i] - 2.0f * air[i - 1] + air[i - 2] : 0.0f;
        l[i] = std::clamp (gain * (bass + noise[i] + wide), -kCeiling, kCeiling);
        r[i] = std::clamp (gain * kRightTrim * (bass + noise[i >= delay ? i - delay : 0] - wide), -kCeiling, kCeiling);
    }
    // Fed in slices with a pause after each, so the analysis thread keeps up and the history graphs fill like in a host
    // (it skips frames when audio arrives much faster than real time).
    const size_t slice = static_cast<size_t> (2.0 * fs);
    for (size_t pos = 0; pos < total; pos += slice)
    {
        runBlocks (proc, l.data() + pos, r.data() + pos, std::min (slice, total - pos), block);
        settle();
    }
}

static constexpr int kEditorWidth = 1440, kEditorHeight = 900; // the editor's default size, and the size of the README screenshot

static void pump (int ms) { juce::MessageManager::getInstance()->runDispatchLoopUntil (ms); }

// Every tab of the two history graphs, stacked: the level graph (BANDS, MID / SIDE looking at the low range, LEFT / RIGHT,
// PEAKS, SPECTROGRAM) on the left and the fundamental graph (BALANCE, LEVELS, PITCH) on the right. Leaves the panels on their first tab.
static juce::Image renderGraphs (juce::Component& editor)
{
    ui::HistoryPanel *level = nullptr, *fundamental = nullptr;
    for (auto* child : editor.getChildren())
    {
        if (auto* p = dynamic_cast<ui::LevelView*> (child))
            level = p;
        else if (auto* f = dynamic_cast<ui::FundamentalView*> (child))
            fundamental = f;
    }
    if (level == nullptr || fundamental == nullptr)
        return {};

    const auto snapshot = [] (ui::HistoryPanel& panel, int tab, int column) {
        panel.selectTab (tab);
        panel.selectColumn (column);
        panel.update();
        return panel.createComponentSnapshot (panel.getLocalBounds(), true, 1.0f);
    };
    constexpr int kGap = 8;
    const int levelTabs[] = { 0, 1, 2, 3, 4 }, levelColumns[] = { themis::kAllRanges, 0, themis::kAllRanges, themis::kAllRanges, themis::kAllRanges };
    const int fundamentalTabs[] = { 0, 1, 2 };
    const int h = level->getHeight(), w = level->getWidth() + kGap + fundamental->getWidth();
    juce::Image out (juce::Image::RGB, w, 5 * h + 4 * kGap, true);
    juce::Graphics g (out);
    g.fillAll (colours::background);
    for (int i = 0; i < 5; ++i)
        g.drawImageAt (snapshot (*level, levelTabs[i], levelColumns[i]), 0, i * (h + kGap));
    for (int i = 0; i < 3; ++i)
        g.drawImageAt (snapshot (*fundamental, fundamentalTabs[i], themis::kAllRanges), level->getWidth() + kGap, i * (h + kGap));
    snapshot (*level, 0, themis::kAllRanges);
    snapshot (*fundamental, 0, themis::kAllRanges);
    return out;
}

// Clicks along a row of a graph and reports which tabs and range chips answered (the hit areas are laid out by the panel).
static void clickAlong (ui::HistoryPanel& panel, float y, float fromX, std::set<int>& tabs, std::set<int>& columns)
{
    for (float x = fromX; x < static_cast<float> (panel.getWidth()); x += 3.0f)
    {
        const juce::Point<float> pos (x, y);
        const auto now = juce::Time::getCurrentTime();
        const juce::MouseEvent e (juce::Desktop::getInstance().getMainMouseSource(), pos, juce::ModifierKeys(), 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, &panel, &panel, now, pos, now, 1, false);
        const int tabBefore = panel.selectedTab(), columnBefore = panel.selectedColumn();
        panel.mouseDown (e);
        if (panel.selectedTab() != tabBefore)
            tabs.insert (panel.selectedTab());
        if (panel.selectedColumn() != columnBefore)
            columns.insert (panel.selectedColumn());
    }
}

static bool writePng (const juce::Image& img, const char* path)
{
    juce::File png (path);
    png.deleteFile();
    juce::PNGImageFormat fmt;
    if (auto out = png.createOutputStream())
        fmt.writeImageToStream (img, *out);
    std::printf ("    snapshot -> %s\n", png.getFullPathName().toRawUTF8());
    return png.existsAsFile();
}

// The newest `n` frames of the history, oldest first.
static std::vector<themis::FrameStats> recentFrames (const ThemisProcessor& proc)
{
    std::vector<themis::FrameStats> frames (themis::FrameHistory::kCapacity);
    frames.resize (static_cast<size_t> (proc.frameHistory().copyRecent (frames.data(), themis::FrameHistory::kCapacity)));
    return frames;
}

int main (int argc, char** argv)
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    const double fs = 48000.0;

    ThemisProcessor proc;
    proc.setPlayConfigDetails (2, 2, fs, 512);
    proc.prepareToPlay (fs, 512);

    std::printf ("[1] audio passes through untouched, zero latency, curve builds up\n");
    CHECK (proc.getLatencySamples() == 0, "latency=%d", proc.getLatencySamples());
    CHECK (proc.getName() == juce::String (ui::kProjectWord) + " " + ui::kPluginWord, "name %s", proc.getName().toRawUTF8());
    {
        const size_t n = 512;
        const auto l = pinkNoise (n, 1, 0.5f), r = pinkNoise (n, 2, 0.5f);
        juce::AudioBuffer<float> buf (2, static_cast<int> (n));
        juce::MidiBuffer midi;
        buf.copyFrom (0, 0, l.data(), static_cast<int> (n));
        buf.copyFrom (1, 0, r.data(), static_cast<int> (n));
        proc.processBlock (buf, midi);
        bool identical = true;
        for (size_t i = 0; i < n; ++i)
            identical = identical && buf.getSample (0, static_cast<int> (i)) == l[i] && buf.getSample (1, static_cast<int> (i)) == r[i];
        CHECK (identical, "processBlock must not alter the audio");
    }
    {
        themis::BandArray m {};
        runAudio (proc, 14.0, fs, 512, 0.5f, 1);
        settle();
        CHECK (proc.readSpectrum (m), "analyser should have data after 14 s");
        const auto one = static_cast<size_t> (std::lround (themis::BandGrid::indexForHz (1000)));
        const auto eight = static_cast<size_t> (std::lround (themis::BandGrid::indexForHz (8000)));
        std::printf ("    pink noise: %.2f dB @1k, %.2f dB @8k (relative to anchor mean)\n", m[one], m[eight]);
        CHECK (std::abs (m[one]) < 1.5f && std::abs (m[eight]) < 1.5f);
        float crest = 0.0f;
        CHECK (proc.readLowCrest (crest), "low-end crest meter should have data after 14 s");
        std::printf ("    low-end crest of pink noise: %.2f dB\n", crest);
        CHECK (crest > 6.0f && crest < 16.0f, "noise crest factor should be a plausible 8-14 dB");
        proc.resetAnalyzer();
        runAudio (proc, 0.1, fs, 512, 0.5f, 1);
        settle();
        CHECK (!proc.readSpectrum (m), "reset should clear the curve");
        CHECK (!proc.readLowCrest (crest), "reset should clear the low-end crest");
    }

    std::printf ("[1a] audition: Solo / Mute change the output only while engaged; the measurements keep seeing the input\n");
    {
        const size_t n = 4800;
        const auto tone = [&] (double hz) {
            std::vector<float> v (n);
            for (size_t i = 0; i < n; ++i)
                v[i] = static_cast<float> (0.25 * std::sin (2.0 * juce::MathConstants<double>::pi * hz * static_cast<double> (i) / fs));
            return v;
        };
        const auto l = tone (300.0); // a mono signal: soloing the side leaves nothing
        juce::MidiBuffer midi;
        const auto run = [&] (int blocks) {
            juce::AudioBuffer<float> buf (2, static_cast<int> (n));
            for (int b = 0; b < blocks; ++b)
            {
                buf.copyFrom (0, 0, l.data(), static_cast<int> (n));
                buf.copyFrom (1, 0, l.data(), static_cast<int> (n));
                proc.processBlock (buf, midi);
            }
            return buf;
        };
        const auto peak = [&] (const juce::AudioBuffer<float>& b) { return b.getMagnitude (0, 0, static_cast<int> (n)); };

        proc.resetAnalyzer();
        run (1); // applies the reset
        themis::Switches side;
        side.solo = themis::Switches::bit (themis::Source::side);
        proc.setAudition (side);
        CHECK (proc.auditionSwitches() == side);
        const auto soloed = run (3);
        CHECK (peak (soloed) < 1.0e-6f, "soloing the side of a mono signal should be silent, peak %.6f", static_cast<double> (peak (soloed)));
        CHECK (proc.getLatencySamples() == 0);

        proc.setNonRealtime (true); // a render is never altered
        const auto rendered = run (3);
        CHECK (peak (rendered) > 0.24f, "an offline render must ignore the audition, peak %.4f", static_cast<double> (peak (rendered)));
        proc.setNonRealtime (false);

        proc.setAudition ({});
        const auto released = run (3);
        bool identical = true;
        for (size_t i = 0; i < n; ++i)
            identical = identical && released.getSample (0, static_cast<int> (i)) == l[i] && released.getSample (1, static_cast<int> (i)) == l[i];
        CHECK (identical, "releasing every button must restore the input bit for bit");
        proc.resetAnalyzer();
        run (1);
    }

    std::printf ("[1a'] streaming preview and the band hand-off: a loud master is turned down only while the preview is on\n");
    {
        const int n = 4800;
        std::vector<float> loud (static_cast<size_t> (n));
        for (int i = 0; i < n; ++i)
            loud[static_cast<size_t> (i)] = static_cast<float> (0.9 * std::sin (2.0 * juce::MathConstants<double>::pi * 1000.0 * i / fs));
        juce::MidiBuffer midi;
        juce::AudioBuffer<float> buf (2, n);
        const auto run = [&] (int blocks) {
            for (int b = 0; b < blocks; ++b)
            {
                buf.copyFrom (0, 0, loud.data(), n);
                buf.copyFrom (1, 0, loud.data(), n);
                proc.processBlock (buf, midi);
            }
            return buf.getMagnitude (0, 0, n);
        };
        proc.resetAnalyzer();
        run (60); // 6 s: past the loudness monitor's minimum
        proc.setStreamingPreview (true);
        const float expected = juce::Decibels::decibelsToGain (proc.previewGainDb()) * 0.9f;
        const float previewed = run (3);
        CHECK (proc.previewGainDb() < -5.0f && std::abs (previewed - expected) < 0.01f,
               "the preview turns a -1 dBFS sine down to the target: gain %.1f dB, peak %.3f (expected %.3f)",
               static_cast<double> (proc.previewGainDb()), static_cast<double> (previewed), static_cast<double> (expected));
        proc.setNonRealtime (true);
        CHECK (run (1) > 0.89f, "an offline render ignores the preview");
        proc.setNonRealtime (false);
        proc.setStreamingPreview (false);
        run (1);
        bool identical = true;
        run (1);
        for (int i = 0; i < n; ++i)
            identical = identical && buf.getSample (0, i) == loud[static_cast<size_t> (i)];
        CHECK (identical, "switching the preview off restores the input bit for bit");
        proc.resetAnalyzer();
        run (1);

        themis::EqMove cut;
        cut.kind = themis::EqMove::Kind::lowCut;
        cut.place = themis::EqMove::Place::side;
        cut.freqHz = 150.0f;
        std::vector<pdsp::LinkBand> back;
        CHECK (pdsp::link::decode (ui::linkText ({ cut }).toStdString(), back) && back.size() == 1 && back[0].kind == pdsp::LinkBand::Kind::lowCut
                   && back[0].place == pdsp::LinkBand::Place::side && std::abs (back[0].freq - 150.0) < 0.5,
               "a copied side low cut reads back as one");
        // A key from a newer writer is ignored whatever its value, so the band still arrives (the format's promise).
        CHECK (pdsp::link::decode ("pantheon-bands 1\nbell freq=270 gain=-2.0 q=0.70 place=mid shape=tilt\nbell freq=500 gain=1 q=1", back)
                   && back.size() == 2 && std::abs (back[0].freq - 270.0) < 0.5 && back[0].place == pdsp::LinkBand::Place::mid,
               "an unknown key with a non-numeric value does not drop the band");
        CHECK (pdsp::link::decode ("pantheon-bands 1\nbell freq=abc gain=1 q=1", back) && back.empty(),
               "a known key with a bad number still drops the band");
        // Copy EQ hands the target curve over with its bands (Iris draws it as its reference); a reader without curves still gets the bands.
        const auto target = proc.activeTarget();
        pdsp::LinkCurve curve;
        CHECK (pdsp::link::decode (ui::linkText ({ cut }, &target).toStdString(), back, &curve) && back.size() == 1 && curve.valid()
                   && curve.db.size() == static_cast<size_t> (themis::BandGrid::kNum) && curve.name == target.name,
               "a copied target curve travels with the bands");
        bool curveOk = true;
        for (size_t i = 0; i < curve.db.size(); ++i)
            curveOk = curveOk && std::abs (curve.db[i] - static_cast<double> (target.medianDb[i])) < 0.06;
        CHECK (curveOk && std::abs (curve.frequency (10) - themis::BandGrid::centreHz (10)) < 1.0e-6, "... point for point, on the analysis grid");
        CHECK (pdsp::link::decode (ui::linkText ({ cut }, &target).toStdString(), back) && back.size() == 1, "... and a reader without curves skips it");
    }

    std::printf ("[1b] stereo scope: correlation follows the channel coherence, level does not matter\n");
    {
        const auto& scope = proc.stereoScope();
        runAudio (proc, 3.0, fs, 512, 0.5f, 1, 0.0f);
        CHECK (scope.hasData());
        std::printf("    independent channels: correlation %.2f\n", scope.correlation());
        CHECK (std::abs (scope.correlation()) < 0.25f);
        runAudio (proc, 3.0, fs, 512, 0.5f, 1, 1.0f);
        CHECK (scope.correlation() > 0.99f);
        runAudio (proc, 3.0, fs, 512, 0.0005f, 1, 1.0f); // 60 dB quieter
        CHECK (scope.correlation() > 0.99f);
        float l[64], r[64];
        CHECK (scope.copyRecent (l, r, 64) == 64);
        CHECK (std::abs (l[63] - r[63]) < 1.0e-6f);
        proc.resetAnalyzer();
        runAudio (proc, 0.1, fs, 512, 0.5f, 1);
        settle();
    }

    std::printf ("[1c] mono fold + device preview: reads the analysis, nothing touches the audio\n");
    {
        themis::BandArray fold {};
        const auto at = [&] (double hz) { return fold[static_cast<size_t> (std::lround (themis::BandGrid::indexForHz (hz)))]; };
        proc.resetAnalyzer();
        runAudio (proc, 0.1, fs, 512, 0.5f, 1);
        settle();
        CHECK (!proc.readMonoFold (fold), "reset should clear the mono fold");

        runAudio (proc, 8.0, fs, 512, 0.5f, 1, 0.0f);
        settle();
        CHECK (proc.readMonoFold (fold));
        std::printf ("    independent channels: mono fold %.2f dB @100, %.2f dB @4k\n", at (100), at (4000));
        CHECK (std::abs (at (100) + 3.0f) < 1.5f && std::abs (at (4000) + 3.0f) < 1.5f);

        proc.resetAnalyzer();
        runAudio (proc, 0.1, fs, 512, 0.5f, 1);
        settle(); // the reset is applied by the worker; audio pushed before that would be discarded
        runAudio (proc, 8.0, fs, 512, 0.5f, 1, 1.0f);
        settle();
        CHECK (proc.readMonoFold (fold));
        CHECK (std::abs (at (100)) < 0.5f && std::abs (at (4000)) < 0.5f);

        CHECK (proc.activeDevice() != nullptr && proc.activeDevice()->name == "Phone speaker"); // default
        auto* dev = proc.apvts.getParameter (params::device);
        dev->setValueNotifyingHost (dev->convertTo0to1 (0.0f));
        CHECK (proc.activeDevice() == nullptr);
        dev->setValueNotifyingHost (dev->convertTo0to1 (static_cast<float> (params::deviceNames().size() - 1)));
        CHECK (proc.activeDevice() == &themis::DeviceProfile::builtIns().back());
        dev->setValueNotifyingHost (dev->convertTo0to1 (1.0f));
        proc.resetAnalyzer();
        runAudio (proc, 0.1, fs, 512, 0.5f, 1);
        settle();
    }

    std::printf ("[1d] phase check + history: a polarity flip is caught without any mid signal, and the graphs get frames\n");
    {
        themis::PhaseBands ph {};
        proc.resetAnalyzer();
        runAudio (proc, 0.1, fs, 512, 0.5f, 1);
        settle();
        CHECK (!proc.readPhase (ph), "reset should clear the phase data");
        CHECK (proc.frameHistory().sequence() <= 2, "reset should clear the history");

        const auto l = pinkNoise (static_cast<size_t> (10.0 * fs), 1, 0.5f);
        auto inverted = l;
        for (auto& x : inverted)
            x = -x;
        runBlocks (proc, l.data(), inverted.data(), l.size(), 512);
        settle();
        CHECK (proc.readPhase (ph));
        const auto report = themis::analysePhase (ph);
        std::printf ("    inverted pink noise: cause %d, level %d, %d bands flagged\n", static_cast<int> (report.cause),
                     static_cast<int> (report.level), report.flaggedBands);
        CHECK (report.cause == themis::PhaseCause::polarity && report.level == themis::Level::bad);

        themis::FrameStats frames[16];
        const int n = proc.frameHistory().copyRecent (frames, 16);
        CHECK (n == 16, "history should hold the analysed frames, got %d", n);
        CHECK (n > 0 && frames[n - 1].energy (themis::Signal::stereo, 1) > -40.0f && frames[n - 1].energy (themis::Signal::stereo, 1) < 0.0f, "low-mid energy %.1f dB", n > 0 ? frames[n - 1].energy (themis::Signal::stereo, 1) : 0.0f);
        proc.resetAnalyzer();
        runAudio (proc, 0.1, fs, 512, 0.5f, 1);
        settle();
    }

    std::printf ("[1e] clipping: clean audio reads clean, a hard clipper is found, reset clears it\n");
    {
        proc.resetAnalyzer();
        runAudio (proc, 0.1, fs, 512, 0.5f, 1);
        settle();
        themis::ClipTotals totals;
        runAudio (proc, 3.0, fs, 512, 0.2f, 1);
        settle();
        CHECK (proc.readClip (totals), "the clip meter should have data after audio");
        std::printf ("    clean pink noise: sample peak %.1f dBFS, true peak %.1f dBTP, %llu clips, %llu overs\n", totals.samplePeakDb, totals.truePeakDb,
                     static_cast<unsigned long long> (totals.clipEvents), static_cast<unsigned long long> (totals.overSamples));
        CHECK (totals.clipEvents == 0 && totals.overSamples == 0 && totals.truePeakDb >= totals.samplePeakDb);
        CHECK (themis::history::clip::level (totals.truePeakDb, totals.clipEvents, totals.overSamples) != themis::Level::bad);

        std::vector<float> hot (static_cast<size_t> (3.0 * fs));
        for (size_t i = 0; i < hot.size(); ++i)
            hot[i] = std::clamp (1.6f * static_cast<float> (std::sin (2.0 * juce::MathConstants<double>::pi * 220.0 * static_cast<double> (i) / fs)), -0.95f, 0.95f);
        runBlocks (proc, hot.data(), hot.data(), hot.size(), 512);
        settle();
        CHECK (proc.readClip (totals));
        std::printf ("    clipped 220 Hz tone: sample peak %.2f dBFS, %llu clips\n", totals.samplePeakDb, static_cast<unsigned long long> (totals.clipEvents));
        CHECK (totals.clipEvents > 500 && std::abs (totals.samplePeakDb + 0.45f) < 0.05f);
        CHECK (themis::history::clip::level (totals.truePeakDb, totals.clipEvents, totals.overSamples) == themis::Level::bad);
        const auto frames = recentFrames (proc);
        CHECK (std::any_of (frames.begin(), frames.end(), [] (const themis::FrameStats& f) { return f.clipped(); }), "the history should flag the clipped frames");

        proc.resetAnalyzer();
        CHECK (!proc.readClip (totals), "reset should clear the clip totals");
        runAudio (proc, 0.1, fs, 512, 0.5f, 1);
        settle();
    }

    std::printf ("[2] averaging parameter reaches the analyser setting\n");
    {
        auto* p = proc.apvts.getParameter (params::avgTime);
        p->setValueNotifyingHost (p->convertTo0to1 (3.0f));
        CHECK (proc.paramReader.averagingSeconds() == 0.0f);
        p->setValueNotifyingHost (p->convertTo0to1 (0.0f));
        CHECK (proc.paramReader.averagingSeconds() == 10.0f);
    }

    std::printf ("[4] state round trip incl. custom curve\n");
    {
        themis::TargetCurve c = themis::TargetCurve::builtIns()[2];
        c.name = "RoundTrip";
        proc.setCustomCurve (c, true);
        proc.apvts.getParameter (params::tolerance)->setValueNotifyingHost (proc.apvts.getParameter (params::tolerance)->convertTo0to1 (1.5f));
        juce::MemoryBlock blob;
        proc.getStateInformation (blob);

        ThemisProcessor other;
        other.setPlayConfigDetails (2, 2, fs, 512);
        other.prepareToPlay (fs, 512);
        other.setStateInformation (blob.getData(), static_cast<int> (blob.getSize()));
        CHECK (std::abs (other.paramReader.value (params::tolerance) - 1.5f) < 0.01f);
        CHECK (other.hasCustomCurve());
        CHECK (other.activeTarget().name == "RoundTrip");
        CHECK (std::abs (other.activeTarget().medianDb[10] - c.medianDb[10]) < 0.01f);
    }

    std::printf ("[5] learn a target from an audio file\n");
    {
        const auto wav = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("themis_smoke_pink.wav");
        wav.deleteFile();
        {
            juce::WavAudioFormat fmt;
            std::unique_ptr<juce::OutputStream> out (wav.createOutputStream());
            const auto options = juce::AudioFormatWriterOptions().withSampleRate (44100.0).withNumChannels (2).withBitsPerSample (16);
            auto w = fmt.createWriterFor (out, options); // takes the stream on success
            if (w != nullptr)
            {
                const auto l = pinkNoise (44100 * 14, 5, 0.5f), r = pinkNoise (44100 * 14, 6, 0.5f);
                const float* chans[] = { l.data(), r.data() };
                w->writeFromFloatArrays (chans, 2, static_cast<int> (l.size()));
            }
        }
        CHECK (wav.existsAsFile());
        bool done = false;
        juce::String error = "not called";
        proc.learnFromFiles ({ wav }, [&] (const juce::String& e) {
            error = e;
            done = true;
        });
        for (int i = 0; i < 400 && !done; ++i)
            pump (50);
        CHECK (done, "callback never fired");
        CHECK (error.isEmpty(), "%s", error.toRawUTF8());
        const auto t = proc.activeTarget();
        std::printf ("    learned \"%s\"; median @1k = %.2f dB, @100 = %.2f dB, @8k = %.2f dB\n", t.name.c_str(),
                     t.medianDb[static_cast<size_t> (std::lround (themis::BandGrid::indexForHz (1000)))],
                     t.medianDb[static_cast<size_t> (std::lround (themis::BandGrid::indexForHz (100)))],
                     t.medianDb[static_cast<size_t> (std::lround (themis::BandGrid::indexForHz (8000)))]);
        CHECK (t.name == "themis_smoke_pink");
        CHECK (std::abs (t.medianDb[static_cast<size_t> (std::lround (themis::BandGrid::indexForHz (1000)))]) < 1.5f);
        CHECK (std::abs (t.medianDb[static_cast<size_t> (std::lround (themis::BandGrid::indexForHz (8000)))]) < 1.5f);
        wav.deleteFile();

        proc.learnFromFiles ({ juce::File ("/nonexistent/nothing.wav") }, [&] (const juce::String& e) {
            error = e;
            done = true;
        });
        done = false;
        for (int i = 0; i < 100 && !done; ++i)
            pump (50);
        CHECK (done && error.isNotEmpty(), "bad file should report an error");
    }

    std::printf ("[6] editor builds, paints and survives resizing\n");
    {
        // The screenshot: a built-in target (like a real session) and the demo programme, which has 0.5 ms of channel delay.
        auto* target = proc.apvts.getParameter (params::target);
        target->setValueNotifyingHost (target->convertTo0to1 (static_cast<float> (params::kDefaultTarget)));
        auto* avg = proc.apvts.getParameter (params::avgTime);
        avg->setValueNotifyingHost (avg->convertTo0to1 (static_cast<float> (params::kDefaultAvgTime)));
        proc.resetAnalyzer();
        runAudio (proc, 0.1, fs, 512, 0.5f, 1);
        settle(); // the reset is applied by the worker; audio pushed before that would be discarded
        runDemoProgramme (proc, 45.0, fs, 512, 0.0005);
        settle();
        themis::PhaseBands ph {};
        CHECK (proc.readPhase (ph));
        const auto phase = themis::analysePhase (ph);
        CHECK (phase.cause == themis::PhaseCause::delay && std::abs (phase.delayMs - 0.5f) < 0.05f, "demo programme: cause %d, delay %.2f ms",
               static_cast<int> (phase.cause), phase.delayMs);
        themis::ClipTotals clipTotals;
        CHECK (proc.readClip (clipTotals));
        CHECK (clipTotals.clipEvents > 100 && clipTotals.truePeakDb < 0.0f, "demo programme: %llu clips, true peak %.2f dBTP",
               static_cast<unsigned long long> (clipTotals.clipEvents), clipTotals.truePeakDb);
        {
            const auto frames = recentFrames (proc);
            const auto bass = themis::history::analyseBass (frames.data(), static_cast<int> (frames.size()));
            const auto peaks = themis::history::analysePeaks (frames.data(), static_cast<int> (frames.size()));
            const auto width = themis::history::analyseWidth (frames.data(), static_cast<int> (frames.size()), 0);
            const auto balance = themis::history::analyseBalance (frames.data(), static_cast<int> (frames.size()), themis::kAllRanges);
            for (int c = 1; c < themis::kNumEnergyRanges; ++c)
            {
                const auto w = themis::history::analyseWidth (frames.data(), static_cast<int> (frames.size()), c);
                std::printf ("    %s width: side %+.1f dB against mid, a mono sum loses %.1f dB\n", themis::energyColumnName (c), w.sideMinusMidDb, w.monoLossDb);
            }
            std::printf ("    bass %.0f-%.0f Hz, %d clipped frames, low-end width S-M %.1f dB, left-right %.2f dB\n", bass.lowHz, bass.highHz,
                         peaks.clippedFrames, width.sideMinusMidDb, balance.leftMinusRightDb);
            CHECK (bass.have && peaks.have && peaks.clippedFrames > 3 && peaks.clippedFrames < 90 && peaks.level == themis::Level::bad);
            CHECK (balance.have && std::abs (balance.leftMinusRightDb - 1.0f) < 0.4f);
        }
        std::unique_ptr<juce::AudioProcessorEditor> editor (proc.createEditor());
        CHECK (editor != nullptr);
        if (editor != nullptr)
        {
            editor->setSize (kEditorWidth, kEditorHeight);
            pump (700); // let the 30 Hz timer pull data and the plan refresh
            AdvicePanel* plan = nullptr;
            AuditionPanel* auditionPanel = nullptr;
            for (auto* child : editor->getChildren())
            {
                plan = plan != nullptr ? plan : dynamic_cast<AdvicePanel*> (child);
                auditionPanel = auditionPanel != nullptr ? auditionPanel : dynamic_cast<AuditionPanel*> (child);
            }
            CHECK (plan != nullptr && auditionPanel != nullptr);
            if (plan != nullptr)
            {
                const auto& advice = plan->currentAdvice();
                std::printf ("    master plan: %d checks, %d passed, %d items\n", advice.checks, advice.passed, static_cast<int> (advice.items.size()));
                for (const auto& item : advice.items)
                    std::printf ("      [%s] %s\n", item.level == themis::Level::bad ? "bad " : "warn", item.headline.c_str());
                CHECK (!advice.measuring && !advice.items.empty());
                const auto has = [&] (themis::Topic t) { return std::any_of (advice.items.begin(), advice.items.end(), [t] (const themis::Advice& a) { return a.topic == t; }); };
                CHECK (has (themis::Topic::clipping) && has (themis::Topic::phase), "the demo programme clips and has a channel delay");
                CHECK (has (themis::Topic::width), "the demo programme has a widened top");
                CHECK (advice.level() == themis::Level::bad && advice.items.front().level == themis::Level::bad);

                // The HEAR chip of the first item that has one: click it, the buttons follow; click again, they release.
                const auto hear = plan->firstHearable();
                CHECK (hear.active(), "some item should be audible");
                const auto before = proc.auditionSwitches();
                CHECK (!before.active());
                proc.setAudition (hear);
                pump (150);
                CHECK (auditionPanel != nullptr && auditionPanel->isVisible());
                if (argc > 1)
                    std::printf ("    screenshot taken with %s soloed\n", themis::soloText (hear).c_str());
            }
            auto img = editor->createComponentSnapshot (editor->getLocalBounds(), true, 1.0f);
            CHECK (img.isValid() && img.getWidth() == kEditorWidth && img.getHeight() == kEditorHeight);
            if (argc > 1)
                CHECK (writePng (img, argv[1]));
            for (auto* child : editor->getChildren())
                if (auto* panel = dynamic_cast<ui::HistoryPanel*> (child))
                {
                    std::set<int> tabs, columns;
                    panel->selectTab (0);
                    clickAlong (*panel, 16.0f, 4.0f, tabs, columns); // the title row: the tabs on the left...
                    CHECK (tabs.count (1) == 1 && tabs.count (2) == 1, "clicking the tabs should select them");
                    if (dynamic_cast<ui::LevelView*> (panel) != nullptr)
                        CHECK (tabs.count (4) == 1, "the SPECTROGRAM tab can be clicked");
                    if (dynamic_cast<ui::LevelView*> (panel) != nullptr)
                    {
                        panel->selectTab (1); // ...and, on a tab that has them, the range chips on the right
                        clickAlong (*panel, 16.0f, static_cast<float> (panel->getWidth()) * 0.5f, tabs, columns);
                        CHECK (columns.size() >= 4, "clicking the range chips should select %d ranges", static_cast<int> (columns.size()));
                    }
                    panel->selectTab (0);
                    panel->selectColumn (themis::kAllRanges);
                }
            proc.setAudition ({}); // the graphs are shown without it
            pump (100);
            const auto graphs = renderGraphs (*editor);
            CHECK (graphs.isValid());
            if (argc > 2 && graphs.isValid())
                CHECK (writePng (graphs, argv[2]));
            editor->setSize (1120, 760);
            editor->setSize (2200, 1300);
            pump (100);
        }
    }

    proc.releaseResources();
    std::printf ("\n%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
