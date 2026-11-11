// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 The Pantheon authors

// Drives the real spectrum component with synthetic mouse events and checks what lands in the
// parameters: edge and body drags, "narrowest band wins", the minimum width, fill-band limits,
// slot allocation. Also loads a session saved before bands existed, and follows latency reporting
// through the processor. Headless; no window is shown.
//
//   HephaestusUiCheck

#include "Check.h"
#include "PluginEditor.h"
#include "Advice.h"
#include "PluginProcessor.h"
#include "UI/SpectrumView.h"
#include <pantheon/ui/Controls.h>

using namespace hep;
using namespace test;

namespace
{
constexpr int viewWidth = 1008, viewHeight = 224;

// The view's plot is its bounds inset by 6 px on every side (see SpectrumView::plotArea).
float xOf (double hz)
{
    return 6.0f + gui::FreqAxis::toNormalised ((float) hz) * (float) (viewWidth - 12);
}

float value (HephaestusProcessor& p, const juce::String& id) { return p.apvts.getRawParameterValue (id)->load(); }

juce::MouseEvent mouse (juce::Component& c, float x, float downX, bool dragged)
{
    return { juce::Desktop::getInstance().getMainMouseSource(), { x, 100.0f }, juce::ModifierKeys(),
             juce::MouseInputSource::defaultPressure, juce::MouseInputSource::defaultOrientation,
             juce::MouseInputSource::defaultRotation, juce::MouseInputSource::defaultTiltX,
             juce::MouseInputSource::defaultTiltY, &c, &c, juce::Time::getCurrentTime(),
             { downX, 100.0f }, juce::Time::getCurrentTime(), 1, dragged };
}

// A full press-drag-release gesture from one x position to another.
void dragFrom (juce::Component& view, float fromX, float toX)
{
    view.mouseDown (mouse (view, fromX, fromX, false));
    for (int step = 1; step <= 8; ++step)
        view.mouseDrag (mouse (view, fromX + (toX - fromX) * (float) step / 8.0f, fromX, true));
    view.mouseUp (mouse (view, toX, fromX, true));
}
}

int main()
{
    juce::ScopedJuceInitialiser_GUI gui;
    HephaestusProcessor proc;
    proc.setPlayConfigDetails (2, 2, 48000.0, 512);
    proc.prepareToPlay (48000.0, 512);

    gui::SpectrumView view (proc.engine, proc.apvts, proc.currentSampleRate);
    view.setBounds (0, 0, viewWidth, viewHeight);

    check (proc.getName() == juce::String (gui::kProjectWord) + " " + gui::kPluginWord, "the reported name is the nameplate's two words");

    std::printf ("\nBand slots\n");
    check (isBandOn (proc.apvts, 0) && ! isBandOn (proc.apvts, 1), "a fresh plugin has one band (a gentle full-range saturator)");

    const int slot = addBand (proc.apvts, BandMode::saturate, 1000.0);
    check (slot == 1 && std::fabs (value (proc, bandIds (1).lo) - 1000.0f / 1.8f) < 1.0f
               && std::fabs (value (proc, bandIds (1).hi) - 1000.0f * 1.8f) < 1.0f,
           fmt ("a band added at 1 kHz takes the first free slot (%d), spanning %.0f - %.0f Hz", slot,
                value (proc, bandIds (1).lo), value (proc, bandIds (1).hi)));

    std::printf ("\nMouse\n");
    view.setSelectedBand (0);
    view.mouseDown (mouse (view, xOf (1000.0), xOf (1000.0), false));
    view.mouseUp (mouse (view, xOf (1000.0), xOf (1000.0), false));
    check (view.getSelectedBand() == 1, "clicking where bands overlap selects the narrowest one");

    dragFrom (view, xOf (value (proc, bandIds (1).lo)), xOf (400.0));
    check (std::fabs (value (proc, bandIds (1).lo) / 400.0f - 1.0f) < 0.03f && std::fabs (value (proc, bandIds (1).hi) - 1800.0f) < 2.0f,
           fmt ("dragging the low edge to 400 Hz: low %.0f Hz, high untouched at %.0f Hz", value (proc, bandIds (1).lo), value (proc, bandIds (1).hi)));

    const float ratioBefore = value (proc, bandIds (1).hi) / value (proc, bandIds (1).lo);
    const float centreX = (xOf (value (proc, bandIds (1).lo)) + xOf (value (proc, bandIds (1).hi))) * 0.5f;
    dragFrom (view, centreX, centreX + 120.0f);
    const float ratioAfter = value (proc, bandIds (1).hi) / value (proc, bandIds (1).lo);
    check (value (proc, bandIds (1).lo) > 800.0f && std::fabs (ratioAfter / ratioBefore - 1.0f) < 0.01f,
           fmt ("dragging the body right: %.0f - %.0f Hz, width ratio %.3f (was %.3f)", value (proc, bandIds (1).lo),
                value (proc, bandIds (1).hi), ratioAfter, ratioBefore));

    dragFrom (view, xOf (value (proc, bandIds (1).hi)), xOf (100.0));
    check (std::fabs (value (proc, bandIds (1).hi) / value (proc, bandIds (1).lo) - (float) minRangeRatio) < 0.01f,
           fmt ("dragging the high edge past the low one stops at the minimum width (ratio %.2f)",
                value (proc, bandIds (1).hi) / value (proc, bandIds (1).lo)));

    std::printf ("\nModes and limits\n");
    setParameterValue (proc.apvts, bandIds (1).lo, 3000.0f);
    setParameterValue (proc.apvts, bandIds (1).hi, 10000.0f);
    setBandMode (proc.apvts, 1, BandMode::fill);
    check (bandMode (proc.apvts, 1) == BandMode::fill && value (proc, bandIds (1).hi) <= (float) fillMaxHz + 0.5f
               && value (proc, bandIds (1).hi) >= value (proc, bandIds (1).lo) * (float) minRangeRatio - 0.5f,
           fmt ("switching a 3 - 10 kHz band to fill pulls it into range: %.0f - %.0f Hz (limit %.0f)",
                value (proc, bandIds (1).lo), value (proc, bandIds (1).hi), fillMaxHz));

    dragFrom (view, xOf (value (proc, bandIds (1).hi)), xOf (9000.0));
    check (value (proc, bandIds (1).hi) <= (float) fillMaxHz + 0.5f,
           fmt ("a fill band's edge can't be dragged above %.0f Hz (now %.0f)", fillMaxHz, value (proc, bandIds (1).hi)));

    std::printf ("\nSlots\n");
    for (int i = 0; i < maxBands; ++i)
        addBand (proc.apvts, BandMode::fill, 80.0);
    check (addBand (proc.apvts, BandMode::saturate, 1000.0) == -1, "no free slot left: adding returns -1");

    removeBand (proc.apvts, 3);
    check (addBand (proc.apvts, BandMode::saturate, 1000.0, 3) == 3, "a freed slot is reused, and is the preferred one when asked for");

    std::printf ("\nLatency follows oversampling (processor level)\n");
    {
        HephaestusProcessor p2;
        p2.setPlayConfigDetails (2, 2, 48000.0, 512);
        p2.prepareToPlay (48000.0, 512);
        juce::AudioBuffer<float> buffer (2, 512);
        juce::MidiBuffer midi;
        std::string line;
        bool ok = true;
        int previous = -1;
        for (int mode = 0; mode < 4; ++mode)
        {
            setParameterValue (p2.apvts, id::oversampling, (float) mode);
            for (int i = 0; i < 3; ++i)
            {
                buffer.clear();
                p2.processBlock (buffer, midi);
                juce::MessageManager::getInstance()->runDispatchLoopUntil (50);
            }
            line += fmt (" %d:%d", mode, p2.getLatencySamples());
            ok = ok && (mode == 0) == (p2.getLatencySamples() == 0) && p2.getLatencySamples() >= previous;
            previous = p2.getLatencySamples();
        }
        check (ok, "reported latency by oversampling mode (0 = off):" + line);

        // An Octave band adds its generator's latency, and only while a band uses it.
        setParameterValue (p2.apvts, id::oversampling, 0.0f);
        const auto settle = [&]
        {
            for (int i = 0; i < 3; ++i)
            {
                buffer.clear();
                p2.processBlock (buffer, midi);
                juce::MessageManager::getInstance()->runDispatchLoopUntil (50);
            }
            return p2.getLatencySamples();
        };
        const int plain = settle();
        setBandMode (p2.apvts, 0, BandMode::fill);
        setParameterValue (p2.apvts, bandIds (0).character, 2.0f);
        const int withOctave = settle();
        setParameterValue (p2.apvts, bandIds (0).character, 0.0f);
        const int after = settle();
        check (plain == 0 && withOctave == hep::OctaveGenerator::latencyFor (48000.0) && after == 0,
               fmt ("reported latency with oversampling off: %d, with an Octave band %d (a frame less one sample, %d), after it is Warm again %d",
                    plain, withOctave, hep::OctaveGenerator::latencyFor (48000.0), after));

        // A fresh plugin told at prepare time that a band is Octave reports it before the first block.
        HephaestusProcessor p3;
        for (int i = 0; i < (int) presets().size(); ++i)
            if (juce::String (presets()[(size_t) i].name) == "Octave Air")
                applyPreset (p3.apvts, i);
        p3.setPlayConfigDetails (2, 2, 48000.0, 512);
        p3.prepareToPlay (48000.0, 512);
        check (p3.engine.getLatency() >= hep::OctaveGenerator::latencyFor (48000.0), fmt ("prepared with an Octave band: the engine already reports %d", p3.engine.getLatency()));
    }

    std::printf ("\nLegacy sessions (3 fixed saturator bands + one bass engine)\n");
    {
        HephaestusProcessor p3;
        p3.setPlayConfigDetails (2, 2, 48000.0, 512);
        p3.prepareToPlay (48000.0, 512);

        juce::XmlElement legacy ("STATE");
        for (auto [pid, v] : { std::pair<const char*, double> { "driveLow", 6.0 }, { "driveMid", 2.0 }, { "driveHigh", 9.0 },
                               { "xoverLow", 200.0 }, { "xoverHigh", 3000.0 }, { "fundLo", 45.0 }, { "fundHi", 100.0 },
                               { "extAmount", -6.0 }, { "satOn", 1.0 } })
        {
            auto* node = legacy.createNewChildElement ("PARAM");
            node->setAttribute ("id", pid);
            node->setAttribute ("value", v);
        }
        juce::MemoryBlock block;
        juce::AudioProcessor::copyXmlToBinary (legacy, block);
        p3.setStateInformation (block.getData(), (int) block.getSize());

        const auto at = [&] (int band, const juce::String BandIds::* field) { return value (p3, bandIds (band).*field); };
        check (std::fabs (at (0, &BandIds::hi) - 200.0f) < 0.5f && std::fabs (at (1, &BandIds::hi) - 3000.0f) < 0.5f
                   && std::fabs (at (2, &BandIds::lo) - 3000.0f) < 0.5f && std::fabs (at (2, &BandIds::drive) - 9.0f) < 0.05f,
               fmt ("three saturate bands: 20-%.0f, %.0f-%.0f, %.0f-%.0f Hz at %.0f / %.0f / %.0f dB",
                    at (0, &BandIds::hi), at (1, &BandIds::lo), at (1, &BandIds::hi), at (2, &BandIds::lo), at (2, &BandIds::hi),
                    at (0, &BandIds::drive), at (1, &BandIds::drive), at (2, &BandIds::drive)));
        check (isBandOn (p3.apvts, 3) && bandMode (p3.apvts, 3) == BandMode::fill && std::fabs (at (3, &BandIds::lo) - 45.0f) < 0.5f
                   && std::fabs (at (3, &BandIds::hi) - 100.0f) < 0.5f && at (3, &BandIds::adapt) == 0.0f
                   && ! isBandOn (p3.apvts, 4),
               fmt ("the bass engine becomes fill band 4: %.0f-%.0f Hz, amount %.1f dB, adapt %.0f %% (the old engine was open-loop)",
                    at (3, &BandIds::lo), at (3, &BandIds::hi), at (3, &BandIds::amount), at (3, &BandIds::adapt)));
    }

    std::printf ("\nTyping a value into a knob\n");
    {
        ui::Knob knob (proc.apvts, id::outGain, "Output", "tip", ui::Knob::Layout::below);
        knob.setBounds (0, 0, 120, 140);
        const auto entry = [&] () -> juce::TextEditor*
        {
            for (auto* child : knob.getChildren())
                if (auto* te = dynamic_cast<juce::TextEditor*> (child))
                    return te;
            return nullptr;
        };
        const auto typeIn = [&] (const juce::String& text)
        {
            knob.mouseDown (mouse (knob, 60.0f, 60.0f, false).withNewPosition (juce::Point<float> (60.0f, 130.0f))); // on the read-out
            auto* te = entry();
            if (te == nullptr || ! te->isVisible())
                return false;
            te->setText (text, false);
            te->onReturnKey();
            return true;
        };

        const auto expectTyped = [&] (const juce::String& text, float expectedDb, const char* what)
        {
            const bool opened = typeIn (text);
            const float now = value (proc, id::outGain);
            check (opened && std::fabs (now - expectedDb) < 0.01f, fmt ("%s (%.2f dB)", what, (double) now));
        };
        expectTyped ("-7.5", -7.5f, "typing -7.5 sets the output gain");
        expectTyped ("3 dB", 3.0f, "a value with its unit is accepted");
        expectTyped ("99", 12.0f, "a value out of range is clamped");
        typeIn ("abc");
        check (std::fabs (value (proc, id::outGain) - 12.0f) < 0.01f, "text that is not a number leaves the value alone");
        check (entry() != nullptr && ! entry()->isVisible(), "the entry closes after Enter");

        knob.mouseDown (mouse (knob, 60.0f, 60.0f, false).withNewPosition (juce::Point<float> (60.0f, 130.0f)));
        entry()->setText ("-1", false);
        entry()->onEscapeKey();
        check (std::fabs (value (proc, id::outGain) - 12.0f) < 0.01f && ! entry()->isVisible(), "Esc cancels the entry");

        // A word the parameter itself shows ("Off") is a value too; the knob is re-bound, as the band panel does.
        const auto rebuild = bandIds (0).rebuild;
        knob.bind (proc.apvts, rebuild);
        typeIn ("0");
        const float before = value (proc, rebuild);
        typeIn ("off");
        check (std::fabs (before) < 0.01f && value (proc, rebuild) <= offDb + 0.05f,
               fmt ("typing \"off\" turns Rebuild off (%.1f dB, then %.1f dB)", (double) before, (double) value (proc, rebuild)));
        typeIn ("loud");
        check (value (proc, rebuild) <= offDb + 0.05f, "a word the parameter does not show is ignored");

        // A detent switch takes a position's name, or as much of it as tells it apart.
        knob.bind (proc.apvts, id::curve); // Tanh, Atan, Soft
        typeIn ("soft");
        check (std::lround (value (proc, id::curve)) == 2, "typing \"soft\" picks Soft");
        typeIn ("a");
        check (std::lround (value (proc, id::curve)) == 1, "a unique start (\"a\") picks Atan");
        typeIn ("sine");
        check (std::lround (value (proc, id::curve)) == 1, "a word that names no position (\"sine\") is ignored");
        knob.bind (proc.apvts, id::stereo); // Stereo (L/R), Mid/Side
        typeIn ("mid / side");
        check (std::lround (value (proc, id::stereo)) == 1, "spaces and case do not matter (\"mid / side\")");
        typeIn ("s");
        check (std::lround (value (proc, id::stereo)) == 0, "\"s\" picks Stereo (L/R)");
    }

    std::printf ("\nPaste bands from another plugin\n");
    {
        HephaestusProcessor fresh;
        for (int i = 0; i < maxBands; ++i)
            removeBand (fresh.apvts, i);
        int skipped = 0;
        const auto slots = pasteBands (fresh.apvts, "pantheon-bands 1\nfill lo=40 hi=90 place=mid\nbell freq=270 gain=-2.0 q=0.70 place=mid", skipped);
        check (slots.size() == 1 && skipped == 1, fmt ("the fill band is added, the equaliser band left for Iris (%d added, %d skipped)", (int) slots.size(), skipped));
        if (slots.size() == 1)
            check (bandMode (fresh.apvts, slots[0]) == BandMode::fill
                       && std::fabs (fresh.apvts.getRawParameterValue (bandIds (slots[0]).lo)->load() - 40.0f) < 0.5f
                       && std::fabs (fresh.apvts.getRawParameterValue (bandIds (slots[0]).hi)->load() - 90.0f) < 0.5f,
                   "... over the range it was given");
    }

    std::printf ("\nBand solo\n");
    {
        HephaestusProcessor host;
        host.setPlayConfigDetails (2, 2, 48000.0, 512);
        host.prepareToPlay (48000.0, 512);
        const auto pressed = [] (juce::Component& parent, const char* text)
        {
            for (int i = 0; i < parent.getNumChildComponents(); ++i)
                if (auto* b = dynamic_cast<juce::TextButton*> (parent.getChildComponent (i)); b != nullptr && b->getButtonText() == text)
                {
                    b->onClick(); // triggerClick is posted, not run now
                    return true;
                }
            return false;
        };
        {
            HephaestusEditor editor (host);
            auto& m = host.engine.monitor;
            check (m.soloBand.load() == -1, "no solo to begin with");
            check (pressed (editor, "SOLO ADDS") && m.soloBand.load() == 0 && m.soloKind.load() == (int) SoloKind::adds,
                   "SOLO ADDS solos the selected band, in its ADDS way");
            check (pressed (editor, "SOLO BAND") && m.soloBand.load() == 0 && m.soloKind.load() == (int) SoloKind::band,
                   "SOLO BAND switches it to the band's region");
            check (pressed (editor, "SOLO BAND") && m.soloBand.load() == -1, "pressing the same button again releases it");
            pressed (editor, "SOLO ADDS");

            // The band solo strip: the six main bands, lined up with the display's axis.
            namespace mb = pdsp::bands;
            bool named = true, ordered = true;
            for (int k = 0; k < mb::kCount; ++k)
            {
                auto& rb = editor.regionButton (k);
                named = named && rb.getButtonText() == juce::String (mb::kBands[(size_t) k].name).toUpperCase() && rb.isVisible() && rb.getWidth() > 20;
                if (k > 0)
                    ordered = ordered && rb.getX() >= editor.regionButton (k - 1).getRight();
            }
            check (named && ordered, "the strip has a switch per main band (SUB ... HIGH), left to right, wide enough to click");
            check (pressed (editor, "BASS") && m.regions.load() == 0b10u, "BASS solos the 60 - 250 Hz band");
            check (pressed (editor, "UPPER-MID") && m.regions.load() == 0b10010u && editor.regionButton (4).getToggleState() && editor.regionButton (1).getToggleState(),
                   "another band adds to it, and both switches are lit");
            check (pressed (editor, "BASS") && m.regions.load() == 0b10000u, "pressing a lit band again takes it out");
            pressed (editor, "SUB");
        }
        check (host.engine.monitor.soloBand.load() == -1, "closing the editor ends the solo (it is not saved state)");
        check (host.engine.monitor.regions.load() == 0u, "... and the band solo strip");
    }

    std::printf ("\nCharacter of a fill band (Warm, Clean, Octave)\n");
    {
        HephaestusProcessor host;
        host.setPlayConfigDetails (2, 2, 48000.0, 512);
        host.prepareToPlay (48000.0, 512);

        check (value (host, bandIds (0).character) < 0.5f && value (host, bandIds (3).character) < 0.5f,
               "every band starts Warm, so a session saved before the choice existed sounds as it did");
        const auto character = [&] (int band) { return readParams (host.apvts).bands[(size_t) band].fill.character; };

        const auto presetNamed = [&] (const char* name)
        {
            for (int i = 0; i < (int) presets().size(); ++i)
                if (juce::String (presets()[(size_t) i].name) == name)
                    return i;
            return -1;
        };
        applyPreset (host.apvts, presetNamed ("Clean Sub Fill"));
        check (bandMode (host.apvts, 0) == BandMode::fill && character (0) == Character::clean, "the Clean Sub Fill preset makes band 1 a Clean fill band");
        applyPreset (host.apvts, presetNamed ("Octave Air"));
        check (bandMode (host.apvts, 0) == BandMode::fill && character (0) == Character::octave && usesOctave (readParams (host.apvts)),
               "the Octave Air preset makes band 1 an Octave fill band, which is what asks for the latency");
        applyPreset (host.apvts, 0);
        check (character (0) == Character::warm && ! usesOctave (readParams (host.apvts)), "a preset starts every control from its default, Warm included");

        // The selector in the band panel: only for a fill band, and it follows (and drives) the band's own parameter.
        HephaestusEditor editor (host);
        auto& box = editor.characterSelector();
        check (! box.isVisible(), "a saturate band shows no character selector");
        setBandMode (host.apvts, 0, BandMode::fill);
        editor.selectBand (0);
        check (box.isVisible() && box.getNumItems() == 3 && box.getSelectedItemIndex() == 0, "a fill band shows it with its three choices, on Warm");

        setParameterValue (host.apvts, bandIds (0).character, 2.0f);
        juce::MessageManager::getInstance()->runDispatchLoopUntil (50);
        check (box.getSelectedItemIndex() == 2 && character (0) == Character::octave, "setting the parameter moves the selector and reaches the engine's parameters");

        box.setSelectedItemIndex (1, juce::sendNotificationSync);
        juce::MessageManager::getInstance()->runDispatchLoopUntil (50);
        check (character (0) == Character::clean, "choosing in the selector sets the parameter");

        addBand (host.apvts, BandMode::fill, 80.0, 1);
        setParameterValue (host.apvts, bandIds (1).character, 2.0f);
        removeBand (host.apvts, 1);
        addBand (host.apvts, BandMode::fill, 80.0, 1);
        check (value (host, bandIds (1).character) < 0.5f, "a band added into a used slot starts Warm again");

        editor.selectBand (1);
        check (box.isVisible() && box.getSelectedItemIndex() == 0, "selecting another fill band re-binds the selector to that band");
    }

    std::printf ("\nAdvice: the notes that keep a tool from being used for the wrong job\n");
    {
        using ui::Severity;
        const auto has = [] (const std::vector<ui::Note>& notes, const char* word, Severity severity) { return ui::hasNote (notes, word, severity); };

        AdviceInput fine;
        fine.bandOn = true;
        fine.lo = 40.0; fine.hi = 120.0; fine.mode = BandMode::fill; fine.character = Character::clean;
        check (adviceFor (fine).empty(), "a clean fill band on one bass note: nothing to warn about");

        auto in = fine; in.delta = true;
        check (has (adviceFor (in), "DELTA", Severity::warning) && adviceFor (in).front().severity == Severity::warning, "DELTA on: a warning, first");
        in = fine; in.soloActive = true;
        check (has (adviceFor (in), "SOLO", Severity::warning), "a solo on: a warning");
        in = fine; in.amountOff = true;
        check (has (adviceFor (in), "AMOUNT is off", Severity::warning), "a fill band with AMOUNT off: it adds nothing");
        in = fine; in.character = Character::octave; in.lo = 40.0; in.hi = 120.0;
        check (has (adviceFor (in), "cannot tell bass partials", Severity::warning) && has (adviceFor (in), "43 ms", Severity::info), "Octave on bass: the resolution warning, and the latency");
        in.lo = 600.0; in.hi = 2000.0;
        check (! has (adviceFor (in), "cannot tell bass partials", Severity::warning) && has (adviceFor (in), "43 ms", Severity::info), "Octave on mids: no resolution warning, the latency still named");
        in = fine; in.hi = 900.0;
        check (has (adviceFor (in), "CLEAN follows one note", Severity::warning), "Clean over the mids: it follows one note");
        in = fine; in.character = Character::warm; in.lo = 600.0; in.hi = 2000.0;
        check (has (adviceFor (in), "WARM mixes", Severity::info), "Warm over the mids: chords turn dirty");
        in = fine; in.rebuildOn = true;
        check (has (adviceFor (in), "REBUILD is experimental", Severity::warning), "Rebuild on: experimental");
        in = fine; in.adaptPercent = 0.0;
        check (has (adviceFor (in), "ADAPT is 0", Severity::info), "Adapt at 0 %: adds regardless");
        in = fine; in.mode = BandMode::saturate; in.driveDb = 18.0; in.oversamplingMode = 0;
        check (has (adviceFor (in), "Heavy DRIVE", Severity::warning) && has (adviceFor (in), "OVERSAMPLING is off", Severity::warning), "a hard-driven saturate band with no oversampling: both warnings");
        in.driveDb = 3.0; in.oversamplingMode = 2;
        check (adviceFor (in).size() == 1 && has (adviceFor (in), "cannot change the level", Severity::info), "a gentle saturate band: only the note that it is not an equaliser");
        in = fine; in.bandOn = false;
        check (adviceFor (in).empty(), "a free slot: no band notes");

        // Every note must fit a line of the band panel, whatever the combination.
        size_t longest = 0;
        for (int mask = 0; mask < (1 << 10); ++mask)
            for (auto mode : { BandMode::saturate, BandMode::fill })
                for (auto character : { Character::warm, Character::clean, Character::octave })
                    for (auto range : { std::pair<double, double> { 40.0, 120.0 }, { 600.0, 2000.0 } })
                    {
                        AdviceInput all;
                        all.bandOn = true; all.mode = mode; all.character = character; all.lo = range.first; all.hi = range.second;
                        all.delta = mask & 1; all.soloActive = mask & 2; all.amountOff = mask & 4; all.rebuildOn = mask & 8; all.midSide = mask & 16;
                        all.gainMatch = mask & 32; all.oversamplingMode = (mask & 64) ? 0 : 2; all.driveDb = (mask & 128) ? 18.0 : 3.0;
                        all.adaptPercent = (mask & 256) ? 0.0 : 100.0; all.mixPercent = (mask & 512) ? 50.0 : 100.0;
                        for (auto& n : adviceFor (all))
                            longest = std::max (longest, n.text.size());
                    }
        check ((int) longest <= ui::kMaxNoteChars, fmt ("the longest note is %d characters (a line holds %d)", (int) longest, ui::kMaxNoteChars));

        // Every preset says what it is for, in one line.
        bool described = true;
        for (auto& preset : presets())
            described = described && preset.purpose != nullptr && std::strlen (preset.purpose) > 20 && std::strlen (preset.purpose) <= 130;
        check (described, "every preset has a purpose of one line");

        // The editor reads the real parameters: DELTA on in the plugin shows up in its notes, and the GUIDE button exists.
        HephaestusProcessor host;
        host.setPlayConfigDetails (2, 2, 48000.0, 512);
        host.prepareToPlay (48000.0, 512);
        HephaestusEditor editor (host);
        const auto shows = [&] (const char* word) { return has (editor.notes(), word, Severity::warning); };
        check (! shows ("DELTA"), "the editor shows no DELTA warning at first");
        setParameterValue (host.apvts, id::delta, 1.0f);
        check (shows ("DELTA"), "DELTA switched on in the plugin reaches the editor's notes");
        setParameterValue (host.apvts, id::delta, 0.0f);
        host.engine.monitor.regions = 1u;
        check (shows ("SOLO"), "a main-band solo reaches the editor's notes");
        host.engine.monitor.regions = 0u;
        check (editor.guide().isVisible() && std::strlen (guideText()) > 400 && std::string (guideText()).find ("NOT FOR") != std::string::npos,
               "the GUIDE button is there and its text says what the plugin is not for");
    }

    return finish ("UI CHECK");
}
