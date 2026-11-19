// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 The Pantheon authors

#include "PluginEditor.h"

#include <pantheon/ui/Aspect.h>

using namespace castor;
namespace hw = ui::hw;

namespace
{
constexpr int headerHeight = 84;
constexpr int cellWidth = 104;
constexpr int cellHeight = 112;
constexpr int goniometerSize = 330;
constexpr int kWidth = 24 + 6 * cellWidth + 14 + goniometerSize + 8, kHeight = ui::editorHeightFor (kWidth); // 16:10, the kit's shape (1000 x 625)
constexpr int bodyHeight = kHeight - (headerHeight + 10 + 8 + ui::kNoteStripHeight + 12);
constexpr int widthGlassHeight = bodyHeight - (2 * cellHeight + 40) - 6; // the glass over the band knobs takes what the rest leaves
constexpr int kTimerHz = 30;
constexpr int kNoteTicks = 30 * 8; // a line of the strip (PASTE) stays eight seconds

juce::String hzName (double hz) { return hz < 1000.0 ? juce::String (juce::roundToInt (hz)) + " Hz" : juce::String (hz / 1000.0, hz < 10000.0 ? 1 : 0) + " kHz"; }
}

CastorEditor::CastorEditor (CastorProcessor& p)
    : AudioProcessorEditor (&p),
      midKnob (p.apvts, id::mid, "MID", "The level of the middle (what is the same in both channels), after the widths.", ui::Knob::Layout::below),
      sideKnob (p.apvts, id::side, "SIDE", "The level of the sides (what differs between the channels), after the widths. Above +6 dB the image can fall apart in mono.", ui::Knob::Layout::below),
      monoKnob (p.apvts, id::monoBelow, "MONO BELOW", "Takes the sides away below this frequency (a 24 dB/oct high-pass on them), so the bass is in the middle: safe for mono, club systems and vinyl. Off at 20 Hz.", ui::Knob::Layout::below),
      balanceKnob (p.apvts, id::balance, "BALANCE", "Turns one channel down (not a pan: the loudness changes).", ui::Knob::Layout::below),
      outKnob (p.apvts, id::out, "OUTPUT", "Output gain, after everything.", ui::Knob::Layout::below),
      mixKnob (p.apvts, id::mix, "MIX", "100%: the processed sound. Less: it is blended with the sound as it came in (before BALANCE and OUTPUT), so you can hear how much of the change you want. There is no delay in Castor, so the blend does not comb.", ui::Knob::Layout::below),
      listen (p.apvts, id::listen, "A check on what you hear (the goniometer and the correlation still show the sound):\nStereo: as it is.\nMono: the sum of both channels, what a mono speaker plays. What fades or changes here is a phase problem.\nSide: only what differs between the channels. Set it back to Stereo before you bounce."),
      presetBox (p, presets(), "BALANCE and OUTPUT are kept; the widths, MID, SIDE and MONO BELOW are set."),
      undoButtons (p.undo, *this),
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
    for (auto* c : std::initializer_list<juce::Component*> { &midKnob, &sideKnob, &monoKnob, &balanceKnob, &outKnob, &mixKnob, &listen, &minButton, &presetBox, &guideButton, &pasteButton })
        addAndMakeVisible (*c);
    addAndMakeVisible (undoButtons);
    pasteButton.setTooltip ("Take what Themis copied from its master plan: the side width of each band a width item names (COPY on a width item; the others stay as they are), "
                            "or MONO BELOW from the low-end mono item (COPY on it). UNDO takes it back.");
    pasteButton.onClick = [this] { pasteFromClipboard (juce::SystemClipboard::getTextFromClipboard()); };
    minButton.setTooltip ("The lowest the correlation has read since the last click (the bar's thin mark shows it too): a dip under 0 is a moment that cancels in mono. Click to start again.");
    minButton.onClick = [this] { minHold = 1.0f; proc.engine.correlationMin.store (1.0f); repaint(); };
    scratchL.resize (4096);
    scratchR.resize (4096);
    startTimerHz (kTimerHz);
    setSize (kWidth, kHeight);
}

CastorEditor::~CastorEditor() { setLookAndFeel (nullptr); }

void CastorEditor::resized()
{
    auto bounds = getLocalBounds().reduced (12, 0);
    auto header = bounds.removeFromTop (headerHeight).withTrimmedTop (8).withTrimmedBottom (8);
    nameplate = header.removeFromLeft (250);
    guideButton.setBounds (header.removeFromRight (64).withSizeKeepingCentre (64, 28));
    header.removeFromRight (8);
    pasteButton.setBounds (header.removeFromRight (64).withSizeKeepingCentre (64, 28));
    header.removeFromRight (12);
    header.removeFromLeft (24);
    auto presetRow = header.withTrimmedTop (28).withHeight (28);
    presetLabel = { presetRow.getX() + 2, presetRow.getY() - 15, 120, 12 };
    presetBox.setBounds (presetRow.removeFromLeft (190));
    // UNDO / REDO in the top strip of the header, which the label of the preset leaves free.
    const auto strip = header.withHeight (16).translated (0, -6);
    undoButtons.setBounds (strip.getX(), strip.getY(), ui::UndoButtons::kWidth, strip.getHeight());

    bounds.removeFromTop (10);
    auto body = bounds.removeFromTop (bodyHeight);
    auto right = body.removeFromRight (goniometerSize);
    body.removeFromRight (14);
    gonio = right.removeFromTop (bodyHeight - 56); // the goniometer, then 8 + the correlation 16 + 6 + the check row 26
    right.removeFromTop (8);
    correlationBar = right.removeFromTop (16);
    right.removeFromTop (6);
    auto checkRow = right.removeFromTop (26);
    listen.setBounds (checkRow.removeFromLeft (110));
    checkRow.removeFromLeft (6);
    minButton.setBounds (checkRow.removeFromLeft (110));

    bandsLabel = body.removeFromTop (16).withTrimmedLeft (4);
    widthGlass = body.removeFromTop (widthGlassHeight).withTrimmedBottom (6);
    auto row = body.removeFromTop (cellHeight);
    for (int b = 0; b < kBands; ++b)
        widths[(size_t) b]->setBounds (row.removeFromLeft (cellWidth));
    body.removeFromTop (8);
    otherLabel = body.removeFromTop (16).withTrimmedLeft (4);
    row = body.removeFromTop (cellHeight);
    for (auto* k : { &midKnob, &sideKnob, &monoKnob, &balanceKnob, &outKnob, &mixKnob })
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

// The width of the sides of each band as a bar over its knob (the knobs below set it; this shows the whole shape at once). 100% is the line: above it the band is wider than
// the recording, below it narrower, and at the bottom it is mono.
void CastorEditor::drawWidthGlass (juce::Graphics& g) const
{
    const auto glass = widthGlass.toFloat();
    hw::drawGlass (g, glass, 4.0f, colours::accent);
    const auto params = readParams (proc.apvts);
    const auto plot = glass.reduced (0.0f, 14.0f);
    const auto yOf = [&plot] (float width) { return plot.getBottom() - plot.getHeight() * juce::jlimit (0.0f, 1.0f, width / kMaxWidth); };
    g.setColour (colours::glassGrid);
    for (float mark : { 0.5f, 1.5f })
        g.drawHorizontalLine ((int) yOf (mark), glass.getX() + 4.0f, glass.getRight() - 4.0f);
    for (int b = 0; b < kBands; ++b)
    {
        const float width = params.width[(size_t) b];
        const auto column = juce::Rectangle<float> (glass.getX() + (float) (b * cellWidth), plot.getY(), (float) cellWidth, plot.getHeight());
        const auto bar = column.withSizeKeepingCentre (juce::jmin (44.0f, column.getWidth() - 24.0f), column.getHeight());
        const float top = yOf (width);
        g.setColour (colours::accent.withAlpha (0.55f));
        g.fillRoundedRectangle (bar.getX(), top, bar.getWidth(), plot.getBottom() - top, 2.0f);
        g.setColour (colours::accent);
        g.fillRect (bar.getX(), top - 1.0f, bar.getWidth(), 2.0f);
        g.setColour (colours::dim);
        g.setFont (ui::lcdFont (10.0f));
        g.drawText (juce::String (juce::roundToInt (width * 100.0f)) + "%", column.withHeight (12.0f).withY (juce::jmax (glass.getY() + 1.0f, top - 15.0f)), juce::Justification::centred);
    }
    g.setColour (colours::amber.withAlpha (0.8f)); // 100%: the sides as recorded
    g.drawHorizontalLine ((int) yOf (1.0f), glass.getX() + 4.0f, glass.getRight() - 4.0f);
    hw::drawGlassOverlay (g, glass, 4.0f, false);
}

void CastorEditor::paint (juce::Graphics& g)
{
    g.fillAll (colours::background);
    hw::drawHeaderPlate (g, getLocalBounds().removeFromTop (headerHeight).toFloat());
    hw::drawNameplate (g, nameplate, gui::kProjectWord, gui::kPluginWord, "Stereo imager", live, 25.0f);
    hw::drawLabel (g, "Preset", presetLabel, juce::Justification::left, colours::dim, 9.0f);
    hw::drawLabel (g, "Width of the sides, by band", bandsLabel, juce::Justification::left, colours::dim, 9.0f);
    hw::drawLabel (g, "Whole sound", otherLabel, juce::Justification::left, colours::dim, 9.0f);
    ui::drawNoteStrip (g, noteStrip, shownNotes());

    drawWidthGlass (g);

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
    if (minHold < 1.0f)
    {
        const float mx = bar.getX() + 6.0f + (bar.getWidth() - 12.0f) * (minHold + 1.0f) * 0.5f;
        g.setColour (minHold < kNegativeCorrelation ? colours::bad : colours::dim);
        g.fillRect (mx - 0.75f, bar.getY() + 3.0f, 1.5f, bar.getHeight() - 6.0f);
    }
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
    minHold = std::min (minHold, proc.engine.correlationMin.exchange (1.0f, std::memory_order_relaxed));
    minButton.setButtonText (minHold >= 1.0f ? juce::String ("MIN --") : "MIN " + juce::String (minHold, 2));
    presetBox.sync();
    if (stripTicks > 0)
        --stripTicks;
    repaint();
}

std::vector<ui::Note> CastorEditor::shownNotes() const
{
    auto list = castor::adviceFor (adviceInput());
    if (stripTicks > 0 && ! stripSaid.empty())
        list.insert (list.begin(), { ui::Severity::info, stripSaid });
    return list;
}

void CastorEditor::pasteFromClipboard (const juce::String& text)
{
    setStripNote (pasteFromThemis (proc.apvts, text), kNoteTicks); // one UNDO step for what the text sets (Parameters.h)
    repaint();
}

void CastorEditor::setStripNote (std::string text, int ticks)
{
    stripSaid = std::move (text);
    stripTicks = ticks;
    repaint();
}
