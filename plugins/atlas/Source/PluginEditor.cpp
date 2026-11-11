// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 The Pantheon authors

#include "PluginEditor.h"

using namespace atlas;
namespace hw = ui::hw;

namespace
{
constexpr int headerHeight = 84;
constexpr int stripHeight = 478;
constexpr int stripGap = 8;
constexpr int cellHeight = 104; // a knob: name, knob, read-out
constexpr int kTimerHz = 30;

juce::String hzName (double hz) { return hz < 1000.0 ? juce::String (juce::roundToInt (hz)) + " Hz" : juce::String (hz / 1000.0, hz < 10000.0 ? 1 : 0) + " kHz"; }
}

AtlasEditor::Strip::Strip (AtlasProcessor& p, int band)
    : on (p.apvts, bandIds (band).on, "ON", "This band's compressor. Off: the band passes as it is."),
      match (p.apvts, bandIds (band).match, "MATCH", "Gain match: brings the band back to the loudness it had before its compressor, so the band is not judged by "
                                                      "being quieter. Slow, and never more than 12 dB. MAKEUP adds to it."),
      channels (p.apvts, bandIds (band).channels,
                "What the band's compressor works on.\nStereo: left and right with one gain, so the image does not move.\nL/R: left and right, each with its own gain.\n"
                "M/S: the centre and the sides, each with its own gain.\nLeft / Right / Mid / Side: that one only; the rest of the band is untouched."),
      threshold (p.apvts, bandIds (band).threshold, "THRESH", "The level (peak, in dB) of the band above which it is turned down. The knee is 6 dB wide.", ui::Knob::Layout::below),
      ratio (p.apvts, bandIds (band).ratio, "RATIO", "How many dB the band has to rise past the threshold for 1 dB more to come out.", ui::Knob::Layout::below),
      attack (p.apvts, bandIds (band).attack, "ATTACK", "How fast the gain goes down.", ui::Knob::Layout::below),
      release (p.apvts, bandIds (band).release, "RELEASE", "How fast the gain comes back.", ui::Knob::Layout::below),
      makeup (p.apvts, bandIds (band).makeup, "MAKEUP", "Gain after the band's compressor.", ui::Knob::Layout::below)
{
    const auto& info = pdsp::bands::kBands[(size_t) band];
    if (band < kCrossovers)
        crossover = std::make_unique<ui::Knob> (p.apvts, id::crossover[(size_t) band], "XOVER",
                                                "Where this band ends and the " + juce::String (pdsp::bands::kBands[(size_t) (band + 1)].title) + " begin. The edges keep their order.",
                                                ui::Knob::Layout::below);
    solo.setTooltip ("Solo the " + juce::String (info.title) + " band: " + info.role + ". Click more bands to hear them together. Not saved with the session.");
    delta.setTooltip ("Listen to only what this band's compressor changes (its output minus its input). Not saved with the session.");
    auto& monitor = p.engine.monitor;
    solo.onClick = [&monitor, band] { monitor.solo ^= 1u << band; };
    delta.onClick = [&monitor, band] { monitor.delta ^= 1u << band; };
}

AtlasEditor::AtlasEditor (AtlasProcessor& p)
    : AudioProcessorEditor (&p), proc (p),
      phaseBox (p.apvts, id::phase, "ZERO LATENCY: the bands are split by minimum-phase crossovers, the way an analogue one does: no delay, and the phase turns "
                                    "around each edge.\nLINEAR: the same bands with no phase shift at all, so what the compressors do not touch is the input, only later; "
                                    "the whole sound is delayed (the host compensates)."),
      oversamplingBox (p.apvts, id::oversampling, "Runs the compressors at 2 or 4 times the sample rate, so a fast gain on high notes does not fold back as "
                                                   "false tones. Adds a short delay (the host compensates) and processor load."),
      outKnob (p.apvts, id::out, "OUTPUT", "Output gain, after everything.")
{
    setLookAndFeel (&look);
    for (int b = 0; b < kBands; ++b)
    {
        auto& s = *(strips[(size_t) b] = std::make_unique<Strip> (p, b));
        for (auto* c : std::initializer_list<juce::Component*> { &s.on, &s.match, &s.solo, &s.delta, &s.channels, &s.threshold, &s.ratio, &s.attack, &s.release, &s.makeup, s.crossover.get() })
            if (c != nullptr)
                addAndMakeVisible (*c);
    }
    clearButton.setTooltip ("End every solo and delta: all six bands, as they come out.");
    clearButton.onClick = [this] { proc.engine.monitor.solo = 0u; proc.engine.monitor.delta = 0u; };
    for (auto* c : std::initializer_list<juce::Component*> { &phaseBox, &oversamplingBox, &outKnob, &clearButton })
        addAndMakeVisible (*c);

    startTimerHz (kTimerHz);
    setSize (24 + kBands * 186 + (kBands - 1) * stripGap, headerHeight + 10 + stripHeight + 12);
}

AtlasEditor::~AtlasEditor()
{
    // Monitoring never outlives the window that asked for it.
    proc.engine.monitor.solo = 0u;
    proc.engine.monitor.delta = 0u;
    setLookAndFeel (nullptr);
}

void AtlasEditor::resized()
{
    auto bounds = getLocalBounds().reduced (12, 0);
    auto header = bounds.removeFromTop (headerHeight).withTrimmedTop (8).withTrimmedBottom (8);
    nameplate = header.removeFromLeft (250);
    header.removeFromLeft (24);
    outKnob.setBounds (header.removeFromRight (170).withSizeKeepingCentre (170, 52));
    header.removeFromRight (16);
    auto row = header.withTrimmedTop (28).withHeight (28);
    phaseLabel = { row.getX() + 2, row.getY() - 15, 120, 12 };
    phaseBox.setBounds (row.removeFromLeft (140));
    row.removeFromLeft (14);
    oversamplingLabel = { row.getX() + 2, row.getY() - 15, 120, 12 };
    oversamplingBox.setBounds (row.removeFromLeft (84));
    row.removeFromLeft (14);
    latencyLabel = row.removeFromLeft (150);
    clearButton.setBounds (row.removeFromRight (96));

    bounds.removeFromTop (10);
    auto area = bounds.removeFromTop (stripHeight);
    const int width = (area.getWidth() - (kBands - 1) * stripGap) / kBands;
    for (auto& strip : strips)
    {
        auto& s = *strip;
        s.bounds = area.removeFromLeft (width);
        area.removeFromLeft (stripGap);
        auto r = s.bounds.reduced (10, 0).withTrimmedTop (32);
        const auto pair = [&r] (juce::Component& a, juce::Component& b)
        {
            auto line = r.removeFromTop (22);
            a.setBounds (line.removeFromLeft ((line.getWidth() - 6) / 2));
            b.setBounds (line.withTrimmedLeft (6));
            r.removeFromTop (6);
        };
        pair (s.on, s.match);
        pair (s.solo, s.delta);
        s.channels.setBounds (r.removeFromTop (24));
        r.removeFromTop (8);
        s.meter = r.removeFromTop (24);
        r.removeFromTop (6);
        const int cell = r.getWidth() / 2;
        ui::Knob* knobs[] = { &s.threshold, &s.ratio, &s.attack, &s.release, &s.makeup, s.crossover.get() };
        for (int k = 0; k < 6; ++k)
            if (knobs[k] != nullptr)
                knobs[k]->setBounds (r.getX() + cell * (k % 2), r.getY() + cellHeight * (k / 2), cell, cellHeight);
    }
}

void AtlasEditor::paint (juce::Graphics& g)
{
    g.fillAll (colours::background);
    hw::drawHeaderPlate (g, getLocalBounds().removeFromTop (headerHeight).toFloat());
    hw::drawNameplate (g, nameplate, gui::kProjectWord, gui::kPluginWord, "Multiband compressor", inLevel > 0.001f, 25.0f);
    hw::drawLabel (g, "Phase", phaseLabel, juce::Justification::left, colours::dim, 9.0f);
    hw::drawLabel (g, "Oversampling", oversamplingLabel, juce::Justification::left, colours::dim, 9.0f);
    const int latency = proc.engine.latencySamples.load();
    hw::drawLabel (g, latency == 0 ? juce::String ("No delay") : "Delay " + juce::String (1000.0 * latency / std::max (proc.getSampleRate(), 1.0), 1) + " ms",
                   latencyLabel, juce::Justification::centredLeft, colours::dim, 9.5f);

    const unsigned solo = proc.engine.monitor.solo.load(), delta = proc.engine.monitor.delta.load();
    for (int b = 0; b < kBands; ++b)
    {
        const auto& s = *strips[(size_t) b];
        hw::drawModule (g, s.bounds.toFloat());
        const bool heard = solo == 0u || (solo & (1u << b)) != 0u;
        const auto range = bandRangeHz (proc.apvts, b);
        hw::drawLabel (g, pdsp::bands::kBands[(size_t) b].name, { s.bounds.getX() + 16, s.bounds.getY() + 10, 90, 14 }, juce::Justification::left,
                       heard ? colours::amber : colours::dim, 10.5f);
        hw::drawLabel (g, hzName (range.first) + " - " + hzName (range.second), { s.bounds.getRight() - 116, s.bounds.getY() + 10, 100, 14 },
                       juce::Justification::right, colours::dim, 8.5f);

        // The reduction meter: a bar that grows from the right, and the number.
        const auto m = s.meter.toFloat();
        hw::drawGlass (g, m, 3.0f, colours::amber);
        const float share = juce::jlimit (0.0f, 1.0f, s.shownReduction / gui::kMeterRangeDb);
        g.setColour (colours::amber.withAlpha (0.55f));
        g.fillRect (m.reduced (3.0f).withTrimmedLeft ((m.getWidth() - 6.0f) * (1.0f - share)));
        g.setFont (ui::lcdFont (11.5f));
        g.setColour (colours::amber);
        juce::String text = s.shownReduction < 0.05f ? juce::String ("0.0 dB") : "-" + juce::String (s.shownReduction, 1) + " dB";
        if ((delta & (1u << b)) != 0u)
            text = "DELTA  " + text;
        else if (const float matched = proc.engine.matchMeter[(size_t) b].load(); std::fabs (matched) > 0.05f)
            text << "   M " << (matched > 0.0f ? "+" : "") << juce::String (matched, 1);
        g.drawText (text, s.meter.reduced (8, 0), juce::Justification::centredLeft);
        hw::drawGlassOverlay (g, m, 3.0f, false);
    }
}

void AtlasEditor::timerCallback()
{
    const unsigned solo = proc.engine.monitor.solo.load(), delta = proc.engine.monitor.delta.load();
    for (int b = 0; b < kBands; ++b)
    {
        auto& s = *strips[(size_t) b];
        s.solo.setToggleState ((solo & (1u << b)) != 0u, juce::dontSendNotification);
        s.delta.setToggleState ((delta & (1u << b)) != 0u, juce::dontSendNotification);
        // Up at once, down slowly, so a short reduction can be read.
        const float now = proc.engine.reductionMeter[(size_t) b].load();
        s.shownReduction = std::max (now, s.shownReduction - gui::kMeterRangeDb / (float) kTimerHz);
    }
    inLevel = proc.engine.inPeak.exchange (0.0f);
    proc.engine.outPeak.store (0.0f);
    repaint();
}
