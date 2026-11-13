// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 The Pantheon authors

#include "PluginEditor.h"

using namespace hermes;
namespace hw = ui::hw;

namespace
{
constexpr int headerHeight = 84;
constexpr int displayHeight = 230;
constexpr int cellWidth = 112;
constexpr int cellHeight = 108;
constexpr int kTimerHz = 10;
constexpr int kWidth = 900;

juce::String readout (float v, const char* unit)
{
    return v < gui::kNothing ? juce::String ("--") : (v > 0.0f ? "+" : "") + juce::String (v, 1) + " " + unit;
}
}

HermesEditor::HermesEditor (HermesProcessor& p)
    : AudioProcessorEditor (&p),
      trim (p.apvts, id::trim, "TRIM", "The level of your sound: the meters read it after the trim, and so does everything downstream.", ui::Knob::Layout::below),
      referenceTrim (p.apvts, id::referenceTrim, "REF TRIM", "Adds to the reference's level (after LEVEL MATCH): when the reference should be a little louder or quieter.", ui::Knob::Layout::below),
      listen (p.apvts, id::listen,
              "A check on what you hear (the meters still read the whole sound):\nStereo: as it is.\nMono: the sum of both channels (what a mono speaker plays).\nLeft / Right: one channel in both ears.\n"
              "Side: only what differs between the channels.\nSwap: left and right exchanged."),
      polarity (p.apvts, id::polarity, "POLARITY", "Inverts both channels. It sounds the same alone and different against other tracks."),
      reference (p.apvts, id::reference, "REF (B)", "Hear the reference instead of your sound (A). The switch is a short crossfade, and the reference plays on while you listen to A."),
      levelMatch (p.apvts, id::levelMatch, "LEVEL MATCH", "Brings the reference to the integrated loudness of your sound, so you compare the tone and not the level. Let the meter run first: until it has read a few seconds there is nothing to match."),
      presetBox (p, presets(), "LISTEN and the reference are set; TRIM, POLARITY and REF TRIM are kept."),
      proc (p)
{
    setLookAndFeel (&look);
    momentaryHistory.fill (gui::kNothing - 1.0f);
    shortTermHistory.fill (gui::kNothing - 1.0f);
    loadButton.setTooltip ("Choose a track to compare with (WAV, AIFF, FLAC, OGG or MP3 where the system can read it; at most 10 minutes). It is kept in the session.");
    restartButton.setTooltip ("Takes the reference back to its start. It loops, and plays on whether it is heard or not.");
    resetButton.setTooltip ("Clears the integrated loudness, the loudness range and the peaks, and starts again.");
    loadButton.onClick = [this]
    {
        chooser = std::make_unique<juce::FileChooser> ("Choose a reference track", juce::File(), "*.wav;*.aif;*.aiff;*.flac;*.ogg;*.mp3");
        chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles, [this] (const juce::FileChooser& c)
        {
            if (c.getResult().existsAsFile())
                loadFile (c.getResult());
        });
    };
    restartButton.onClick = [this] { proc.engine.restartReference(); };
    resetButton.onClick = [this] { proc.engine.resetMeters(); };
    for (auto* c : std::initializer_list<juce::Component*> { &trim, &referenceTrim, &listen, &polarity, &reference, &levelMatch, &loadButton, &restartButton, &resetButton, &presetBox, &guideButton })
        addAndMakeVisible (*c);
    startTimerHz (kTimerHz);
    setSize (kWidth, headerHeight + 10 + displayHeight + 8 + cellHeight + 8 + ui::kNoteStripHeight + 12);
}

HermesEditor::~HermesEditor() { setLookAndFeel (nullptr); }

void HermesEditor::loadFile (const juce::File& file)
{
    status = proc.loadReference (file);
    repaint();
}

void HermesEditor::resized()
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
    auto display = bounds.removeFromTop (displayHeight);
    readouts = display.removeFromLeft (330);
    display.removeFromLeft (8);
    graph = display;
    bounds.removeFromTop (8);
    auto row = bounds.removeFromTop (cellHeight);
    trim.setBounds (row.removeFromLeft (cellWidth));
    referenceTrim.setBounds (row.removeFromLeft (cellWidth));
    row.removeFromLeft (14);
    auto column = row.removeFromLeft (170).withTrimmedTop (4);
    listenLabel = { column.getX() + 2, column.getY(), 100, 10 };
    column.removeFromTop (12);
    listen.setBounds (column.removeFromTop (26));
    column.removeFromTop (6);
    auto two = column.removeFromTop (26);
    polarity.setBounds (two.removeFromLeft (80));
    two.removeFromLeft (6);
    resetButton.setBounds (two.removeFromLeft (80));
    row.removeFromLeft (14);
    referenceArea = row.withTrimmedTop (4);
    auto ref = referenceArea;
    auto top = ref.removeFromTop (26);
    loadButton.setBounds (top.removeFromLeft (80));
    top.removeFromLeft (6);
    restartButton.setBounds (top.removeFromLeft (92));
    ref.removeFromTop (30);
    auto bottom = ref.removeFromTop (26);
    reference.setBounds (bottom.removeFromLeft (110));
    bottom.removeFromLeft (6);
    levelMatch.setBounds (bottom.removeFromLeft (130));
    noteStrip = bounds.withTrimmedTop (8).withHeight (ui::kNoteStripHeight);
}

AdviceInput HermesEditor::adviceInput() const
{
    AdviceInput in;
    in.params = readParams (proc.apvts);
    in.referenceLoaded = proc.referenceName().isNotEmpty();
    in.integratedLufs = integrated;
    in.truePeakDb = truePeak;
    in.rangeLu = range;
    return in;
}

void HermesEditor::paint (juce::Graphics& g)
{
    g.fillAll (colours::background);
    hw::drawHeaderPlate (g, getLocalBounds().removeFromTop (headerHeight).toFloat());
    hw::drawNameplate (g, nameplate, gui::kProjectWord, gui::kPluginWord, "Gain staging and loudness", momentary > gui::kNothing, 25.0f);
    hw::drawLabel (g, "Preset", presetLabel, juce::Justification::left, colours::dim, 9.0f);
    hw::drawLabel (g, "Listen", listenLabel, juce::Justification::left, colours::dim, 9.0f);
    ui::drawNoteStrip (g, noteStrip, adviceFor (adviceInput()));

    // The reference: its name, how long it has played, what it was brought by.
    {
        const auto name = proc.referenceName();
        auto area = referenceArea.withTrimmedTop (28).withTrimmedBottom (34).withTrimmedLeft (0);
        area = { referenceArea.getX(), referenceArea.getY() + 28, referenceArea.getWidth(), 14 };
        hw::drawLabel (g, name.isEmpty() ? juce::String (status.isNotEmpty() ? status : "No reference loaded") : name + "   " + juce::String (proc.referenceLufs(), 1) + " LUFS",
                       area, juce::Justification::left, status.isNotEmpty() && name.isEmpty() ? colours::warn : colours::dim, 9.5f);
        if (name.isNotEmpty())
        {
            const auto bar = juce::Rectangle<float> ((float) referenceArea.getX(), (float) referenceArea.getBottom() - 12.0f, (float) referenceArea.getWidth() - 20.0f, 5.0f);
            g.setColour (colours::glassGrid);
            g.fillRect (bar);
            g.setColour (colours::accent);
            g.fillRect (bar.withWidth (bar.getWidth() * position));
        }
    }

    // The readings.
    {
        const auto glass = readouts.toFloat();
        hw::drawGlass (g, glass, 4.0f, colours::amber);
        struct Cell { const char* name; juce::String value; bool alarm; };
        const Cell cells[] = { { "MOMENTARY", readout (momentary, "LUFS"), false }, { "SHORT-TERM", readout (shortTerm, "LUFS"), false }, { "INTEGRATED", readout (integrated, "LUFS"), false },
                               { "RANGE", range <= 0.0f ? juce::String ("--") : juce::String (range, 1) + " LU", false }, { "TRUE PEAK", readout (truePeak, "dBTP"), truePeak > gui::kNothing && truePeak > -1.0f },
                               { "SAMPLE PEAK", readout (samplePeak, "dBFS"), samplePeak > gui::kNothing && samplePeak >= 0.0f } };
        const auto inner = glass.reduced (12.0f, 10.0f);
        const float cw = inner.getWidth() / 2.0f, ch = inner.getHeight() / 3.0f;
        for (int i = 0; i < 6; ++i)
        {
            const auto c = juce::Rectangle<float> (inner.getX() + cw * (float) (i % 2), inner.getY() + ch * (float) (i / 2), cw, ch);
            hw::drawLabel (g, cells[i].name, c.toNearestInt().withHeight (12).translated (4, 2), juce::Justification::left, colours::dim, 8.5f);
            g.setFont (ui::lcdFont (i < 3 ? 19.0f : 16.0f));
            g.setColour (cells[i].alarm ? colours::bad : colours::amber);
            g.drawText (cells[i].value, c.withTrimmedTop (16.0f).reduced (4.0f, 0.0f), juce::Justification::centredLeft);
        }
        hw::drawGlassOverlay (g, glass, 4.0f, false);
    }

    // The last minute of loudness: the momentary (dim), the short-term (amber), the integrated (a line); -14 and -23 marked.
    {
        const auto glass = graph.toFloat();
        hw::drawGlass (g, glass, 4.0f, colours::accent);
        const auto r = glass.reduced (44.0f, 14.0f).withTrimmedBottom (8.0f);
        const auto yOf = [&] (float lufs) { return r.getY() + r.getHeight() * (gui::kGraphTopLufs - juce::jlimit (gui::kGraphBottomLufs, gui::kGraphTopLufs, lufs)) / (gui::kGraphTopLufs - gui::kGraphBottomLufs); };
        g.setFont (ui::lcdFont (9.5f));
        for (int l = -10; l >= -50; l -= 10)
        {
            g.setColour (colours::glassGrid);
            g.drawHorizontalLine ((int) yOf ((float) l), r.getX(), r.getRight());
            g.setColour (colours::dim);
            g.drawText (juce::String (l), juce::Rectangle<float> (glass.getX() + 6.0f, yOf ((float) l) - 7.0f, 32.0f, 14.0f), juce::Justification::centredRight);
        }
        for (const float mark : { -14.0f, -23.0f })
        {
            g.setColour (colours::good.withAlpha (0.45f));
            g.drawHorizontalLine ((int) yOf (mark), r.getX(), r.getRight());
        }
        const auto trace = [&] (const std::array<float, kHistory>& h, juce::Colour c, float thickness)
        {
            juce::Path p;
            bool started = false;
            for (int i = 0; i < kHistory; ++i)
            {
                if (h[(size_t) i] < gui::kNothing)
                    continue;
                const float x = r.getX() + r.getWidth() * (float) i / (float) (kHistory - 1);
                started ? p.lineTo (x, yOf (h[(size_t) i])) : p.startNewSubPath (x, yOf (h[(size_t) i]));
                started = true;
            }
            g.setColour (c);
            g.strokePath (p, juce::PathStrokeType (thickness));
        };
        trace (momentaryHistory, colours::dim, 1.0f);
        trace (shortTermHistory, colours::amber, 1.8f);
        if (integrated > gui::kNothing)
        {
            g.setColour (colours::accent);
            g.drawHorizontalLine ((int) yOf (integrated), r.getX(), r.getRight());
        }
        hw::drawLabel (g, "LAST MINUTE (LUFS): MOMENTARY, SHORT-TERM, INTEGRATED;  -14 AND -23 MARKED", { (int) glass.getX() + 48, (int) glass.getBottom() - 12, (int) glass.getWidth() - 56, 11 }, juce::Justification::left, colours::dim, 8.0f);
        hw::drawGlassOverlay (g, glass, 4.0f, false);
    }
}

void HermesEditor::timerCallback()
{
    momentary = proc.engine.momentary.load();
    shortTerm = proc.engine.shortTerm.load();
    integrated = proc.engine.integrated.load();
    range = proc.engine.range.load();
    truePeak = proc.engine.truePeakDb.load();
    samplePeak = proc.engine.samplePeakDb.load();
    position = proc.engine.referencePosition.load();
    std::copy (momentaryHistory.begin() + 1, momentaryHistory.end(), momentaryHistory.begin());
    std::copy (shortTermHistory.begin() + 1, shortTermHistory.end(), shortTermHistory.begin());
    momentaryHistory.back() = momentary;
    shortTermHistory.back() = shortTerm;
    ++ticks;
    presetBox.sync();
    repaint();
}
