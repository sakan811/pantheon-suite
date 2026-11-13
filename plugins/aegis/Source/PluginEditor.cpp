// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 The Pantheon authors

#include "PluginEditor.h"

using namespace aegis;
namespace hw = ui::hw;

namespace
{
constexpr int headerHeight = 84;
constexpr int displayHeight = 190;
constexpr int controlsHeight = 118;
constexpr int kTimerHz = 30;
constexpr int kWidth = 700;
}

AegisEditor::AegisEditor (AegisProcessor& p)
    : AudioProcessorEditor (&p),
      gainKnob (p.apvts, id::gain, "GAIN", "How much the sound is raised before the ceiling. The reduction is what it takes to keep every peak under CEILING.", ui::Knob::Layout::below),
      ceilingKnob (p.apvts, id::ceiling, "CEILING", "The most the output reaches, in dBTP (true peak, the peaks between the samples included). -1 dBTP is the usual for a master.", ui::Knob::Layout::below),
      releaseKnob (p.apvts, id::release, "RELEASE", "How fast the gain comes back after a peak (63% of the way in this time). Short is louder and can distort bass; long is smoother.", ui::Knob::Layout::below),
      lookaheadBox (p.apvts, id::lookahead, "How long before a peak the limiter starts turning the sound down. Longer is smoother, shorter is a smaller delay. The sound is delayed by "
                                           "this plus a fraction of a millisecond (the host compensates). Changing it while playing restarts the limiter."),
      truePeakButton (p.apvts, id::truePeak, "TRUE PEAK", "Read the peaks between the samples too (8x), as a converter or a codec will: the ceiling is then in dBTP. Off, only the samples are held."),
      presetBox (p, presets(), "Everything is set by a preset, GAIN too (set it by ear afterwards); what it does not list goes back to its default."),
      proc (p)
{
    setLookAndFeel (&look);
    for (auto* c : std::initializer_list<juce::Component*> { &gainKnob, &ceilingKnob, &releaseKnob, &lookaheadBox, &truePeakButton, &presetBox, &guideButton })
        addAndMakeVisible (*c);
    startTimerHz (kTimerHz);
    setSize (kWidth, headerHeight + 10 + displayHeight + 10 + controlsHeight + 8 + ui::kNoteStripHeight + 12);
}

AegisEditor::~AegisEditor() { setLookAndFeel (nullptr); }

void AegisEditor::resized()
{
    auto bounds = getLocalBounds().reduced (12, 0);
    auto header = bounds.removeFromTop (headerHeight).withTrimmedTop (8).withTrimmedBottom (8);
    nameplate = header.removeFromLeft (250);
    guideButton.setBounds (header.removeFromRight (64).withSizeKeepingCentre (64, 28));
    header.removeFromRight (12);
    latencyLabel = header.removeFromRight (110);
    header.removeFromLeft (24);
    auto presetRow = header.withTrimmedTop (28).withHeight (28);
    presetLabel = { presetRow.getX() + 2, presetRow.getY() - 15, 120, 12 };
    presetBox.setBounds (presetRow.removeFromLeft (190));

    bounds.removeFromTop (10);
    display = bounds.removeFromTop (displayHeight);
    bounds.removeFromTop (10);
    auto controls = bounds.removeFromTop (controlsHeight);
    const int cell = 120;
    gainKnob.setBounds (controls.removeFromLeft (cell));
    ceilingKnob.setBounds (controls.removeFromLeft (cell));
    releaseKnob.setBounds (controls.removeFromLeft (cell));
    controls.removeFromLeft (24);
    auto right = controls.withTrimmedTop (22);
    lookaheadLabel = { right.getX() + 2, right.getY() - 15, 120, 12 };
    lookaheadBox.setBounds (right.removeFromTop (28).removeFromLeft (150));
    right.removeFromTop (14);
    truePeakButton.setBounds (right.removeFromTop (28).removeFromLeft (150));
    noteStrip = bounds.withTrimmedTop (8).withHeight (ui::kNoteStripHeight);
}

float AegisEditor::heldReductionDb() const noexcept { return *std::max_element (history.begin(), history.end()); }

AdviceInput AegisEditor::adviceInput() const
{
    AdviceInput in;
    in.params = readParams (proc.apvts);
    in.reductionDb = shownReduction;
    in.heldDb = heldReductionDb();
    in.latencyMs = 1000.0 * proc.engine.latencySamples.load() / std::max (proc.getSampleRate(), 1.0);
    return in;
}

void AegisEditor::paint (juce::Graphics& g)
{
    g.fillAll (colours::background);
    hw::drawHeaderPlate (g, getLocalBounds().removeFromTop (headerHeight).toFloat());
    hw::drawNameplate (g, nameplate, gui::kProjectWord, gui::kPluginWord, "True-peak limiter", inLevelDb > gui::kLevelFloorDb, 25.0f);
    const int latency = proc.engine.latencySamples.load();
    hw::drawLabel (g, "Preset", presetLabel, juce::Justification::left, colours::dim, 9.0f);
    hw::drawLabel (g, "Delay " + juce::String (1000.0 * latency / std::max (proc.getSampleRate(), 1.0), 1) + " ms", latencyLabel, juce::Justification::centredRight, colours::dim, 9.5f);
    hw::drawLabel (g, "Look-ahead", lookaheadLabel, juce::Justification::left, colours::dim, 9.0f);
    ui::drawNoteStrip (g, noteStrip, adviceFor (adviceInput()));

    // The reduction of the last nine seconds, hanging from the top: the deeper, the lower. The scale is dB.
    const auto glass = display.toFloat();
    hw::drawGlass (g, glass, 4.0f, colours::amber);
    const auto plot = glass.reduced (46.0f, 14.0f).withTrimmedRight (74.0f);
    g.setFont (ui::lcdFont (10.0f));
    for (int d = 0; d <= (int) gui::kReductionRangeDb; d += 3)
    {
        const float y = plot.getY() + plot.getHeight() * (float) d / gui::kReductionRangeDb;
        g.setColour (colours::glassGrid);
        g.drawHorizontalLine ((int) y, plot.getX(), plot.getRight());
        g.setColour (colours::dim);
        g.drawText (d == 0 ? "0" : "-" + juce::String (d), juce::Rectangle<float> (glass.getX() + 8.0f, y - 7.0f, 30.0f, 14.0f), juce::Justification::centredRight);
    }
    juce::Path area;
    area.startNewSubPath (plot.getX(), plot.getY());
    for (int i = 0; i < kHistory; ++i)
    {
        const float x = plot.getX() + plot.getWidth() * (float) i / (float) (kHistory - 1);
        area.lineTo (x, plot.getY() + plot.getHeight() * juce::jlimit (0.0f, 1.0f, history[(size_t) i] / gui::kReductionRangeDb));
    }
    area.lineTo (plot.getRight(), plot.getY());
    area.closeSubPath();
    g.setColour (colours::amber.withAlpha (0.35f));
    g.fillPath (area);
    g.setColour (colours::amber);
    g.strokePath (area, juce::PathStrokeType (1.4f));

    // The input level, as a bar on the right of the glass, and the numbers.
    const auto bar = juce::Rectangle<float> (plot.getRight() + 36.0f, plot.getY(), 12.0f, plot.getHeight());
    g.setColour (colours::glassGrid);
    g.fillRect (bar);
    const float level = juce::jlimit (0.0f, 1.0f, (inLevelDb - gui::kLevelFloorDb) / -gui::kLevelFloorDb);
    g.setColour (inLevelDb > 0.0f ? colours::bad : colours::accent);
    g.fillRect (bar.withTrimmedTop (bar.getHeight() * (1.0f - level)));
    hw::drawLabel (g, "IN", { (int) bar.getX() - 6, (int) glass.getBottom() - 15, 24, 12 }, juce::Justification::centred, colours::dim, 8.5f);
    g.setFont (ui::lcdFont (13.0f));
    g.setColour (colours::amber);
    g.drawText (shownReduction < 0.05f ? juce::String ("0.0 dB") : "-" + juce::String (shownReduction, 1) + " dB",
                juce::Rectangle<float> (plot.getRight() + 4.0f, plot.getY(), 74.0f, 16.0f), juce::Justification::centredLeft);
    hw::drawLabel (g, "REDUCTION", { (int) plot.getRight() + 6, (int) plot.getY() + 16, 70, 12 }, juce::Justification::left, colours::dim, 8.5f);
    hw::drawGlassOverlay (g, glass, 4.0f, false);
}

void AegisEditor::timerCallback()
{
    // Up at once, down slowly, so a short reduction can be read.
    const float now = proc.engine.reductionMeter.exchange (0.0f);
    std::copy (history.begin() + 1, history.end(), history.begin());
    history.back() = now;
    const float fall = gui::kReductionRangeDb / (float) kTimerHz;
    shownReduction = now >= shownReduction ? now : std::max (now, shownReduction - fall);
    const float in = proc.engine.inPeak.exchange (0.0f);
    inLevelDb = in > 1e-5f ? 20.0f * std::log10 (in) : gui::kLevelFloorDb;
    presetBox.sync();
    repaint();
}
