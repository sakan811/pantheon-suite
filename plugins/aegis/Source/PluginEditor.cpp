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
constexpr int kWidth = 780;
constexpr float kReadoutWidth = 150.0f; // the right side of the glass: numbers, then the IN and OUT bars (paint() and the CLEAR button's place in resized())
constexpr int kPasteTicks = 30 * 8; // eight seconds
}

AegisEditor::AegisEditor (AegisProcessor& p)
    : AudioProcessorEditor (&p),
      gainKnob (p.apvts, id::gain, "GAIN", "How much the sound is raised before the ceiling. The reduction is what it takes to keep every peak under CEILING.", ui::Knob::Layout::below),
      ceilingKnob (p.apvts, id::ceiling, "CEILING", "The most the output reaches, in dBTP (true peak, the peaks between the samples included). -1 dBTP is the usual for a master.", ui::Knob::Layout::below),
      releaseKnob (p.apvts, id::release, "RELEASE", "How fast the gain comes back after a peak (63% of the way in this time). Short is louder and can distort bass; long is smoother.", ui::Knob::Layout::below),
      lookaheadBox (p.apvts, id::lookahead, "How long before a peak the limiter starts turning the sound down. Longer is smoother, shorter is a smaller delay. The sound is delayed by "
                                           "this plus a fraction of a millisecond (the host compensates). Changing it while playing restarts the limiter."),
      truePeakButton (p.apvts, id::truePeak, "TRUE PEAK", "Read the peaks between the samples too (8x), as a converter or a codec will: the ceiling is then in dBTP. Off, only the samples are held."),
      autoReleaseButton (p.apvts, id::autoRelease, "AUTO RELEASE", "A short peak is let go faster than RELEASE; a reduction that lasts is let go at RELEASE. Smoother on music with transients, and RELEASE is then the slow end. Off, the gain always comes back at RELEASE."),
      deltaButton (p.apvts, id::delta, "DELTA", "Listen to what the limiter takes away: the sound after GAIN minus the output, aligned by the delay. If it is the sound itself (not just the peaks), the limiter is working too hard. Not for the bounce: switch it off. Presets switch it off."),
      matchButton (p.apvts, id::matchLevel, "MATCH", "Turns the output down to the loudness of the input (read over about a second), so raising GAIN does not win the comparison just by being louder. It only ever turns down, so the ceiling holds. For comparing: switch it off before the bounce. Presets switch it off."),
      presetBox (p, presets(), "Everything is set by a preset, GAIN too (set it by ear afterwards); what it does not list goes back to its default."),
      proc (p)
{
    setLookAndFeel (&look);
    for (auto* c : std::initializer_list<juce::Component*> { &gainKnob, &ceilingKnob, &releaseKnob, &lookaheadBox, &truePeakButton, &autoReleaseButton, &deltaButton, &matchButton, &presetBox, &guideButton, &pasteButton, &clearOutButton })
        addAndMakeVisible (*c);
    pasteButton.setTooltip ("Take the limiter setting that Themis copied from its master plan (COPY on the ceiling or the loudness item): the CEILING it names and how far to move GAIN. "
                            "A ceiling in dBTP needs TRUE PEAK, so a paste that sets one switches it on.");
    pasteButton.onClick = [this] { pasteFromClipboard (juce::SystemClipboard::getTextFromClipboard()); };
    clearOutButton.setTooltip ("Clear the OUT PEAK hold and the MAX REDUCTION hold (clicking the MAX REDUCTION number clears that one alone). They also clear when CEILING or TRUE PEAK changes, because the old peaks were read against another limit.");
    clearOutButton.onClick = [this] { outHold = gui::kLevelFloorDb; reductionHold = 0.0f; };
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
    header.removeFromRight (8);
    pasteButton.setBounds (header.removeFromRight (64).withSizeKeepingCentre (64, 28));
    header.removeFromRight (12);
    latencyLabel = header.removeFromRight (110);
    header.removeFromLeft (24);
    auto presetRow = header.withTrimmedTop (28).withHeight (28);
    presetLabel = { presetRow.getX() + 2, presetRow.getY() - 15, 120, 12 };
    presetBox.setBounds (presetRow.removeFromLeft (190));

    bounds.removeFromTop (10);
    display = bounds.removeFromTop (displayHeight);
    {
        const auto plot = display.toFloat().reduced (46.0f, 14.0f).withTrimmedRight (kReadoutWidth);
        holdArea = { (int) plot.getRight() + 10, (int) plot.getY() + 34, 104, 30 }; // the MAX REDUCTION number (paint() draws it at the same place)
    }
    clearOutButton.setBounds ((int) (display.toFloat().reduced (46.0f, 14.0f).withTrimmedRight (kReadoutWidth).getRight()) + 10, display.getY() + 14 + 116, 56, 20);
    bounds.removeFromTop (10);
    auto controls = bounds.removeFromTop (controlsHeight);
    const int cell = 120;
    gainKnob.setBounds (controls.removeFromLeft (cell));
    ceilingKnob.setBounds (controls.removeFromLeft (cell));
    releaseKnob.setBounds (controls.removeFromLeft (cell));
    controls.removeFromLeft (24);
    auto right = controls.withTrimmedTop (22);
    lookaheadLabel = { right.getX() + 2, right.getY() - 15, 120, 12 };
    auto lookRow = right.removeFromTop (28);
    lookaheadBox.setBounds (lookRow.removeFromLeft (150));
    lookRow.removeFromLeft (12);
    deltaButton.setBounds (lookRow.removeFromLeft (84));
    lookRow.removeFromLeft (6);
    matchButton.setBounds (lookRow.removeFromLeft (84));
    right.removeFromTop (14);
    auto toggles = right.removeFromTop (28);
    truePeakButton.setBounds (toggles.removeFromLeft (150));
    toggles.removeFromLeft (12);
    autoReleaseButton.setBounds (toggles.removeFromLeft (150));
    noteStrip = bounds.withTrimmedTop (8).withHeight (ui::kNoteStripHeight);
}

void AegisEditor::mouseDown (const juce::MouseEvent& e)
{
    if (holdArea.contains (e.getPosition()))
    {
        reductionHold = 0.0f;
        repaint (holdArea);
    }
}

float AegisEditor::heldReductionDb() const noexcept { return *std::max_element (history.begin(), history.end()); }

std::vector<ui::Note> AegisEditor::shownNotes() const
{
    auto list = aegis::adviceFor (adviceInput());
    if (pasteTicks > 0 && ! pasteSaid.empty())
        list.insert (list.begin(), { ui::Severity::info, pasteSaid });
    return list;
}

void AegisEditor::pasteFromClipboard (const juce::String& text)
{
    auto said = pasteLimiter (proc.apvts, text);
    if (said.empty())
        said = "Nothing to paste: copy a limiter setting from Themis' master plan first (COPY on the ceiling or the loudness item).";
    pasteSaid = said;
    pasteTicks = kPasteTicks;
    repaint();
}

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
    ui::drawNoteStrip (g, noteStrip, shownNotes());

    // The reduction of the last nine seconds, hanging from the top: the deeper, the lower. The scale is dB.
    const auto glass = display.toFloat();
    hw::drawGlass (g, glass, 4.0f, colours::amber);
    const auto plot = glass.reduced (46.0f, 14.0f).withTrimmedRight (kReadoutWidth);
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

    // The numbers (the reduction now, the highest output peak) and, beside them, the input and output levels as bars. OUT is the true peak of what leaves, read
    // as the detector reads it: with TRUE PEAK on it cannot be over CEILING, and a bar at the ceiling is where the limiter stops.
    const float x0 = plot.getRight() + 10.0f;
    g.setFont (ui::lcdFont (13.0f));
    g.setColour (colours::amber);
    g.drawText (shownReduction < 0.05f ? juce::String ("0.0 dB") : "-" + juce::String (shownReduction, 1) + " dB", juce::Rectangle<float> (x0, plot.getY(), 96.0f, 16.0f), juce::Justification::centredLeft);
    hw::drawLabel (g, "REDUCTION", { (int) x0 + 2, (int) plot.getY() + 16, 90, 12 }, juce::Justification::left, colours::dim, 8.5f);
    g.setColour (colours::amber);
    g.drawText (reductionHold < 0.05f ? juce::String ("0.0 dB") : "-" + juce::String (reductionHold, 1) + " dB", juce::Rectangle<float> (x0, plot.getY() + 34.0f, 96.0f, 16.0f), juce::Justification::centredLeft);
    hw::drawLabel (g, "MAX (CLICK TO CLEAR)", { (int) x0 + 2, (int) plot.getY() + 50, 104, 12 }, juce::Justification::left, colours::dim, 8.5f);
    const bool over = outHold > shownCeilingDb + 0.1f;
    g.setColour (over ? colours::bad : colours::amber);
    g.drawText (outHold <= gui::kLevelFloorDb ? juce::String ("--") : juce::String (outHold, 1) + (shownTruePeak ? " dBTP" : " dBFS"), juce::Rectangle<float> (x0, plot.getY() + 76.0f, 104.0f, 16.0f), juce::Justification::centredLeft);
    hw::drawLabel (g, "OUT PEAK", { (int) x0 + 2, (int) plot.getY() + 92, 90, 12 }, juce::Justification::left, colours::dim, 8.5f);

    const auto bar = [&] (float x) { return juce::Rectangle<float> (x, plot.getY(), 12.0f, plot.getHeight()); };
    const auto level = [] (float db) { return juce::jlimit (0.0f, 1.0f, (db - gui::kLevelFloorDb) / -gui::kLevelFloorDb); };
    const auto inBar = bar (x0 + 108.0f), outBar = bar (x0 + 134.0f);
    for (const auto& b : { inBar, outBar })
    {
        g.setColour (colours::glassGrid);
        g.fillRect (b);
    }
    g.setColour (inLevelDb > 0.0f ? colours::bad : colours::accent);
    g.fillRect (inBar.withTrimmedTop (inBar.getHeight() * (1.0f - level (inLevelDb))));
    g.setColour (over ? colours::bad : colours::accent);
    g.fillRect (outBar.withTrimmedTop (outBar.getHeight() * (1.0f - level (outLevelDb))));
    const float ceilingY = outBar.getY() + outBar.getHeight() * (1.0f - level (shownCeilingDb));
    g.setColour (colours::amber);
    g.drawLine (outBar.getX() - 3.0f, ceilingY, outBar.getRight() + 3.0f, ceilingY, 1.2f);
    hw::drawLabel (g, "IN", { (int) inBar.getX() - 6, (int) glass.getBottom() - 15, 24, 12 }, juce::Justification::centred, colours::dim, 8.5f);
    hw::drawLabel (g, "OUT", { (int) outBar.getX() - 8, (int) glass.getBottom() - 15, 28, 12 }, juce::Justification::centred, colours::dim, 8.5f);
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
    const float out = proc.engine.outPeak.exchange (0.0f);
    outLevelDb = out > 1e-5f ? 20.0f * std::log10 (out) : gui::kLevelFloorDb;
    const auto params = readParams (proc.apvts);
    if (params.ceilingDb != shownCeilingDb || params.truePeak != shownTruePeak)
    {
        outHold = gui::kLevelFloorDb; // the old peaks were read against another limit
        reductionHold = 0.0f;
    }
    shownCeilingDb = params.ceilingDb;
    shownTruePeak = params.truePeak;
    outHold = std::max (outHold, outLevelDb);
    reductionHold = std::max (reductionHold, now);
    if (pasteTicks > 0)
        --pasteTicks;
    presetBox.sync();
    repaint();
}
