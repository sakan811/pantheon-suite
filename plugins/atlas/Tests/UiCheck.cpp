// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 The Pantheon authors

// Builds the editor on the real processor and checks what a host and a user would meet: the parameters, the name, the strips,
// and that solo and delta are monitoring that ends with the window.

#include "Advice.h"
#include "Check.h"
#include "PluginEditor.h"
#include <pantheon/ui/Scaled.h>

using namespace atlas;
using test::check;
using test::fmt;

int main()
{
    juce::ScopedJuceInitialiser_GUI gui;
    std::printf ("Atlas UI check\n");
    AtlasProcessor proc;
    proc.setPlayConfigDetails (2, 2, 48000.0, 512);
    proc.prepareToPlay (48000.0, 512);

    check (proc.getName() == juce::String (gui::kProjectWord) + " " + gui::kPluginWord, "the reported name is the nameplate's two words");
    check (proc.getParameters().size() == 5 + 2 * kCrossovers + 10 * kBands,
           fmt ("%d parameters: phase, oversampling, output, look-ahead, mix, the %d splits (frequency and on) and 10 for each of %d bands", proc.getParameters().size(), kCrossovers, kBands));
    check (channelNames.size() == kNumChannelModes && oversamplingNames.size() == kNumOsModes && lookaheadNames().size() == kNumLookahead && listenNames.size() == 2 && directionNames.size() == 2, "the choice lists match the engine's modes");
    for (int k = 0; k < kCrossovers; ++k)
        check (std::fabs (proc.apvts.getRawParameterValue (id::crossover[(size_t) k])->load() - (float) pdsp::bands::crossoverHz (k)) < 0.5f,
               fmt ("crossover %d starts on the shared band edge, %.0f Hz", k + 1, pdsp::bands::crossoverHz (k)));

    // The bands: one to six, made by splitting and joining. The settings travel with the bands.
    {
        AtlasProcessor q;
        auto& s = q.apvts;
        check (layoutOf (s).numBands == 6 && addSplit (s, 1000.0) == -1, "a new instance has the six bands of music, and a seventh is refused");
        setValue (s, bandIds (1).threshold, -21.0f);
        setValue (s, bandIds (4).threshold, -33.0f);
        check (removeSplit (s, 1) && layoutOf (s).numBands == 5 && std::fabs (layoutOf (s).edgeHz[2] - 500.0) < 1.0
                   && std::fabs (readParams (s).bands[1].thresholdDb + 21.0) < 0.01 && std::fabs (readParams (s).bands[3].thresholdDb + 33.0) < 0.01,
               "joining bands 2 and 3: five bands, band 2 now reaches 500 Hz with its own settings, and the bands above move down a strip with theirs");
        check (addSplit (s, 510.0) == -1 && addSplit (s, 30.0) == -1, "a split too close to an edge, or under 40 Hz, is refused");
        const int made = addSplit (s, 120.0);
        const auto l = layoutOf (s);
        check (made == 2 && l.numBands == 6 && std::fabs (l.edgeHz[2] - 120.0) < 1.0 && std::fabs (readParams (s).bands[2].thresholdDb + 21.0) < 0.01
                   && std::fabs (readParams (s).bands[4].thresholdDb + 33.0) < 0.01 && bandOwners (readParams (s).split) == std::array<int, kBands> { 0, 1, 2, 3, 4, 5 },
               "splitting band 2 at 120 Hz: both halves have its settings, the bands above move up, six bands again");
        for (int k = 0; k < 5; ++k) removeSplit (s, 0);
        check (layoutOf (s).numBands == 1 && ! removeSplit (s, 0) && bandOwners (readParams (s).split) == std::array<int, kBands> { 0, 0, 0, 0, 0, 0 },
               "every split removed: one band over the whole spectrum");
        moveSplit (s, addSplit (s, 300.0) - 1, 5.0);
        check (std::fabs (layoutOf (s).edgeHz[1] - kMinCrossoverHz) < 1.0, "an edge dragged off the bottom stops at 40 Hz");
    }

    // A split added below bands that sit in scattered strips: every band must get the settings of its own strip, not of one an earlier copy wrote.
    {
        AtlasProcessor q;
        auto& s = q.apvts;
        for (int k : { 0, 1, 3 }) setValue (s, id::split[(size_t) k], 0.0f); // bands on strips 0, 3 and 5
        setValue (s, bandIds (0).threshold, -11.0f);
        setValue (s, bandIds (3).threshold, -22.0f);
        setValue (s, bandIds (5).threshold, -33.0f);
        const bool scattered = layoutOf (s).numBands == 3 && layoutOf (s).slot[1] == 3 && layoutOf (s).slot[2] == 5;
        const int made = addSplit (s, 100.0);
        const auto e = readParams (s);
        check (scattered && made == 1 && layoutOf (s).numBands == 4 && std::fabs (e.bands[0].thresholdDb + 11.0) < 0.01 && std::fabs (e.bands[1].thresholdDb + 11.0) < 0.01
                   && std::fabs (e.bands[2].thresholdDb + 22.0) < 0.01 && std::fabs (e.bands[3].thresholdDb + 33.0) < 0.01,
               "a split below bands on strips 0, 3 and 5: the new band has the lower band's settings and the others keep their own (copies read a snapshot)");
    }

    auto& monitor = proc.engine.monitor;
    {
        std::unique_ptr<juce::AudioProcessorEditor> holder (proc.createEditor());
        auto& editor = ui::innerOf<AtlasEditor> (*holder);
        editor.setVisible (true);
        check (editor.getWidth() > 900 && editor.strips[5]->bounds.getRight() <= editor.getWidth() && editor.strips[5]->crossover == nullptr && editor.strips[0]->crossover != nullptr,
               "six strips fit the window; every band but the top one has the knob of its upper edge");
        check (editor.strips[2]->channels.getNumItems() == kNumChannelModes, "a strip's channel selector has the parameter's seven items");

        editor.strips[1]->solo.triggerClick();
        editor.strips[3]->solo.triggerClick();
        juce::MessageManager::getInstance()->runDispatchLoopUntil (100);
        check (monitor.solo.load() == ((1u << 1) | (1u << 3)), "SOLO on two strips solos those two bands");
        check (editor.strips[1]->solo.getToggleState() && ! editor.strips[0]->solo.getToggleState(), "... and their buttons light up");
        editor.strips[1]->solo.triggerClick();
        editor.strips[4]->delta.triggerClick();
        juce::MessageManager::getInstance()->runDispatchLoopUntil (100);
        check (monitor.solo.load() == (1u << 3) && monitor.delta.load() == (1u << 4), "a second click releases a solo; DELTA is per band");
        editor.clearButton.triggerClick();
        juce::MessageManager::getInstance()->runDispatchLoopUntil (100);
        check (monitor.solo.load() == 0u && monitor.delta.load() == 0u, "ALL BANDS ends every solo and delta");

        editor.strips[0]->channels.setSelectedItemIndex ((int) Channels::midSide, juce::sendNotificationSync);
        check (readParams (proc.apvts).bands[0].channels == Channels::midSide, "the channel selector sets the band's parameter");

        // The display: a double-click splits, a double-click on an edge joins, a drag moves an edge.
        auto& view = editor.view;
        const auto clickAt = [&] (double hz, int clicks)
        {
            const juce::Point<float> at (view.xForHz (hz), (float) view.getHeight() * 0.5f);
            return juce::MouseEvent (juce::Desktop::getInstance().getMainMouseSource(), at, juce::ModifierKeys::leftButtonModifier, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f,
                                     &view, &view, juce::Time::getCurrentTime(), at, juce::Time::getCurrentTime(), clicks, false);
        };
        view.mouseDoubleClick (clickAt (2000.0, 2));
        check (layoutOf (proc.apvts).numBands == 5 && ! editor.strips[5]->on.isVisible() && editor.strips[4]->on.isVisible(),
               "a double-click on the 2 kHz edge joins the two bands: five strips are shown");
        view.mouseDoubleClick (clickAt (3000.0, 2));
        check (layoutOf (proc.apvts).numBands == 6 && std::fabs (layoutOf (proc.apvts).edgeHz[4] - 3000.0) < 40.0 && editor.strips[5]->on.isVisible(),
               "a double-click at 3 kHz splits the band there: six strips again");
        view.mouseDown (clickAt (layoutOf (proc.apvts).edgeHz[4], 1));
        view.mouseDrag (clickAt (2500.0, 1));
        view.mouseUp (clickAt (2500.0, 1));
        check (std::fabs (layoutOf (proc.apvts).edgeHz[4] - 2500.0) < 40.0, "dragging the edge moves the split");

        // A region that leaps, made a band from its marker: the splits are added and the band listens for its threshold.
        removeSplit (proc.apvts, 0);
        removeSplit (proc.apvts, 0);
        pdsp::IssueFinder::Swing swing { 70.0, 160.0, 14.0f, -20.0f };
        view.makeBand (swing);
        const auto made = layoutOf (proc.apvts);
        check (made.numBands == 6 && std::fabs (made.edgeHz[1] - 70.0) < 1.0 && std::fabs (made.edgeHz[2] - 160.0) < 1.0 && readParams (proc.apvts).bands[1].on,
               "a JUMPS region of 70 to 160 Hz becomes a band with those edges, switched on");
        juce::AudioBuffer<float> audio (2, 512);
        juce::MidiBuffer noMidi;
        for (int block = 0; block < 260; ++block)
        {
            for (int i = 0; i < 512; ++i)
            {
                const float v = 0.25f * std::sin (6.2831853f * 100.0f * (float) (block * 512 + i) / 48000.0f);
                audio.setSample (0, i, v);
                audio.setSample (1, i, v);
            }
            proc.processBlock (audio, noMidi);
            if (block % 3 == 0)
                juce::MessageManager::getInstance()->runDispatchLoopUntil (34);
        }
        const float learned = (float) readParams (proc.apvts).bands[1].thresholdDb;
        const float read = proc.engine.levelMeter[1].load(); // the tone is about 3 dB down inside a 70 to 160 Hz band
        // The LEARN button of a strip: the same listening, with a 6 dB swing, so the threshold is 3 dB under the loudest the band read.
        setValue (proc.apvts, bandIds (1).threshold, 0.0f);
        editor.strips[1]->learn.triggerClick();
        juce::MessageManager::getInstance()->runDispatchLoopUntil (50);
        check (view.learningSlot() == 1, "LEARN on a strip starts listening to that band");
        for (int block = 0; block < 260; ++block)
        {
            for (int i = 0; i < 512; ++i)
            {
                const float v = 0.25f * std::sin (6.2831853f * 100.0f * (float) (block * 512 + i) / 48000.0f);
                audio.setSample (0, i, v);
                audio.setSample (1, i, v);
            }
            proc.processBlock (audio, noMidi);
            if (block % 3 == 0)
                juce::MessageManager::getInstance()->runDispatchLoopUntil (34);
        }
        const float learnedByButton = (float) readParams (proc.apvts).bands[1].thresholdDb;
        check (view.learningSlot() < 0 && std::fabs (learnedByButton - (proc.engine.levelMeter[1].load() - 3.0f)) < 0.5f,
               fmt ("... and two seconds later THRESH is %.1f dB, 3 dB under what the band reads (%.1f dB)", learnedByButton, (double) proc.engine.levelMeter[1].load()));

        // TO ALL: THRESH, RATIO, ATTACK and RELEASE go to every band, and nothing else does.
        setValue (proc.apvts, bandIds (1).ratio, 7.0f);
        setValue (proc.apvts, bandIds (1).attack, 33.0f);
        setValue (proc.apvts, bandIds (1).release, 444.0f);
        setValue (proc.apvts, bandIds (1).makeup, 5.0f);
        setValue (proc.apvts, bandIds (4).channels, (float) (int) Channels::midSide);
        editor.strips[1]->toAll.triggerClick();
        juce::MessageManager::getInstance()->runDispatchLoopUntil (50);
        bool all = true;
        const auto after = readParams (proc.apvts);
        for (int band = 0; band < layoutOf (proc.apvts).numBands; ++band)
            all = all && std::fabs (after.bands[(size_t) band].thresholdDb - learnedByButton) < 0.01 && std::fabs (after.bands[(size_t) band].ratio - 7.0) < 0.01
                  && std::fabs (after.bands[(size_t) band].attackMs - 33.0) < 0.01 && std::fabs (after.bands[(size_t) band].releaseMs - 444.0) < 0.01;
        check (all && std::fabs (after.bands[3].makeupDb) < 0.01 && after.bands[4].channels == Channels::midSide,
               "TO ALL copies THRESH, RATIO, ATTACK and RELEASE to every band and leaves MAKEUP and the channel mode alone");
        for (auto* param : proc.getParameters()) // the rest of the check starts from a fresh plugin's bands
            if (auto* ranged = dynamic_cast<juce::RangedAudioParameter*> (param))
                if (ranged->paramID.startsWith ("b") && ranged->paramID.contains ("_") && ranged->paramID != bandIds (0).channels) // band 1 keeps the M/S the next check reads
                    setValue (proc.apvts, ranged->paramID, ranged->convertFrom0to1 (ranged->getDefaultValue()));

        check (read < -13.0f && read > -17.0f && std::fabs (learned - (read - 7.0f)) < 0.5f,
               fmt ("... and after two seconds of a 100 Hz tone at -12 dB (%.1f dB in the band) its threshold is half the jump under that: %.1f dB", read, learned));

        editor.strips[2]->solo.triggerClick();
        editor.strips[2]->delta.triggerClick();
        juce::MessageManager::getInstance()->runDispatchLoopUntil (100);
    }
    check (monitor.solo.load() == 0u && monitor.delta.load() == 0u, "closing the editor ends solo and delta");

    // The latency reaches the host from the message thread.
    proc.apvts.getParameter (id::phase)->setValueNotifyingHost (1.0f);
    proc.apvts.getParameter (id::oversampling)->setValueNotifyingHost (1.0f);
    juce::AudioBuffer<float> buffer (2, 512);
    juce::MidiBuffer midi;
    buffer.clear();
    proc.processBlock (buffer, midi);
    juce::MessageManager::getInstance()->runDispatchLoopUntil (100);
    check (proc.getLatencySamples() == proc.engine.latencySamples.load() && proc.getLatencySamples() > LinearSplit::latencyFor (48000.0),
           fmt ("Linear with 4x oversampling: the host is told %d samples", proc.getLatencySamples()));

    // Bypass: a bypassed plugin that reports latency delays by it (the host compensates for it either way).
    {
        AtlasProcessor q;
        setValue (q.apvts, id::phase, 1.0f);
        setValue (q.apvts, id::oversampling, 2.0f);
        setValue (q.apvts, id::lookahead, 4.0f);
        q.setPlayConfigDetails (2, 2, 48000.0, 512);
        q.prepareToPlay (48000.0, 512);
        const int latency = q.getLatencySamples();
        juce::AudioBuffer<float> in (2, 512);
        juce::MidiBuffer none;
        int found = -1;
        for (int block = 0; block < 40 && found < 0; ++block)
        {
            in.clear();
            if (block == 0)
                in.setSample (0, 5, 1.0f);
            q.processBlockBypassed (in, none);
            for (int i = 0; i < 512 && found < 0; ++i)
                if (in.getSample (0, i) > 0.5f)
                    found = block * 512 + i;
        }
        check (latency > 2000 && found == 5 + latency, fmt ("bypassed with Linear, 4x oversampling and 10 ms look-ahead: an impulse comes out %d samples late, the reported latency is %d", found - 5, latency));
    }

    // State round trip.
    juce::MemoryBlock state;
    proc.getStateInformation (state);
    AtlasProcessor other;
    other.setStateInformation (state.getData(), (int) state.getSize());
    const auto a = readParams (proc.apvts), b = readParams (other.apvts);
    check (b.phase == PhaseMode::linear && b.oversampling == a.oversampling && b.bands[0].channels == Channels::midSide, "a saved state comes back in another instance");

    std::printf ("\nAdvice: the notes that keep a compressor from being used for the wrong job\n");
    {
        using ui::Severity;
        const auto has = [] (const std::vector<ui::Note>& n, const char* word, Severity sev) { return ui::hasNote (n, word, sev); };

        BandAdvice band;
        band.number = 2; band.loHz = 500.0; band.hiHz = 2000.0;
        band.params.on = true; band.params.thresholdDb = -20.0; band.params.ratio = 3.0; band.params.attackMs = 10.0;
        AdviceInput fine;
        fine.bands = { band };
        check (adviceFor (fine).empty(), "a band with a threshold, a ratio of 3 and a 10 ms attack: nothing to say");

        auto in = fine; in.delta = true;
        check (has (adviceFor (in), "DELTA", Severity::warning), "DELTA on: a warning");
        in = fine; in.solo = true;
        check (has (adviceFor (in), "SOLO", Severity::warning), "a solo on: a warning");
        in = fine; in.bands[0].params.on = false;
        check (has (adviceFor (in), "Every band is off", Severity::warning), "every band off: the plugin passes the sound");
        in = fine; in.bands[0].params.thresholdDb = 0.0;
        check (has (adviceFor (in), "THRESH is at the top", Severity::warning), "a threshold at the top: it hardly compresses");
        in = fine; in.bands[0].params.ratio = 1.0;
        check (has (adviceFor (in), "1:1", Severity::warning), "a ratio of 1:1 does nothing");
        in = fine; in.bands[0].params.ratio = 20.0;
        check (has (adviceFor (in), "limiting", Severity::warning), "a ratio of 20:1: limiting without a true-peak reading");
        in = fine; in.bands[0].params.sidechain = true;
        check (has (adviceFor (in), "SIDECHAIN, but none is connected", Severity::warning), "a band set to the sidechain with none connected: a warning");
        in.sidechainConnected = true;
        check (adviceFor (in).empty(), "... and nothing once one is");
        in = fine; in.bands[0].params.upward = true; in.bands[0].params.thresholdDb = -24.0;
        check (has (adviceFor (in), "UPWARD lifts what is quiet", Severity::info) && adviceFor (in).size() == 1, "an upward band: it is said what it does, and nothing else");
        in.bands[0].params.thresholdDb = 0.0;
        check (has (adviceFor (in), "lifts the whole band", Severity::warning) && ! has (adviceFor (in), "hardly compresses", Severity::warning), "an upward band with THRESH at the top lifts everything");
        in = fine; in.bands[0].params.upward = true; in.bands[0].params.match = true; in.bands[0].reductionDb = -10.0;
        check (has (adviceFor (in), "MATCH undoes the lift", Severity::warning) && has (adviceFor (in), "lifting 10 dB", Severity::warning), "upward with MATCH on, and 10 dB of lift: both warned");
        in = fine; in.bands[0].params.upward = true; in.bands[0].params.ratio = 20.0;
        check (! has (adviceFor (in), "limiting", Severity::warning), "a ratio of 20:1 upward is not limiting");
        in = fine; in.lookaheadMs = 2.0; in.bands[0].params.attackMs = 10.0;
        check (has (adviceFor (in), "longer than the look-ahead", Severity::info) && has (adviceFor (in), "2 ms", Severity::info), "a 10 ms attack with 2 ms of look-ahead: the gain is still falling; the delay is named");
        in.bands[0].params.attackMs = 2.0;
        check (! has (adviceFor (in), "longer than the look-ahead", Severity::info), "... but not with an attack of 2 ms");
        in = fine; in.bands[0].reductionDb = 15.0;
        check (has (adviceFor (in), "pump", Severity::warning), "15 dB of reduction: it will pump");
        in = fine; in.bands[0].hiHz = 120.0; in.bands[0].params.attackMs = 1.0;
        check (has (adviceFor (in), "fast ATTACK on bass", Severity::info), "a 1 ms attack on bass: it distorts");
        in.bands[0].hiHz = 2000.0;
        check (! has (adviceFor (in), "fast ATTACK on bass", Severity::info), "... but not on the midrange");
        in = fine; in.bands[0].params.channels = Channels::dual;
        check (has (adviceFor (in), "image can drift", Severity::info), "L/R: the image can drift");
        in = fine; in.phase = PhaseMode::linear; in.latencyMs = 64.0;
        check (has (adviceFor (in), "64 ms", Severity::info), "Linear phase: the delay is named");

        size_t longest = 0;
        for (int mask = 0; mask < 2048; ++mask)
        {
            AdviceInput all;
            BandAdvice b = band;
            b.number = 6; b.hiHz = (mask & 1) ? 120.0 : 2000.0;
            b.params.on = mask & 2; b.params.thresholdDb = (mask & 4) ? 0.0 : -20.0; b.params.ratio = (mask & 8) ? 1.0 : ((mask & 16) ? 20.0 : 3.0);
            b.params.attackMs = (mask & 32) ? 1.0 : 10.0; b.params.channels = (mask & 64) ? Channels::dual : Channels::stereo; b.params.match = mask & 128;
            b.reductionDb = (mask & 256) ? 99.0 : ((mask & 512) ? -99.0 : 0.0);
            b.params.upward = mask & 128 ? false : (mask & 4) != 0;
            all.bands = { b };
            all.delta = mask & 1024; all.solo = mask & 1024; all.phase = PhaseMode::linear; all.latencyMs = 999.0;
            for (auto& n : adviceFor (all))
                longest = std::max (longest, n.text.size());
        }
        check ((int) longest <= ui::kMaxNoteChars, fmt ("the longest note is %d characters (a line holds %d)", (int) longest, ui::kMaxNoteChars));

        AtlasEditor editor (proc);
        check (has (editor.notes(), "THRESH is at the top", Severity::warning), "the editor reads the real bands: a fresh plugin has thresholds at the top");
        proc.engine.monitor.delta = 1u;
        check (has (editor.notes(), "DELTA", Severity::warning), "DELTA in the engine reaches the editor's notes");
        proc.engine.monitor.delta = 0u;
        check (editor.guide().isVisible() && std::strlen (guideText()) > 400 && std::string (guideText()).find ("NOT FOR") != std::string::npos,
               "the GUIDE button is there and its text says what the plugin is not for");
    }

    std::printf ("\nLook-ahead, sidechain and presets, through the processor\n");
    {
        AtlasProcessor q;
        q.setPlayConfigDetails (2, 2, 48000.0, 512);
        q.prepareToPlay (48000.0, 512);
        juce::AudioBuffer<float> buffer (2, 512);
        juce::MidiBuffer midi;
        const auto settle = [&] { buffer.clear(); q.processBlock (buffer, midi); juce::MessageManager::getInstance()->runDispatchLoopUntil (100); };

        setValue (q.apvts, id::lookahead, 3.0f); // 5 ms
        settle();
        check (q.getLatencySamples() == lookaheadSamples (48000.0, 3), fmt ("look-ahead 5 ms: the host is told %d samples", q.getLatencySamples()));
        setValue (q.apvts, id::oversampling, 1.0f);
        settle();
        check (q.getLatencySamples() > lookaheadSamples (48000.0, 3), fmt ("with 2x oversampling too: %d samples", q.getLatencySamples()));

        // Presets: each applies, keeps the session's own controls, and puts the bands where it says.
        bool all = true;
        for (int i = 0; i < q.getNumPrograms(); ++i)
        {
            q.setCurrentProgram (i);
            all = all && q.getProgramName (i).isNotEmpty() && q.getCurrentProgram() == i && std::strlen (presets()[(size_t) i].purpose) > 20 && std::strlen (presets()[(size_t) i].purpose) <= 140;
        }
        check (all && q.getNumPrograms() == (int) presets().size(), fmt ("all %d presets apply, have a name and a one-line purpose", q.getNumPrograms()));
        int duck = -1;
        for (int i = 0; i < q.getNumPrograms(); ++i)
            if (q.getProgramName (i) == "Kick Ducks Bass") duck = i;
        q.setCurrentProgram (duck);
        const auto d = readParams (q.apvts);
        check (duck > 0 && d.bands[0].sidechain && d.bands[1].sidechain && ! d.bands[2].sidechain && ! d.bands[2].on && d.lookahead == 3 && d.oversampling == 1,
               "Kick Ducks Bass puts the sub and bass on the sidechain, the rest off, and leaves the look-ahead and oversampling alone");
        q.setCurrentProgram (0);
        check (! readParams (q.apvts).bands[0].sidechain && readParams (q.apvts).bands[2].on, "Init puts every band back to its own signal, on");
        q.setCurrentProgram (duck);
        juce::MemoryBlock state;
        q.getStateInformation (state);
        AtlasProcessor other;
        other.setStateInformation (state.getData(), (int) state.getSize());
        check (other.getCurrentProgram() == duck && readParams (other.apvts).bands[0].sidechain, "the chosen preset and its settings are saved with the session");

        // A sidechain bus: the layout is accepted, and a block with a loud key takes down a band that is listening to it.
        auto layout = q.getBusesLayout();
        layout.inputBuses.getReference (1) = juce::AudioChannelSet::stereo();
        check (q.isBusesLayoutSupported (layout) && q.setBusesLayout (layout), "a stereo sidechain input is accepted");
        q.setCurrentProgram (duck);
        setValue (q.apvts, id::lookahead, 0.0f);
        setValue (q.apvts, id::oversampling, 0.0f);
        q.prepareToPlay (48000.0, 512);
        juce::AudioBuffer<float> four (4, 512);
        const auto measure = [&] (float key)
        {
            q.reset();
            float peak = 0.0f;
            for (int block = 0; block < 100; ++block)
            {
                for (int i = 0; i < 512; ++i)
                {
                    const float t = (float) (block * 512 + i) / 48000.0f;
                    const float bass = 0.05f * std::sin (6.2831853f * 100.0f * t), kick = key * std::sin (6.2831853f * 80.0f * t);
                    four.setSample (0, i, bass); four.setSample (1, i, bass); four.setSample (2, i, kick); four.setSample (3, i, kick);
                }
                q.processBlock (four, midi);
                if (block >= 90)
                    for (int i = 0; i < 512; ++i) peak = std::max (peak, std::fabs (four.getSample (0, i)));
            }
            return peak;
        };
        const float quiet = measure (0.0f), keyed = measure (0.5f);
        check (q.sidechainConnected.load() && keyed < 0.6f * quiet, fmt ("a 100 Hz bass at the output: %.4f with a silent key, %.4f with a loud kick on the sidechain (the bass ducks)", quiet, keyed));
    }

    return test::finish ("UI CHECK");
}
