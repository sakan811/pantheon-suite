// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 The Pantheon authors

// Builds the editor on the real processor and checks what a host and a user would meet: the parameters, the name, the strips, the node on the display,
// the latency the host is told, the state, and the notes.

#include "Advice.h"
#include "Check.h"
#include "PluginEditor.h"
#include <pantheon/ui/Scaled.h>
#include <random>

using namespace apollo;
using test::check;
using test::fmt;

int main()
{
    juce::ScopedJuceInitialiser_GUI gui;
    std::printf ("Apollo UI check\n");
    ApolloProcessor proc;
    proc.setPlayConfigDetails (2, 2, 48000.0, 512);
    proc.prepareToPlay (48000.0, 512);

    check (proc.getName() == juce::String (gui::kProjectWord) + " " + gui::kPluginWord, "the reported name is the nameplate's two words");
    check (proc.getParameters().size() == 3 + 7 * kBands, fmt ("%d parameters: phase, output, gain match and 7 for each of %d bands", proc.getParameters().size(), kBands));
    check (typeNames.size() == kNumTypes && targetNames.size() == kNumTargets && slopeNames.size() == kNumSlopes && phaseNames.size() == kNumPhases, "the choice lists match the engine's");
    const auto d = readParams (proc.apvts);
    bool defaultsOk = d.phase == Phase::minimum && std::fabs (d.outDb) < 1e-4 && ! d.match;
    for (int i = 0; i < kBands; ++i)
    {
        const auto want = defaultBand (i);
        const auto& got = d.bands[(size_t) i];
        defaultsOk = defaultsOk && got.on == want.on && got.type == want.type && std::fabs (got.freqHz - want.freqHz) < want.freqHz * 1e-3 && std::fabs (got.gainDb) < 1e-4;
    }
    check (defaultsOk, "a new instance has the bands of defaultBand(), all at 0 dB, minimum phase, output 0, GAIN MATCH off");
    check (std::none_of (d.bands.begin(), d.bands.end(), [] (const BandParams& b) { return ! isIdentity (b); }), "... and none of them changes the sound");

    // The latency the host is told follows the phase mode.
    check (proc.getLatencySamples() == 0, "minimum phase: the host is told 0 samples");
    {
        setValue (proc.apvts, id::phase, 1.0f);
        juce::AudioBuffer<float> buffer (2, 512);
        buffer.clear();
        juce::MidiBuffer midi;
        proc.processBlock (buffer, midi);
        juce::MessageManager::getInstance()->runDispatchLoopUntil (100);
        check (proc.getLatencySamples() == linearSizesFor (48000.0).latency(), fmt ("linear phase: the host is told %d samples", proc.getLatencySamples()));
        // Bypassed by the host: the sound is still delayed by the reported latency, or the track would jump early against the rest of the mix.
        {
            const int latency = proc.getLatencySamples();
            int at = -1, seen = 0;
            for (int block = 0; block < 8; ++block)
            {
                buffer.clear();
                if (block == 0)
                    buffer.setSample (0, 0, 1.0f), buffer.setSample (1, 0, 0.5f);
                proc.processBlockBypassed (buffer, midi);
                for (int i = 0; i < buffer.getNumSamples(); ++i)
                    if (buffer.getSample (0, i) != 0.0f)
                        at = block * 512 + i, ++seen;
            }
            check (latency > 0 && at == latency && seen == 1, fmt ("bypassed in linear phase, an impulse comes out %d samples late (the reported latency, %d)", at, latency));
        }
        setValue (proc.apvts, id::phase, 0.0f);
    }

    {
        std::unique_ptr<juce::AudioProcessorEditor> holder (proc.createEditor());
        auto& editor = ui::innerOf<ApolloEditor> (*holder);
        check (ui::hasEditorAspect (editor.getWidth(), editor.getHeight()), "the editor is drawn at 16:10, the shape of every plugin");
        editor.setVisible (true);
        check (editor.getWidth() > 900 && editor.strips[7]->bounds.getRight() <= editor.getWidth() && editor.strips[0]->type.getNumItems() == kNumTypes, "eight strips fit the window and have the parameters' items");
        check (editor.strips[3]->gain.isVisible() && editor.strips[3]->q.isVisible() && ! editor.strips[3]->slope.isVisible(), "a bell shows GAIN and Q");
        check (! editor.strips[0]->gain.isVisible() && ! editor.strips[0]->q.isVisible() && editor.strips[0]->slope.isVisible(), "a high-pass shows its slope, and no GAIN or Q");

        // A node on the display: dragging it sets the frequency and the gain.
        auto& view = editor.view;
        const auto node = view.nodeOf (3); // band 4: a bell at 1 kHz
        check (view.nodeNear (node) == 3 && std::fabs (view.hzAt (node.x) - 1000.0) < 10.0, "band 4's node is on the display at 1 kHz");
        view.dragTo (3, { view.xOf (2000.0), view.yOf (6.0) });
        const auto b = readParams (proc.apvts).bands[3];
        check (std::fabs (b.freqHz - 2000.0) < 20.0 && std::fabs (b.gainDb - 6.0) < 0.1, fmt ("dragging the node to 2 kHz, +6 dB sets the band (%.0f Hz, %+.1f dB)", b.freqHz, b.gainDb));
        view.dragTo (0, { view.xOf (60.0), view.yOf (9.0) });
        check (std::fabs (readParams (proc.apvts).bands[0].freqHz - 60.0) < 1.0 && std::fabs (readParams (proc.apvts).bands[0].gainDb) < 1e-3f, "dragging a high-pass's node moves its frequency only (it has no gain)");
        view.mouseDoubleClick (juce::MouseEvent (juce::Desktop::getInstance().getMainMouseSource(), view.nodeOf (3), juce::ModifierKeys(), 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, &view, &view,
                                                  juce::Time::getCurrentTime(), view.nodeOf (3), juce::Time::getCurrentTime(), 1, false));
        check (! readParams (proc.apvts).bands[3].on, "a double-click on a node switches its band off");
        setValue (proc.apvts, bandIds (3).on, 1.0f);
        setValue (proc.apvts, bandIds (3).type, (float) (int) Type::highPass);
        editor.showTypes();
        check (editor.strips[3]->slope.isVisible() && ! editor.strips[3]->gain.isVisible(), "a strip changes its controls with its type");

        // Double-click on empty glass adds a band in the first free slot, at that frequency and gain.
        const juce::Point<float> empty { view.xOf (5000.0), view.yOf (-6.0) };
        view.mouseDoubleClick (juce::MouseEvent (juce::Desktop::getInstance().getMainMouseSource(), empty, juce::ModifierKeys(), 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, &view, &view,
                                                  juce::Time::getCurrentTime(), empty, juce::Time::getCurrentTime(), 1, false));
        const auto added = readParams (proc.apvts).bands[2];
        check (added.on && added.type == Type::bell && std::fabs (added.freqHz - 5000.0) < 50.0 && std::fabs (added.gainDb + 6.0) < 0.2 && view.selected == 2,
               fmt ("a double-click on empty glass adds a bell in the first free band (%.0f Hz, %+.1f dB) and selects it", added.freqHz, added.gainDb));
        for (int i = 0; i < kBands; ++i)
            if (isIdentity (readParams (proc.apvts).bands[(size_t) i]))
                setValue (proc.apvts, bandIds (i).on, 1.0f), setValue (proc.apvts, bandIds (i).gain, 4.0f);
        check (view.addBandAt (empty) == -1, "with every band in use a double-click adds nothing");

        // LISTEN: monitoring, not a parameter; it follows the selected band and ends with the window.
        view.mouseDown (juce::MouseEvent (juce::Desktop::getInstance().getMainMouseSource(), view.listenButton().getCentre(), juce::ModifierKeys(), 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, &view, &view,
                                          juce::Time::getCurrentTime(), view.listenButton().getCentre(), juce::Time::getCurrentTime(), 1, false));
        check (proc.engine.listen.load() == 2 && ui::hasNote (editor.notes(), "LISTEN is on", ui::Severity::warning), "the LISTEN button plays the selected band alone, and a warning says so");
        view.select (5);
        check (proc.engine.listen.load() == 5 && proc.getParameters().size() == 3 + 7 * kBands, "LISTEN follows the selected band and is not a parameter");
    }

    check (proc.engine.listen.load() == -1, "closing the editor ends LISTEN");

    // The state survives a round trip.
    {
        setValue (proc.apvts, bandIds (2).gain, -7.5f);
        setValue (proc.apvts, bandIds (2).target, 2.0f);
        setValue (proc.apvts, id::out, 3.0f);
        juce::MemoryBlock saved;
        proc.getStateInformation (saved);
        ApolloProcessor other;
        other.setStateInformation (saved.getData(), (int) saved.getSize());
        const auto o = readParams (other.apvts);
        check (std::fabs (o.bands[2].gainDb + 7.5f) < 0.01 && o.bands[2].target == Target::side && std::fabs (o.outDb - 3.0) < 0.01, "the state of a session comes back as it was saved");
    }

    // The audio path through the real processor: a boost comes out.
    {
        ApolloProcessor q;
        q.setPlayConfigDetails (2, 2, 48000.0, 512);
        q.prepareToPlay (48000.0, 512);
        setValue (q.apvts, bandIds (3).gain, 12.0f);
        juce::AudioBuffer<float> buffer (2, 512);
        juce::MidiBuffer midi;
        float peak = 0.0f;
        for (int block = 0; block < 100; ++block)
        {
            for (int i = 0; i < 512; ++i)
                for (int c = 0; c < 2; ++c)
                    buffer.setSample (c, i, 0.1f * std::sin (2.0f * 3.14159265f * 1000.0f * (float) (block * 512 + i) / 48000.0f));
            q.processBlock (buffer, midi);
            if (block > 50)
                peak = std::max (peak, buffer.getMagnitude (0, 512));
        }
        check (std::fabs (20.0f * std::log10 (peak / 0.1f) - 12.0f) < 0.1f, fmt ("a 12 dB bell at 1 kHz lifts a tone through the processor by %.2f dB", 20.0f * std::log10 (peak / 0.1f)));

        // The analyser behind the curve reads the sound before and after the bands: the bell's 12 dB shows between the two traces.
        gui::CurveView display (q.apvts, [&q] { return q.getSampleRate(); }, q.tapIn, q.tapOut);
        display.setSize (800, 262);
        check (display.analyzerOn(), "the analyser is on in a new instance");
        display.tick();
        const float before = display.inTrace.levelAtFrequency (1000.0), after = display.outTrace.levelAtFrequency (1000.0);
        check (std::fabs (before + 20.0f) < 2.5f, fmt ("the analyser reads the tone before the bands at %.1f dB (a 0.1 tone is -20 dBFS)", before));
        check (std::fabs ((after - before) - 12.0f) < 1.0f, fmt ("... and after the 12 dB bell %.1f dB higher", after - before));
        display.mouseDown (juce::MouseEvent (juce::Desktop::getInstance().getMainMouseSource(), display.analyzerButton().getCentre(), juce::ModifierKeys(), 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, &display, &display,
                                             juce::Time::getCurrentTime(), display.analyzerButton().getCentre(), juce::Time::getCurrentTime(), 1, false));
        check (! display.analyzerOn() && ! (bool) q.apvts.state.getProperty ("analyzer", true), "the ANALYZER switch turns it off, and that is kept in the session's state");
        q.setPlayConfigDetails (2, 2, 48000.0, 512);
        for (int i = 0; i < 512; ++i)
            for (int c = 0; c < 2; ++c)
                buffer.setSample (c, i, 0.0f);
        for (int block = 0; block < 40; ++block)
            q.processBlock (buffer, midi);
        display.tick();
        check (std::fabs (display.outTrace.levelAtFrequency (1000.0) - after) < 0.01f, "off, the traces are not read or moved");
    }

    // The notes.
    {
        AdviceInput in;
        check (adviceFor (in).size() == 1 && ui::hasNote (adviceFor (in), "No band changes anything", ui::Severity::info), "a default instance says that nothing is changed yet");
        in.params.bands[3] = { true, Type::bell, 1000.0, 10.0, 1.0, 0, Target::stereo };
        check (ui::hasNote (adviceFor (in), "Band 4: a boost of 10 dB", ui::Severity::warning), "a 10 dB boost is flagged");
        in.params.bands[4] = { true, Type::bell, 1500.0, 4.0, 8.0, 0, Target::stereo };
        check (ui::hasNote (adviceFor (in), "add up to 14 dB", ui::Severity::warning) && ui::hasNote (adviceFor (in), "narrow bell", ui::Severity::info), "boosts within an octave add up, and a narrow bell is said to ring");
        in.params.phase = Phase::linear;
        in.params.bands[1] = { true, Type::lowShelf, 40.0, 3.0, 0.7, 0, Target::stereo };
        in.latencyMs = 48.0;
        check (ui::hasNote (adviceFor (in), "linear phase resolves", ui::Severity::info) && ui::hasNote (adviceFor (in), "delays the sound by 48 ms", ui::Severity::info), "linear phase: a low band is called soft, and the delay is said");
        bool tooLong = false;
        for (const auto& n : adviceFor (in))
            if ((int) n.text.size() > ui::kMaxNoteChars)
            {
                tooLong = true;
                std::printf ("  too long (%d): %s\n", (int) n.text.size(), n.text.c_str());
            }
        in.params.bands[0] = { true, Type::highPass, 30.0, 0.0, 0.7, 3, Target::stereo };
        for (const auto& n : adviceFor (in))
            if ((int) n.text.size() > ui::kMaxNoteChars)
            {
                tooLong = true;
                std::printf ("  too long (%d): %s\n", (int) n.text.size(), n.text.c_str());
            }
        check (! tooLong, "every note fits a line of the strip");
    }

    // Presets: each applies, has a name and a one-line purpose, the chosen one is kept with the session, and the box is in the header.
    {
        ApolloProcessor q;
        q.setPlayConfigDetails (2, 2, 48000.0, 512);
        q.prepareToPlay (48000.0, 512);
        bool all = q.getNumPrograms() == (int) presets().size() && q.getNumPrograms() >= 6;
        for (int i = 0; i < q.getNumPrograms(); ++i)
        {
            q.setCurrentProgram (i);
            const auto len = std::strlen (presets()[(size_t) i].purpose);
            all = all && q.getProgramName (i).isNotEmpty() && q.getCurrentProgram() == i && len > 20 && len <= 140;
        }
        check (all, fmt ("all %d presets apply, have a name and a one-line purpose", q.getNumPrograms()));
        const auto indexOf = [&] (const char* name)
        {
            for (int i = 0; i < q.getNumPrograms(); ++i)
                if (q.getProgramName (i) == name)
                    return i;
            check (false, std::string ("a preset called ") + name);
            return 0;
        };
        const auto raw = [&] (const juce::String& pid) { return q.apvts.getRawParameterValue (pid)->load(); };
        setValue (q.apvts, id::phase, 1.0f);
        setValue (q.apvts, id::out, 3.0f);
        setValue (q.apvts, id::match, 1.0f);
        q.setCurrentProgram (indexOf ("Telephone"));
        check (raw (bandIds (0).on) > 0.5f && std::fabs (raw (bandIds (0).freq) - 500.0f) < 1.0f && (int) raw (bandIds (7).type) == (int) Type::lowPass && raw (bandIds (7).on) > 0.5f, "Telephone: a high-pass at 500 Hz and a low-pass, both on");
        q.setCurrentProgram (indexOf ("Mud Cut"));
        check (std::fabs (raw (bandIds (2).gain) + 3.0f) < 0.01f && raw (bandIds (0).on) < 0.5f && raw (bandIds (7).on) < 0.5f, "Mud Cut: a 3 dB dip at 300 Hz, and the Telephone's passes are off again (a preset does not inherit the last one)");
        check ((int) raw (id::phase) == 1 && std::fabs (raw (id::out) - 3.0f) < 0.01f, "PHASE and OUTPUT are kept by a preset");
        check (raw (id::match) > 0.5f, "GAIN MATCH is kept by a preset");
        q.setCurrentProgram (indexOf ("Sides Open Top"));
        check ((int) raw (bandIds (0).target) == (int) Target::side && (int) raw (bandIds (6).target) == (int) Target::side, "Sides Open Top works on the sides");
        q.setCurrentProgram (0);
        bool silent = true;
        for (int i = 0; i < kBands; ++i)
            silent = silent && isIdentity (readParams (q.apvts).bands[(size_t) i]);
        check (silent, "Init: no band changes the sound");
        const int chosen = indexOf ("Vocal Presence");
        q.setCurrentProgram (chosen);
        juce::MemoryBlock saved;
        q.getStateInformation (saved);
        ApolloProcessor other;
        other.setStateInformation (saved.getData(), (int) saved.getSize());
        check (other.getCurrentProgram() == chosen, "the chosen preset is saved with the session");
        std::unique_ptr<juce::AudioProcessorEditor> holder (q.createEditor());
        auto& editor = ui::innerOf<ApolloEditor> (*holder);
        editor.setVisible (true);
        check (editor.presetBox.getNumItems() == q.getNumPrograms() && editor.presetBox.getSelectedId() == chosen + 1, "the editor's preset box lists the presets and shows the current one");
        check (editor.presetBox.getRight() < editor.getWidth() && ! editor.presetBox.getBounds().intersects (editor.guideButton.getBounds()), "the preset box fits the header beside the GUIDE button");
    }

    // GAIN MATCH and DELTA: the buttons, and what the notes say about them.
    {
        ApolloProcessor q;
        q.setPlayConfigDetails (2, 2, 48000.0, 512);
        q.prepareToPlay (48000.0, 512);
        {
            std::unique_ptr<juce::AudioProcessorEditor> holder (q.createEditor());
            auto& editor = ui::innerOf<ApolloEditor> (*holder);
            editor.setVisible (true);
            check (editor.matchButton.getRight() < editor.deltaButton.getX() && editor.deltaButton.getRight() < editor.pasteButton.getX() && editor.pasteButton.getRight() < editor.guideButton.getX()
                       && editor.presetBox.getRight() < editor.matchButton.getX() && editor.guideButton.getRight() <= editor.outKnob.getX(),
                   "GAIN MATCH, DELTA and PASTE sit in the header between the preset box and GUIDE, overlapping nothing");
            editor.matchButton.triggerClick();
            juce::MessageManager::getInstance()->runDispatchLoopUntil (100);
            check (readParams (q.apvts).match, "the GAIN MATCH button sets the parameter");
            editor.matchButton.triggerClick();
            juce::MessageManager::getInstance()->runDispatchLoopUntil (100);
            editor.deltaButton.triggerClick();
            juce::MessageManager::getInstance()->runDispatchLoopUntil (100);
            check (q.engine.delta.load() && ui::hasNote (editor.notes(), "DELTA is on", ui::Severity::warning), "DELTA turns the engine's monitor on, and a warning says you do not hear your track");
            check (! (bool) q.apvts.state.getProperty ("delta", false) && q.getParameters().size() == 3 + 7 * kBands, "DELTA is not a parameter and not in the state: it is never saved or automated");
        }
        check (! q.engine.delta.load(), "closing the editor ends DELTA: monitoring never outlives its window");

        AdviceInput in;
        in.params.bands[3] = { true, Type::bell, 1000.0, 10.0, 1.0, 0, Target::stereo };
        check (ui::hasNote (adviceFor (in), "GAIN MATCH to judge it fairly", ui::Severity::info), "a boost of 9 dB or more with GAIN MATCH off says to turn it on");
        in.params.match = true;
        check (! ui::hasNote (adviceFor (in), "to judge it fairly", ui::Severity::info) && ui::hasNote (adviceFor (in), "GAIN MATCH is on", ui::Severity::info), "... and with it on, says what it does instead");
        in.delta = true;
        in.listen = 7;
        check (ui::hasNote (adviceFor (in), "LISTEN is on", ui::Severity::warning), "LISTEN on says you do not hear your track");
        in.params.phase = Phase::linear;
        in.latencyMs = 48.0;
        check (ui::hasNote (adviceFor (in), "Linear phase delays the sound by 48 ms", ui::Severity::info), "linear phase says how late the sound is");
        in.params.bands[0] = { true, Type::bell, 30.0, 3.0, 1.0, 0, Target::stereo };
        in.params.bands[1] = { true, Type::highPass, 30.0, 0.0, 0.7071, 2, Target::stereo };
        check (ui::hasNote (adviceFor (in), "linear phase resolves about 12 Hz", ui::Severity::info), "a bell under 60 Hz in linear phase says the kernel resolves 12 Hz");
        check (ui::hasNote (adviceFor (in), "a steep high-pass this low is softer in linear phase", ui::Severity::info), "a steep high-pass under 60 Hz in linear phase says it is softer");
        in.params.phase = Phase::minimum;
        check (! ui::hasNote (adviceFor (in), "resolves about 12 Hz", ui::Severity::info) && ! ui::hasNote (adviceFor (in), "Linear phase delays", ui::Severity::info), "... and in minimum phase says neither");
        in.params.phase = Phase::linear;
        bool fits = true;
        for (const auto& n : adviceFor (in))
        {
            fits = fits && (int) n.text.size() <= ui::kMaxNoteChars;
            if ((int) n.text.size() > ui::kMaxNoteChars)
                std::printf ("  too long (%d): %s\n", (int) n.text.size(), n.text.c_str());
        }
        check (fits, "the new notes fit a line of the strip");
    }

    // PASTE: what Themis copies (bands of a plan item, and Copy EQ's bands and target curve) goes into free bands.
    {
        ApolloProcessor q;
        q.setPlayConfigDetails (2, 2, 48000.0, 512);
        q.prepareToPlay (48000.0, 512);
        using K = pdsp::LinkBand::Kind;
        using P = pdsp::LinkBand::Place;
        const auto band = [] (K kind, double hz, double gain, double qv, P place = P::stereo)
        {
            pdsp::LinkBand b;
            b.kind = kind;
            b.freq = hz;
            b.gainDb = gain;
            b.q = qv;
            b.place = place;
            return b;
        };
        auto cut = band (K::lowCut, 150.0, 0.0, 0.7, P::side);
        cut.slopeDbPerOct = 24.0;
        auto fill = band (K::fill, 0.0, 0.0, 0.7);
        pdsp::LinkCurve curve;
        curve.f0Hz = 20.0;
        curve.perOctave = 6.0;
        curve.name = "Modern Pop";
        curve.db.assign (60, 0.0);
        const auto text = pdsp::link::encode ({ band (K::lowShelf, 90.0, 3.0, 0.7), band (K::bell, 270.0, -2.5, 1.5), band (K::bell, 3200.0, 2.0, 0.9, P::mid), band (K::highShelf, 9000.0, 1.5, 0.7),
                                               cut, fill, band (K::bell, 500.0, 2.0, 1.0, P::left) },
                                             &curve);
        const auto result = pasteBands (q.apvts, juce::String (text));
        const auto p = readParams (q.apvts);
        check (result.slots.size() == 5 && result.skipped == 2 && result.curve && result.curveName == "Modern Pop", fmt ("PASTE sets %d bands, leaves out a fill band and a left band (%d), and reports the curve", (int) result.slots.size(), result.skipped));
        check (p.bands[1].type == Type::lowShelf && std::fabs (p.bands[1].freqHz - 90.0) < 1.0 && std::fabs (p.bands[1].gainDb - 3.0) < 0.01, "a low shelf goes in band 2, where the default low shelf is");
        check (p.bands[6].type == Type::highShelf && std::fabs (p.bands[6].gainDb - 1.5) < 0.01, "a high shelf goes in band 7");
        check (p.bands[2].type == Type::bell && std::fabs (p.bands[2].gainDb + 2.5) < 0.01 && std::fabs (p.bands[2].q - 1.5) < 0.01 && p.bands[3].target == Target::mid && std::fabs (p.bands[3].gainDb - 2.0) < 0.01,
               "bells go in the bell bands in turn, with their Q, and a Mid band stays Mid");
        check (p.bands[0].on && p.bands[0].type == Type::highPass && p.bands[0].target == Target::side && p.bands[0].slope == 1 && std::fabs (p.bands[0].freqHz - 150.0) < 1.0,
               "Themis' low cut is a 24 dB/oct high-pass in band 1, on the Side");
        check (! p.bands[7].on && std::none_of (p.bands.begin(), p.bands.end(), [] (const BandParams& b) { return b.on && std::fabs (b.freqHz - 500.0) < 1.0; }), "the left band and the fill band went nowhere");

        // amount=0.5 takes half of a pasted band's gain (-4 dB comes in at -2 dB), into the next free band.
        auto halfBand = band (K::bell, 1000.0, -4.0, 1.0);
        halfBand.amount = 0.5;
        const auto half = pasteBands (q.apvts, juce::String (pdsp::link::encode ({ halfBand })));
        check (half.slots.size() == 1 && std::fabs (readParams (q.apvts).bands[(size_t) half.slots[0]].gainDb + 2.0) < 0.01,
               "amount=0.5 halves a pasted gain: -4 dB comes in at -2 dB");

        // Full: a paste with no free band says so.
        for (int i = 0; i < kBands; ++i)
        {
            setValue (q.apvts, bandIds (i).on, 1.0f);
            setValue (q.apvts, bandIds (i).type, (float) (int) Type::bell);
            setValue (q.apvts, bandIds (i).gain, 4.0f);
        }
        const auto full = pasteBands (q.apvts, juce::String (pdsp::link::encode ({ band (K::bell, 1000.0, -3.0, 1.0) })));
        check (full.slots.empty() && full.skipped == 1, "with every band in use, a paste changes none and counts it as left out");
        const auto nothing = pasteBands (q.apvts, "not bands at all");
        check (nothing.slots.empty() && nothing.skipped == 0 && ! nothing.curve && apollo::pasteSaid (nothing).find ("Nothing to paste") == 0, "text that is not Themis' says there is nothing to paste");

        // The editor: the line in the strip, the selected band, the reference on the display, kept in the session, cleared by the menu's action.
        ApolloProcessor r;
        r.setPlayConfigDetails (2, 2, 48000.0, 512);
        r.prepareToPlay (48000.0, 512);
        std::unique_ptr<juce::AudioProcessorEditor> holder (r.createEditor());
        auto& editor = ui::innerOf<ApolloEditor> (*holder);
        editor.setVisible (true);
        editor.pasteFromClipboard (juce::String (text));
        check (ui::hasNote (editor.shownNotes(), "Pasted 5 bands", ui::Severity::info) && ui::hasNote (editor.shownNotes(), "2 left out", ui::Severity::info), "PASTE says what it did in the strip under the bands");
        check (editor.view.hasReference() && editor.view.referenceLabel() == "Modern Pop" && editor.view.selected == 1, "the curve is the display's reference, and the first pasted band is selected");
        juce::MemoryBlock saved;
        r.getStateInformation (saved);
        ApolloProcessor other;
        other.setStateInformation (saved.getData(), (int) saved.getSize());
        gui::CurveView restored (other.apvts, [] { return 48000.0; }, other.tapIn, other.tapOut);
        check (restored.hasReference() && restored.referenceLabel() == "Modern Pop", "the reference is kept with the session");

        // Drawn at the level of this track: white noise through the processor, and the flat curve lands on the analyser's own trace at 1 kHz.
        std::mt19937 gen (3);
        std::uniform_real_distribution<float> white (-0.2f, 0.2f);
        juce::AudioBuffer<float> buffer (2, 512);
        juce::MidiBuffer midi;
        check (! editor.view.referenceAnchored(), "before there is sound the reference waits for it");
        for (int block = 0; block < 200; ++block)
        {
            for (int c = 0; c < 2; ++c)
                for (int i = 0; i < 512; ++i)
                    buffer.setSample (c, i, white (gen));
            r.processBlock (buffer, midi);
            if (block % 4 == 3)
                editor.view.tick();
        }
        const float traceAt1k = editor.view.inTrace.levelAtFrequency (1000.0), referenceAt1k = editor.view.referenceDisplayDb (1000.0);
        check (editor.view.referenceAnchored() && std::fabs (referenceAt1k - traceAt1k) < 4.0f, fmt ("with sound, a flat reference is drawn at this track's level: %.1f dB against the trace's %.1f dB at 1 kHz", referenceAt1k, traceAt1k));
        editor.view.clearReference();
        check (! editor.view.hasReference() && ! r.apvts.state.hasProperty ("referenceCurve"), "clearing the reference removes it from the display and from the session");
    }

    // Clearing and removing bands: CLEAR ALL is one undo step, Delete and Backspace switch the selected band off, and a preset is one undo step.
    std::printf ("\nClearing and removing bands\n");
    {
        ApolloProcessor q;
        q.setPlayConfigDetails (2, 2, 48000.0, 512);
        q.prepareToPlay (48000.0, 512);
        std::unique_ptr<juce::AudioProcessorEditor> holder (q.createEditor());
        auto& editor = ui::innerOf<ApolloEditor> (*holder);
        editor.setVisible (true);
        auto& um = *q.apvts.undoManager;
        const auto onCount = [&]
        {
            int n = 0;
            for (const auto& b : readParams (q.apvts).bands)
                n += b.on ? 1 : 0;
            return n;
        };
        const auto settle = [] { juce::MessageManager::getInstance()->runDispatchLoopUntil (300); }; // the undo history takes a parameter change when the message loop runs
        const auto snapshot = [&]
        {
            std::vector<float> v;
            for (const auto& b : readParams (q.apvts).bands)
            {
                v.push_back (b.on ? 1.0f : 0.0f);
                v.push_back ((float) b.freqHz);
                v.push_back ((float) b.gainDb);
            }
            return v;
        };

        // CLEAR ALL's enabled state is set by the editor's timer: all bands off disables it, and the bands back on enable it again.
        const auto defaults = readParams (q.apvts);
        for (int i = 0; i < kBands; ++i)
            setValue (q.apvts, bandIds (i).on, 0.0f);
        settle();
        check (onCount() == 0 && ! editor.clearAllButton.isEnabled(), "CLEAR ALL is disabled while every band is off (the timer sets it)");
        for (int i = 0; i < kBands; ++i)
            setValue (q.apvts, bandIds (i).on, defaults.bands[(size_t) i].on ? 1.0f : 0.0f);
        settle();
        check (onCount() == 6 && editor.clearAllButton.isEnabled(), "CLEAR ALL is enabled while bands are on (the default has six)");
        settle(); // pending state changes reach the history first, so they are not undone
        um.clearUndoHistory();
        editor.view.setListen (true);
        editor.clearAllButton.triggerClick();
        settle();
        check (onCount() == 0 && um.getUndoDescription() == "Clear bands", "CLEAR ALL switches every band off as one undo step named Clear bands");
        check (q.engine.listen.load() == -1, "CLEAR ALL ends LISTEN");
        settle();
        check (! editor.clearAllButton.isEnabled(), "CLEAR ALL is disabled with no band on");
        um.undo();
        settle();
        check (onCount() == 6 && editor.clearAllButton.isEnabled(), "one UNDO brings every band back, and CLEAR ALL is enabled again");
        check (std::fabs (readParams (q.apvts).bands[1].freqHz - 100.0) < 0.5, "... with the settings each band had");

        // Nothing on: no step is opened, and nothing changes.
        for (int i = 0; i < kBands; ++i)
            setValue (q.apvts, bandIds (i).on, 0.0f);
        settle(); // pending state changes reach the history first, so they are not undone
        um.clearUndoHistory();
        check (clearBands (q.apvts) == 0 && ! um.canUndo(), "clearing with no band on changes nothing and opens no undo step");

        // Delete switches the selected band off and selects the next one that is on; the settings stay.
        setValue (q.apvts, bandIds (1).on, 1.0f);
        setValue (q.apvts, bandIds (4).on, 1.0f);
        settle(); // pending state changes reach the history first, so they are not undone
        um.clearUndoHistory();
        editor.view.select (1);
        check (! editor.view.keyPressed (juce::KeyPress ('x', juce::ModifierKeys(), 'x')) && onCount() == 2, "a key other than Delete or Backspace is not taken");
        const bool deleted = editor.view.keyPressed (juce::KeyPress (juce::KeyPress::deleteKey));
        settle(); // the state records a change when the message loop runs (as with CLEAR ALL)
        check (deleted && onCount() == 1 && ! readParams (q.apvts).bands[1].on && editor.view.selected == 4,
               "Delete switches the selected band off and selects the next band that is on");
        check (std::fabs (readParams (q.apvts).bands[1].freqHz - 100.0) < 0.5 && um.getUndoDescription() == "Remove band", "the band keeps its settings, and the step is named Remove band");
        const bool backspaced = editor.view.keyPressed (juce::KeyPress (juce::KeyPress::backspaceKey));
        settle();
        check (backspaced && onCount() == 0 && editor.view.selected == 4, "Backspace switches the last band on off too, and the selection stays put");
        const bool nothingSelected = editor.view.keyPressed (juce::KeyPress (juce::KeyPress::deleteKey));
        settle();
        check (! nothingSelected && onCount() == 0, "Delete on a band that is off does nothing");
        um.undo();
        settle();
        check (onCount() == 1 && readParams (q.apvts).bands[4].on, "UNDO brings back the band that Backspace removed");
        um.undo();
        settle();
        check (onCount() == 2, "and a second UNDO brings back the one Delete removed");

        // A preset is one undo step, and UNDO takes it back.
        settle(); // pending state changes reach the history first, so they are not undone
        um.clearUndoHistory();
        settle();
        const auto beforePreset = snapshot();
        q.setCurrentProgram (1); // Low Cut
        settle();
        check (um.getUndoDescription() == "Preset" && snapshot() != beforePreset, "a preset is one undo step named Preset");
        um.undo();
        settle();
        const auto after = snapshot(); // the values come back through the state as text, so compare to a tolerance
        bool same = after.size() == beforePreset.size();
        for (size_t k = 0; same && k < after.size(); ++k)
            same = std::fabs (after[k] - beforePreset[k]) < 1e-3f * std::max (1.0f, std::fabs (beforePreset[k]));
        check (same, "UNDO takes a preset back as one step");
    }

    // A paste of several bands is one undo step; the UNDO / REDO buttons and Cmd+Z take it back and put it on again. The display's tooltip names Delete.
    std::printf ("\nUndo of a paste, and the UNDO / REDO buttons\n");
    {
        ApolloProcessor q;
        q.setPlayConfigDetails (2, 2, 48000.0, 512);
        q.prepareToPlay (48000.0, 512);
        std::unique_ptr<juce::AudioProcessorEditor> holder (q.createEditor());
        auto& editor = ui::innerOf<ApolloEditor> (*holder);
        editor.setVisible (true);
        auto& um = *q.apvts.undoManager;
        const auto settle = [] { juce::MessageManager::getInstance()->runDispatchLoopUntil (300); };
        const auto onCount = [&]
        {
            int n = 0;
            for (const auto& b : readParams (q.apvts).bands)
                n += b.on ? 1 : 0;
            return n;
        };
        const auto cmd = juce::ModifierKeys (juce::ModifierKeys::commandModifier);

        // Clear the bands so there are free ones for the paste, then start from an empty history.
        settle();
        clearBands (q.apvts);
        settle();
        um.clearUndoHistory();
        settle();
        check (! editor.undoButtons.undoSwitch().isEnabled() && ! editor.undoButtons.redoSwitch().isEnabled(), "with no change in the history, UNDO and REDO are both off");

        // Three bands pasted are one step named Paste bands, and one UNDO takes all three back.
        using K = pdsp::LinkBand::Kind;
        const auto bandOf = [] (K kind, double hz, double gain, double qv)
        {
            pdsp::LinkBand b;
            b.kind = kind;
            b.freq = hz;
            b.gainDb = gain;
            b.q = qv;
            b.place = pdsp::LinkBand::Place::stereo;
            return b;
        };
        editor.pasteFromClipboard (juce::String (pdsp::link::encode ({ bandOf (K::lowShelf, 90.0, 3.0, 0.7), bandOf (K::bell, 270.0, -2.5, 1.5), bandOf (K::highShelf, 9000.0, 1.5, 0.7) })));
        settle();
        check (onCount() == 3 && um.getUndoDescription() == "Paste bands", "a paste of three bands sets three bands as one undo step named Paste bands");
        check (editor.undoButtons.undoSwitch().isEnabled() && ! editor.undoButtons.redoSwitch().isEnabled(), "after the paste UNDO is on and REDO is off");
        editor.undoButtons.undoSwitch().triggerClick();
        settle();
        check (onCount() == 0, "UNDO takes back all three pasted bands in one step");
        check (! editor.undoButtons.undoSwitch().isEnabled() && editor.undoButtons.redoSwitch().isEnabled(), "after that UNDO is off and REDO is on");
        editor.undoButtons.redoSwitch().triggerClick();
        settle();
        check (onCount() == 3, "REDO puts the three bands back");
        check (editor.undoButtons.handleKey (juce::KeyPress ('z', cmd, 'z')) && onCount() == 0, "Cmd+Z takes the paste back");
        check (editor.undoButtons.handleKey (juce::KeyPress ('z', cmd.withFlags (juce::ModifierKeys::shiftModifier), 'Z')) && onCount() == 3, "Shift+Cmd+Z puts it back");
        check (! editor.undoButtons.handleKey (juce::KeyPress ('q', cmd, 'q')), "another key is not taken");

        check (editor.view.getTooltip().contains ("Delete"), "the display's tooltip says that Delete or Backspace switches the selected band off");
    }

    // A / B: the header-row buttons, each slot keeps its settings, the kept parameters (OUTPUT, PHASE, GAIN MATCH) stay across a switch, and the active slot is saved.
    std::printf ("\nA / B\n");
    {
        ApolloProcessor q;
        q.setPlayConfigDetails (2, 2, 48000.0, 512);
        q.prepareToPlay (48000.0, 512);
        std::unique_ptr<juce::AudioProcessorEditor> holder (q.createEditor());
        auto& editor = ui::innerOf<ApolloEditor> (*holder);
        editor.setVisible (true);
        const auto pumpUntil = [] (auto&& done) {
            for (int waited = 0; ! done() && waited < 2000; waited += 10)
                juce::MessageManager::getInstance()->runDispatchLoopUntil (10);
            return done();
        };
        const auto inTree = [&q] (const juce::String& pid, float plain) {
            for (int i = 0; i < q.apvts.state.getNumChildren(); ++i)
            {
                const auto child = q.apvts.state.getChild (i);
                if (child.getProperty ("id").toString() == pid)
                    return std::fabs ((float) child.getProperty ("value") - plain) < 1e-3f;
            }
            return false;
        };
        const auto gainOf = [&q] (int band) { return readParams (q.apvts).bands[(size_t) band].gainDb; };

        const auto abR = editor.abButtons.getBounds(), undoR = editor.undoButtons.getBounds(), clearR = editor.clearAllButton.getBounds();
        check (editor.abButtons.isVisible() && editor.abButtons.button (1).isVisible(), "A and B are on screen in the row above the display");
        check (editor.getLocalBounds().contains (abR) && ! abR.intersects (undoR) && ! abR.intersects (clearR) && abR.getX() > clearR.getRight(),
               "A / B sits right of CLEAR ALL, inside the editor and overlapping nothing");
        check (! abR.intersects (editor.view.getBounds()) && abR.getBottom() <= editor.view.getY(), "A / B is above the display, not on it");

        // Each slot keeps its settings: B starts as a copy of A, then switching restores both.
        q.undo.clearUndoHistory();
        setValue (q.apvts, bandIds (2).gain, -7.5f);
        setValue (q.apvts, id::out, 3.0f);
        pumpUntil ([&] { return inTree (bandIds (2).gain, -7.5f) && inTree (id::out, 3.0f); });
        editor.abButtons.button (1).onClick(); // B starts as a copy of A
        check (q.ab.active() == 1 && editor.abButtons.button (1).getToggleState() && ! editor.abButtons.button (0).getToggleState(), "B is chosen and lit, A is not");
        setValue (q.apvts, bandIds (2).gain, 4.0f);
        setValue (q.apvts, id::out, -2.0f); // OUTPUT is kept by A / B: this stays through the switches below
        pumpUntil ([&] { return inTree (bandIds (2).gain, 4.0f) && inTree (id::out, -2.0f); });
        editor.abButtons.button (0).onClick();
        pumpUntil ([&] { return inTree (bandIds (2).gain, -7.5f); });
        check (std::fabs (gainOf (2) + 7.5f) < 0.05f, "switching to A brings back A's band gain (-7.5 dB)");
        check (std::fabs (readParams (q.apvts).outDb + 2.0f) < 0.05f, "OUTPUT is kept across the switch (-2 dB, what it was set to in B)");
        editor.abButtons.button (1).onClick();
        pumpUntil ([&] { return inTree (bandIds (2).gain, 4.0f); });
        check (std::fabs (gainOf (2) - 4.0f) < 0.05f, "switching back to B brings back B's band gain (+4 dB)");
        editor.undoButtons.undo();
        pumpUntil ([&] { return inTree (bandIds (2).gain, -7.5f); });
        check (std::fabs (gainOf (2) + 7.5f) < 0.05f, "UNDO takes back the A / B switch as one step (A again)");

        // A session keeps the active slot; one from before A / B opens on slot A.
        editor.abButtons.button (1).onClick();
        juce::MemoryBlock saved;
        q.getStateInformation (saved);
        ApolloProcessor onB;
        onB.setStateInformation (saved.getData(), (int) saved.getSize());
        check (onB.ab.active() == 1 && ! onB.ab.hasSettings (0), "a session saved on B opens on B, with only the active slot kept");

        if (auto xml = juce::AudioProcessor::getXmlFromBinary (saved.getData(), (int) saved.getSize()))
        {
            check (xml->getIntAttribute ("abSlot", -1) == 1, "the B session carries abSlot 1 before it is taken out");
            xml->removeAttribute ("abSlot");
            juce::MemoryBlock legacy;
            juce::AudioProcessor::copyXmlToBinary (*xml, legacy);
            ApolloProcessor before;
            before.setStateInformation (legacy.getData(), (int) legacy.getSize());
            check (before.ab.active() == 0, "a session from before A / B (no abSlot) opens on slot A");
        }
        else
            check (false, "the saved session is readable as XML");
    }

    return test::finish ("ApolloUiCheck");
}
