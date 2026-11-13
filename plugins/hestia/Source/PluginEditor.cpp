// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 The Pantheon authors

#include "PluginEditor.h"

using namespace hestia;
namespace hw = ui::hw;

namespace
{
constexpr int kWidth = 1110;
constexpr int headerHeight = 84;
constexpr int viewHeight = 300;
constexpr int knobPanelHeight = 134;
constexpr int cellHeight = 104;
constexpr int kTimerHz = 30;
}

HestiaEditor::HestiaEditor (HestiaProcessor& p)
    : AudioProcessorEditor (&p),
      roleBox (p.apvts, id::role, "What this stem is. It sets which bands it has a claim on: a kick's thump, a bass's low end, a voice's presence. "
                                  "These are rules of thumb: IMPORTANCE moves a whole stem up or down."),
      mixBox (p.apvts, id::mix, "Which stems see each other. Give every stem of one mix the same letter. Off: this stem is alone and nothing is cut. "
                                "Stems see each other only inside one host process."),
      scRoleBox (p.apvts, id::scRole, "What the track on the SIDECHAIN input is (a track with no Hestia on it, routed in the host): it is one more stem of the mix, "
                                      "with this role. It is only listened to, never heard."),
      importance (p.apvts, id::importance, "IMPORTANCE", "How much this stem matters in the mix. 50% leaves the role's claim as it is; more makes other stems give way to it, "
                                                         "less makes it give way to them.", ui::Knob::Layout::below),
      distance (p.apvts, id::distance, "DEPTH", "How far back the stem sits: 0% at the front, 100% at the back. The air takes the highs (to -9 dB), the level falls (to -4 dB), "
                                                "the sides narrow (to 70%) and the first reflections of a room come up (up to 37 ms, no tail). A stem at the back also gives way "
                                                "to the ones in front.", ui::Knob::Layout::below),
      carve (p.apvts, id::carve, "CARVE", "The most any band is cut, when a stem with a bigger claim is playing there as loud as this one.", ui::Knob::Layout::below),
      reach (p.apvts, id::reach, "REACH", "How much quieter than this stem another can be and still be given room. 0 dB: only when it is as loud or louder; "
                                          "wider cuts more.", ui::Knob::Layout::below),
      attack (p.apvts, id::attack, "ATTACK", "How fast a cut comes in.", ui::Knob::Layout::below),
      release (p.apvts, id::release, "RELEASE", "How fast a cut goes out when the other stem stops.", ui::Knob::Layout::below),
      outKnob (p.apvts, id::out, "OUTPUT", "Output gain, after everything."),
      scImportance (p.apvts, id::scImportance, "SC IMPORT", "How much the track on the sidechain matters, as IMPORTANCE does for a stem.", ui::Knob::Layout::beside),
      presetBox (p, presets(), "The mix letter, the sidechain's role and importance, and OUTPUT are kept."),
      view (p),
      proc (p)
{
    setLookAndFeel (&look);
    deltaButton.setClickingTogglesState (false);
    deltaButton.setTooltip ("Listen to only what Hestia changes in this stem: the stem as it comes out minus as it came in (the cut and the depth). Not saved with the session.");
    deltaButton.onClick = [this] { proc.engine.deltaOn = ! proc.engine.deltaOn.load(); };
    for (auto* c : std::initializer_list<juce::Component*> { &view, &roleBox, &mixBox, &scRoleBox, &importance, &distance, &carve, &reach, &attack, &release, &outKnob, &scImportance,
                                                              &deltaButton, &presetBox, &guideButton })
        addAndMakeVisible (*c);

    startTimerHz (kTimerHz);
    setSize (kWidth, headerHeight + 10 + viewHeight + 10 + knobPanelHeight + 8 + ui::kNoteStripHeight + 12);
}

HestiaEditor::~HestiaEditor()
{
    proc.engine.deltaOn = false; // monitoring never outlives the window that asked for it
    setLookAndFeel (nullptr);
}

void HestiaEditor::resized()
{
    auto bounds = getLocalBounds().reduced (12, 0);
    auto header = bounds.removeFromTop (headerHeight).withTrimmedTop (8).withTrimmedBottom (8);
    nameplate = header.removeFromLeft (250);
    header.removeFromLeft (24);
    outKnob.setBounds (header.removeFromRight (170).withSizeKeepingCentre (170, 52));
    header.removeFromRight (16);
    auto row = header.withTrimmedTop (28).withHeight (28);
    roleLabel = { row.getX() + 2, row.getY() - 15, 120, 12 };
    roleBox.setBounds (row.removeFromLeft (180));
    row.removeFromLeft (14);
    mixLabel = { row.getX() + 2, row.getY() - 15, 120, 12 };
    mixBox.setBounds (row.removeFromLeft (90));
    row.removeFromLeft (14);
    presetLabel = { row.getX() + 2, row.getY() - 15, 120, 12 };
    presetBox.setBounds (row.removeFromLeft (150));
    row.removeFromLeft (14);
    deltaButton.setBounds (row.removeFromLeft (96));
    guideButton.setBounds (row.removeFromRight (64));

    bounds.removeFromTop (10);
    view.setBounds (bounds.removeFromTop (viewHeight));
    bounds.removeFromTop (10);
    knobPanel = bounds.removeFromTop (knobPanelHeight);
    auto cells = knobPanel.reduced (24, 12);
    scPanel = cells.removeFromRight (190);
    cells.removeFromRight (16);
    const int cell = cells.getWidth() / 6;
    for (auto* k : { &importance, &distance, &carve, &reach, &attack, &release })
        k->setBounds (cells.removeFromLeft (cell).withHeight (cellHeight).reduced (10, 0));
    auto sc = scPanel.withTrimmedLeft (12);
    scLabel = { sc.getX() + 2, sc.getY() + 2, sc.getWidth(), 12 };
    scRoleBox.setBounds (sc.withTrimmedTop (20).removeFromTop (28));
    scImportance.setBounds (sc.withTrimmedTop (62).removeFromTop (48));
    noteStrip = bounds.withTrimmedTop (8).withHeight (ui::kNoteStripHeight);
}

AdviceInput HestiaEditor::adviceInput() const
{
    AdviceInput in;
    in.params = readParams (proc.apvts);
    in.others = proc.engine.linkedStems(); // the stems of the bus and the sidechain: what the engine found
    for (int j = 0; j < view.otherStems(); ++j)
    {
        const auto& other = view.stems()[(size_t) j];
        if (other.role == in.params.role && std::fabs (other.importance - in.params.importance) <= kSameImportance)
            ++in.sameClaim;
    }
    in.strongestCutDb = view.strongestCutDb();
    in.delta = proc.engine.deltaOn.load();
    in.sidechain = proc.sidechainConnected.load (std::memory_order_relaxed);
    return in;
}

void HestiaEditor::paint (juce::Graphics& g)
{
    g.fillAll (colours::background);
    hw::drawHeaderPlate (g, getLocalBounds().removeFromTop (headerHeight).toFloat());
    hw::drawNameplate (g, nameplate, gui::kProjectWord, gui::kPluginWord, "Stem spacer", inLevel > 0.001f, 25.0f);
    hw::drawLabel (g, "Preset", presetLabel, juce::Justification::left, colours::dim, 9.0f);
    hw::drawLabel (g, "Role", roleLabel, juce::Justification::left, colours::dim, 9.0f);
    hw::drawLabel (g, "Mix", mixLabel, juce::Justification::left, colours::dim, 9.0f);
    hw::drawModule (g, knobPanel.toFloat());
    g.setColour (colours::panelEdge);
    g.fillRect (scPanel.getX(), scPanel.getY() + 6, 1, scPanel.getHeight() - 12);
    hw::drawLabel (g, "Sidechain input", scLabel, juce::Justification::left, proc.sidechainConnected.load (std::memory_order_relaxed) ? colours::amber : colours::dim, 9.0f);
    ui::drawNoteStrip (g, noteStrip, adviceFor (adviceInput()));
}

void HestiaEditor::timerCallback()
{
    deltaButton.setToggleState (proc.engine.deltaOn.load(), juce::dontSendNotification);
    view.tick();
    float level = kFloorDb;
    for (const auto& l : proc.engine.ownLevel)
        level = std::fmax (level, l.load (std::memory_order_relaxed));
    inLevel = level > -80.0f ? 1.0f : 0.0f;
    presetBox.sync();
    repaint();
}
