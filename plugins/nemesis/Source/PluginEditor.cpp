// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 The Pantheon authors

#include "PluginEditor.h"

#include <pantheon/ui/Aspect.h>
#include <pantheon/ui/FreqAxis.h>
#include <pantheon/ui/Undo.h>

using namespace nemesis;
namespace hw = ui::hw;

namespace
{
constexpr int headerHeight = 84;
constexpr int cellWidth = 112;
constexpr int cellHeight = 108;
constexpr int kTimerHz = 30;
constexpr int kWidth = 864, kHeight = ui::editorHeightFor (kWidth); // 16:10, the kit's shape
constexpr int displayHeight = kHeight - (headerHeight + 10 + 8 + cellHeight + 8 + ui::kNoteStripHeight + 12); // the display takes what the rest leaves
}

NemesisEditor::NemesisEditor (NemesisProcessor& p)
    : AudioProcessorEditor (&p),
      freq (p.apvts, id::freq, "FREQ", "The centre of the sibilance. Switch LISTEN on and move it until you hear the hiss of the s. Male voices sit at 4 to 7 kHz, female at 6 to 10.", ui::Knob::Layout::below),
      width (p.apvts, id::q, "WIDTH", "How wide the band is (Q: higher is narrower). Narrow takes only the hiss; wide takes more of the voice with it.", ui::Knob::Layout::below),
      threshold (p.apvts, id::threshold, "THRESH", "ABSOLUTE: the level of the band above which it is turned down (dBFS). RELATIVE: how far under the whole sound the band may be before it is turned down.", ui::Knob::Layout::below),
      range (p.apvts, id::range, "RANGE", "The most it takes off. Under 10 dB sounds natural; more makes a lisp.", ui::Knob::Layout::below),
      release (p.apvts, id::release, "RELEASE", "How fast the gain comes back after an s (63% of the way in this time). The attack is fixed at 0.3 ms.", ui::Knob::Layout::below),
      mode (p.apvts, id::mode, "SPLIT-BAND: only the band is turned down; the rest of the voice is not touched.\nWIDEBAND: the whole sound is turned down, as with a compressor."),
      detection (p.apvts, id::detection, "ABSOLUTE: THRESH is a level in dBFS: it has to be set for each recording.\nRELATIVE: the band is compared with the whole sound, so one setting works for quiet and loud passages."),
      lookahead (p.apvts, id::lookahead, "The sound is heard this long after the detector has seen it, so the first of a loud s is already turned down (the attack is 0.3 ms).\n"
                                         "It is a delay: the host compensates it. Changing it while playing restarts the de-esser (a short gap)."),
      presetBox (p, presets(), "Everything is set by a preset: what it does not list goes back to its default."),
      undoButtons (p.undo, *this),
      listen (p.apvts, id::listen, "LISTEN", "Hear only the band the detector hears, to find the s. Not saved as a sound: switch it off before you bounce."),
      delta (p.apvts, id::delta, "DELTA", "Hear only what is turned down (the sound minus the output): if it is the voice and not just the hiss of the s, THRESH is too low or RANGE too high. It takes the place of LISTEN while it is on. Switch it off before you bounce."),
      proc (p)
{
    setLookAndFeel (&look);
    for (auto* c : std::initializer_list<juce::Component*> { &freq, &width, &threshold, &range, &release, &mode, &detection, &lookahead, &presetBox, &listen, &delta, &findButton, &setThreshButton, &guideButton })
        addAndMakeVisible (*c);
    addAndMakeVisible (undoButtons);
    setThreshButton.setTooltip ("Listens to a second and a half of your sound and sets THRESH where its loudest few percent of the s begin: a starting point. FREQ is not moved; "
                                "if the sound has no s in it, or is silent, THRESH stays as it is and the strip says why.");
    setThreshButton.onClick = [this]
    {
        setCountAtClick = proc.engine.setCount.load (std::memory_order_acquire);
        setDetectionAtClick = readParams (proc.apvts).detection;
        setAwaiting = true;
        proc.engine.startSetThreshold (setDetectionAtClick);
        setNoteText = "SET THRESH: listening for 1.5 s. Play a passage with the s in it.";
    };
    findButton.setTooltip ("With LISTEN on: listens to the next second and a half of your sound and sets FREQ to the strongest place between 4 and 10 kHz (the s). Play a passage with the s in it; if the sound is too quiet nothing changes.");
    findButton.onClick = [this]
    {
        findCountAtClick = proc.engine.findCount.load (std::memory_order_acquire);
        awaiting = true;
        proc.engine.startFind();
        findButton.setButtonText ("FINDING");
    };
    startTimerHz (kTimerHz);
    setSize (kWidth, kHeight);
}

NemesisEditor::~NemesisEditor() { setLookAndFeel (nullptr); }

void NemesisEditor::resized()
{
    auto bounds = getLocalBounds().reduced (12, 0);
    auto header = bounds.removeFromTop (headerHeight).withTrimmedTop (8).withTrimmedBottom (8);
    nameplate = header.removeFromLeft (250);
    guideButton.setBounds (header.removeFromRight (64).withSizeKeepingCentre (64, 28));
    header.removeFromLeft (20);
    // UNDO / REDO in the top strip of the header, above the preset and look-ahead labels, which the header row leaves no room for beside SET THRESH.
    const auto strip = header.withHeight (16).translated (0, -6);
    undoButtons.setBounds (strip.getX(), strip.getY(), ui::UndoButtons::kWidth, strip.getHeight());
    auto headerRow = header.withTrimmedTop (28).withHeight (28);
    presetLabel = { headerRow.getX() + 2, headerRow.getY() - 15, 120, 12 };
    presetBox.setBounds (headerRow.removeFromLeft (160));
    headerRow.removeFromLeft (12);
    lookaheadLabel = { headerRow.getX() + 2, headerRow.getY() - 15, 120, 12 };
    lookahead.setBounds (headerRow.removeFromLeft (100));
    headerRow.removeFromLeft (10);
    latencyLabel = headerRow.removeFromLeft (100);
    headerRow.removeFromLeft (6);
    setThreshButton.setBounds (headerRow.removeFromLeft (110));

    bounds.removeFromTop (10);
    auto display = bounds.removeFromTop (displayHeight);
    bandArea = display.removeFromLeft (display.getWidth() / 2 - 4);
    display.removeFromLeft (8);
    historyArea = display;
    bounds.removeFromTop (8);
    auto knobs = bounds.removeFromTop (cellHeight);
    for (auto* k : { &freq, &width, &threshold, &range, &release })
        k->setBounds (knobs.removeFromLeft (cellWidth));
    knobs.removeFromLeft (14);
    auto column = knobs.withTrimmedTop (4);
    modeLabel = { column.getX() + 2, column.getY(), 100, 10 };
    column.removeFromTop (12);
    mode.setBounds (column.removeFromTop (26).removeFromLeft (150));
    column.removeFromTop (4);
    detectionLabel = { column.getX() + 2, column.getY(), 100, 10 };
    column.removeFromTop (12);
    detection.setBounds (column.removeFromTop (26).removeFromLeft (150));
    column.removeFromTop (6);
    auto listenRow = column.removeFromTop (26);
    listen.setBounds (listenRow.removeFromLeft (80));
    listenRow.removeFromLeft (6);
    delta.setBounds (listenRow.removeFromLeft (80));
    listenRow.removeFromLeft (6);
    findButton.setBounds (listenRow.removeFromLeft (80));
    noteStrip = bounds.withTrimmedTop (8).withHeight (ui::kNoteStripHeight);
}

AdviceInput NemesisEditor::adviceInput() const
{
    AdviceInput in;
    in.params = readParams (proc.apvts);
    in.reductionDb = shownReduction;
    in.heldDb = heldReductionDb();
    in.threshNote = setNoteText;
    return in;
}

void NemesisEditor::paint (juce::Graphics& g)
{
    g.fillAll (colours::background);
    hw::drawHeaderPlate (g, getLocalBounds().removeFromTop (headerHeight).toFloat());
    hw::drawNameplate (g, nameplate, gui::kProjectWord, gui::kPluginWord, "De-esser", shownReduction > 0.05f, 25.0f);
    hw::drawLabel (g, "Mode", modeLabel, juce::Justification::left, colours::dim, 9.0f);
    hw::drawLabel (g, "Detection", detectionLabel, juce::Justification::left, colours::dim, 9.0f);
    hw::drawLabel (g, "Preset", presetLabel, juce::Justification::left, colours::dim, 9.0f);
    hw::drawLabel (g, "Look-ahead", lookaheadLabel, juce::Justification::left, colours::dim, 9.0f);
    const int latency = proc.engine.latencySamples.load();
    hw::drawLabel (g, latency == 0 ? juce::String ("No delay") : "Delay " + juce::String (1000.0 * latency / std::max (proc.getSampleRate(), 1.0), 1) + " ms", latencyLabel,
                   juce::Justification::centredLeft, colours::dim, 9.5f);
    ui::drawNoteStrip (g, noteStrip, adviceFor (adviceInput()));
    const auto params = readParams (proc.apvts);

    // The band the detector hears: its response on the frequency axis.
    {
        const auto glass = bandArea.toFloat();
        hw::drawGlass (g, glass, 4.0f, colours::accent);
        const auto r = glass.reduced (34.0f, 16.0f).withTrimmedBottom (8.0f);
        const auto xOf = [&] (double hz) { return r.getX() + r.getWidth() * ui::FreqAxis::toNormalised ((float) hz); };
        const auto yOf = [&] (double dbv) { return r.getY() + r.getHeight() * (float) (-dbv / -gui::kBandFloorDb); };
        g.setFont (ui::lcdFont (9.5f));
        for (const double hz : { 100.0, 1000.0, 10000.0 })
        {
            g.setColour (colours::glassGrid);
            g.drawVerticalLine ((int) xOf (hz), r.getY(), r.getBottom());
            g.setColour (colours::dim);
            g.drawText (hz < 1000.0 ? juce::String ((int) hz) : juce::String ((int) (hz / 1000.0)) + "k", juce::Rectangle<float> (xOf (hz) - 16.0f, r.getBottom() + 1.0f, 32.0f, 10.0f), juce::Justification::centred);
        }
        for (int d = 0; d >= (int) gui::kBandFloorDb; d -= 12)
        {
            g.setColour (colours::glassGrid);
            g.drawHorizontalLine ((int) yOf (d), r.getX(), r.getRight());
            g.setColour (colours::dim);
            g.drawText (juce::String (d), juce::Rectangle<float> (glass.getX() + 4.0f, yOf (d) - 7.0f, 26.0f, 14.0f), juce::Justification::centredRight);
        }
        pdsp::Biquad bp;
        const double fs = std::max (proc.getSampleRate(), 8000.0);
        bp.setBandpass (fs, std::clamp ((double) params.freqHz, 1000.0, 16000.0), std::clamp ((double) params.q, 0.3, 8.0));
        juce::Path curve;
        const int steps = (int) r.getWidth();
        for (int i = 0; i <= steps; ++i)
        {
            const double hz = ui::FreqAxis::toHz ((float) i / (float) steps);
            const float y = juce::jlimit (r.getY(), r.getBottom(), yOf (20.0 * std::log10 (std::fmax (bp.magnitudeAt (fs, hz), 1e-4))));
            i == 0 ? curve.startNewSubPath (r.getX(), y) : curve.lineTo (r.getX() + (float) i, y);
        }
        juce::Path filled (curve);
        filled.lineTo (r.getRight(), r.getBottom());
        filled.lineTo (r.getX(), r.getBottom());
        filled.closeSubPath();
        g.setColour ((params.listen ? colours::accent : colours::amber).withAlpha (0.25f));
        g.fillPath (filled);
        g.setColour (params.listen ? colours::accent : colours::amber);
        g.strokePath (curve, juce::PathStrokeType (1.6f));
        hw::drawLabel (g, "WHAT IT LISTENS TO", { (int) glass.getX() + 40, (int) glass.getY() + 4, 150, 11 }, juce::Justification::left, colours::dim, 8.0f);
        hw::drawGlassOverlay (g, glass, 4.0f, false);
    }

    // The reduction of the last eight seconds, hanging from the top.
    {
        const auto glass = historyArea.toFloat();
        hw::drawGlass (g, glass, 4.0f, colours::amber);
        const auto r = glass.reduced (34.0f, 16.0f).withTrimmedRight (74.0f);
        g.setFont (ui::lcdFont (10.0f));
        for (int d = 0; d <= (int) gui::kReductionRangeDb; d += 6)
        {
            const float y = r.getY() + r.getHeight() * (float) d / gui::kReductionRangeDb;
            g.setColour (colours::glassGrid);
            g.drawHorizontalLine ((int) y, r.getX(), r.getRight());
            g.setColour (colours::dim);
            g.drawText (d == 0 ? "0" : "-" + juce::String (d), juce::Rectangle<float> (glass.getX() + 4.0f, y - 7.0f, 26.0f, 14.0f), juce::Justification::centredRight);
        }
        juce::Path area;
        area.startNewSubPath (r.getX(), r.getY());
        for (int i = 0; i < kHistory; ++i)
            area.lineTo (r.getX() + r.getWidth() * (float) i / (float) (kHistory - 1), r.getY() + r.getHeight() * juce::jlimit (0.0f, 1.0f, history[(size_t) i] / gui::kReductionRangeDb));
        area.lineTo (r.getRight(), r.getY());
        area.closeSubPath();
        g.setColour (colours::amber.withAlpha (0.35f));
        g.fillPath (area);
        g.setColour (colours::amber);
        g.strokePath (area, juce::PathStrokeType (1.4f));
        g.setFont (ui::lcdFont (13.0f));
        g.drawText (shownReduction < 0.05f ? juce::String ("0.0 dB") : "-" + juce::String (shownReduction, 1) + " dB", juce::Rectangle<float> (r.getRight() + 6.0f, r.getY(), 70.0f, 16.0f),
                    juce::Justification::centredLeft);
        hw::drawLabel (g, "REDUCTION", { (int) r.getRight() + 8, (int) r.getY() + 16, 66, 12 }, juce::Justification::left, colours::dim, 8.5f);
        hw::drawGlassOverlay (g, glass, 4.0f, false);
    }
}

void NemesisEditor::timerCallback()
{
    // Up at once, down slowly, so a short reduction can be read.
    const float now = proc.engine.reductionMeter.exchange (0.0f);
    std::copy (history.begin() + 1, history.end(), history.begin());
    history.back() = now;
    shownReduction = now >= shownReduction ? now : std::max (now, shownReduction - gui::kReductionRangeDb / (float) kTimerHz);
    if (awaiting && proc.engine.findCount.load (std::memory_order_acquire) != findCountAtClick)
    {
        awaiting = false;
        const float found = proc.engine.foundHz.load (std::memory_order_relaxed);
        if (found > 0.0f)
            setValue (proc.apvts, id::freq, found);
    }
    if (! awaiting)
        findButton.setButtonText ("FIND");
    findButton.setEnabled (readParams (proc.apvts).listen && ! awaiting);

    if (setAwaiting && proc.engine.setCount.load (std::memory_order_acquire) != setCountAtClick)
    {
        setAwaiting = false;
        const auto status = (SetStatus) proc.engine.setStatus.load (std::memory_order_relaxed);
        const bool relative = setDetectionAtClick == Detection::relative;
        if (status == SetStatus::silent)
            setNoteText = "SET THRESH: the sound was silent, so THRESH is unchanged.";
        else if (status == SetStatus::tooLittle)
            setNoteText = "SET THRESH: no s stands out of the sound, so THRESH is unchanged. Play a passage with s in it.";
        else if (status == SetStatus::stopped)
            setNoteText = "SET THRESH: it stopped before it had listened, so THRESH is unchanged.";
        else if (readParams (proc.apvts).detection != setDetectionAtClick)
            setNoteText = "SET THRESH: the detection changed while it listened, so THRESH is unchanged.";
        else
        {
            // THRESH's own range (Parameters.h) bounds the value; the step is one undo step, as a preset is.
            const float db = proc.engine.setDb.load (std::memory_order_relaxed);
            const float value = static_cast<juce::AudioParameterFloat*> (proc.apvts.getParameter (id::threshold))->range.getRange().clipValue (db);
            ui::beginUndoStep (proc.apvts, "SET THRESH");
            setValue (proc.apvts, id::threshold, value);
            setNoteText = "SET THRESH: " + juce::String (value, 1).toStdString() + (relative ? " dB under the sound" : " dBFS") +
                          ". Only the loudest few percent of the s is over it. DELTA shows what is cut.";
        }
    }
    setThreshButton.setButtonText (setAwaiting ? "SETTING" : "SET THRESH");
    setThreshButton.setEnabled (! setAwaiting);
    presetBox.sync();
    repaint();
}
