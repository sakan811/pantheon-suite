// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 The Pantheon authors

// The session (undo, partial rendering, project files, export), the player, and the real window driven with synthetic mouse
// events and key presses. Headless: no window is shown and no audio device is opened.
//
//   OrpheusUiCheck

#include "Check.h"
#include "TestSignals.h"
#include "UI/MainComponent.h"

using namespace orpheus;
using namespace orpheus::gui;

namespace
{
juce::MouseEvent mouse (juce::Component& c, juce::Point<float> at, juce::Point<float> downAt, bool dragged, juce::ModifierKeys mods = {}, int clicks = 1)
{
    return { juce::Desktop::getInstance().getMainMouseSource(), at, mods,
             juce::MouseInputSource::defaultPressure, juce::MouseInputSource::defaultOrientation,
             juce::MouseInputSource::defaultRotation, juce::MouseInputSource::defaultTiltX,
             juce::MouseInputSource::defaultTiltY, &c, &c, juce::Time::getCurrentTime(),
             downAt, juce::Time::getCurrentTime(), clicks, dragged };
}

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

void doubleClick (juce::Component& view, juce::Point<float> at)
{
    view.mouseDown (mouse (view, at, at, false, {}, 2));
    view.mouseDoubleClick (mouse (view, at, at, false, {}, 2));
    view.mouseUp (mouse (view, at, at, false, {}, 2));
}

std::unique_ptr<MainComponent> makeWindow (bool chromatic = false, double detune = 0.0)
{
    auto w = std::make_unique<MainComponent> (false);
    w->setSize (1240, 760);
    auto notes = test::outOfTunePhrase();
    if (detune != 0.0) // every note on the same off-grid: the singer is flat or sharp all the way through
        for (auto& n : notes)
        {
            n.midi = std::round (n.midi) + detune;
            n.vibratoCents = n.driftCents = 0.0; // a note that wanders has no single pitch to be off by
        }
    const auto voice = test::sing (notes, 7.0);
    juce::AudioBuffer<float> audio (1, (int) voice.audio.size());
    std::copy (voice.audio.begin(), voice.audio.end(), audio.getWritePointer (0));
    w->session.adoptAudio (std::move (audio), voice.rate, juce::File ("phrase.wav"));
    w->analyseNow();
    w->session.key.root = 0;
    w->session.key.scale = chromatic ? kChromatic : kMajor;
    return w;
}

bool sameAudio (const juce::AudioBuffer<float>& a, const juce::AudioBuffer<float>& b)
{
    if (a.getNumChannels() != b.getNumChannels() || a.getNumSamples() != b.getNumSamples())
        return false;
    for (int ch = 0; ch < a.getNumChannels(); ++ch)
        if (std::memcmp (a.getReadPointer (ch), b.getReadPointer (ch), sizeof (float) * (size_t) a.getNumSamples()) != 0)
            return false;
    return true;
}

juce::Point<float> centreOf (MainComponent& w, size_t note) { return w.roll.noteRect (w.session.notes[note]).getCentre(); }
juce::KeyPress key (int code, juce::ModifierKeys mods = {}) { return { code, mods, 0 }; }
juce::KeyPress character (juce::juce_wchar c, juce::ModifierKeys mods = {}) { return { (int) c, mods, c }; }
} // namespace

int main()
{
    juce::ScopedJuceInitialiser_GUI gui;
    std::printf ("Orpheus UI check\n");

    // ---- [1] the session ----------------------------------------------------------------------------------------------
    std::printf ("\n[1] session\n");
    {
        auto w = makeWindow();
        auto& s = w->session;
        test::check (s.notes.size() == 9, test::fmt ("the nine sung notes are found (%zu)", s.notes.size()));
        const auto detected = detectKey (s.notes);
        test::check (detected.root == 0 && ! detected.minor, test::fmt ("the key of the phrase is found: %s %s", kNoteNames[detected.root], detected.minor ? "minor" : "major"));
        test::check (sameAudio (s.output, s.source), "before any edit the output is the source, bit for bit");

        s.correct (1.0f);
        bool onGrid = true;
        for (const auto& n : s.notes)
            onGrid = onGrid && std::fabs (n.target - std::round (n.target)) < 1e-4f && s.key.allows ((int) std::lround (n.target));
        test::check (onGrid, "Correct all puts every note on a note of C major");
        test::check (! sameAudio (s.output, s.source), "and the audio changes");
        juce::AudioBuffer<float> corrected;
        corrected.makeCopyOf (s.output);

        s.notes[3].target += 0.35f;
        s.changed (s.notes[3].start, s.notes[3].end);
        juce::AudioBuffer<float> partial;
        partial.makeCopyOf (s.output);
        s.changedAll();
        test::check (sameAudio (partial, s.output), "after moving one note, rebuilding only that note gives the same audio as rebuilding everything");

        s.notes[3].target -= 0.35f;
        s.changed (s.notes[3].start, s.notes[3].end);
        test::check (sameAudio (corrected, s.output), "moving it back gives the corrected audio again");

        test::check (s.undo(), "undo works");
        bool untouched = true;
        for (const auto& n : s.notes)
            untouched = untouched && ! n.edited();
        test::check (untouched && sameAudio (s.output, s.source), "undoing Correct all gives the original audio again, bit for bit");
        test::check (s.redo() && sameAudio (s.output, corrected), "redo brings the correction back");
        s.undo();
        test::check (! s.canUndo() && s.canRedo(), "undo and redo know where they are");

        // a project file
        s.correct (0.6f);
        s.notes[2].vibrato = 0.4f;
        s.notes[2].formant = 1.5f;
        s.notes[4].gainDb = -3.0f;
        s.changedAll();
        s.key.root = 7;
        s.key.scale = 8; // Mixolydian
        s.key.a4Hz = 432.0;
        const auto xml = s.toXml();
        auto w2 = makeWindow();
        const bool applied = w2->session.applyXml (*xml);
        bool same = applied && w2->session.notes.size() == s.notes.size();
        for (size_t i = 0; same && i < s.notes.size(); ++i)
            same = std::fabs (w2->session.notes[i].target - s.notes[i].target) < 1e-5f && std::fabs (w2->session.notes[i].vibrato - s.notes[i].vibrato) < 1e-6f
                   && std::fabs (w2->session.notes[i].formant - s.notes[i].formant) < 1e-6f && std::fabs (w2->session.notes[i].gainDb - s.notes[i].gainDb) < 1e-6f;
        test::check (same && w2->session.key.root == 7 && w2->session.key.a4Hz == 432.0 && sameAudio (w2->session.output, s.output), "a saved project, applied to the same recording, gives the same notes, key and audio");
        juce::XmlElement wrong ("Orpheus");
        wrong.setAttribute ("frames", 5);
        test::check (! w2->session.applyXml (wrong), "a project for another recording is refused");

        // the file
        const auto tmp = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("orpheus-check.wav");
        juce::String error;
        test::check (s.exportWav (tmp, error), test::fmt ("export writes a WAV %s", error.toRawUTF8()));
        Session read;
        const bool loaded = read.load (tmp, error);
        double worst = 0.0;
        for (int i = 0; loaded && i < s.output.getNumSamples(); ++i)
            worst = std::max (worst, (double) std::fabs (read.source.getSample (0, i) - s.output.getSample (0, i)));
        test::check (loaded && read.numSamples() == s.numSamples() && worst < 1.0e-6, test::fmt ("read back, it is the corrected audio to 24-bit accuracy (%.2e)", worst));
        tmp.deleteFile();
    }

    // ---- [2] playback ---------------------------------------------------------------------------------------------------
    std::printf ("\n[2] player\n");
    {
        auto w = makeWindow();
        auto& s = w->session;
        s.correct (1.0f);
        auto& p = w->playback;
        juce::AudioBuffer<float> block (2, 512);
        juce::AudioSourceChannelInfo info (&block, 0, 512);
        p.setNextReadPosition (20000);
        p.getNextAudioBlock (info);
        bool matches = true;
        for (int i = 0; i < 512; ++i)
            matches = matches && block.getSample (0, i) == s.output.getSample (0, 20000 + i) && block.getSample (1, i) == block.getSample (0, i);
        test::check (matches && p.position() == 20512, "plays the corrected audio (a mono recording on both channels)");
        p.corrected.store (false);
        p.setNextReadPosition (20000);
        p.getNextAudioBlock (info);
        matches = true;
        for (int i = 0; i < 512; ++i)
            matches = matches && block.getSample (0, i) == s.source.getSample (0, 20000 + i);
        test::check (matches, "ORIGINAL plays the recording as it was, at the same position");
        p.corrected.store (true);
        p.loopStart.store (1000);
        p.loopEnd.store (1300);
        p.looping.store (true);
        p.setNextReadPosition (1200);
        p.getNextAudioBlock (info);
        test::check (block.getSample (0, 100) == s.output.getSample (0, 1000) && p.position() >= 1000 && p.position() < 1300, "a loop wraps at its end");
        p.looping.store (false);
        p.setNextReadPosition (s.numSamples() - 100);
        p.getNextAudioBlock (info);
        test::check (block.getSample (0, 200) == 0.0f, "at the end of the recording it plays silence");
        p.playTone (69.0);
        p.getNextAudioBlock (info);
        test::check (block.getMagnitude (0, 512) > 0.01f, "the reference tone sounds");
    }

    // ---- [3] the window -------------------------------------------------------------------------------------------------
    std::printf ("\n[3] the window\n");
    {
        auto w = makeWindow (true); // chromatic: dragging snaps to semitones, so the result is predictable
        auto& roll = w->roll;
        auto& s = w->session;
        roll.fitToSession();
        w->refreshInspector();

        click (roll, centreOf (*w, 2));
        test::check (roll.selectedIndices() == std::vector<size_t> { 2 }, "a click on a note selects it");
        click (roll, centreOf (*w, 4), juce::ModifierKeys (juce::ModifierKeys::shiftModifier));
        test::check (roll.selectedIndices() == std::vector<size_t> { 2, 4 }, "shift-click adds a note");
        click (roll, centreOf (*w, 4), juce::ModifierKeys (juce::ModifierKeys::shiftModifier));
        test::check (roll.selectedIndices() == std::vector<size_t> { 2 }, "shift-click on a selected note takes it out");
        click (roll, { (float) roll.graphArea().getX() + 6.0f, (float) roll.graphArea().getBottom() - 10.0f });
        test::check (roll.selectedIndices().empty(), "a click on empty space selects nothing");

        // dragging a note: one row up is one semitone, and it snaps to the semitone grid
        const float before = s.notes[1].target;
        const auto from = centreOf (*w, 1);
        dragFrom (roll, from, from - juce::Point<float> (0.0f, (float) roll.rowH));
        test::check (std::fabs (s.notes[1].target - std::round (before + 1.0f)) < 1e-4f, test::fmt ("dragging a note one row up moves it to the next semitone (%.3f -> %.3f)", before, s.notes[1].target));
        test::check (roll.selectedIndices() == std::vector<size_t> { 1 } && s.canUndo(), "the dragged note is selected, and the drag can be undone");
        test::check (std::fabs (s.plan.outMidi[(size_t) s.notes[1].start + 10] - s.contour.midi[(size_t) s.notes[1].start + 10] - (s.notes[1].target - s.notes[1].centre)) < 0.02, "and the plan follows it");
        w->undo();
        test::check (std::fabs (s.notes[1].target - before) < 1e-6f && sameAudio (s.output, s.source), "undo puts the note and the audio back");

        // fine drag
        const auto at = centreOf (*w, 1);
        dragFrom (roll, at, at - juce::Point<float> (0.0f, (float) roll.rowH * 2.0f), juce::ModifierKeys (juce::ModifierKeys::shiftModifier));
        test::check (std::fabs (s.notes[1].target - (before + 0.2f)) < 0.01f, test::fmt ("Shift-drag moves in tenths: two rows = 20 cents (%.1f)", (s.notes[1].target - before) * 100.0));
        w->undo();

        // free drag with snap off
        w->snapButton.setToggleState (false, juce::sendNotification);
        const auto at2 = centreOf (*w, 1);
        dragFrom (roll, at2, at2 - juce::Point<float> (0.0f, (float) roll.rowH * 0.5f));
        test::check (std::fabs (s.notes[1].target - (before + 0.5f)) < 0.05f, test::fmt ("with SNAP off a drag is free: half a row = %.1f cents (50, to the pixel)", (s.notes[1].target - before) * 100.0));
        w->undo();
        // Alt reverses it: with SNAP off, Alt-drag snaps (to the semitone, the scale being chromatic here)
        const auto at3 = centreOf (*w, 1);
        dragFrom (roll, at3, at3 - juce::Point<float> (0.0f, (float) roll.rowH * 0.5f), juce::ModifierKeys (juce::ModifierKeys::altModifier));
        test::check (std::fabs (s.notes[1].target - std::round (s.notes[1].target)) < 1e-4f, "Alt reverses SNAP: with it off, an Alt-drag lands on a note of the scale");
        w->undo();
        w->snapButton.setToggleState (true, juce::sendNotification);
        const auto at4 = centreOf (*w, 1);
        dragFrom (roll, at4, at4 - juce::Point<float> (0.0f, (float) roll.rowH * 0.5f), juce::ModifierKeys (juce::ModifierKeys::altModifier));
        test::check (std::fabs (s.notes[1].target - (before + 0.5f)) < 0.05f, "and with it on, an Alt-drag is free");
        w->undo();

        // double click: snap, then back
        doubleClick (roll, centreOf (*w, 3));
        const bool snapped = std::fabs (s.notes[3].target - std::round (s.notes[3].centre)) < 1e-4f;
        doubleClick (roll, centreOf (*w, 3));
        test::check (snapped && std::fabs (s.notes[3].target - s.notes[3].centre) < 1e-6f, "double-click snaps a note, and a second one takes it back");

        // marquee
        dragFrom (roll, { (float) roll.graphArea().getX() + 2.0f, (float) roll.graphArea().getY() + 2.0f }, { (float) roll.graphArea().getX() + 560.0f, (float) roll.graphArea().getBottom() - 4.0f });
        const auto picked = roll.selectedIndices();
        test::check (picked.size() >= 3 && picked.front() == 0, test::fmt ("dragging a box on empty space selects the notes in it (%zu)", picked.size()));

        // keys
        roll.select ({ 5 });
        const float k0 = s.notes[5].target;
        w->keyPressed (key (juce::KeyPress::upKey));
        test::check (std::fabs (s.notes[5].target - (k0 + 1.0f)) < 1e-5f, "Up moves the selection a semitone");
        w->keyPressed (key (juce::KeyPress::downKey, juce::ModifierKeys (juce::ModifierKeys::shiftModifier)));
        test::check (std::fabs (s.notes[5].target - (k0 + 0.9f)) < 1e-5f, "Shift+Down moves it 10 cents");
        w->keyPressed (key (juce::KeyPress::downKey, juce::ModifierKeys (juce::ModifierKeys::altModifier)));
        test::check (std::fabs (s.notes[5].target - (k0 + 0.89f)) < 1e-5f, "Alt+Down moves it 1 cent");
        w->keyPressed (key (juce::KeyPress::backspaceKey));
        test::check (! s.notes[5].edited(), "Backspace takes the correction back");
        w->keyPressed (character ('z', juce::ModifierKeys (juce::ModifierKeys::commandModifier)));
        test::check (std::fabs (s.notes[5].target - (k0 + 0.89f)) < 1e-5f, "Cmd+Z undoes it");
        w->keyPressed (character ('z', juce::ModifierKeys (juce::ModifierKeys::commandModifier | juce::ModifierKeys::shiftModifier)));
        test::check (! s.notes[5].edited(), "Shift+Cmd+Z redoes it");
        roll.select ({ 5 });
        w->keyPressed (key (juce::KeyPress::returnKey));
        test::check (std::fabs (s.notes[5].target - std::round (s.notes[5].centre)) < 1e-4f, "Enter snaps the selection");

        // split and merge
        roll.select ({ 6 });
        const size_t count = s.notes.size();
        const auto noteStart = s.notes[6].start, noteEnd = s.notes[6].end;
        w->seek ((juce::int64) ((noteStart + (noteEnd - noteStart) / 2) * s.contour.hop));
        w->keyPressed (character ('s'));
        test::check (s.notes.size() == count + 1 && s.notes[6].end == s.notes[7].start && roll.selectedIndices().size() == 2, "S cuts the selected note at the playhead; both halves stay selected");
        w->keyPressed (character ('m'));
        test::check (s.notes.size() == count && s.notes[6].start == noteStart && s.notes[6].end == noteEnd, "M joins them again");
        w->undo();
        w->undo();

        // the inspector
        roll.select ({ 2 });
        s.notes[2].target = s.notes[2].centre + 0.37f;
        w->refreshInspector();
        test::check (std::fabs (w->shiftKnob.getValue() - 37.0) < 1e-3, "SHIFT shows the offset of the selected note in cents");
        w->shiftKnob.onGestureStart();
        w->shiftKnob.onChange (-80.0);
        test::check (std::fabs (s.notes[2].target - (s.notes[2].centre - 0.8f)) < 1e-5f, "turning SHIFT moves the note to centre - 80 cents");
        w->driftKnob.onGestureStart();
        w->driftKnob.onChange (100.0);
        w->vibratoKnob.onGestureStart();
        w->vibratoKnob.onChange (0.0);
        w->gainKnob.onGestureStart();
        w->gainKnob.onChange (-6.0);
        w->formantKnob.onGestureStart();
        w->formantKnob.onChange (2.0);
        test::check (s.notes[2].drift == 1.0f && s.notes[2].vibrato == 0.0f && s.notes[2].gainDb == -6.0f && s.notes[2].formant == 2.0f, "DRIFT, VIBRATO, GAIN and FORMANT set the note");
        w->correctKnob.onGestureStart();
        w->correctKnob.onChange (100.0);
        test::check (std::fabs (s.notes[2].target - std::round (s.notes[2].centre)) < 1e-4f && std::fabs (w->shiftKnob.getValue() - 100.0 * s.notes[2].shift()) <= 0.5, "CORRECT 100 % lands on the grid, and SHIFT follows (to the cent)");
        roll.select ({ 2, 3 });
        test::check (s.notes[3].drift == 0.0f && s.notes[2].drift == 1.0f, "(selecting two notes with different settings does not change them)");
        w->undo();
        w->resetAllButton.onClick();
        bool clean = true;
        for (const auto& n : s.notes)
            clean = clean && ! n.edited();
        test::check (clean && sameAudio (s.output, s.source), "RESET ALL takes every correction back");

        // scale panel
        s.key.scale = kMajor;
        w->refreshScalePanel();
        w->correctAllButton.onClick();
        bool onScale = true;
        for (const auto& n : s.notes)
            onScale = onScale && s.key.allows ((int) std::lround (n.target));
        test::check (onScale, "CORRECT ALL pulls every note onto the scale");
        w->amountKnob.onChange (50.0);
        w->resetAllButton.onClick();
        w->correctAllButton.onClick();
        double worst = 0.0;
        for (const auto& n : s.notes)
            worst = std::max (worst, std::fabs ((n.target - n.centre) - 0.5 * (s.key.snap (n.centre) - n.centre)));
        test::check (worst < 1e-4, "with AMOUNT at 50 % CORRECT ALL moves every note half the way");
        w->detectKeyButton.onClick();
        test::check (s.key.root == 0 && s.key.scale == kMajor, "DETECT KEY finds C major");
        w->detectTuneButton.onClick();
        test::check (s.key.a4Hz == 440.0, "DETECT A4 on notes that are each off in their own way finds no reference and leaves 440 Hz");
    }
    {
        // a singer who is 30 cents sharp all the way through
        auto w = makeWindow (false, 0.30);
        auto& s = w->session;
        w->detectTuneButton.onClick();
        test::check (std::fabs (s.key.a4Hz - 440.0 * std::exp2 (0.3 / 12.0)) < 0.2, test::fmt ("a singer 30 cents sharp: DETECT A4 reads %.1f Hz (%.1f)", s.key.a4Hz, 440.0 * std::exp2 (0.3 / 12.0)));
        w->detectKeyButton.onClick();
        test::check (s.key.root == 0 && s.key.scale == kMajor, "and the key is still C major");
        w->correctAllButton.onClick();
        double worst = 0.0;
        for (const auto& n : s.notes)
            worst = std::max (worst, std::fabs (n.target - n.centre) * 100.0);
        test::check (worst < 3.0, test::fmt ("correcting to the singer's own grid moves no note by more than %.1f cents (they were already on it)", worst));
    }

    {
        // navigation: the bar, the zoom buttons, the wheel, the pitch scrollbar
        auto w = makeWindow();
        auto& roll = w->roll;
        w->setSize (1240, 760);
        roll.setViewSeconds (0.0, 2.0);
        const double visible0 = roll.visibleSeconds();
        test::check (std::fabs (visible0 - 2.0) < 0.01, test::fmt ("the display can be set to show exactly 2 s (%.2f)", visible0));
        auto& nav = w->navigator;
        const auto view = nav.viewRect();
        dragFrom (nav, view.getCentre(), view.getCentre() + juce::Point<float> (nav.getWidth() * 0.4f, 0.0f));
        test::check (roll.scrollSec > 1.5 && std::fabs (roll.visibleSeconds() - visible0) < 0.01, test::fmt ("dragging the window in the navigator scrolls the display (now at %.2f s, same zoom)", roll.scrollSec));
        const auto view2 = nav.viewRect();
        dragFrom (nav, { view2.getRight(), view2.getCentreY() }, { view2.getRight() + 150.0f, view2.getCentreY() });
        test::check (roll.visibleSeconds() > visible0 * 1.3, test::fmt ("dragging its right edge zooms out (%.2f s visible)", roll.visibleSeconds()));
        click (nav, { 8.0f, nav.getHeight() * 0.5f });
        test::check (roll.scrollSec < 0.3, "clicking the start of the bar goes to the start");
        const double before = roll.pxPerSec;
        w->zoomInButton.onClick();
        test::check (roll.pxPerSec > before * 1.4, "the + button zooms in");
        w->zoomOutButton.onClick();
        w->zoomOutButton.onClick();
        test::check (roll.pxPerSec < before, "and the - button zooms out");
        w->fitButton.onClick();
        test::check (roll.scrollSec == 0.0, "FIT goes back to the start");
        roll.scrollToSeconds (0.0);
        juce::MouseWheelDetails wheelMove;
        wheelMove.deltaX = 0.0f;
        wheelMove.deltaY = -0.2f;
        wheelMove.isReversed = false;
        roll.mouseWheelMove (mouse (roll, { 300.0f, 300.0f }, { 300.0f, 300.0f }, false), wheelMove);
        test::check (roll.scrollSec > 0.5, test::fmt ("the mouse wheel scrolls along the recording (to %.2f s)", roll.scrollSec));
        const double top = roll.topMidi;
        roll.mouseWheelMove (mouse (roll, { 300.0f, 300.0f }, { 300.0f, 300.0f }, false, juce::ModifierKeys (juce::ModifierKeys::shiftModifier)), wheelMove);
        test::check (std::fabs (roll.topMidi - top) > 0.5, "Shift + wheel scrolls the pitch");
        roll.scrollPitch (-3.0);
        test::check (roll.topMidi < top + 0.01, "scrolling the pitch moves the rows");
    }

    {
        // seeking and the clock, without a device
        auto w = makeWindow();
        w->seek (48000);
        test::check (w->playback.position() == 48000, "seek moves the playhead");
        click (w->roll, { (float) w->roll.xOfSample (96000.0), (float) w->roll.kRulerHeight / 2.0f });
        test::check (std::abs (w->playback.position() - 96000) < 400, "a click on the ruler moves the playhead there");
    }

    return test::finish ("OrpheusUiCheck");
}
