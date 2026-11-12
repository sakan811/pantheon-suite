// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 The Pantheon authors

// Drives the real display component with synthetic mouse events and checks what lands in the parameters and the monitor:
// adding a band, dragging a node, the wheel, the frequency-region strip (solo, crossover handles), and what the editor shows for
// each kind of band. Headless; no window is shown.
//
//   IrisUiCheck

#include "Check.h"
#include "Advice.h"
#include "PluginEditor.h"
#include <pantheon/ui/Scaled.h>
#include "PluginProcessor.h"
#include "TestSignals.h"

#include <functional>

using namespace iris;
using namespace test;

namespace
{
constexpr int viewWidth = 1156, viewHeight = 322;
constexpr float stripY = 6.0f + 322.0f - 12.0f - 12.0f; // the middle of the region strip: the view's content, less its last 24 px

float xOf (double hz) { return 6.0f + gui::FreqAxis::toNormalised ((float) hz) * (float) (viewWidth - 12); }

// The plot is the view inset by 6 px, less the strip (24 px) and a 4 px gap.
constexpr float plotTop = 6.0f, plotHeight = viewHeight - 12.0f - 28.0f;
float yOfGain (double db, double scale = 12.0) { return plotTop + plotHeight * 0.5f - (float) (db / scale) * plotHeight * 0.5f; }

float value (IrisProcessor& p, const juce::String& id) { return p.apvts.getRawParameterValue (id)->load(); }

juce::MouseEvent mouse (juce::Component& c, juce::Point<float> at, juce::Point<float> downAt, bool dragged, juce::ModifierKeys mods = {})
{
    return { juce::Desktop::getInstance().getMainMouseSource(), at, mods,
             juce::MouseInputSource::defaultPressure, juce::MouseInputSource::defaultOrientation,
             juce::MouseInputSource::defaultRotation, juce::MouseInputSource::defaultTiltX,
             juce::MouseInputSource::defaultTiltY, &c, &c, juce::Time::getCurrentTime(),
             downAt, juce::Time::getCurrentTime(), 1, dragged };
}

// A full press-drag-release gesture from one point to another.
void dragFrom (juce::Component& view, juce::Point<float> from, juce::Point<float> to, juce::ModifierKeys mods = {})
{
    view.mouseDown (mouse (view, from, from, false, mods));
    for (int step = 1; step <= 8; ++step)
        view.mouseDrag (mouse (view, from + (to - from) * ((float) step / 8.0f), from, true, mods));
    view.mouseUp (mouse (view, to, from, true, mods));
}

void click (juce::Component& view, juce::Point<float> at, juce::ModifierKeys mods = {})
{
    view.mouseDown (mouse (view, at, at, false, mods));
    view.mouseUp (mouse (view, at, at, false, mods));
}

void wheel (gui::EqView& view, juce::Point<float> at, float deltaY)
{
    juce::MouseWheelDetails details;
    details.deltaX = 0.0f;
    details.deltaY = deltaY;
    details.isReversed = false;
    details.isSmooth = false;
    details.isInertial = false;
    view.mouseWheelMove (mouse (view, at, at, false), details);
}

void pump (int ms) { juce::MessageManager::getInstance()->runDispatchLoopUntil (ms); }

juce::TextButton* findButton (juce::Component& parent, const juce::String& text)
{
    for (auto* child : parent.getChildren())
        if (auto* b = dynamic_cast<juce::TextButton*> (child))
            if (b->getButtonText() == text)
                return b;
    return nullptr;
}
}

int main()
{
    juce::ScopedJuceInitialiser_GUI gui;
    IrisProcessor proc;
    proc.setPlayConfigDetails (2, 2, 48000.0, 512);
    proc.prepareToPlay (48000.0, 512);

    check (proc.getName() == juce::String (gui::kProjectWord) + " " + gui::kPluginWord, "the reported name is the nameplate's two words");

    std::printf ("\nParameters\n");
    check (proc.getParameters().size() == 6 + numCrossovers + 16 * maxBands, fmt ("%d parameters: %d global (six settings and the %d region crossovers) and 16 for each of %d bands", proc.getParameters().size(), 6 + numCrossovers, numCrossovers, maxBands));
    bool presetsOk = true;
    for (auto& preset : presets())
        for (auto& [pid, v] : preset.values)
            presetsOk = presetsOk && proc.apvts.getParameter (pid) != nullptr;
    check (presetsOk, "every parameter a preset sets exists");
    for (int i = 0; i < proc.getNumPrograms(); ++i)
        proc.setCurrentProgram (i);
    proc.setCurrentProgram (0);
    check (! isBandOn (proc.apvts, 0) && std::fabs (value (proc, id::out)) < 1.0e-6f, fmt ("the Init preset starts with no band on (band 1 on = %d, output %.2f dB)", (int) isBandOn (proc.apvts, 0), value (proc, id::out)));

    gui::EqView view (proc.engine, proc.apvts, proc.currentSampleRate);
    view.setBounds (0, 0, viewWidth, viewHeight);
    int selectionCalls = 0;
    view.onSelectionChanged = [&] (int) { ++selectionCalls; };

    std::printf ("\nBands on the display\n");
    view.mouseDoubleClick (mouse (view, { xOf (1000.0), yOfGain (0.0) }, { xOf (1000.0), yOfGain (0.0) }, false));
    check (isBandOn (proc.apvts, 0) && std::fabs (value (proc, bandIds (0).freq) / 1000.0f - 1.0f) < 0.02f && std::fabs (value (proc, bandIds (0).gain)) < 0.6f,
           fmt ("double-click adds a bell where it was clicked (%.0f Hz, %+.1f dB)", value (proc, bandIds (0).freq), value (proc, bandIds (0).gain)));
    check (bandType (proc.apvts, 0) == BandType::bell && bandPlacement (proc.apvts, 0) == Placement::stereo, "... on Stereo");

    view.mouseDoubleClick (mouse (view, { xOf (200.0), yOfGain (6.0) }, { xOf (200.0), yOfGain (6.0) }, false));
    check (isBandOn (proc.apvts, 1) && std::fabs (value (proc, bandIds (1).gain) - 6.0f) < 0.3f && view.getSelectedBand() == 1 && selectionCalls >= 1,
           fmt ("a double-click above the line makes a boost (%+.1f dB) and selects the new band", value (proc, bandIds (1).gain)));

    // Drag node 1 (200 Hz, +6 dB) an octave up in frequency and 3 dB down.
    dragFrom (view, { xOf (200.0), yOfGain (6.0) }, { xOf (400.0), yOfGain (3.0) });
    check (std::fabs (value (proc, bandIds (1).freq) / 400.0f - 1.0f) < 0.03f && std::fabs (value (proc, bandIds (1).gain) - 3.0f) < 0.3f,
           fmt ("dragging a node sets frequency and gain (%.0f Hz, %+.1f dB; wanted 400 Hz, +3 dB)", value (proc, bandIds (1).freq), value (proc, bandIds (1).gain)));

    dragFrom (view, { xOf (400.0), yOfGain (3.0) }, { xOf (800.0), yOfGain (-3.0) }, juce::ModifierKeys (juce::ModifierKeys::shiftModifier));
    check (value (proc, bandIds (1).freq) < 500.0f && value (proc, bandIds (1).freq) > 420.0f && value (proc, bandIds (1).gain) > 1.0f,
           fmt ("with Shift the same drag is a fifth as far (%.0f Hz, %+.1f dB)", value (proc, bandIds (1).freq), value (proc, bandIds (1).gain)));

    // The gain stops at the ends of its range.
    dragFrom (view, { xOf (value (proc, bandIds (1).freq)), yOfGain (value (proc, bandIds (1).gain)) }, { xOf (value (proc, bandIds (1).freq)), -400.0f });
    check (value (proc, bandIds (1).gain) == maxGainDb, "dragging far above the display stops at the top of the gain range");

    // Wheel: Q.
    const float q0 = value (proc, bandIds (0).q);
    wheel (view, { xOf (1000.0), yOfGain (0.0) }, 0.1f);
    const float q1 = value (proc, bandIds (0).q);
    wheel (view, { xOf (1000.0), yOfGain (0.0) }, -0.2f);
    check (q1 > q0 * 1.2f && value (proc, bandIds (0).q) < q1 * 0.7f && view.getSelectedBand() == 0,
           fmt ("the wheel over a node changes Q (%.2f -> %.2f -> %.2f)", q0, q1, value (proc, bandIds (0).q)));

    // A cut: no gain to drag, and the wheel steps the slope.
    removeBand (proc.apvts, 1);
    const int cut = addBand (proc.apvts, BandType::lowCut, 60.0);
    check (cut == 1, "a cut goes into the first free slot");
    const int slope0 = (int) value (proc, bandIds (cut).slope);
    dragFrom (view, { xOf (60.0), yOfGain (0.0) }, { xOf (60.0), yOfGain (9.0) });
    check (std::fabs (value (proc, bandIds (cut).gain)) < 1.0e-6f && std::fabs (value (proc, bandIds (cut).freq) / 60.0f - 1.0f) < 0.03f,
           "dragging a cut's node up and down changes nothing but is harmless");
    wheel (view, { xOf (60.0), yOfGain (0.0) }, 0.1f);
    check ((int) value (proc, bandIds (cut).slope) == std::min (3, slope0 + 1), "the wheel over a cut steps its slope");
    const int slope1 = (int) value (proc, bandIds (cut).slope);
    wheel (view, { xOf (60.0), yOfGain (0.0) }, 0.0f);
    check ((int) value (proc, bandIds (cut).slope) == slope1, "a sideways scroll (no vertical delta) leaves the slope alone");
    removeBand (proc.apvts, cut);
    removeBand (proc.apvts, 0);

    std::printf ("\nFrequency regions\n");
    auto& monitor = proc.engine.monitor;
    const auto strip = [&] (double hz) { return juce::Point<float> { xOf (hz), stripY }; };
    check (monitor.regions.load() == 0u, "no region is soloed to begin with");
    click (view, strip (35.0));
    check (monitor.regions.load() == 1u, "clicking SUB solos it");
    click (view, strip (1000.0));
    check (monitor.regions.load() == 9u, "clicking MIDRANGE adds it: regions are additive");
    click (view, strip (35.0));
    check (monitor.regions.load() == 8u, "clicking SUB again releases it");
    click (view, strip (85.0));
    check (monitor.regions.load() == 10u, "clicking BASS (60 to 250 Hz) adds it");
    click (view, strip (85.0));
    click (view, strip (12000.0), juce::ModifierKeys (juce::ModifierKeys::altModifier));
    check (monitor.regions.load() == 32u, "Alt-click solos HIGH alone");
    click (view, strip (12000.0), juce::ModifierKeys (juce::ModifierKeys::altModifier));
    check (monitor.regions.load() == 0u, "Alt-click on the only soloed region releases it");

    click (view, strip (300.0));
    click (view, strip (3500.0));
    check (monitor.regions.load() == 20u, "LOW-MID and UPPER-MID (bits 2 and 4)");
    click (view, { 20.0f, 6.0f + 14.0f });
    check (monitor.regions.load() == 0u && monitor.soloBand.load() == -1, "clicking the SOLO pill clears every solo");

    // The crossover handles.
    check (std::fabs (value (proc, id::crossover[0]) - 60.0f) < 0.5f && std::fabs (value (proc, id::crossover[1]) - 250.0f) < 0.5f,
          "the first two crossovers start at the main bands' edges, 60 Hz (sub / bass) and 250 Hz (bass / low-mid)");
    dragFrom (view, strip (250.0), strip (150.0));
    check (std::fabs (value (proc, id::crossover[1]) / 150.0f - 1.0f) < 0.03f && monitor.regions.load() == 0u,
           fmt ("dragging the handle moves the crossover to %.0f Hz without soloing anything", value (proc, id::crossover[1])));
    dragFrom (view, strip (150.0), strip (2500.0));
    check (value (proc, id::crossover[1]) <= value (proc, id::crossover[2]) / 1.24f, fmt ("a handle cannot be dragged into the next one (stops at %.0f Hz, the next is at %.0f Hz)", value (proc, id::crossover[1]), value (proc, id::crossover[2])));
    view.mouseDoubleClick (mouse (view, strip (value (proc, id::crossover[1])), strip (value (proc, id::crossover[1])), false));
    check (std::fabs (value (proc, id::crossover[1]) - 250.0f) < 0.5f, "double-clicking a handle puts it back");

    // A session saved before the sub / bass crossover existed has xo1 ... xo4 and no xo0: they keep their values, the new one is its default.
    {
        IrisProcessor before;
        setParameterValue (before.apvts, id::crossover[1], 300.0f);
        auto state = before.apvts.copyState();
        for (int i = state.getNumChildren(); --i >= 0;)
            if (state.getChild (i).getProperty ("id").toString() == id::crossover[0])
                state.removeChild (i, nullptr);
        juce::MemoryBlock block;
        if (auto xml = state.createXml())
            juce::AudioProcessor::copyXmlToBinary (*xml, block);
        IrisProcessor after;
        after.setStateInformation (block.getData(), (int) block.getSize());
        check (std::fabs (value (after, id::crossover[1]) - 300.0f) < 0.5f && std::fabs (value (after, id::crossover[0]) - 60.0f) < 0.5f,
               fmt ("an older session loads: its crossover stays at %.0f Hz and the new first split at %.0f Hz", value (after, id::crossover[1]), value (after, id::crossover[0])));
    }

    // Band solo through the display's own API (the buttons and the menu call it).
    addBand (proc.apvts, BandType::bell, 1000.0, 4.0);
    view.toggleBandSolo (0, SoloKind::curve);
    check (monitor.soloBand.load() == 0 && monitor.soloKind.load() == (int) SoloKind::curve, "soloing a band's curve");
    view.toggleBandSolo (0, SoloKind::band);
    check (monitor.soloBand.load() == 0 && monitor.soloKind.load() == (int) SoloKind::band, "... switches to its region");
    view.toggleBandSolo (0, SoloKind::band);
    check (monitor.soloBand.load() == -1, "... and the same button releases it");
    view.toggleBandSolo (0, SoloKind::curve);
    removeBand (proc.apvts, 0);
    pump (150);
    check (monitor.soloBand.load() == -1, "removing the soloed band ends its solo (it does not wait for the slot to be used again)");
    view.clearSolos();

    std::printf ("\nEditor\n");
    {
        std::unique_ptr<juce::AudioProcessorEditor> made (proc.createEditor());
        auto& editor = ui::innerOf<IrisEditor> (*made);
        made->setVisible (true);
        pump (150);

        auto* add = findButton (editor, "ADD BAND");
        auto* remove = findButton (editor, "REMOVE");
        check (add != nullptr && add->isVisible() && remove != nullptr && ! remove->isVisible(), "a free slot offers ADD BAND and no REMOVE");
        add->onClick();
        pump (150);
        check (isBandOn (proc.apvts, 0) && ! add->isVisible() && remove->isVisible(), "ADD BAND puts a bell in the slot and the panel switches to the band's controls");

        const auto count = [&] (const juce::String& text) { int n = 0; for (auto* c : editor.getChildren()) if (c->isVisible()) if (auto* b = dynamic_cast<juce::TextButton*> (c)) n += b->getButtonText() == text; return n; };
        check (count ("DYNAMIC") == 1, "a bell offers DYNAMIC");

        // What the dynamics listen to: one selector for the shown band, with the parameter's own two items, and a new band starts on Self.
        const auto listenBoxes = [&] { std::vector<juce::ComboBox*> v; for (auto* c : editor.getChildren()) if (c->isVisible()) if (auto* b = dynamic_cast<juce::ComboBox*> (c)) if (b->getNumItems() == listenNames.size() && b->getItemText (1) == listenNames[1]) v.push_back (b); return v; };
        check (listenBoxes().size() == 1 && listenBoxes()[0]->getSelectedItemIndex() == 0, "a bell offers the Self / Sidechain selector, on Self");
        listenBoxes()[0]->setSelectedItemIndex (1, juce::sendNotificationSync);
        pump (150);
        check (readParams (proc.apvts).bands[0].external, "choosing Sidechain reaches the engine's parameters");
        removeBand (proc.apvts, 0);
        addBand (proc.apvts, BandType::bell, 1000.0, 0.0, 0);
        pump (150);
        check (! readParams (proc.apvts).bands[0].external, "a band added to the slot again listens to itself");

        setParameterValue (proc.apvts, bandIds (0).type, (float) (int) BandType::lowCut);
        pump (150);
        check (count ("DYNAMIC") == 1 && findButton (editor, "BAND") != nullptr && listenBoxes().empty(), "a cut: the controls follow the type without leaving the editor (no dynamics, so nothing to listen to)");

        // Solo buttons act on the selected band.
        findButton (editor, "CURVE")->onClick();
        check (monitor.soloBand.load() == 0 && monitor.soloKind.load() == (int) SoloKind::curve, "the CURVE button solos the selected band's curve");
        pump (100);
        check (findButton (editor, "CURVE")->getToggleState() && ! findButton (editor, "BAND")->getToggleState(), "... and lights while it is soloed");

        monitor.delta = true;
        monitor.regions = 3u;
    }
    check (! monitor.delta.load() && monitor.soloBand.load() == -1 && monitor.regions.load() == 0u, "closing the editor ends every solo and Delta: monitoring never outlives its window");

    std::printf ("\nState\n");
    {
        monitor.delta = true;
        monitor.soloBand = 0;
        monitor.regions = 7u;
        juce::MemoryBlock saved;
        proc.getStateInformation (saved);
        IrisProcessor fresh;
        fresh.setStateInformation (saved.getData(), (int) saved.getSize());
        check (! fresh.engine.monitor.delta.load() && fresh.engine.monitor.soloBand.load() == -1 && fresh.engine.monitor.regions.load() == 0u,
               "a project saved while soloing opens with nothing soloed");
        check (isBandOn (fresh.apvts, 0) == isBandOn (proc.apvts, 0) && value (fresh, bandIds (0).type) == value (proc, bandIds (0).type), "... and the bands come back");
    }

    std::printf ("\nPaste bands from another plugin\n");
    {
        IrisProcessor fresh;
        int skipped = 0;
        const auto slots = pasteBands (fresh.apvts, "pantheon-bands 1\nbell freq=270 gain=-2.5 q=1.40 place=mid\n"
                                                    "lowcut freq=150 slope=24 place=side\nfill lo=40 hi=90 place=mid", skipped);
        check (slots.size() == 2 && skipped == 1, fmt ("two equaliser bands added, the fill band left for Hephaestus (%d added, %d skipped)", (int) slots.size(), skipped));
        if (slots.size() == 2)
        {
            check (bandType (fresh.apvts, slots[0]) == BandType::bell && bandPlacement (fresh.apvts, slots[0]) == Placement::mid
                       && std::fabs (value (fresh, bandIds (slots[0]).freq) - 270.0f) < 1.0f && std::fabs (value (fresh, bandIds (slots[0]).gain) + 2.5f) < 0.06f
                       && std::fabs (value (fresh, bandIds (slots[0]).q) - 1.4f) < 0.02f,
                   "the bell keeps its frequency, gain, width and placement");
            check (bandType (fresh.apvts, slots[1]) == BandType::lowCut && bandPlacement (fresh.apvts, slots[1]) == Placement::side
                       && (int) value (fresh, bandIds (slots[1]).slope) == 1,
                   "the cut keeps its placement and its 24 dB/oct slope");
        }
        check (pasteBands (fresh.apvts, "some other text", skipped).empty(), "text that is not a band list adds nothing");
    }

    std::printf ("\nReference and match\n");
    {
        // The display averages what goes in on its timer, so these run in (about) real time: audio in blocks, the message loop between.
        const auto feed = [&] (IrisProcessor& p, float seconds, const std::function<float (test::PinkNoise&, float&)>& sample)
        {
            test::PinkNoise noise (7);
            float memory = 0.0f;
            juce::AudioBuffer<float> buffer (2, 512);
            juce::MidiBuffer midi;
            for (int block = 0; block < (int) (seconds * 48000.0f / 512.0f); ++block)
            {
                for (int i = 0; i < 512; ++i)
                {
                    const float x = 0.4f * sample (noise, memory);
                    buffer.setSample (0, i, x);
                    buffer.setSample (1, i, x);
                }
                p.processBlock (buffer, midi);
                if (block % 3 == 0)
                    pump (34);
            }
        };
        const auto pink = [] (test::PinkNoise& n, float&) { return n.next(); };
        const auto bright = [] (test::PinkNoise& n, float& m) { const float x = n.next(); const float d = x - m; m = x; return 4.0f * d; }; // differenced: +3 dB/octave over the pink

        IrisProcessor a;
        a.setPlayConfigDetails (2, 2, 48000.0, 512);
        a.prepareToPlay (48000.0, 512);
        gui::EqView v (a.engine, a.apvts, a.currentSampleRate);
        v.setBounds (0, 0, viewWidth, viewHeight);
        check (! v.hasReference() && v.matchToReference().empty(), "with no reference, MATCH does nothing and says so");

        v.toggleLearning();
        check (v.isLearning(), "LEARN REF starts learning");
        feed (a, 4.5f, bright);
        check (v.learnedSeconds() >= 2.5, fmt ("... counting what it hears (%.1f s)", v.learnedSeconds()));
        v.toggleLearning();
        check (! v.isLearning() && v.hasReference(), "pressing it again keeps the reference");

        check (v.matchToReference().empty(), "MATCH before the track has played for a few seconds adds nothing");
        feed (a, 4.5f, pink);
        check (v.hasAverage(), "the track's own average builds up while it plays");
        const auto first = v.matchToReference();
        bool brightens = false;
        for (const int slot : first)
            brightens |= value (a, bandIds (slot).gain) > 1.0f && value (a, bandIds (slot).freq) > 1000.0f;
        check (! first.empty() && (int) first.size() <= match::kMaxBands && brightens,
               fmt ("a track 3 dB/octave duller than the reference gets %d band(s), a lift in the highs among them", (int) first.size()));
        for (const int slot : first)
            check (bandPlacement (a.apvts, slot) == Placement::stereo && value (a, bandIds (slot).gain) <= match::kMaxDb + 0.1f, fmt ("band %d: Stereo, %+.1f dB", slot + 1, value (a, bandIds (slot).gain)));
        const auto again = v.matchToReference();
        check (again.size() < first.size(), fmt ("matching again counts the bands already there (%d more, after %d)", (int) again.size(), (int) first.size()));

        // The reference is saved with the project and comes back with the editor.
        juce::MemoryBlock saved;
        a.getStateInformation (saved);
        IrisProcessor b;
        b.setStateInformation (saved.getData(), (int) saved.getSize());
        gui::EqView restored (b.engine, b.apvts, b.currentSampleRate);
        check (restored.hasReference(), "the reference comes back with the project");
        v.clearReference();
        check (! v.hasReference(), "CLEAR removes it");
        juce::MemoryBlock cleared;
        a.getStateInformation (cleared);
        IrisProcessor c;
        c.setStateInformation (cleared.getData(), (int) cleared.getSize());
        gui::EqView none (c.engine, c.apvts, c.currentSampleRate);
        check (! none.hasReference(), "... also from the saved state");

        // A curve pasted along with Themis' bands.
        std::string curve = "pantheon-bands 1\nbell freq=270 gain=-2.5 q=1.40 place=mid\ncurve f0=20 oct=6 name=Modern_Pop db=";
        for (int i = 0; i < 61; ++i)
            curve += (i ? "," : "") + std::to_string (3.0 - 0.1 * i).substr (0, 5);
        check (v.pasteReference (curve) && v.hasReference(), "a curve in the clipboard becomes the reference");
        int skipped = 0;
        check (pasteBands (a.apvts, curve, skipped).size() == 1, "... and its bands are pasted as before");
        check (! v.pasteReference ("pantheon-bands 1\nbell freq=270 gain=-2.5 q=1.40 place=mid") && ! v.pasteReference ("some other text"),
               "text without a curve changes nothing");
        v.clearReference();
    }

    std::printf ("\nLatency follows the phase mode (processor level)\n");
    {
        IrisProcessor p2;
        p2.setPlayConfigDetails (2, 2, 48000.0, 512);
        p2.prepareToPlay (48000.0, 512);
        juce::AudioBuffer<float> buffer (2, 512);
        juce::MidiBuffer midi;
        std::string line;
        bool ok = true;
        for (int mode : { 0, 2, 1, 0 })
        {
            setParameterValue (p2.apvts, id::phase, (float) mode);
            for (int i = 0; i < 3; ++i)
            {
                buffer.clear();
                p2.processBlock (buffer, midi);
                pump (50);
            }
            line += fmt (" %s:%d", phaseNames[mode].toRawUTF8(), p2.getLatencySamples());
            ok = ok && p2.getLatencySamples() == latencyFor ((PhaseMode) mode);
        }
        check (ok, "reported latency by phase mode:" + line);

        // A session saved with a linear mode is prepared with it, so the host hears the latency before the first block.
        setParameterValue (p2.apvts, id::phase, 1.0f);
        p2.prepareToPlay (48000.0, 512);
        check (p2.getLatencySamples() == latencyFor (PhaseMode::linear), "prepared with LINEAR already set, the latency is reported at once");
        setParameterValue (p2.apvts, id::phase, 0.0f);
    }

    std::printf ("\nAdvice: the notes that keep the equaliser from being used for the wrong job\n");
    {
        using ui::Severity;
        const auto has = [] (const std::vector<ui::Note>& n, const char* word, Severity sev) { return ui::hasNote (n, word, sev); };

        AdviceInput fine;
        fine.bandOn = true;
        check (adviceFor (fine).empty(), "a plain bell in Zero Latency: nothing to say");

        auto in = fine; in.delta = true;
        check (has (adviceFor (in), "DELTA", Severity::warning), "DELTA on: a warning");
        in = fine; in.soloActive = true;
        check (has (adviceFor (in), "SOLO", Severity::warning), "a solo on: a warning");
        in = fine; in.satChosen = true; in.phase = PhaseMode::linear; in.latencyMs = 48.0;
        check (has (adviceFor (in), "SAT does nothing", Severity::warning) && has (adviceFor (in), "48 ms", Severity::info), "SAT in a linear mode: it does nothing, and the delay is named");
        in = fine; in.place = Placement::side; in.gainDb = 3.0; in.freqHz = 80.0;
        check (has (adviceFor (in), "Side below 150", Severity::warning), "a bass boost on the Side: weakens the bass in mono");
        in.freqHz = 6000.0;
        check (! has (adviceFor (in), "Side below 150", Severity::warning), "... but a boost up on the Side is the usual air");
        in = fine; in.gainDb = 10.0;
        check (has (adviceFor (in), "GAIN MATCH", Severity::warning), "a 10 dB boost with GAIN MATCH off: louder is not better");
        in.gainMatch = true;
        check (! has (adviceFor (in), "boost of 9 dB", Severity::warning) && has (adviceFor (in), "GAIN MATCH is on", Severity::info), "... with it on, only the note that level is matched away");
        in = fine; in.bandOn = false; in.anyBoostOver = true;
        check (has (adviceFor (in), "Some band boosts", Severity::info), "another band's big boost is named when this slot is free");
        in = fine; in.q = 12.0; in.gainDb = 8.0;
        check (has (adviceFor (in), "rings", Severity::info), "a narrow, strong boost rings");

        size_t longest = 0;
        for (int mask = 0; mask < 512; ++mask)
            for (auto place : { Placement::stereo, Placement::side })
            {
                AdviceInput all;
                all.bandOn = true; all.place = place; all.delta = mask & 1; all.soloActive = mask & 2; all.gainMatch = mask & 4; all.satChosen = mask & 8;
                all.phase = (mask & 16) ? PhaseMode::linearHq : PhaseMode::zeroLatency; all.latencyMs = 999.0; all.gainDb = (mask & 32) ? 12.0 : 0.0;
                all.freqHz = (mask & 64) ? 80.0 : 4000.0; all.q = (mask & 128) ? 12.0 : 1.0; all.anyBoostOver = mask & 256;
                for (auto& n : adviceFor (all))
                    longest = std::max (longest, n.text.size());
            }
        check ((int) longest <= ui::kMaxNoteChars, fmt ("the longest note is %d characters (a line holds %d)", (int) longest, ui::kMaxNoteChars));

        IrisProcessor host;
        host.setPlayConfigDetails (2, 2, 48000.0, 512);
        host.prepareToPlay (48000.0, 512);
        IrisEditor editor (host);
        check (editor.notes().empty(), "the editor starts with no warning");
        host.engine.monitor.delta = true;
        check (has (editor.notes(), "DELTA", Severity::warning), "DELTA on in the engine reaches the editor's notes");
        host.engine.monitor.delta = false;
        host.engine.monitor.regions = 1u;
        check (has (editor.notes(), "SOLO", Severity::warning), "a region solo reaches them too");
        host.engine.monitor.regions = 0u;
        check (editor.guide().isVisible() && std::strlen (guideText()) > 400 && std::string (guideText()).find ("NOT FOR") != std::string::npos,
               "the GUIDE button is there and its text says what the plugin is not for");
    }

    return finish ("UI CHECK");
}
