// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 The Pantheon authors

#include "PluginEditor.h"

using namespace hep;
using namespace hep::gui;

namespace
{
constexpr int editorWidth = 1040, editorHeight = 624;
constexpr int headerHeight = 84;   // the control panel along the top
constexpr int cellHeight = 104;    // a knob: name, knob, read-out
constexpr int panelRowHeight = 284;
constexpr int chipWidth = 34, chipHeight = 26, chipGap = 6;

// Everything that varies between the fixed panels, in one place.
struct PanelSpec
{
    const char* title;
    const char* subtitle;
    int columns;
    int firstColumn; // the first knob's column (a column before it is taken by a combo box)
    int knobTop;     // y offset of the first knob row inside the panel
};

const PanelSpec panelSpecs[] =
{
    { "BAND",      "",                                 6, 0, 126 },
    { "CHARACTER", "shared by all saturate bands",     2, 1, 26 },
    { "MASTER",    "",                                 3, 0, 26 },
};

// Knobs of the character and master panels: which panel, its parameter, caption and tooltip. They
// fill a panel left to right in table order.
struct KnobSpec
{
    int panel; // index into panelSpecs
    const char* paramId;
    const char* caption;
    const char* tooltip;
};

const KnobSpec knobSpecs[] =
{
    { 1, id::even, "EVEN", "0% = symmetric curve, odd harmonics only (3rd, 5th...). Higher = asymmetric, adds 2nd, 4th... (tube-like warmth)." },

    { 2, id::inGain,  "INPUT",  "Input gain. Drives the saturate bands harder without changing the dry reference." },
    { 2, id::mix,     "MIX",    "Parallel mix of processed and dry (dry is latency-compensated)." },
    { 2, id::outGain, "OUTPUT", "Output gain." },
};

// The knobs of the selected band, by mode. They share six slots that are re-attached to the band's
// parameters whenever the selection or the mode changes.
struct BandKnobSpec
{
    BandMode mode;
    juce::String BandIds::* field;
    const char* caption;
    const char* tooltip;
};

const BandKnobSpec bandKnobSpecs[] =
{
    { BandMode::saturate, &BandIds::lo,    "LOW",   "Lower edge of the band. Also set by dragging its edge in the display." },
    { BandMode::saturate, &BandIds::hi,    "HIGH",  "Upper edge of the band. Also set by dragging its edge in the display." },
    { BandMode::saturate, &BandIds::drive, "DRIVE", "How hard the band is driven into the curve. More drive = more harmonics. Small signals stay untouched." },

    { BandMode::fill, &BandIds::lo,      "LOW",     "Lowest fundamental to work on. Also set by dragging the band's edge in the display." },
    { BandMode::fill, &BandIds::hi,      "HIGH",    "Highest fundamental to work on (fill bands stop at 2 kHz)." },
    { BandMode::fill, &BandIds::amount,  "AMOUNT",  "How much harmonic energy the band should have above it, relative to its own energy. Off at the minimum." },
    { BandMode::fill, &BandIds::tone,    "TONE",    "Which harmonics carry the energy. Dark = mostly the 2nd. Middle = the usual roll-off. Bright = led by the 3rd, with 4th and 5th present." },
    { BandMode::fill, &BandIds::adapt,   "ADAPT",   "0% = always add AMOUNT. 100% = only add what is missing: the energy just above the band is compared with the band's own, and only the shortfall to AMOUNT is generated. Watch the FILL % on the band." },
    { BandMode::fill, &BandIds::rebuild, "REBUILD", "Also rebuilds a fundamental that is missing inside the band while its harmonics are present (difference tone, 3f - 2f = f). Off at the minimum. Experimental on dense material." },
};

const char* modeName (BandMode mode) { return mode == BandMode::fill ? "HARMONIC FILL" : "SATURATE"; }
}

HephaestusEditor::HephaestusEditor (HephaestusProcessor& p)
    : AudioProcessorEditor (&p), proc (p),
      spectrum (p.engine, p.apvts, p.currentSampleRate),
      stereoBox (p.apvts, id::stereo,
                 "Mid/Side saturates the centre and sides together; harmonic fill bands then work on the mid channel only, keeping bass centred."),
      osBox (p.apvts, id::oversampling,
             "Oversampling for the saturate bands (linear phase). Reduces aliasing; adds latency, which is reported to the host."),
      curveBox (p.apvts, id::curve,
                "Waveshaper curve for the saturate bands.\nTanh: smooth, tube / transformer-like.\nAtan: more open, tape-like.\n"
                "Soft: algebraic knee, gritty, strongest upper odd harmonics."),
      deltaButton (p.apvts, id::delta, "DELTA",
                   "Listen to only what the plugin adds (wet minus dry). Use it to hear exactly what the harmonics are doing."),
      matchButton (p.apvts, id::match, "GAIN MATCH",
                   "Matches the processed loudness to the dry signal so 'louder' doesn't sound 'better'.")
{
    setLookAndFeel (&look);
    addAndMakeVisible (spectrum);

    for (auto& spec : knobSpecs)
        addKnob (spec.paramId, spec.caption, spec.tooltip);

    for (auto& slot : bandKnobs)
    {
        slot = std::make_unique<ui::Knob> (ui::Knob::Layout::below);
        addChildComponent (*slot);
    }

    // Band panel buttons.
    saturateModeButton.setButtonText ("SATURATE");
    fillModeButton.setButtonText ("HARMONIC FILL");
    removeButton.setButtonText ("REMOVE");
    addSaturateButton.setButtonText ("+ SATURATE BAND");
    addFillButton.setButtonText ("+ HARMONIC FILL BAND");
    saturateModeButton.setColour (juce::TextButton::buttonOnColourId, forMode (BandMode::saturate));
    fillModeButton.setColour (juce::TextButton::buttonOnColourId, forMode (BandMode::fill));
    addFillButton.setColour (juce::TextButton::buttonOnColourId, forMode (BandMode::fill));
    saturateModeButton.setTooltip ("Add harmonics by driving this band into a waveshaper (uses Curve and Even).");
    fillModeButton.setTooltip ("Add harmonics above this band, only as much as is missing (see ADAPT).");
    removeButton.setTooltip ("Free this band slot.");
    saturateModeButton.onClick = [this] { setBandMode (proc.apvts, spectrum.getSelectedBand(), BandMode::saturate); };
    fillModeButton.onClick = [this] { setBandMode (proc.apvts, spectrum.getSelectedBand(), BandMode::fill); };
    removeButton.onClick = [this] { removeBand (proc.apvts, spectrum.getSelectedBand()); };
    addSaturateButton.onClick = [this] { addBand (proc.apvts, BandMode::saturate, 800.0, spectrum.getSelectedBand()); };
    addFillButton.onClick = [this] { addBand (proc.apvts, BandMode::fill, 80.0, spectrum.getSelectedBand()); };
    pasteButton.setTooltip ("Add the harmonic-fill bands another plugin of the suite copied (Themis: COPY on a bass item), each in the next free "
                            "slot, over the range it measured.");
    pasteButton.onClick = [this]
    {
        int skipped = 0;
        const auto slots = pasteBands (proc.apvts, juce::SystemClipboard::getTextFromClipboard(), skipped);
        if (! slots.empty())
            spectrum.setSelectedBand (slots.front());
        else
            juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::InfoIcon, "Paste",
                                                    skipped > 0 ? "No free slot, or no harmonic-fill band to add (equaliser bands go to Iris)."
                                                                : "The clipboard holds no bands. Copy them in Themis first (COPY on a plan item).");
    };
    addAndMakeVisible (pasteButton);
    for (auto* b : { &saturateModeButton, &fillModeButton, &removeButton, &addSaturateButton, &addFillButton })
        addChildComponent (b);

    // Header.
    for (int i = 0; i < (int) presets().size(); ++i)
        presetBox.addItem (presets()[(size_t) i].name, i + 1);
    presetBox.setSelectedId (proc.getCurrentProgram() + 1, juce::dontSendNotification);
    presetBox.onChange = [this] { proc.setCurrentProgram (presetBox.getSelectedId() - 1); };
    presetBox.setTooltip ("Presets. Global settings (gain, mix, stereo mode, oversampling) are kept.");
    addAndMakeVisible (presetBox);

    clearMetersButton.setTooltip ("Clear the IN and OUT level meters. Use it if a reading looks stuck.");
    clearMetersButton.onClick = [this]
    {
        proc.engine.clearMeters();
        inLevel = outLevel = 0.0f;
        repaint (meterArea.expanded (24, 4));
    };

    for (auto* c : std::initializer_list<juce::Component*> { &stereoBox, &osBox, &curveBox, &deltaButton, &matchButton, &clearMetersButton })
        addAndMakeVisible (c);

    // The first band that exists is the one to show.
    for (int i = 0; i < maxBands; ++i)
        if (isBandOn (proc.apvts, i))
        {
            spectrum.setSelectedBand (i);
            break;
        }
    refreshBandPanel();

    // Size last: resized() lays out the controls, which must all exist by then.
    setSize (editorWidth, editorHeight);
    startTimerHz (30);
}

HephaestusEditor::~HephaestusEditor()
{
    setLookAndFeel (nullptr);
}

void HephaestusEditor::addKnob (const char* paramId, const char* caption, const char* tooltip)
{
    auto knob = std::make_unique<ui::Knob> (proc.apvts, paramId, caption, tooltip, ui::Knob::Layout::below);
    addAndMakeVisible (*knob);
    knobs.push_back (std::move (knob));
}

// What changes the band panel: which band, whether it exists, and its mode.
int HephaestusEditor::bandSignature() const
{
    int signature = spectrum.getSelectedBand();
    for (int i = 0; i < maxBands; ++i)
        signature = signature * 3 + (isBandOn (proc.apvts, i) ? 1 + (int) bandMode (proc.apvts, i) : 0);
    return signature;
}

// Points the six knob slots and the buttons at the selected band.
void HephaestusEditor::refreshBandPanel()
{
    shownBandSignature = bandSignature();
    const int band = spectrum.getSelectedBand();
    const bool on = isBandOn (proc.apvts, band);
    const auto mode = bandMode (proc.apvts, band);

    bandKnobCount = 0;
    for (auto& spec : bandKnobSpecs)
    {
        if (! on || spec.mode != mode)
            continue;

        auto& knob = *bandKnobs[(size_t) bandKnobCount++];
        knob.setCaption (spec.caption);
        knob.setKnobTooltip (spec.tooltip);
        knob.bind (proc.apvts, bandIds (band).*spec.field);
    }

    for (int k = 0; k < maxBands; ++k)
    {
        auto& knob = *bandKnobs[(size_t) k];
        if (k >= bandKnobCount)
            knob.unbind();
        knob.setVisible (k < bandKnobCount);
    }

    saturateModeButton.setToggleState (mode == BandMode::saturate, juce::dontSendNotification);
    fillModeButton.setToggleState (mode == BandMode::fill, juce::dontSendNotification);
    for (auto* b : { &saturateModeButton, &fillModeButton, &removeButton })
        b->setVisible (on);
    for (auto* b : { &addSaturateButton, &addFillButton })
        b->setVisible (! on);
}

juce::String HephaestusEditor::bandHint() const
{
    const int band = spectrum.getSelectedBand();
    if (! isBandOn (proc.apvts, band))
        return "Slot " + juce::String (band + 1) + " is free. Add a band here, or double-click the display where you want one.";

    if (bandMode (proc.apvts, band) == BandMode::saturate)
        return "Drives the sound inside the band into the curve, so harmonics appear above it (the shaded zone, 2nd to 5th harmonic). "
               "Anything outside the band is left exactly as it is.";

    return "Adds harmonics above the band (the shaded zone, 2nd to 5th harmonic), but only as many as are missing: the energy just above the band is "
           "measured against the band's own, and only the shortfall to AMOUNT is generated. FILL % on the band shows how much is being added.";
}

void HephaestusEditor::layoutBandPanel()
{
    const auto panel = panelBounds[(size_t) Panel::band];

    for (int i = 0; i < maxBands; ++i)
        chipBounds[(size_t) i] = { panel.getX() + 14 + i * (chipWidth + chipGap), panel.getY() + 34, chipWidth, chipHeight };

    pasteButton.setBounds (panel.getRight() - 14 - 64, panel.getY() + 6, 64, 20);
    auto buttons = juce::Rectangle<int> (panel.getX(), panel.getY() + 34, panel.getWidth() - 14, chipHeight);
    removeButton.setBounds (buttons.removeFromRight (76));
    buttons.removeFromRight (8);
    fillModeButton.setBounds (buttons.removeFromRight (124));
    buttons.removeFromRight (6);
    saturateModeButton.setBounds (buttons.removeFromRight (90));

    // A free slot offers the two ways to fill it.
    auto offer = juce::Rectangle<int> (panel.getX() + 14, panel.getY() + 118, panel.getWidth() - 28, 44);
    addSaturateButton.setBounds (offer.removeFromLeft (offer.getWidth() / 2 - 6));
    offer.removeFromLeft (12);
    addFillButton.setBounds (offer);

    const auto& spec = panelSpecs[(size_t) Panel::band];
    const auto area = panel.reduced (8, 0).withTrimmedTop (spec.knobTop);
    const int cellW = area.getWidth() / spec.columns;
    for (int k = 0; k < bandKnobCount; ++k)
    {
        bandKnobs[(size_t) k]->setBounds (area.getX() + k * cellW, area.getY(), cellW, cellHeight);
    }
}

void HephaestusEditor::layoutPanelKnobs (Panel which)
{
    const auto& spec = panelSpecs[(size_t) which];
    const auto area = panelBounds[(size_t) which].reduced (8, 0).withTrimmedTop (spec.knobTop);
    const int cellW = area.getWidth() / spec.columns;

    int index = spec.firstColumn;
    for (size_t k = 0; k < knobs.size(); ++k)
    {
        if (knobSpecs[k].panel != (int) which)
            continue;

        knobs[k]->setBounds (area.getX() + (index % spec.columns) * cellW, area.getY() + (index / spec.columns) * cellHeight, cellW, cellHeight);
        ++index;
    }
}

void HephaestusEditor::resized()
{
    auto bounds = getLocalBounds().reduced (12, 0);

    // Header: the nameplate, the selectors and switches, and the level meters in a window of glass.
    auto header = bounds.removeFromTop (headerHeight).withTrimmedTop (8).withTrimmedBottom (8);
    nameplate = header.removeFromLeft (250);
    header.removeFromLeft (24);
    meterArea = header.removeFromRight (120).reduced (0, 10);
    header.removeFromRight (8);
    {
        // The clear button sits right next to the meters it clears; the row is tight at the fixed editor width.
        auto h = header.withTrimmedTop (28).withHeight (28);
        clearMetersButton.setBounds (h.removeFromRight (64));
        h.removeFromRight (10);
        for (auto [component, width, gap] : { std::tuple<juce::Component*, int, int> { &presetBox, 158, 10 },
                                              { &stereoBox, 112, 10 }, { &osBox, 72, 12 },
                                              { &deltaButton, 64, 8 }, { &matchButton, 96, 0 } })
        {
            component->setBounds (h.removeFromLeft (width));
            h.removeFromLeft (gap);
        }
    }

    bounds.removeFromTop (10);
    spectrum.setBounds (bounds.removeFromTop (224));
    bounds.removeFromTop (10);

    // Panels: the selected band | character over master.
    auto row = bounds.removeFromTop (panelRowHeight);
    panelBounds[(size_t) Panel::band] = row.removeFromLeft (690);
    row.removeFromLeft (12);
    panelBounds[(size_t) Panel::character] = row.removeFromTop ((panelRowHeight - 8) / 2);
    row.removeFromTop (8);
    panelBounds[(size_t) Panel::master] = row;

    // Character panel: the curve sits in the first column, ahead of the EVEN knob, level with the knobs' faces.
    {
        const auto& spec = panelSpecs[(size_t) Panel::character];
        const auto area = panelBounds[(size_t) Panel::character].reduced (8, 0).withTrimmedTop (spec.knobTop);
        curveBox.setBounds (area.getX() + 10, area.getY() + 48, area.getWidth() / spec.columns - 20, 28);
    }

    layoutBandPanel();
    layoutPanelKnobs (Panel::character);
    layoutPanelKnobs (Panel::master);
}

void HephaestusEditor::selectBand (int band)
{
    spectrum.setSelectedBand (band);
    refreshBandPanel();
    layoutBandPanel();
    repaint();
}

void HephaestusEditor::mouseDown (const juce::MouseEvent& e)
{
    for (int i = 0; i < maxBands; ++i)
        if (chipBounds[(size_t) i].contains (e.getPosition()))
            selectBand (i);
}

void HephaestusEditor::paint (juce::Graphics& g)
{
    namespace hw = ui::hw;

    hw::fillMetal (g, getLocalBounds().toFloat(), colours::background);
    hw::drawHeaderPlate (g, juce::Rectangle<float> (0.0f, 0.0f, (float) getWidth(), (float) headerHeight));
    hw::drawNameplate (g, nameplate, gui::kProjectWord, gui::kPluginWord, "Harmonic saturator", inLevel > 0.001f, 21.0f);

    // Small captions above the header selectors.
    for (auto [box, caption] : { std::pair<juce::Component*, const char*> { &presetBox, "Preset" },
                                 { &stereoBox, "Stereo mode" }, { &osBox, "Oversampling" } })
        hw::drawLabel (g, caption, { box->getX() + 2, box->getY() - 14, 110, 12 }, juce::Justification::left, colours::dim, 9.0f);

    // Panels.
    const int band = spectrum.getSelectedBand();
    const bool bandOn = isBandOn (proc.apvts, band);
    const auto mode = bandMode (proc.apvts, band);

    for (size_t i = 0; i < panelBounds.size(); ++i)
    {
        const auto r = panelBounds[i];
        hw::drawModule (g, r.toFloat());

        juce::String title = panelSpecs[i].title;
        if (i == (size_t) Panel::band)
            title << " " << (band + 1) << (bandOn ? juce::String ("  -  ") + modeName (mode) : "  -  FREE");

        hw::drawLabel (g, title, { r.getX() + 18, r.getY() + 10, 300, 14 }, juce::Justification::left,
                       i == (size_t) Panel::band && bandOn ? forMode (mode) : colours::text, 10.5f);
        hw::drawLabel (g, panelSpecs[i].subtitle, { r.getX() + 14, r.getY() + 10, r.getWidth() - 32, 14 }, juce::Justification::right, colours::dim, 9.0f);
    }

    {
        const auto& spec = panelSpecs[(size_t) Panel::character];
        hw::drawLabel (g, "Curve", { curveBox.getX(), panelBounds[(size_t) Panel::character].getY() + spec.knobTop, curveBox.getWidth(), 16 },
                       juce::Justification::centred, colours::dim, 9.5f);
    }

    // Band chips: one push switch per slot, lit in the band's colour when the band exists; the selected one is ringed.
    for (int i = 0; i < maxBands; ++i)
    {
        const auto r = chipBounds[(size_t) i].toFloat();
        const bool on = isBandOn (proc.apvts, i);
        const auto c = forMode (bandMode (proc.apvts, i));

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

    // What the selected band does.
    {
        const auto bandPanel = panelBounds[(size_t) Panel::band];
        g.setColour (colours::dim);
        g.setFont (juce::FontOptions (11.5f));
        g.drawFittedText (bandHint(), bandPanel.getX() + 18, bandPanel.getY() + 74, bandPanel.getWidth() - 36, 36,
                          juce::Justification::topLeft, 2);
    }

    // Level meters: two bars in one window of glass, green until -6 dBFS, amber to -0.5, red above (a lamp's verdict colours).
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

void HephaestusEditor::timerCallback()
{
    inLevel = proc.engine.inPeak.load (std::memory_order_relaxed);
    outLevel = proc.engine.outPeak.load (std::memory_order_relaxed);
    repaint (meterArea.expanded (24, 4));

    if (bandSignature() != shownBandSignature)
    {
        refreshBandPanel();
        layoutBandPanel();
        repaint (panelBounds[(size_t) Panel::band]);
    }

    if (const int prog = proc.getCurrentProgram(); prog != lastProgram)
    {
        lastProgram = prog;
        presetBox.setSelectedId (prog + 1, juce::dontSendNotification);
    }
}
