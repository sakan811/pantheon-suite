// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 The Pantheon authors

#include "PluginEditor.h"

using namespace apollo;
namespace hw = ui::hw;

namespace
{
constexpr int headerHeight = 84;
constexpr int viewHeight = 262;
constexpr int stripWidth = 156;
constexpr int stripGap = 6;
constexpr int stripHeight = 332;
constexpr int cellHeight = 104;
constexpr int kTimerHz = 20;
}

ApolloEditor::Strip::Strip (ApolloProcessor& p, int band)
    : on (p.apvts, bandIds (band).on, "ON", "This band. Off: it changes nothing (and a bell or shelf of 0 dB is not run at all)."),
      type (p.apvts, bandIds (band).type,
            "Bell: raises or cuts around a frequency; Q is its width.\nLow / High shelf: raises or cuts everything below / above it.\nHigh-pass / Low-pass: removes everything below / above it, "
            "12 to 48 dB per octave.\nNotch: removes a narrow band entirely."),
      target (p.apvts, bandIds (band).target,
              "What the band works on.\nStereo: left and right alike.\nMid: only the centre (what is the same in both channels).\nSide: only the sides (what differs between them)."),
      slope (p.apvts, bandIds (band).slope, "How steeply the high-pass or low-pass falls: 12 to 48 dB for every octave past the corner (-3 dB at the corner)."),
      freq (p.apvts, bandIds (band).freq, "FREQ", "The centre of a bell or notch, the corner of a shelf, high-pass or low-pass (also: drag the node on the display).", ui::Knob::Layout::below),
      gain (p.apvts, bandIds (band).gain, "GAIN", "How much the band raises (+) or cuts (-). Cutting is gentler than boosting.", ui::Knob::Layout::below),
      q (p.apvts, bandIds (band).q, "Q", "The width of a bell or notch (higher is narrower), or the sharpness of a shelf's turn. The wheel over a node changes it.", ui::Knob::Layout::below)
{
}

ApolloEditor::ApolloEditor (ApolloProcessor& p)
    : AudioProcessorEditor (&p),
      phaseBox (p.apvts, id::phase, "MINIMUM: the usual equaliser: no delay, and the phase turns around each band.\nLINEAR: no phase shift at all (a master's transients and "
                                    "stereo image stay together), at the price of about 48 ms of delay (the host compensates) and a softer low end (about 12 Hz resolution). "
                                    "Changing it while playing restarts the equaliser."),
      outKnob (p.apvts, id::out, "OUTPUT", "Output gain, after every band."),
      view (p.apvts, [&p] { return p.getSampleRate(); }),
      proc (p)
{
    setLookAndFeel (&look);
    for (int b = 0; b < kBands; ++b)
    {
        auto& s = *(strips[(size_t) b] = std::make_unique<Strip> (p, b));
        for (auto* c : std::initializer_list<juce::Component*> { &s.on, &s.type, &s.target, &s.slope, &s.freq, &s.gain, &s.q })
            addChildComponent (*c);
    }
    view.onSelect = [this] (int) { repaint(); };
    for (auto* c : std::initializer_list<juce::Component*> { &view, &phaseBox, &outKnob, &guideButton })
        addAndMakeVisible (*c);
    startTimerHz (kTimerHz);
    setSize (24 + kBands * stripWidth + (kBands - 1) * stripGap, headerHeight + 10 + viewHeight + 10 + stripHeight + 8 + ui::kNoteStripHeight + 12);
    showTypes();
}

ApolloEditor::~ApolloEditor() { setLookAndFeel (nullptr); }

void ApolloEditor::resized()
{
    auto bounds = getLocalBounds().reduced (12, 0);
    auto header = bounds.removeFromTop (headerHeight).withTrimmedTop (8).withTrimmedBottom (8);
    nameplate = header.removeFromLeft (250);
    header.removeFromLeft (24);
    outKnob.setBounds (header.removeFromRight (170).withSizeKeepingCentre (170, 52));
    header.removeFromRight (16);
    auto row = header.withTrimmedTop (28).withHeight (28);
    phaseLabel = { row.getX() + 2, row.getY() - 15, 120, 12 };
    phaseBox.setBounds (row.removeFromLeft (130));
    row.removeFromLeft (14);
    latencyLabel = row.removeFromLeft (120);
    guideButton.setBounds (row.removeFromRight (64));

    bounds.removeFromTop (10);
    view.setBounds (bounds.removeFromTop (viewHeight));
    bounds.removeFromTop (10);
    stripArea = bounds.removeFromTop (stripHeight);
    noteStrip = bounds.withTrimmedTop (8).withHeight (ui::kNoteStripHeight);

    auto area = stripArea;
    for (int b = 0; b < kBands; ++b)
    {
        auto& s = *strips[(size_t) b];
        s.bounds = area.removeFromLeft (stripWidth);
        area.removeFromLeft (stripGap);
        auto r = s.bounds.reduced (10, 0).withTrimmedTop (30);
        s.on.setBounds (r.removeFromTop (22));
        r.removeFromTop (6);
        s.type.setBounds (r.removeFromTop (24));
        r.removeFromTop (6);
        s.target.setBounds (r.removeFromTop (24));
        r.removeFromTop (6);
        const int cell = r.getWidth() / 2;
        s.freq.setBounds (r.getX(), r.getY(), cell + 4, cellHeight);
        s.gain.setBounds (r.getX() + cell, r.getY(), cell, cellHeight);
        s.q.setBounds (r.getX(), r.getY() + cellHeight, cell + 4, cellHeight);
        s.slope.setBounds (r.getX(), r.getY() + cellHeight + 40, r.getWidth(), 24);
    }
}

// A strip shows the controls its type uses: a gain for a bell and the shelves, a slope for a high-pass and a low-pass, a Q for the rest.
void ApolloEditor::showTypes()
{
    const auto params = readParams (proc.apvts);
    int signature = 0;
    for (int b = 0; b < kBands; ++b)
    {
        const auto type = params.bands[(size_t) b].type;
        signature = signature * 7 + (int) type;
        auto& s = *strips[(size_t) b];
        for (auto* c : std::initializer_list<juce::Component*> { &s.on, &s.type, &s.target, &s.freq })
            c->setVisible (true);
        s.gain.setVisible (hasGain (type));
        s.q.setVisible (! hasSlope (type));
        s.slope.setVisible (hasSlope (type));
    }
    shownTypes = signature;
}

AdviceInput ApolloEditor::adviceInput() const
{
    AdviceInput in;
    in.params = readParams (proc.apvts);
    in.latencyMs = 1000.0 * proc.engine.latencySamples.load() / std::max (proc.getSampleRate(), 1.0);
    return in;
}

void ApolloEditor::paint (juce::Graphics& g)
{
    g.fillAll (colours::background);
    hw::drawHeaderPlate (g, getLocalBounds().removeFromTop (headerHeight).toFloat());
    hw::drawNameplate (g, nameplate, gui::kProjectWord, gui::kPluginWord, "Parametric equaliser", true, 25.0f);
    hw::drawLabel (g, "Phase", phaseLabel, juce::Justification::left, colours::dim, 9.0f);
    const int latency = proc.engine.latencySamples.load();
    hw::drawLabel (g, latency == 0 ? juce::String ("No delay") : "Delay " + juce::String (1000.0 * latency / std::max (proc.getSampleRate(), 1.0), 1) + " ms",
                   latencyLabel, juce::Justification::centredLeft, colours::dim, 9.5f);
    ui::drawNoteStrip (g, noteStrip, adviceFor (adviceInput()));
    const auto params = readParams (proc.apvts);
    for (int b = 0; b < kBands; ++b)
    {
        const auto& s = *strips[(size_t) b];
        hw::drawModule (g, s.bounds.toFloat());
        hw::drawLabel (g, "Band " + juce::String (b + 1), { s.bounds.getX() + 16, s.bounds.getY() + 10, 90, 14 }, juce::Justification::left,
                       params.bands[(size_t) b].on ? gui::bandColour (b) : colours::dim, 10.5f);
        if (b == view.selected)
        {
            g.setColour (gui::bandColour (b).withAlpha (0.8f));
            g.drawRoundedRectangle (s.bounds.toFloat().reduced (1.5f), 4.0f, 1.5f);
        }
    }
}

void ApolloEditor::timerCallback()
{
    int signature = 0;
    const auto params = readParams (proc.apvts);
    for (const auto& b : params.bands)
        signature = signature * 7 + (int) b.type;
    if (signature != shownTypes)
        showTypes();
    repaint();
}
