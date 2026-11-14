// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 The Pantheon authors

#include "PluginEditor.h"

using namespace dionysus;
namespace hw = ui::hw;

namespace
{
constexpr int headerHeight = 84;
constexpr int displayHeight = 220;
constexpr int cellWidth = 112;
constexpr int cellHeight = 108;
constexpr int kTimerHz = 20;
constexpr int kWidth = 1000;
constexpr int kMeterWidth = 92;
constexpr float kMeterMinDb = -36.0f, kMeterMaxDb = 12.0f; // the drive meter's scale: 0 dB is where the curves turn over
}

DionysusEditor::DionysusEditor (DionysusProcessor& p)
    : AudioProcessorEditor (&p),
      drive (p.apvts, id::drive, "DRIVE", "How hard the sound is pushed into the curve. More is denser, then harsher.", ui::Knob::Layout::below),
      bias (p.apvts, id::bias, "BIAS", "Moves the working point of the curve: even harmonics (a rounder, thicker sound). The offset it makes is taken away by a 10 Hz high-pass.", ui::Knob::Layout::below),
      tone (p.apvts, id::tone, "TONE", "A tilt after the curve: + is brighter (high shelf up 6 dB, low shelf down 6), - is darker. Turns around 1 kHz.", ui::Knob::Layout::below),
      lowCut (p.apvts, id::lowCut, "LOW CUT", "A high-pass on the saturated path only, before the curve: the bass is not saturated (so it stays clean and does not intermodulate with the rest). "
                                              "The dry sound is not filtered, so under 100% MIX the lows stay. Off at 20 Hz.", ui::Knob::Layout::below),
      mix (p.apvts, id::mix, "MIX", "The saturated sound blended with the dry one (parallel saturation keeps the transients and lifts the body). The dry sound is delayed to match the oversampler.", ui::Knob::Layout::below),
      out (p.apvts, id::out, "OUTPUT", "Output gain, after everything (and after AUTO GAIN).", ui::Knob::Layout::below),
      shape (p.apvts, id::shape,
             "Tape: a gentle symmetric curve (odd harmonics).\nTube: softer onset, more high harmonics as it is driven.\nTransformer: a cubic, thick (strong 3rd).\n"
             "Diode: uneven (even harmonics of its own).\nFold: peaks turn back on themselves.\nClip: peaks are cut."),
      oversampling (p.apvts, id::oversampling,
                    "Runs the curve at 2, 4 or 8 times the sample rate, so the harmonics above half the rate do not fold back as tones that were never there. Adds a delay (the host compensates) "
                    "and processor load; the dry sound is delayed to match. Changing it while playing restarts the saturator."),
      autoGain (p.apvts, id::autoGain, "AUTO GAIN", "Keeps the loudness of the sound the same whatever DRIVE is (measured on a sine at -12 dBFS), so you judge the tone, not the level."),
      presetBox (p, presets(), "Everything is set by a preset, oversampling included."),
      proc (p)
{
    setLookAndFeel (&look);
    for (auto* c : std::initializer_list<juce::Component*> { &drive, &bias, &tone, &lowCut, &mix, &out, &shape, &oversampling, &autoGain, &deltaButton, &presetBox, &guideButton })
        addAndMakeVisible (*c);
    deltaButton.setClickingTogglesState (true);
    deltaButton.setTooltip ("Listen to only what the saturator adds (output minus input, delayed like it). Not saved with the project, and it ends when the window closes.");
    deltaButton.onClick = [this] { proc.engine.delta = deltaButton.getToggleState(); };
    startTimerHz (kTimerHz);
    setSize (kWidth, headerHeight + 10 + displayHeight + 8 + cellHeight + 8 + ui::kNoteStripHeight + 12);
}

DionysusEditor::~DionysusEditor()
{
    proc.engine.delta = false; // monitoring never outlives the window that asked for it
    setLookAndFeel (nullptr);
}

void DionysusEditor::resized()
{
    auto bounds = getLocalBounds().reduced (12, 0);
    auto header = bounds.removeFromTop (headerHeight).withTrimmedTop (8).withTrimmedBottom (8);
    nameplate = header.removeFromLeft (250);
    guideButton.setBounds (header.removeFromRight (64).withSizeKeepingCentre (64, 28));
    header.removeFromRight (12);
    latencyLabel = header.removeFromRight (120);
    header.removeFromLeft (24);
    auto presetRow = header.withTrimmedTop (28).withHeight (28);
    presetLabel = { presetRow.getX() + 2, presetRow.getY() - 15, 120, 12 };
    presetBox.setBounds (presetRow.removeFromLeft (190));

    bounds.removeFromTop (10);
    auto display = bounds.removeFromTop (displayHeight);
    curveArea = display.removeFromLeft (displayHeight);
    display.removeFromLeft (8);
    meterArea = display.removeFromRight (kMeterWidth);
    display.removeFromRight (8);
    harmonicArea = display;
    bounds.removeFromTop (8);
    auto knobs = bounds.removeFromTop (cellHeight);
    for (auto* k : { &drive, &bias, &tone, &lowCut, &mix, &out })
        k->setBounds (knobs.removeFromLeft (cellWidth));
    knobs.removeFromLeft (16);
    auto column = knobs.removeFromLeft (160).withTrimmedTop (4);
    shapeLabel = { column.getX() + 2, column.getY(), 100, 10 };
    column.removeFromTop (12);
    auto row = column.removeFromTop (26);
    shape.setBounds (row.removeFromLeft (140));
    column.removeFromTop (4);
    oversamplingLabel = { column.getX() + 2, column.getY(), 100, 10 };
    column.removeFromTop (12);
    oversampling.setBounds (column.removeFromTop (26).removeFromLeft (140));
    auto toggles = knobs.removeFromLeft (120);
    autoGain.setBounds (toggles.withTrimmedTop (14).withHeight (28));
    deltaButton.setBounds (toggles.withTrimmedTop (50).withHeight (28).withTrimmedRight (30));
    noteStrip = bounds.withTrimmedTop (8).withHeight (ui::kNoteStripHeight);
}

AdviceInput DionysusEditor::adviceInput() const
{
    AdviceInput in;
    in.params = readParams (proc.apvts);
    in.latencyMs = 1000.0 * proc.engine.latencySamples.load() / std::max (proc.getSampleRate(), 1.0);
    in.delta = proc.engine.delta.load();
    return in;
}

void DionysusEditor::paint (juce::Graphics& g)
{
    g.fillAll (colours::background);
    hw::drawHeaderPlate (g, getLocalBounds().removeFromTop (headerHeight).toFloat());
    hw::drawNameplate (g, nameplate, gui::kProjectWord, gui::kPluginWord, "Saturator", level > 0.001f, 25.0f);
    hw::drawLabel (g, "Preset", presetLabel, juce::Justification::left, colours::dim, 9.0f);
    hw::drawLabel (g, "Curve", shapeLabel, juce::Justification::left, colours::dim, 9.0f);
    hw::drawLabel (g, "Oversampling", oversamplingLabel, juce::Justification::left, colours::dim, 9.0f);
    const int latency = proc.engine.latencySamples.load();
    hw::drawLabel (g, latency == 0 ? juce::String ("No delay") : "Delay " + juce::String (1000.0 * latency / std::max (proc.getSampleRate(), 1.0), 1) + " ms", latencyLabel,
                   juce::Justification::centredRight, colours::dim, 9.5f);
    ui::drawNoteStrip (g, noteStrip, adviceFor (adviceInput()));
    const auto params = readParams (proc.apvts);

    // The curve: the input across (-1 .. 1), what comes out up, with the drive applied; the diagonal is "no change".
    {
        const auto glass = curveArea.toFloat();
        hw::drawGlass (g, glass, 4.0f, colours::accent);
        const auto r = glass.reduced (20.0f, 20.0f);
        const auto xOf = [&] (double v) { return r.getCentreX() + r.getWidth() * 0.5f * (float) v; };
        const auto yOf = [&] (double v) { return r.getCentreY() - r.getHeight() * 0.5f * (float) v; };
        g.setColour (colours::glassGrid);
        g.drawVerticalLine ((int) r.getCentreX(), r.getY(), r.getBottom());
        g.drawHorizontalLine ((int) r.getCentreY(), r.getX(), r.getRight());
        g.drawRect (r, 1.0f);
        g.setColour (colours::glassGrid.brighter (0.6f));
        g.drawLine (xOf (-1.0), yOf (-1.0), xOf (1.0), yOf (1.0));
        const double gain = std::pow (10.0, (double) params.driveDb / 20.0);
        juce::Path curve;
        for (int i = 0; i <= 200; ++i)
        {
            const double x = -1.0 + 2.0 * i / 200.0;
            const double y = std::clamp (saturate (params.shape, gain * x, (double) params.bias), -1.0, 1.0);
            i == 0 ? curve.startNewSubPath (xOf (x), yOf (y)) : curve.lineTo (xOf (x), yOf (y));
        }
        g.setColour (colours::amber);
        g.strokePath (curve, juce::PathStrokeType (1.8f));
        hw::drawLabel (g, "IN / OUT", { (int) glass.getX() + 6, (int) glass.getY() + 3, 80, 11 }, juce::Justification::left, colours::dim, 8.0f);
        hw::drawGlassOverlay (g, glass, 4.0f, false);
    }

    // The harmonics of a sine at -12 dBFS: nine bars, in dB under the sound that went in.
    {
        const auto glass = harmonicArea.toFloat();
        hw::drawGlass (g, glass, 4.0f, colours::amber);
        const auto r = glass.reduced (40.0f, 22.0f).withTrimmedBottom (10.0f);
        g.setFont (ui::lcdFont (10.0f));
        for (int d = 0; d >= (int) gui::kHarmonicFloorDb; d -= 12)
        {
            const float y = r.getY() + r.getHeight() * (float) (-d) / -gui::kHarmonicFloorDb;
            g.setColour (colours::glassGrid);
            g.drawHorizontalLine ((int) y, r.getX(), r.getRight());
            g.setColour (colours::dim);
            g.drawText (juce::String (d), juce::Rectangle<float> (glass.getX() + 6.0f, y - 7.0f, 30.0f, 14.0f), juce::Justification::centredRight);
        }
        const auto h = harmonicsDb (params.shape, params.driveDb, params.bias);
        const float slot = r.getWidth() / (float) kHarmonics;
        for (int k = 0; k < kHarmonics; ++k)
        {
            const float x = r.getX() + slot * (float) k + slot * 0.2f;
            const float dbv = (float) std::max (h[(size_t) k], (double) gui::kHarmonicFloorDb);
            const float top = r.getY() + r.getHeight() * (-std::min (dbv, 0.0f)) / -gui::kHarmonicFloorDb;
            g.setColour ((k % 2 == 0 ? colours::amber : colours::accent).withAlpha (0.85f));
            g.fillRect (x, top, slot * 0.6f, r.getBottom() - top);
            g.setColour (colours::dim);
            g.drawText (juce::String (k + 1), juce::Rectangle<float> (x - 4.0f, r.getBottom() + 1.0f, slot * 0.6f + 8.0f, 10.0f), juce::Justification::centred);
        }
        hw::drawLabel (g, "HARMONICS (SINE AT -12 dBFS)", { (int) glass.getX() + 44, (int) glass.getY() + 4, 220, 11 }, juce::Justification::left, colours::dim, 8.0f);
        hw::drawGlassOverlay (g, glass, 4.0f, false);
    }

    // The level that reaches the curve (the sound times DRIVE): 0 dB is where the curves turn over; above it the sound is saturated hard. The lamp is the output reaching 0 dBFS.
    {
        const auto glass = meterArea.toFloat();
        hw::drawGlass (g, glass, 4.0f, colours::amber);
        hw::drawLabel (g, "DRIVE LEVEL", { (int) glass.getX() + 6, (int) glass.getY() + 4, (int) glass.getWidth() - 12, 11 }, juce::Justification::left, colours::dim, 8.0f);
        const auto bar = glass.reduced (30.0f, 0.0f).withTrimmedTop (22.0f).withTrimmedBottom (42.0f).withTrimmedLeft (4.0f);
        const auto yOf = [&] (float dbv) { return bar.getBottom() - bar.getHeight() * (juce::jlimit (kMeterMinDb, kMeterMaxDb, dbv) - kMeterMinDb) / (kMeterMaxDb - kMeterMinDb); };
        g.setFont (ui::lcdFont (9.5f));
        for (int d = -36; d <= 12; d += 12)
        {
            g.setColour (d == 0 ? colours::glassGrid.brighter (0.6f) : colours::glassGrid);
            g.drawHorizontalLine ((int) yOf ((float) d), glass.getX() + 28.0f, bar.getRight());
            g.setColour (colours::dim);
            g.drawText (juce::String (d), juce::Rectangle<float> (glass.getX() + 2.0f, yOf ((float) d) - 7.0f, 24.0f, 14.0f), juce::Justification::centredRight);
        }
        const float top = yOf (drivenDb), turn = yOf (0.0f);
        g.setColour (colours::amber.withAlpha (0.85f));
        g.fillRect (bar.getX(), std::max (top, turn), bar.getWidth(), bar.getBottom() - std::max (top, turn)); // up to the turning point
        if (top < turn)
        {
            g.setColour (colours::bad.withAlpha (0.9f));
            g.fillRect (bar.getX(), top, bar.getWidth(), turn - top); // beyond it: hard saturation
        }
        g.setColour (colours::amber);
        g.setFont (ui::lcdFont (11.0f));
        g.drawText (drivenDb < -59.0f ? juce::String ("-inf") : (drivenDb > 0.05f ? "+" : "") + juce::String (drivenDb, 1), juce::Rectangle<float> (glass.getX(), glass.getBottom() - 38.0f, glass.getWidth(), 14.0f), juce::Justification::centred);
        const juce::Point<float> lamp { glass.getX() + 22.0f, glass.getBottom() - 12.0f };
        hw::drawLamp (g, lamp, 4.5f, colours::bad, clipTicks > 0);
        hw::drawLabel (g, "CLIP", { (int) lamp.x + 10, (int) lamp.y - 6, 40, 12 }, juce::Justification::left, colours::dim, 8.5f);
        hw::drawGlassOverlay (g, glass, 4.0f, false);
    }
}

void DionysusEditor::timerCallback()
{
    level = proc.engine.outPeak.exchange (0.0f);
    if (level >= 1.0f)
        clipTicks = kTimerHz; // a second
    else if (clipTicks > 0)
        --clipTicks;
    // Up at once, down slowly, so a short peak can be read.
    const float driven = proc.engine.drivePeak.exchange (0.0f);
    const float nowDb = driven > 1e-4f ? 20.0f * std::log10 (driven) : -60.0f;
    drivenDb = nowDb >= drivenDb ? nowDb : std::max (nowDb, drivenDb - 24.0f / (float) kTimerHz);
    deltaButton.setToggleState (proc.engine.delta.load(), juce::dontSendNotification);
    presetBox.sync();
    repaint();
}
