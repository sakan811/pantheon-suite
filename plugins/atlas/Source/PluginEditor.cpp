// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 The Pantheon authors

#include "PluginEditor.h"

using namespace atlas;
namespace hw = ui::hw;

namespace
{
constexpr int headerHeight = 84;
constexpr int stripHeight = 540;
constexpr int stripGap = 8;
constexpr int stripWidth = 196;
constexpr int viewHeight = 230;
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
      listen (p.apvts, bandIds (band).listen,
              "What this band's compressor listens to.\nSelf: the band itself.\nSidechain: the same band of the sidechain input (another track, routed in the host), so that "
              "track decides when this band is turned down. With no sidechain connected it listens to itself."),
      direction (p.apvts, bandIds (band).direction,
                 "Downward: turns the band down while it is over THRESH (an ordinary compressor).\nUpward: lifts the band while it is under THRESH, by (1 - 1 / ratio) dB for every dB it is "
                 "under, up to 12 dB, and leaves what is more than 30 dB under it alone (noise, silence). ATTACK is how fast the lift gives way to a louder band; RELEASE how "
                 "fast it comes back. It makes what is there louder; it cannot add what is missing."),
      threshold (p.apvts, bandIds (band).threshold, "THRESH", "The level (peak, in dB) of the band above which it is turned down. The knee is 6 dB wide.", ui::Knob::Layout::below),
      ratio (p.apvts, bandIds (band).ratio, "RATIO", "How many dB the band has to rise past the threshold for 1 dB more to come out.", ui::Knob::Layout::below),
      attack (p.apvts, bandIds (band).attack, "ATTACK", "How fast the gain goes down.", ui::Knob::Layout::below),
      release (p.apvts, bandIds (band).release, "RELEASE", "How fast the gain comes back.", ui::Knob::Layout::below),
      makeup (p.apvts, bandIds (band).makeup, "MAKEUP", "Gain after the band's compressor.", ui::Knob::Layout::below)
{
    if (band < kCrossovers)
        crossover = std::make_unique<ui::Knob> (p.apvts, id::crossover[(size_t) band], "XOVER",
                                                "Where this band ends and the next begins (also: drag the edge on the display). The edges keep their order.",
                                                ui::Knob::Layout::below);
    solo.setTooltip ("Solo this band. Click more bands to hear them together; any combination adds up flat. Not saved with the session.");
    delta.setTooltip ("Listen to only what this band's compressor changes (its output minus its input). Not saved with the session.");
    auto& monitor = p.engine.monitor;
    solo.onClick = [&monitor, band] { monitor.solo ^= 1u << band; };
    delta.onClick = [&monitor, band] { monitor.delta ^= 1u << band; };
}

AtlasEditor::AtlasEditor (AtlasProcessor& p)
    : AudioProcessorEditor (&p), view (p), proc (p),
      phaseBox (p.apvts, id::phase, "ZERO LATENCY: the bands are split by minimum-phase crossovers, the way an analogue one does: no delay, and the phase turns "
                                    "around each edge.\nLINEAR: the same bands with no phase shift at all, so what the compressors do not touch is the input, only later; "
                                    "the whole sound is delayed (the host compensates)."),
      oversamplingBox (p.apvts, id::oversampling, "Runs the compressors at 2 or 4 times the sample rate, so a fast gain on high notes does not fold back as "
                                                   "false tones. Adds a short delay (the host compensates) and processor load."),
      lookaheadBox (p.apvts, id::lookahead,
                    "The compressors read the sound this long before it is heard, so a fast peak is already being turned down when it arrives: set it "
                    "to about the ATTACK time. The whole sound is delayed by it (the host compensates). Changing it while playing restarts the bands."),
      outKnob (p.apvts, id::out, "OUTPUT", "Output gain, after everything.")
{
    setLookAndFeel (&look);
    for (int b = 0; b < kBands; ++b)
    {
        auto& s = *(strips[(size_t) b] = std::make_unique<Strip> (p, b));
        for (auto* c : s.components())
            addChildComponent (*c);
    }
    addAndMakeVisible (view);
    view.onLayoutChanged = [this] { layoutStrips(); };
    clearButton.setTooltip ("End every solo and delta: all six bands, as they come out.");
    clearButton.onClick = [this] { proc.engine.monitor.solo = 0u; proc.engine.monitor.delta = 0u; };
    for (int i = 0; i < (int) presets().size(); ++i)
        presetBox.addItem (presets()[(size_t) i].name, i + 1);
    presetBox.setSelectedId (p.getCurrentProgram() + 1, juce::dontSendNotification);
    presetBox.onChange = [this] { proc.setCurrentProgram (presetBox.getSelectedId() - 1); setPresetTooltip(); };
    setPresetTooltip();
    for (auto* c : std::initializer_list<juce::Component*> { &phaseBox, &oversamplingBox, &lookaheadBox, &presetBox, &outKnob, &clearButton, &guideButton })
        addAndMakeVisible (*c);

    startTimerHz (kTimerHz);
    setSize (24 + kBands * stripWidth + (kBands - 1) * stripGap, headerHeight + 10 + viewHeight + 10 + stripHeight + 6 + ui::kNoteStripHeight + 12);
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
    lookaheadLabel = { row.getX() + 2, row.getY() - 15, 120, 12 };
    lookaheadBox.setBounds (row.removeFromLeft (84));
    row.removeFromLeft (14);
    presetLabel = { row.getX() + 2, row.getY() - 15, 120, 12 };
    presetBox.setBounds (row.removeFromLeft (140));
    row.removeFromLeft (14);
    latencyLabel = row.removeFromLeft (96);
    clearButton.setBounds (row.removeFromRight (96));
    row.removeFromRight (8);
    guideButton.setBounds (row.removeFromRight (64));

    bounds.removeFromTop (10);
    view.setBounds (bounds.removeFromTop (viewHeight));
    bounds.removeFromTop (10);
    stripArea = bounds.removeFromTop (stripHeight);
    noteStrip = bounds.withTrimmedTop (6).withHeight (ui::kNoteStripHeight);
    layoutStrips();
}

void AtlasEditor::setPresetTooltip()
{
    const auto& list = presets();
    const int index = presetBox.getSelectedId() - 1;
    juce::String text = "Presets: starting points. The thresholds suit a master around -14 LUFS, so set THRESH where each band's meter first moves. "
                        "Phase, oversampling, look-ahead and output are kept.";
    if (juce::isPositiveAndBelow (index, (int) list.size()))
        text << "\n\n" << list[(size_t) index].name << ": " << list[(size_t) index].purpose;
    presetBox.setTooltip (text);
}

AdviceInput AtlasEditor::adviceInput() const
{
    const auto l = layoutOf (proc.apvts);
    const auto params = readParams (proc.apvts);
    AdviceInput in;
    for (int band = 0; band < l.numBands; ++band)
    {
        const int slot = l.slot[(size_t) band];
        in.bands.push_back ({ band + 1, l.edgeHz[(size_t) band], l.edgeHz[(size_t) (band + 1)], params.bands[(size_t) slot],
                              (double) strips[(size_t) slot]->shownReduction });
    }
    in.delta = proc.engine.monitor.delta.load() != 0u;
    in.solo = proc.engine.monitor.solo.load() != 0u;
    in.phase = params.phase;
    in.latencyMs = 1000.0 * proc.engine.latencySamples.load() / std::max (proc.getSampleRate(), 1.0);
    in.lookaheadMs = kLookaheadMs[(size_t) params.lookahead];
    in.sidechainConnected = proc.sidechainConnected.load (std::memory_order_relaxed);
    return in;
}

int AtlasEditor::layoutSignature() const
{
    const auto l = layoutOf (proc.apvts);
    int signature = l.numBands;
    for (int b = 0; b < l.numBands; ++b)
        signature = signature * 7 + l.slot[(size_t) b];
    return signature;
}

// One strip for each band there is, from the left, in the order of the spectrum.
void AtlasEditor::layoutStrips()
{
    shownLayout = layoutSignature();
    const auto l = layoutOf (proc.apvts);
    for (auto& strip : strips)
    {
        strip->bounds = {};
        for (auto* c : strip->components())
            c->setVisible (false);
    }
    auto area = stripArea;
    for (int b = 0; b < l.numBands; ++b)
    {
        auto& s = *strips[(size_t) l.slot[(size_t) b]];
        s.bounds = area.removeFromLeft (stripWidth);
        area.removeFromLeft (stripGap);
        auto r = s.bounds.reduced (10, 0).withTrimmedTop (32);
        const auto pair = [&r] (juce::Component& a, juce::Component& b2)
        {
            auto line = r.removeFromTop (22);
            a.setBounds (line.removeFromLeft ((line.getWidth() - 6) / 2));
            b2.setBounds (line.withTrimmedLeft (6));
            r.removeFromTop (6);
        };
        pair (s.on, s.match);
        pair (s.solo, s.delta);
        s.channels.setBounds (r.removeFromTop (24));
        r.removeFromTop (6);
        s.listen.setBounds (r.removeFromTop (24));
        r.removeFromTop (6);
        s.direction.setBounds (r.removeFromTop (24));
        r.removeFromTop (8);
        s.meter = r.removeFromTop (24);
        r.removeFromTop (6);
        const int cell = r.getWidth() / 2;
        // The edge knob belongs to the strip only while the split above the band is the strip's own (the editor keeps it so).
        ui::Knob* knobs[] = { &s.threshold, &s.ratio, &s.attack, &s.release, &s.makeup, l.splitAbove[(size_t) b] == l.slot[(size_t) b] ? s.crossover.get() : nullptr };
        for (auto* c : s.components())
            c->setVisible (c != s.crossover.get());
        for (int k = 0; k < 6; ++k)
            if (knobs[k] != nullptr)
            {
                knobs[k]->setVisible (true);
                knobs[k]->setBounds (r.getX() + cell * (k % 2), r.getY() + cellHeight * (k / 2), cell, cellHeight);
            }
    }
    repaint();
}

void AtlasEditor::paint (juce::Graphics& g)
{
    g.fillAll (colours::background);
    hw::drawHeaderPlate (g, getLocalBounds().removeFromTop (headerHeight).toFloat());
    hw::drawNameplate (g, nameplate, gui::kProjectWord, gui::kPluginWord, "Multiband compressor", inLevel > 0.001f, 25.0f);
    hw::drawLabel (g, "Phase", phaseLabel, juce::Justification::left, colours::dim, 9.0f);
    hw::drawLabel (g, "Oversampling", oversamplingLabel, juce::Justification::left, colours::dim, 9.0f);
    hw::drawLabel (g, "Look-ahead", lookaheadLabel, juce::Justification::left, colours::dim, 9.0f);
    hw::drawLabel (g, "Preset", presetLabel, juce::Justification::left, colours::dim, 9.0f);
    const int latency = proc.engine.latencySamples.load();
    hw::drawLabel (g, latency == 0 ? juce::String ("No delay") : "Delay " + juce::String (1000.0 * latency / std::max (proc.getSampleRate(), 1.0), 1) + " ms",
                   latencyLabel, juce::Justification::centredLeft, colours::dim, 9.5f);

    ui::drawNoteStrip (g, noteStrip, adviceFor (adviceInput()));

    const unsigned solo = proc.engine.monitor.solo.load(), delta = proc.engine.monitor.delta.load();
    const auto l = layoutOf (proc.apvts);
    if (l.numBands < kBands)
        hw::drawLabel (g, "Double-click the display to split a band", stripArea.withTrimmedLeft (l.numBands * (stripWidth + stripGap)).reduced (12, 0).removeFromTop (40),
                       juce::Justification::centredLeft, colours::dim, 9.5f);
    for (int band = 0; band < l.numBands; ++band)
    {
        const int b = l.slot[(size_t) band];
        const auto& s = *strips[(size_t) b];
        hw::drawModule (g, s.bounds.toFloat());
        const bool heard = solo == 0u || (solo & (1u << b)) != 0u;
        const std::pair<double, double> range { l.edgeHz[(size_t) band], l.edgeHz[(size_t) (band + 1)] };
        hw::drawLabel (g, "Band " + juce::String (band + 1), { s.bounds.getX() + 16, s.bounds.getY() + 10, 90, 14 }, juce::Justification::left,
                       heard ? colours::amber : colours::dim, 10.5f);
        hw::drawLabel (g, hzName (range.first) + " - " + hzName (range.second), { s.bounds.getRight() - 116, s.bounds.getY() + 10, 100, 14 },
                       juce::Justification::right, colours::dim, 8.5f);

        // The reduction meter: a bar that grows from the right, and the number.
        const auto m = s.meter.toFloat();
        hw::drawGlass (g, m, 3.0f, colours::amber);
        const float share = juce::jlimit (0.0f, 1.0f, std::fabs (s.shownReduction) / gui::kMeterRangeDb);
        g.setColour ((s.shownReduction < 0.0f ? colours::accent : colours::amber).withAlpha (0.55f)); // teal: lifting
        const auto bar = m.reduced (3.0f).withTrimmedLeft (m.getWidth() * 0.55f); // the left of the window is the read-out's
        g.fillRect (bar.withTrimmedLeft (bar.getWidth() * (1.0f - share)));
        g.setFont (ui::lcdFont (11.5f));
        g.setColour (colours::amber);
        juce::String text = std::fabs (s.shownReduction) < 0.05f ? juce::String ("0.0 dB")
                                                                  : (s.shownReduction > 0.0f ? "-" : "+") + juce::String (std::fabs (s.shownReduction), 1) + " dB";
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
        const float fall = gui::kMeterRangeDb / (float) kTimerHz;
        s.shownReduction = std::fabs (now) >= std::fabs (s.shownReduction) ? now : s.shownReduction + juce::jlimit (-fall, fall, now - s.shownReduction);
    }
    if (const int prog = proc.getCurrentProgram(); prog != lastProgram)
    {
        lastProgram = prog;
        presetBox.setSelectedId (prog + 1, juce::dontSendNotification);
        setPresetTooltip();
    }
    if (layoutSignature() != shownLayout) // a host or a preset changed the splits
        layoutStrips();
    view.tick (1.0f / (float) kTimerHz);
    inLevel = proc.engine.inPeak.exchange (0.0f);
    proc.engine.outPeak.store (0.0f);
    repaint();
}
