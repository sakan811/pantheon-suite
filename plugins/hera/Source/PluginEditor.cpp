// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 The Pantheon authors

#include "PluginEditor.h"

using namespace hera;
namespace hw = ui::hw;

namespace
{
constexpr int headerHeight = 84;
constexpr int displayHeight = 230;
constexpr int kKnobs = 9;
constexpr int cellWidth = 96;
constexpr int cellHeight = 108;
constexpr int rowHeight = 34;
constexpr int kTimerHz = 30;
}

HeraEditor::HeraEditor (HeraProcessor& p)
    : AudioProcessorEditor (&p),
      threshold (p.apvts, id::threshold, "THRESH", "The level (peak of a sine, in dB) above which the sound is turned down. The knee is around it.", ui::Knob::Layout::below),
      ratio (p.apvts, id::ratio, "RATIO", "How many dB the sound has to rise past the threshold for 1 dB more to come out. 20:1 is nearly a limiter.", ui::Knob::Layout::below),
      attack (p.apvts, id::attack, "ATTACK", "How fast the gain goes down (63% of the way in this time). Slow lets the transient through, fast takes it away.", ui::Knob::Layout::below),
      release (p.apvts, id::release, "RELEASE", "How fast the gain comes back (63% of the way in this time). Not used with AUTO release.", ui::Knob::Layout::below),
      knee (p.apvts, id::knee, "KNEE", "How gradually it starts compressing around THRESH: 0 is hard, 24 dB is gentle.", ui::Knob::Layout::below),
      makeup (p.apvts, id::makeup, "MAKEUP", "Gain after the compressor (added to AUTO makeup when that is on).", ui::Knob::Layout::below),
      mix (p.apvts, id::mix, "MIX", "100%: the compressed sound. Less: it is blended with the dry sound (parallel compression keeps the transients and lifts the body).", ui::Knob::Layout::below),
      link (p.apvts, id::link, "LINK", "100%: both channels get the same gain (the louder one decides), so the image does not move. 0%: each channel on its own.", ui::Knob::Layout::below),
      hpf (p.apvts, id::hpf, "SC HPF", "A high-pass on what the detector listens to (not on the sound), so the bass does not make the compressor pump. Off at 20 Hz.", ui::Knob::Layout::below),
      detector (p.apvts, id::detector, "Peak: reads each peak, so it is quick and follows transients.\nRMS: reads the average over 10 ms, closer to what the ear hears and gentler on peaks."),
      lookahead (p.apvts, id::lookahead, "The sound is heard this long after the detector has seen it, so the gain is already down when a peak arrives (a fast attack no longer lets the first of it through).\n"
                                         "It is a delay: the host compensates it. Changing it while playing restarts the compressor (a short gap)."),
      presetBox (p, presets(), "Everything is set by a preset: what it does not list goes back to its default."),
      autoRelease (p.apvts, id::autoRelease, "AUTO REL", "Program-dependent release: a short peak lets go quickly (50 ms), a long loud passage slowly (600 ms). RELEASE is not used."),
      autoMakeup (p.apvts, id::autoMakeup, "AUTO MAKEUP", "Adds what the compression takes from a signal at 0 dBFS, so a full-scale input is full scale out. MAKEUP adds to it."),
      key (p.apvts, id::external, "KEY", "The detector listens to the sidechain input (another track routed in the host) instead of this one. With nothing connected it listens to itself."),
      proc (p)
{
    setLookAndFeel (&look);
    for (auto* c : std::initializer_list<juce::Component*> { &threshold, &ratio, &attack, &release, &knee, &makeup, &mix, &link, &hpf, &detector, &lookahead, &presetBox, &autoRelease, &autoMakeup, &key, &guideButton })
        addAndMakeVisible (*c);
    startTimerHz (kTimerHz);
    setSize (24 + kKnobs * cellWidth, headerHeight + 10 + displayHeight + 8 + cellHeight + 8 + rowHeight + 10 + ui::kNoteStripHeight + 12);
}

HeraEditor::~HeraEditor() { setLookAndFeel (nullptr); }

void HeraEditor::resized()
{
    auto bounds = getLocalBounds().reduced (12, 0);
    auto header = bounds.removeFromTop (headerHeight).withTrimmedTop (8).withTrimmedBottom (8);
    nameplate = header.removeFromLeft (250);
    guideButton.setBounds (header.removeFromRight (64).withSizeKeepingCentre (64, 28));
    header.removeFromLeft (24);
    auto headerRow = header.withTrimmedTop (28).withHeight (28);
    presetLabel = { headerRow.getX() + 2, headerRow.getY() - 15, 120, 12 };
    presetBox.setBounds (headerRow.removeFromLeft (190));
    headerRow.removeFromLeft (14);
    lookaheadLabel = { headerRow.getX() + 2, headerRow.getY() - 15, 120, 12 };
    lookahead.setBounds (headerRow.removeFromLeft (110));
    headerRow.removeFromLeft (14);
    latencyLabel = headerRow.removeFromLeft (120);

    bounds.removeFromTop (10);
    auto display = bounds.removeFromTop (displayHeight);
    curveArea = display.removeFromLeft (displayHeight);
    display.removeFromLeft (8);
    historyArea = display;
    bounds.removeFromTop (8);
    auto knobs = bounds.removeFromTop (cellHeight);
    for (auto* k : { &threshold, &ratio, &attack, &release, &knee, &makeup, &mix, &link, &hpf })
        k->setBounds (knobs.removeFromLeft (cellWidth));
    bounds.removeFromTop (8);
    auto row = bounds.removeFromTop (rowHeight).withTrimmedTop (4).withTrimmedBottom (4);
    detectorLabel = { row.getX() + 2, row.getY() - 11, 80, 10 };
    detector.setBounds (row.removeFromLeft (120));
    row.removeFromLeft (14);
    autoRelease.setBounds (row.removeFromLeft (110));
    row.removeFromLeft (10);
    autoMakeup.setBounds (row.removeFromLeft (130));
    row.removeFromLeft (10);
    key.setBounds (row.removeFromLeft (90));
    noteStrip = bounds.withTrimmedTop (10).withHeight (ui::kNoteStripHeight);
}

AdviceInput HeraEditor::adviceInput() const
{
    AdviceInput in;
    in.params = readParams (proc.apvts);
    in.reductionDb = shownReduction;
    in.heldDb = heldReductionDb();
    in.sidechainConnected = proc.sidechainConnected.load (std::memory_order_relaxed);
    return in;
}

void HeraEditor::paint (juce::Graphics& g)
{
    g.fillAll (colours::background);
    hw::drawHeaderPlate (g, getLocalBounds().removeFromTop (headerHeight).toFloat());
    hw::drawNameplate (g, nameplate, gui::kProjectWord, gui::kPluginWord, "Compressor", inDb > -80.0f, 25.0f);
    ui::drawNoteStrip (g, noteStrip, adviceFor (adviceInput()));
    hw::drawLabel (g, "Detector", detectorLabel, juce::Justification::left, colours::dim, 9.0f);
    hw::drawLabel (g, "Preset", presetLabel, juce::Justification::left, colours::dim, 9.0f);
    hw::drawLabel (g, "Look-ahead", lookaheadLabel, juce::Justification::left, colours::dim, 9.0f);
    const auto params = readParams (proc.apvts);
    const int latency = proc.engine.latencySamples.load();
    hw::drawLabel (g, latency == 0 ? juce::String ("No delay") : "Delay " + juce::String (1000.0 * latency / std::max (proc.getSampleRate(), 1.0), 1) + " ms", latencyLabel,
                   juce::Justification::centredLeft, colours::dim, 9.5f);

    // The transfer curve: input level across, output level up; the diagonal is "no compression".
    {
        const auto glass = curveArea.toFloat();
        hw::drawGlass (g, glass, 4.0f, colours::accent);
        const auto r = glass.reduced (30.0f, 18.0f);
        const auto xOf = [&] (float dbIn) { return r.getX() + r.getWidth() * (dbIn - gui::kCurveMinDb) / (gui::kCurveMaxDb - gui::kCurveMinDb); };
        const auto yOf = [&] (float dbOut) { return r.getBottom() - r.getHeight() * (dbOut - gui::kCurveMinDb) / (gui::kCurveMaxDb - gui::kCurveMinDb); };
        g.setFont (ui::lcdFont (9.5f));
        for (int d = -60; d <= 0; d += 12)
        {
            g.setColour (colours::glassGrid);
            g.drawVerticalLine ((int) xOf ((float) d), r.getY(), r.getBottom());
            g.drawHorizontalLine ((int) yOf ((float) d), r.getX(), r.getRight());
            g.setColour (colours::dim);
            g.drawText (juce::String (d), juce::Rectangle<float> (xOf ((float) d) - 14.0f, r.getBottom() + 3.0f, 28.0f, 11.0f), juce::Justification::centred);
        }
        g.setColour (colours::glassGrid.brighter (0.6f));
        g.drawLine (xOf (gui::kCurveMinDb), yOf (gui::kCurveMinDb), xOf (gui::kCurveMaxDb), yOf (gui::kCurveMaxDb));
        const double makeupDb = params.makeupDb + (params.autoMakeup ? autoMakeupDb (params.thresholdDb, params.ratio, params.kneeDb) : 0.0);
        juce::Path curve;
        for (int i = 0; i <= 120; ++i)
        {
            const float dbIn = gui::kCurveMinDb + (gui::kCurveMaxDb - gui::kCurveMinDb) * (float) i / 120.0f;
            const float dbOut = juce::jlimit (gui::kCurveMinDb, gui::kCurveMaxDb, dbIn - (float) reductionDb (dbIn, params.thresholdDb, params.ratio, params.kneeDb) + (float) makeupDb);
            i == 0 ? curve.startNewSubPath (xOf (dbIn), yOf (dbOut)) : curve.lineTo (xOf (dbIn), yOf (dbOut));
        }
        g.setColour (colours::amber);
        g.strokePath (curve, juce::PathStrokeType (1.8f));
        if (inDb > gui::kCurveMinDb)
        {
            const float outDb = juce::jlimit (gui::kCurveMinDb, gui::kCurveMaxDb, inDb - shownReduction + (float) makeupDb);
            hw::drawLamp (g, { xOf (juce::jlimit (gui::kCurveMinDb, gui::kCurveMaxDb, inDb)), yOf (outDb) }, 5.0f, colours::accent, true);
        }
        hw::drawLabel (g, "IN (ACROSS) / OUT (UP), dB", { (int) glass.getX() + 8, (int) glass.getY() + 3, (int) glass.getWidth() - 16, 11 }, juce::Justification::left, colours::dim, 8.0f);
        hw::drawGlassOverlay (g, glass, 4.0f, false);
    }

    // The reduction of the last ten seconds, hanging from the top.
    {
        const auto glass = historyArea.toFloat();
        hw::drawGlass (g, glass, 4.0f, colours::amber);
        const auto r = glass.reduced (40.0f, 18.0f).withTrimmedRight (86.0f);
        g.setFont (ui::lcdFont (10.0f));
        for (int d = 0; d <= (int) gui::kReductionRangeDb; d += 6)
        {
            const float y = r.getY() + r.getHeight() * (float) d / gui::kReductionRangeDb;
            g.setColour (colours::glassGrid);
            g.drawHorizontalLine ((int) y, r.getX(), r.getRight());
            g.setColour (colours::dim);
            g.drawText (d == 0 ? "0" : "-" + juce::String (d), juce::Rectangle<float> (glass.getX() + 6.0f, y - 7.0f, 28.0f, 14.0f), juce::Justification::centredRight);
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
        g.drawText (shownReduction < 0.05f ? juce::String ("0.0 dB") : "-" + juce::String (shownReduction, 1) + " dB", juce::Rectangle<float> (r.getRight() + 6.0f, r.getY(), 80.0f, 16.0f),
                    juce::Justification::centredLeft);
        hw::drawLabel (g, "REDUCTION", { (int) r.getRight() + 8, (int) r.getY() + 16, 74, 12 }, juce::Justification::left, colours::dim, 8.5f);
        hw::drawGlassOverlay (g, glass, 4.0f, false);
    }
}

void HeraEditor::timerCallback()
{
    // Up at once, down slowly, so a short reduction can be read.
    const float now = proc.engine.reductionMeter.exchange (0.0f);
    std::copy (history.begin() + 1, history.end(), history.begin());
    history.back() = now;
    shownReduction = now >= shownReduction ? now : std::max (now, shownReduction - gui::kReductionRangeDb / (float) kTimerHz);
    const float in = proc.engine.inPeak.exchange (0.0f);
    proc.engine.outPeak.store (0.0f);
    inDb = in > 1e-5f ? 20.0f * std::log10 (in) : -90.0f;
    presetBox.sync();
    repaint();
}
