// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 The Pantheon authors

#include "PluginEditor.h"

using namespace iris;
using namespace iris::gui;

namespace
{
constexpr int kTimerHz = 30;
constexpr int editorWidth = 1180, editorHeight = 704;
constexpr int headerHeight = 84;   // the control panel along the top
constexpr int viewHeight = 322;
constexpr int cellHeight = 104;    // a knob: name, knob, read-out
constexpr int panelRowHeight = 260;
constexpr int chipWidth = 34, chipHeight = 26, chipGap = 5;
constexpr int bandPanelWidth = 440, dynPanelWidth = 480;

const char* panelTitles[] = { "BAND", "DYNAMICS", "OUTPUT  /  SAT" };

IrisProcessor* g_unused = nullptr; // (keeps the anonymous namespace non-empty if the tables below are edited)
}

IrisEditor::IrisEditor (IrisProcessor& p)
    : AudioProcessorEditor (&p), proc (p),
      view (p.engine, p.apvts, p.currentSampleRate),
      freqKnob (ui::Knob::Layout::below), gainKnob (ui::Knob::Layout::below), qKnob (ui::Knob::Layout::below),
      thresholdKnob (ui::Knob::Layout::below), ratioKnob (ui::Knob::Layout::below), rangeKnob (ui::Knob::Layout::below),
      attackKnob (ui::Knob::Layout::below), releaseKnob (ui::Knob::Layout::below),
      driveKnob (p.apvts, id::drive, "DRIVE", "Where SAT starts to bite: the knee sits this far below full scale, on what a boost adds. "
                                               "Small signals stay linear. Past the boost's own level it limits the boost.", ui::Knob::Layout::below),
      outKnob (p.apvts, id::out, "OUTPUT", "Output gain, after the equaliser and Gain Match.", ui::Knob::Layout::below),
      styleBox (p.apvts, id::style, "The curve SAT uses.\nTape: smooth, odd harmonics.\nTube: asymmetric, adds even harmonics (the 2nd).\nConsole: open and gentle."),
      modeBox (p.apvts, id::mode, "CLEAN: a plain equaliser, no harmonics, whatever the level.\nSAT: what a boosting band adds is passed through a soft curve, so a loud "
                                  "boost warms up (see Drive and Style). Cuts stay clean. SAT works in the Zero Latency phase mode only."),
      phaseBox (p.apvts, id::phase, "ZERO LATENCY: every band is a minimum-phase filter, the way an analogue equaliser is: no delay, and the phase turns around each band. "
                                    "For tracking, live, and mixing.\nLINEAR: no phase shift at all, so a boost and a cut leave the waveform's shape alone and parallel or "
                                    "stem processing stays in phase; the whole sound is delayed (the host compensates) and a sharp boost can ring before the beat. For mastering.\n"
                                    "LINEAR HQ: the same with four times the resolution for narrow bass bands, at four times the delay. SAT works in Zero Latency only."),
      matchButton (p.apvts, id::match, "GAIN MATCH", "Matches the output loudness to the input, so a boost that is merely louder doesn't sound better. Slow, and never more than 12 dB.")
{
    (void) g_unused;
    setLookAndFeel (&look);
    addAndMakeVisible (view);
    view.onSelectionChanged = [this] (int) { refreshBandPanel(); layoutBandPanel(); repaint(); };

    // Knobs of the selected band (re-bound in refreshBandPanel).
    const struct { ui::Knob* knob; const char* caption; const char* tip; } captions[] =
    {
        { &freqKnob, "FREQ", "The band's frequency (the corner for a shelf or a cut). Also set by dragging its node across the display." },
        { &gainKnob, "GAIN", "The band's gain. Also set by dragging its node up and down." },
        { &qKnob, "Q", "Width of a bell or notch (higher = narrower); the steepness of a shelf. Also the wheel over the node." },
        { &thresholdKnob, "THRESH", "The level (peak, in dB) of the band's own frequency region at which the dynamics start to work." },
        { &ratioKnob, "RATIO", "How much of every dB past the threshold becomes gain change, until the range is reached. 4:1 = 3 dB per 4 dB." },
        { &rangeKnob, "RANGE", "The most the gain moves by. Negative = the band cuts further as it engages; positive = it boosts. The static GAIN is where it rests." },
        { &attackKnob, "ATTACK", "How fast the gain moves towards its engaged value." },
        { &releaseKnob, "RELEASE", "How fast the gain comes back." },
    };
    for (auto& c : captions)
    {
        c.knob->setCaption (c.caption);
        c.knob->setKnobTooltip (c.tip);
        addChildComponent (*c.knob);
    }

    for (int i = 0; i < maxBands; ++i)
    {
        const auto& ids = bandIds (i);
        typeBoxes[(size_t) i] = std::make_unique<ui::Choice> (p.apvts, ids.type, "The band's shape.");
        placeBoxes[(size_t) i] = std::make_unique<ui::Choice> (p.apvts, ids.place,
            "Which signal the band works on.\nStereo: both channels. Left / Right: one channel, the other untouched.\n"
            "Mid / Side: the centre or the sides; the rest of the image is untouched.");
        slopeBoxes[(size_t) i] = std::make_unique<ui::Choice> (p.apvts, ids.slope, "How steep the cut is.");
        dynModeBoxes[(size_t) i] = std::make_unique<ui::Choice> (p.apvts, ids.dynMode, "Above: the dynamics engage while the band's level is over the threshold. Below: while it is under it.");
        dynToggles[(size_t) i] = std::make_unique<ui::Toggle> (p.apvts, ids.dyn, "DYNAMIC",
            "Makes the band dynamic: its gain moves with the level of its own frequency region (bells and shelves).");
        for (auto* c : std::initializer_list<juce::Component*> { typeBoxes[(size_t) i].get(), placeBoxes[(size_t) i].get(), slopeBoxes[(size_t) i].get(),
                                                                 dynModeBoxes[(size_t) i].get(), dynToggles[(size_t) i].get() })
            addChildComponent (*c);
    }

    removeButton.setTooltip ("Free this band slot.");
    removeButton.onClick = [this] { removeBand (proc.apvts, view.getSelectedBand()); };
    addButton.setTooltip ("Put a bell band in this slot. (Double-click the display to place one where you want it.)");
    addButton.onClick = [this] { addBand (proc.apvts, BandType::bell, defaultFreq (view.getSelectedBand()), 0.0, view.getSelectedBand()); };

    soloBandButton.setTooltip ("Listen to this band's region: the output through a filter at the band's frequency and width "
                               "(below a low shelf or low cut, above a high one). Use it to find what a band is working on.");
    soloCurveButton.setTooltip ("Listen to only what this band adds or takes away: its own curve, nothing else. "
                                "Solo wins over Delta.");
    for (auto* b : { &soloBandButton, &soloCurveButton })
    {
        b->setClickingTogglesState (false);
        b->setColour (juce::TextButton::buttonOnColourId, colours::text);
    }
    soloBandButton.onClick = [this] { view.toggleBandSolo (view.getSelectedBand(), SoloKind::band); };
    soloCurveButton.onClick = [this] { view.toggleBandSolo (view.getSelectedBand(), SoloKind::curve); };
    for (auto* b : { &removeButton, &addButton, &soloBandButton, &soloCurveButton })
        addChildComponent (*b);
    pasteButton.setTooltip ("Add the bands another plugin of the suite copied (Themis: COPY on a plan item, or Copy EQ for the tonal curve), "
                            "each in the next free slot, with its type, frequency, gain, width and placement. Copy EQ also hands over the target curve, which "
                            "becomes the reference drawn behind the spectrum.");
    pasteButton.onClick = [this]
    {
        int skipped = 0;
        const auto text = juce::SystemClipboard::getTextFromClipboard();
        const bool curve = view.pasteReference (text); // Themis' Copy EQ hands over its target curve along with the bands
        const auto slots = pasteBands (proc.apvts, text, skipped);
        if (! slots.empty())
            view.setSelectedBand (slots.front());
        else if (! curve)
            juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::InfoIcon, "Paste",
                                                    skipped > 0 ? "No free slot, or nothing an equaliser can make (a harmonic-fill band goes to Hephaestus)."
                                                                : "The clipboard holds no bands. Copy them in Themis first (COPY on a plan item, or Copy EQ).");
    };
    addAndMakeVisible (pasteButton);

    // Header.
    for (int i = 0; i < (int) presets().size(); ++i)
        presetBox.addItem (presets()[(size_t) i].name, i + 1);
    presetBox.setSelectedId (proc.getCurrentProgram() + 1, juce::dontSendNotification);
    presetBox.onChange = [this] { proc.setCurrentProgram (presetBox.getSelectedId() - 1); };
    presetBox.setTooltip ("Presets. Output gain, Gain Match and the region crossovers are kept.");

    learnRefButton.setClickingTogglesState (false);
    learnRefButton.setColour (juce::TextButton::buttonOnColourId, colours::amber);
    deltaButton.setClickingTogglesState (true);
    deltaButton.setTooltip ("Listen to only what the equaliser changes (output minus input). Not saved with the project; "
                            "a band solo takes over from it.");
    deltaButton.onClick = [this] { proc.engine.monitor.delta = deltaButton.getToggleState(); };

    learnRefButton.setTooltip ("Press, play the track you want this one to sound like through Iris (a few seconds at least; the longer the better), "
                               "press again: its long-term shape is kept as the reference and drawn as a dashed line behind the spectrum, levelled to "
                               "this track's average. Needs the editor open. The reference is saved with the project.");
    learnRefButton.onClick = [this] { view.toggleLearning(); };
    matchEqButton.setTooltip ("Play the track you are working on, then press: adds up to six bands that bring its long-term shape (the thin line) to "
                              "the reference (the dashed one), counting the stereo bands already on. A starting point to judge by ear and Delta, not a fix: "
                              "a difference from another track is taste. Right-click the display for more (clear the reference, restart the average).");
    matchEqButton.onClick = [this] { view.matchToReference(); };

    clearMetersButton.setTooltip ("Clear the IN and OUT level meters. Use it if a reading looks stuck.");
    clearMetersButton.onClick = [this]
    {
        proc.engine.clearMeters();
        inLevel = outLevel = 0.0f;
        repaint (meterArea.expanded (24, 4));
    };

    for (auto* c : std::initializer_list<juce::Component*> { &presetBox, &modeBox, &deltaButton, &matchButton, &clearMetersButton, &learnRefButton, &matchEqButton,
                                                             &driveKnob, &outKnob, &styleBox, &phaseBox })
        addAndMakeVisible (c);

    // The first band that exists is the one to show.
    for (int i = 0; i < maxBands; ++i)
        if (isBandOn (proc.apvts, i))
        {
            view.setSelectedBand (i);
            break;
        }
    refreshBandPanel();

    // Size last: resized() lays out the controls, which must all exist by then.
    setSize (editorWidth, editorHeight);
    view.setSidechain (&p.sidechainTap);
    startTimerHz (kTimerHz);
}

IrisEditor::~IrisEditor()
{
    proc.engine.monitor.delta = false; // monitoring never outlives the window that asked for it
    view.clearSolos();
    setLookAndFeel (nullptr);
}

// What changes the band panel: which band, whether it exists, its type and whether it is dynamic.
int IrisEditor::bandSignature() const
{
    // Unsigned: eight bands of 14 states each do not fit an int, and it is the wrap-around that is wanted here.
    unsigned signature = (unsigned) view.getSelectedBand();
    for (int i = 0; i < maxBands; ++i)
    {
        const bool on = isBandOn (proc.apvts, i);
        signature = signature * 7u + (on ? 1u + (unsigned) bandType (proc.apvts, i) : 0u);
        signature = signature * 2u + (on && proc.apvts.getRawParameterValue (bandIds (i).dyn)->load() > 0.5f ? 1u : 0u);
    }
    return (int) (signature & 0x7fffffffu);
}

void IrisEditor::refreshBandPanel()
{
    shownSignature = bandSignature();
    const int band = view.getSelectedBand();
    const bool on = isBandOn (proc.apvts, band);
    const auto type = bandType (proc.apvts, band);
    const auto& ids = bandIds (band);
    const bool dynamic = on && proc.apvts.getRawParameterValue (ids.dyn)->load() > 0.5f;

    struct Binding { ui::Knob* knob; const juce::String* param; bool visible; };
    const Binding bindings[] =
    {
        { &freqKnob, &ids.freq, on },
        { &gainKnob, &ids.gain, on && hasGain (type) },
        { &qKnob, &ids.q, on && ! isCut (type) },
        { &thresholdKnob, &ids.threshold, on && hasGain (type) },
        { &ratioKnob, &ids.ratio, on && hasGain (type) },
        { &rangeKnob, &ids.range, on && hasGain (type) },
        { &attackKnob, &ids.attack, on && hasGain (type) },
        { &releaseKnob, &ids.release, on && hasGain (type) },
    };
    for (auto& b : bindings)
    {
        if (b.visible)
            b.knob->bind (proc.apvts, *b.param);
        else
            b.knob->unbind();
        b.knob->setVisible (b.visible);
    }
    for (auto* k : { &thresholdKnob, &ratioKnob, &rangeKnob, &attackKnob, &releaseKnob })
        k->setAlpha (dynamic ? 1.0f : 0.4f);

    for (int i = 0; i < maxBands; ++i)
    {
        const bool mine = i == band && on;
        typeBoxes[(size_t) i]->setVisible (mine);
        placeBoxes[(size_t) i]->setVisible (mine);
        slopeBoxes[(size_t) i]->setVisible (mine && isCut (type));
        dynToggles[(size_t) i]->setVisible (mine);
        dynToggles[(size_t) i]->setEnabled (hasGain (type));
        dynModeBoxes[(size_t) i]->setVisible (mine && hasGain (type));
    }
    removeButton.setVisible (on);
    soloBandButton.setVisible (on);
    soloCurveButton.setVisible (on);
    addButton.setVisible (! on);
}

juce::String IrisEditor::bandHint() const
{
    const int band = view.getSelectedBand();
    if (! isBandOn (proc.apvts, band))
        return "Slot " + juce::String (band + 1) + " is free. Add a band here, or double-click the display where you want one.";
    return {};
}

juce::String IrisEditor::dynamicsHint() const
{
    const int band = view.getSelectedBand();
    if (! isBandOn (proc.apvts, band))
        return {};
    if (! hasGain (bandType (proc.apvts, band)))
        return "Dynamics move a band's gain, so they need a bell or a shelf.";
    if (proc.apvts.getRawParameterValue (bandIds (band).dyn)->load() < 0.5f)
        return "Switch DYNAMIC on to make this band follow the level of its own frequency region.";

    // Measured over the last seconds: a band that never moves, or never lets go, is a static band with a detector.
    const auto& ids = bandIds (band);
    const auto value = [&] (const juce::String& id) { return proc.apvts.getRawParameterValue (id)->load(); };
    const bool above = value (ids.dynMode) < 0.5f;
    const auto s = usage[(size_t) band].summary();
    const auto db = [] (double x) { return juce::String (x, 1) + " dB"; };
    const auto suggestion = " Try THRESHOLD " + db (above ? s.maxLevelDb - 3.0 : s.minLevelDb + 3.0) + ".";
    if (s.verdict == DynamicsUsage::Verdict::idle)
        return "NEVER MOVES: the level stayed " + db (s.minLevelDb) + " to " + db (s.maxLevelDb) + ", THRESHOLD is "
               + db (value (ids.threshold)) + "." + suggestion;
    if (s.verdict == DynamicsUsage::Verdict::pinned)
        return "ALWAYS AT FULL RANGE, so it acts as a static band (level " + db (s.minLevelDb) + " to " + db (s.maxLevelDb) + ")." + suggestion;
    return "GAIN is where the band rests. While the level is past the threshold the gain moves towards GAIN + RANGE. The display shows the range as a bar and the live gain as a white dot.";
}

void IrisEditor::layoutBandPanel()
{
    const auto panel = panelBounds[(size_t) Panel::band];
    const int band = view.getSelectedBand();

    for (int i = 0; i < maxBands; ++i)
        chipBounds[(size_t) i] = { panel.getX() + 14 + i * (chipWidth + chipGap), panel.getY() + 30, chipWidth, chipHeight };

    const auto buttonSlot = juce::Rectangle<int> (panel.getRight() - 14 - 84, panel.getY() + 30, 84, chipHeight);
    removeButton.setBounds (buttonSlot);
    addButton.setBounds (buttonSlot);
    pasteButton.setBounds (panel.getRight() - 14 - 64, panel.getY() + 6, 64, 20);

    const int boxY = panel.getY() + 78;
    typeBoxes[(size_t) band]->setBounds (panel.getX() + 14, boxY, 118, 26);
    placeBoxes[(size_t) band]->setBounds (panel.getX() + 140, boxY, 100, 26);
    slopeBoxes[(size_t) band]->setBounds (panel.getX() + 248, boxY, 110, 26);

    const int knobY = panel.getY() + 112;
    const int cell = 108;
    freqKnob.setBounds (panel.getX() + 8, knobY, cell, cellHeight);
    gainKnob.setBounds (panel.getX() + 8 + cell, knobY, cell, cellHeight);
    qKnob.setBounds (panel.getX() + 8 + 2 * cell, knobY, cell, cellHeight);

    soloBandButton.setBounds (panel.getRight() - 14 - 84, knobY + 22, 84, 28);
    soloCurveButton.setBounds (panel.getRight() - 14 - 84, knobY + 58, 84, 28);

    // Dynamics panel.
    const auto dyn = panelBounds[(size_t) Panel::dynamics];
    dynToggles[(size_t) band]->setBounds (dyn.getRight() - 14 - 90, dyn.getY() + 6, 90, 22);
    dynModeBoxes[(size_t) band]->setBounds (dyn.getRight() - 14 - 90 - 8 - 92, dyn.getY() + 6, 92, 22);
    dynReadout = { dyn.getX() + 14, dyn.getY() + 34, dyn.getWidth() - 28, 26 };
    const int dynCell = (dyn.getWidth() - 16) / 5;
    int k = 0;
    for (auto* knob : { &thresholdKnob, &ratioKnob, &rangeKnob, &attackKnob, &releaseKnob })
        knob->setBounds (dyn.getX() + 8 + dynCell * k++, dyn.getY() + 66, dynCell, cellHeight);
}

void IrisEditor::resized()
{
    auto bounds = getLocalBounds().reduced (12, 0);

    // Header: the nameplate, the selectors and switches, and the level meters in a window of glass.
    auto header = bounds.removeFromTop (headerHeight).withTrimmedTop (8).withTrimmedBottom (8);
    nameplate = header.removeFromLeft (250);
    header.removeFromLeft (24);
    meterArea = header.removeFromRight (120).reduced (0, 10);
    header.removeFromRight (8);
    {
        auto h = header.withTrimmedTop (28).withHeight (28);
        clearMetersButton.setBounds (h.removeFromRight (64));
        h.removeFromRight (10);
        for (auto [component, width, gap] : { std::tuple<juce::Component*, int, int> { &presetBox, 190, 14 }, { &modeBox, 96, 18 },
                                              { &deltaButton, 70, 8 }, { &matchButton, 104, 12 }, { &learnRefButton, 86, 6 }, { &matchEqButton, 74, 0 } })
        {
            component->setBounds (h.removeFromLeft (width));
            h.removeFromLeft (gap);
        }
    }

    bounds.removeFromTop (10);
    view.setBounds (bounds.removeFromTop (viewHeight));
    bounds.removeFromTop (10);

    auto row = bounds.removeFromTop (panelRowHeight);
    panelBounds[(size_t) Panel::band] = row.removeFromLeft (bandPanelWidth);
    row.removeFromLeft (12);
    panelBounds[(size_t) Panel::dynamics] = row.removeFromLeft (dynPanelWidth);
    row.removeFromLeft (12);
    panelBounds[(size_t) Panel::output] = row;

    const auto out = panelBounds[(size_t) Panel::output];
    phaseBox.setBounds (out.getX() + 14, out.getY() + 52, out.getWidth() - 28, 26);
    styleBox.setBounds (out.getX() + 14, out.getY() + 112, out.getWidth() - 28, 26);
    const int cell = (out.getWidth() - 16) / 2;
    driveKnob.setBounds (out.getX() + 8, out.getY() + 146, cell, cellHeight);
    outKnob.setBounds (out.getX() + 8 + cell, out.getY() + 146, cell, cellHeight);

    layoutBandPanel();
}

void IrisEditor::selectBand (int band)
{
    view.setSelectedBand (band);
}

void IrisEditor::mouseDown (const juce::MouseEvent& e)
{
    for (int i = 0; i < maxBands; ++i)
        if (chipBounds[(size_t) i].contains (e.getPosition()))
            selectBand (i);
}

void IrisEditor::paint (juce::Graphics& g)
{
    namespace hw = ui::hw;

    hw::fillMetal (g, getLocalBounds().toFloat(), colours::background);
    hw::drawHeaderPlate (g, juce::Rectangle<float> (0.0f, 0.0f, (float) getWidth(), (float) headerHeight));
    hw::drawNameplate (g, nameplate, gui::kProjectWord, gui::kPluginWord, "Dynamic EQ", inLevel > 0.001f, 25.0f);

    for (auto [box, caption] : { std::pair<juce::Component*, const char*> { &presetBox, "Preset" }, { &modeBox, "Mode" } })
        hw::drawLabel (g, caption, { box->getX() + 2, box->getY() - 14, 110, 12 }, juce::Justification::left, colours::dim, 9.0f);

    const auto params = readParams (proc.apvts);
    if (params.match) // what Gain Match is doing to the level right now
    {
        const float db = proc.engine.matchDb.load (std::memory_order_relaxed);
        hw::drawLabel (g, "Applying " + juce::String (db, 1) + " dB", { matchButton.getX() + 2, matchButton.getY() - 14, 130, 12 }, juce::Justification::left, colours::amber, 9.0f);
    }

    const int band = view.getSelectedBand();
    const bool bandOn = isBandOn (proc.apvts, band);
    const auto& b = params.bands[(size_t) band];
    const bool dynamicOn = bandOn && b.isDynamic();

    for (size_t i = 0; i < panelBounds.size(); ++i)
    {
        const auto r = panelBounds[i];
        hw::drawModule (g, r.toFloat());

        juce::String title = panelTitles[i];
        if (i != (size_t) Panel::output)
            title << " " << (band + 1) << "  -  " << (bandOn ? (i == (size_t) Panel::band ? typeNames[(int) b.type].toUpperCase() : (dynamicOn ? "ON" : "OFF")) : juce::String ("FREE"));
        hw::drawLabel (g, title, { r.getX() + 18, r.getY() + 10, 300, 14 }, juce::Justification::left,
                       i == (size_t) Panel::band && bandOn ? forPlacement (b.place) : colours::text, 10.5f);
    }

    // Band panel: chips, captions, hint.
    const auto bandPanel = panelBounds[(size_t) Panel::band];
    for (int i = 0; i < maxBands; ++i)
    {
        const auto r = chipBounds[(size_t) i].toFloat();
        const bool on = isBandOn (proc.apvts, i);
        const auto c = forPlacement (bandPlacement (proc.apvts, i));

        hw::drawPushButton (g, r, on, i == band ? c : c.withMultipliedBrightness (0.7f));
        if (i == band)
        {
            g.setColour (colours::text);
            g.drawRoundedRectangle (r.expanded (1.5f), 4.0f, 1.2f);
        }
        g.setColour (on ? colours::lampInk : colours::dim);
        g.setFont (ui::labelFont (11.5f));
        g.drawText (juce::String (i + 1), r, juce::Justification::centred, false);
    }

    if (bandOn)
    {
        const int y = typeBoxes[(size_t) band]->getY() - 13;
        hw::drawLabel (g, "Type", { typeBoxes[(size_t) band]->getX() + 2, y, 100, 12 }, juce::Justification::left, colours::dim, 9.0f);
        hw::drawLabel (g, "Works on", { placeBoxes[(size_t) band]->getX() + 2, y, 100, 12 }, juce::Justification::left, colours::dim, 9.0f);
        if (isCut (b.type))
            hw::drawLabel (g, "Slope", { slopeBoxes[(size_t) band]->getX() + 2, y, 100, 12 }, juce::Justification::left, colours::dim, 9.0f);
        hw::drawLabel (g, "Solo", { soloBandButton.getX(), soloBandButton.getY() - 16, soloBandButton.getWidth(), 12 }, juce::Justification::centred, colours::dim, 9.5f);
    }

    g.setColour (colours::dim);
    g.setFont (juce::FontOptions (11.5f));
    g.drawFittedText (bandHint(), bandPanel.getX() + 18, bandPanel.getY() + 80, bandPanel.getWidth() - 36, 40, juce::Justification::topLeft, 3);

    // Soloed band: a lit frame round the button that is soloing it.
    {
        const int soloed = proc.engine.monitor.soloBand.load();
        const bool curve = proc.engine.monitor.soloKind.load() == (int) SoloKind::curve;
        if (soloed == band && bandOn)
        {
            g.setColour (colours::text);
            g.drawRoundedRectangle ((curve ? soloCurveButton : soloBandButton).getBounds().toFloat().expanded (2.0f), 4.0f, 1.2f);
        }
    }

    // Dynamics panel: what the band hears and what it does about it, then the explanation.
    {
        const auto dyn = panelBounds[(size_t) Panel::dynamics];
        hw::drawGlass (g, dynReadout.toFloat(), 3.0f, colours::amber);
        if (dynamicOn)
        {
            const float live = proc.engine.dynamicDb[band].load (std::memory_order_relaxed);
            const float level = proc.engine.detectedDb[band].load (std::memory_order_relaxed);
            const auto bar = dynReadout.toFloat().reduced (6.0f, 8.0f).withTrimmedLeft (226.0f);
            g.setColour (colours::glassGrid);
            g.fillRoundedRectangle (bar, 2.0f);
            const float range = juce::jmax (1.0f, std::fabs ((float) b.dyn.rangeDb));
            const float len = juce::jlimit (-1.0f, 1.0f, live / range) * bar.getWidth() * 0.5f;
            g.setColour (live < 0.0f ? colours::accent : colours::amber);
            g.fillRect (len < 0.0f ? bar.getCentreX() + len : bar.getCentreX(), bar.getY(), std::fabs (len), bar.getHeight());
            g.setColour (colours::dim);
            g.drawVerticalLine (juce::roundToInt (bar.getCentreX()), bar.getY() - 2.0f, bar.getBottom() + 2.0f);
            g.setColour (colours::amber);
            g.setFont (ui::lcdFont (11.5f));
            g.drawText ("LEVEL " + (level < -100.0f ? juce::String ("---") : juce::String (level, 1)) + " dB   GAIN " + (live >= 0.0f ? "+" : "") + juce::String (live, 1) + " dB",
                        dynReadout.reduced (8, 0), juce::Justification::centredLeft, false);
        }
        else
        {
            g.setColour (colours::dim);
            g.setFont (ui::lcdFont (11.5f));
            g.drawText ("DYNAMICS OFF", dynReadout.reduced (8, 0), juce::Justification::centredLeft, false);
        }
        hw::drawGlassOverlay (g, dynReadout.toFloat(), 3.0f, false);

        g.setColour (colours::dim);
        g.setFont (juce::FontOptions (11.5f));
        g.drawFittedText (dynamicsHint(), dyn.getX() + 18, dyn.getY() + 66 + (hasGain (b.type) && bandOn ? cellHeight : 0) + 2, dyn.getWidth() - 36,
                          hasGain (b.type) && bandOn ? 40 : 60, juce::Justification::topLeft, 3);
    }

    // Output panel.
    {
        const auto out = panelBounds[(size_t) Panel::output];
        const bool satChosen = proc.apvts.getRawParameterValue (id::mode)->load() > 0.5f;
        const int latency = proc.engine.latencySamples.load (std::memory_order_relaxed);
        const bool linear = latency > 0;
        const bool sat = satChosen && ! linear; // SAT only runs in Zero Latency (the shaper is not a linear filter)
        hw::drawLabel (g, "Phase", { phaseBox.getX() + 2, phaseBox.getY() - 14, 100, 12 }, juce::Justification::left, colours::dim, 9.0f);
        hw::drawLabel (g, "Style", { styleBox.getX() + 2, styleBox.getY() - 14, 100, 12 }, juce::Justification::left, colours::dim, 9.0f);
        hw::drawLamp (g, { (float) out.getRight() - 24.0f, (float) out.getY() + 17.0f }, 4.0f, colours::amber, sat);
        hw::drawLabel (g, sat ? "SAT" : satChosen ? "SAT OFF" : "CLEAN", { out.getRight() - 90, out.getY() + 10, 56, 14 }, juce::Justification::right, sat ? colours::amber : colours::dim, 9.5f);
        g.setColour (colours::dim);
        g.setFont (juce::FontOptions (11.5f));
        const auto hint = linear ? "Delay " + juce::String (latency) + " samples, " + juce::String (juce::roundToInt (1000.0 * latency / proc.currentSampleRate)) + " ms"
                                 : juce::String ("No delay");
        g.drawFittedText (hint, out.getX() + 18, out.getY() + 80, out.getWidth() - 36, 14, juce::Justification::topLeft, 1);
    }

    // Level meters: two bars in one window of glass, green until -6 dBFS, amber to -0.5, red above.
    hw::drawGlass (g, meterArea.toFloat(), 3.0f, colours::amber);
    const auto drawMeter = [&] (juce::Rectangle<int> r, float level, const juce::String& name)
    {
        hw::drawLabel (g, name, r.removeFromLeft (26), juce::Justification::centredRight, colours::dim, 8.5f);
        r.removeFromLeft (4);
        g.setColour (colours::glassGrid);
        g.fillRoundedRectangle (r.toFloat(), 2.0f);

        const float db = juce::Decibels::gainToDecibels (level, -60.0f);
        const float fill = juce::jlimit (0.0f, 1.0f, (db + 60.0f) / 60.0f);
        g.setColour (db > -0.5f ? colours::bad : (db > -6.0f ? colours::warn : colours::good));
        g.fillRoundedRectangle (r.toFloat().withWidth ((float) r.getWidth() * fill), 2.0f);
    };
    auto m = meterArea.reduced (8, 7);
    drawMeter (m.removeFromTop (m.getHeight() / 2 - 2), inLevel, "IN");
    m.removeFromTop (4);
    drawMeter (m, outLevel, "OUT");
    hw::drawGlassOverlay (g, meterArea.toFloat(), 3.0f, false);
}

void IrisEditor::trackUsage()
{
    for (int b = 0; b < maxBands; ++b)
    {
        const auto& ids = bandIds (b);
        const auto value = [&] (const juce::String& id) { return proc.apvts.getRawParameterValue (id)->load(); };
        const bool dynamic = isBandOn (proc.apvts, b) && hasGain (bandType (proc.apvts, b)) && value (ids.dyn) > 0.5f;
        const float key = dynamic ? value (ids.threshold) * 7.0f + value (ids.range) * 131.0f + value (ids.dynMode) * 977.0f
                                    + value (ids.ratio) * 3.0f + std::log (value (ids.freq)) * 59.0f
                                  : -1.0e9f;
        if (std::fabs (key - usageKey[(size_t) b]) > 1.0e-3f)
        {
            usageKey[(size_t) b] = key;
            usage[(size_t) b].reset();
        }
        if (dynamic)
            usage[(size_t) b].push (proc.engine.detectedDb[b].load (std::memory_order_relaxed),
                                    proc.engine.dynamicDb[b].load (std::memory_order_relaxed), value (ids.range), 1.0 / kTimerHz);
    }
}

void IrisEditor::timerCallback()
{
    trackUsage();
    inLevel = proc.engine.inPeak.load (std::memory_order_relaxed);
    outLevel = proc.engine.outPeak.load (std::memory_order_relaxed);

    // The solo buttons show what is soloed, whoever asked for it (the menu on a node does too).
    const int soloed = proc.engine.monitor.soloBand.load(), kind = proc.engine.monitor.soloKind.load();
    soloBandButton.setToggleState (soloed == view.getSelectedBand() && kind == (int) SoloKind::band, juce::dontSendNotification);
    soloCurveButton.setToggleState (soloed == view.getSelectedBand() && kind == (int) SoloKind::curve, juce::dontSendNotification);
    deltaButton.setToggleState (proc.engine.monitor.delta.load(), juce::dontSendNotification);
    learnRefButton.setButtonText (view.isLearning() ? "STOP  " + juce::String ((int) view.learnedSeconds()) + " s" : "LEARN REF");
    learnRefButton.setToggleState (view.isLearning(), juce::dontSendNotification);

    if (bandSignature() != shownSignature)
    {
        refreshBandPanel();
        layoutBandPanel();
    }
    repaint();

    if (const int prog = proc.getCurrentProgram(); prog != lastProgram)
    {
        lastProgram = prog;
        presetBox.setSelectedId (prog + 1, juce::dontSendNotification);
    }
}
