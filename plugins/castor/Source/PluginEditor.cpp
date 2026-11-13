// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 The Pantheon authors

#include "PluginEditor.h"

using namespace castor;
namespace hw = ui::hw;

namespace
{
constexpr int headerHeight = 84;
constexpr int cellWidth = 104;
constexpr int cellHeight = 112;
constexpr int goniometerSize = 232;
constexpr int kTimerHz = 30;

juce::String hzName (double hz) { return hz < 1000.0 ? juce::String (juce::roundToInt (hz)) + " Hz" : juce::String (hz / 1000.0, hz < 10000.0 ? 1 : 0) + " kHz"; }
}

CastorEditor::CastorEditor (CastorProcessor& p)
    : AudioProcessorEditor (&p),
      midKnob (p.apvts, id::mid, "MID", "The level of the middle (what is the same in both channels), after the widths.", ui::Knob::Layout::below),
      sideKnob (p.apvts, id::side, "SIDE", "The level of the sides (what differs between the channels), after the widths. Above +6 dB the image can fall apart in mono.", ui::Knob::Layout::below),
      monoKnob (p.apvts, id::monoBelow, "MONO BELOW", "Takes the sides away below this frequency (a 24 dB/oct high-pass on them), so the bass is in the middle: safe for mono, club systems and vinyl. Off at 20 Hz.", ui::Knob::Layout::below),
      balanceKnob (p.apvts, id::balance, "BALANCE", "Turns one channel down (not a pan: the loudness changes).", ui::Knob::Layout::below),
      outKnob (p.apvts, id::out, "OUTPUT", "Output gain, after everything.", ui::Knob::Layout::below),
      presetBox (p, presets(), "BALANCE and OUTPUT are kept; the widths, MID, SIDE and MONO BELOW are set."),
      proc (p)
{
    setLookAndFeel (&look);
    for (int b = 0; b < kBands; ++b)
    {
        const auto& band = pdsp::bands::kBands[(size_t) b];
        widths[(size_t) b] = std::make_unique<ui::Knob> (p.apvts, id::width (b), juce::String (band.name).toUpperCase(),
                                                         juce::String (band.title) + ", " + hzName (band.loHz) + " to " + hzName (band.hiHz) + ": how wide its sides are. 0% is mono, 100% as it is, 200% twice the sides.",
                                                         ui::Knob::Layout::below);
        addAndMakeVisible (*widths[(size_t) b]);
    }
    for (auto* c : std::initializer_list<juce::Component*> { &midKnob, &sideKnob, &monoKnob, &balanceKnob, &outKnob, &presetBox, &guideButton })
        addAndMakeVisible (*c);
    scratchL.resize (4096);
    scratchR.resize (4096);
    startTimerHz (kTimerHz);
    setSize (24 + 6 * cellWidth + 14 + goniometerSize + 8, headerHeight + 10 + 2 * cellHeight + 40 + 8 + ui::kNoteStripHeight + 12);
}

CastorEditor::~CastorEditor() { setLookAndFeel (nullptr); }

void CastorEditor::resized()
{
    auto bounds = getLocalBounds().reduced (12, 0);
    auto header = bounds.removeFromTop (headerHeight).withTrimmedTop (8).withTrimmedBottom (8);
    nameplate = header.removeFromLeft (250);
    guideButton.setBounds (header.removeFromRight (64).withSizeKeepingCentre (64, 28));
    header.removeFromLeft (24);
    auto presetRow = header.withTrimmedTop (28).withHeight (28);
    presetLabel = { presetRow.getX() + 2, presetRow.getY() - 15, 120, 12 };
    presetBox.setBounds (presetRow.removeFromLeft (190));

    bounds.removeFromTop (10);
    auto body = bounds.removeFromTop (2 * cellHeight + 40);
    auto right = body.removeFromRight (goniometerSize);
    body.removeFromRight (14);
    gonio = right.removeFromTop (goniometerSize - 24);
    right.removeFromTop (8);
    correlationBar = right.removeFromTop (16);

    bandsLabel = body.removeFromTop (16).withTrimmedLeft (4);
    auto row = body.removeFromTop (cellHeight);
    for (int b = 0; b < kBands; ++b)
        widths[(size_t) b]->setBounds (row.removeFromLeft (cellWidth));
    body.removeFromTop (8);
    otherLabel = body.removeFromTop (16).withTrimmedLeft (4);
    row = body.removeFromTop (cellHeight);
    for (auto* k : { &midKnob, &sideKnob, &monoKnob, &balanceKnob, &outKnob })
        k->setBounds (row.removeFromLeft (cellWidth));
    noteStrip = bounds.withTrimmedTop (8).withHeight (ui::kNoteStripHeight);
}

AdviceInput CastorEditor::adviceInput() const
{
    AdviceInput in;
    in.params = readParams (proc.apvts);
    in.correlation = correlation;
    return in;
}

void CastorEditor::paint (juce::Graphics& g)
{
    g.fillAll (colours::background);
    hw::drawHeaderPlate (g, getLocalBounds().removeFromTop (headerHeight).toFloat());
    hw::drawNameplate (g, nameplate, gui::kProjectWord, gui::kPluginWord, "Stereo imager", live, 25.0f);
    hw::drawLabel (g, "Preset", presetLabel, juce::Justification::left, colours::dim, 9.0f);
    hw::drawLabel (g, "Width of the sides, by band", bandsLabel, juce::Justification::left, colours::dim, 9.0f);
    hw::drawLabel (g, "Whole sound", otherLabel, juce::Justification::left, colours::dim, 9.0f);
    ui::drawNoteStrip (g, noteStrip, adviceFor (adviceInput()));

    // The goniometer: left and right as a cloud turned a quarter: the middle is the vertical, the sides the horizontal.
    const auto glass = gonio.toFloat();
    hw::drawGlass (g, glass, 4.0f, colours::accent);
    const auto c = glass.getCentre();
    const float radius = std::min (glass.getWidth(), glass.getHeight()) * 0.5f - 22.0f;
    g.setColour (colours::glassGrid);
    g.drawEllipse (c.x - radius, c.y - radius, 2.0f * radius, 2.0f * radius, 1.0f);
    g.drawLine (c.x - radius, c.y, c.x + radius, c.y);
    g.drawLine (c.x, c.y - radius, c.x, c.y + radius);
    g.drawLine (c.x - radius * 0.7071f, c.y + radius * 0.7071f, c.x + radius * 0.7071f, c.y - radius * 0.7071f);
    g.drawLine (c.x - radius * 0.7071f, c.y - radius * 0.7071f, c.x + radius * 0.7071f, c.y + radius * 0.7071f);
    g.setColour (colours::dim);
    g.setFont (ui::lcdFont (10.0f));
    g.drawText ("M", juce::Rectangle<float> (c.x - 10.0f, c.y - radius - 16.0f, 20.0f, 14.0f), juce::Justification::centred);
    g.drawText ("S", juce::Rectangle<float> (c.x + radius + 4.0f, c.y - 7.0f, 16.0f, 14.0f), juce::Justification::centred);
    g.drawText ("L", juce::Rectangle<float> (c.x - radius * 0.7071f - 16.0f, c.y - radius * 0.7071f - 14.0f, 16.0f, 14.0f), juce::Justification::centred);
    g.drawText ("R", juce::Rectangle<float> (c.x + radius * 0.7071f, c.y - radius * 0.7071f - 14.0f, 16.0f, 14.0f), juce::Justification::centred);
    g.setColour (colours::accent.withAlpha (0.5f));
    for (int i = 0; i < kPoints; ++i)
    {
        const float l = pointL[(size_t) i], r = pointR[(size_t) i];
        const float x = c.x + radius * juce::jlimit (-1.2f, 1.2f, (r - l) * 0.7071f * 1.4f), y = c.y - radius * juce::jlimit (-1.2f, 1.2f, (l + r) * 0.7071f * 1.4f);
        g.fillRect (x, y, 1.6f, 1.6f);
    }
    hw::drawGlassOverlay (g, glass, 4.0f, false);

    // The correlation: -1 out of phase, 0 wide, +1 mono.
    const auto bar = correlationBar.toFloat();
    hw::drawGlass (g, bar, 3.0f, colours::amber);
    const float x = bar.getX() + 6.0f + (bar.getWidth() - 12.0f) * (correlation + 1.0f) * 0.5f;
    g.setColour (correlation < kNegativeCorrelation ? colours::bad : colours::amber);
    g.fillRect (x - 2.0f, bar.getY() + 3.0f, 4.0f, bar.getHeight() - 6.0f);
    g.setColour (colours::dim);
    g.setFont (ui::lcdFont (9.0f));
    g.drawText ("-1", bar.withTrimmedLeft (6.0f), juce::Justification::centredLeft);
    g.drawText ("+1", bar.withTrimmedRight (6.0f), juce::Justification::centredRight);
    g.drawText ("0", bar, juce::Justification::centred);
}

void CastorEditor::timerCallback()
{
    // The newest samples of the output, left and right, to the cloud.
    const int l = proc.tapOut.channel[0].pull (scratchL.data(), (int) scratchL.size());
    const int r = proc.tapOut.channel[1].pull (scratchR.data(), (int) scratchR.size());
    const int n = std::min (l, r);
    if (n > 0)
    {
        const int keep = std::min (n, kPoints);
        std::copy (pointL.begin() + keep, pointL.end(), pointL.begin());
        std::copy (pointR.begin() + keep, pointR.end(), pointR.begin());
        std::copy (scratchL.begin() + (n - keep), scratchL.begin() + n, pointL.end() - keep);
        std::copy (scratchR.begin() + (n - keep), scratchR.begin() + n, pointR.end() - keep);
    }
    live = n > 0;
    correlation += 0.3f * (proc.engine.correlation.load (std::memory_order_relaxed) - correlation);
    presetBox.sync();
    repaint();
}
