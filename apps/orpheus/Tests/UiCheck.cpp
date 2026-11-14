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

// What the window told the user instead of opening a dialog, and what it asked (the answer is gAnswer).
std::vector<juce::String> gErrors, gAsked;
bool gAnswer = false;

std::unique_ptr<MainComponent> makeBare()
{
    auto w = std::make_unique<MainComponent> (false);
    w->setSize (MainComponent::kDesignWidth, MainComponent::kDesignHeight);
    w->reportError = [] (const juce::String& m) { gErrors.push_back (m); };
    w->ask = [] (const juce::String& message, const juce::String&, std::function<void()> onOk)
    {
        gAsked.push_back (message);
        if (gAnswer)
            onOk();
    };
    return w;
}

// Lets the message loop run until the analysis the window started has been adopted.
void waitIdle (MainComponent& w)
{
    for (int waited = 0; w.isBusy() && waited < 120000; waited += 20)
        juce::MessageManager::getInstance()->runDispatchLoopUntil (20);
}

std::unique_ptr<MainComponent> makeWindow (bool chromatic = false, double detune = 0.0)
{
    auto w = makeBare();
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
        w->setSize (MainComponent::kDesignWidth, MainComponent::kDesignHeight);
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


    // ---- [4] files, limits, and the tools of the editing session -----------------------------------------------------
    std::printf ("\n[4] files and tools\n");
    {
        const auto dir = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("orpheus-check-" + juce::String (juce::Time::currentTimeMillis()));
        dir.createDirectory();
        const auto take = dir.getChildFile ("take");
        take.createDirectory();
        const auto wav = take.getChildFile ("phrase.wav");
        juce::String error;
        {
            auto src = makeWindow();
            test::check (src->session.exportWav (wav, error) && wav.existsAsFile(), "a recording is written to a WAV for the file checks");
            // exportWav goes through a temporary file: nothing is left beside the result, and an existing file is replaced
            test::check (src->session.exportWav (wav, error) && take.getNumberOfChildFiles (juce::File::findFiles, "*") == 1, "exporting over an existing file replaces it and leaves no temporary file behind");
            test::check (! src->session.exportWav (dir.getChildFile ("no/such/folder/x.wav"), error) && error.isNotEmpty(), "an export that cannot be written says so");
        }

        // a bad file dropped during an analysis neither cancels it nor stops the playback
        {
            auto w = makeBare();
            w->loadFile (wav);
            test::check (w->isBusy(), "opening a recording starts its analysis");
            const auto junk = dir.getChildFile ("junk.wav");
            junk.replaceWithText ("this is not audio");
            gErrors.clear();
            w->loadFile (junk);
            test::check (w->isBusy() && gErrors.size() == 1 && gErrors[0].contains ("Cannot read"), "a file that is not audio is refused, and the analysis that is running goes on");
            waitIdle (*w);
            test::check (w->session.isAnalysed() && w->session.notes.size() == 9, "which then finishes (the nine notes)");
            gErrors.clear();
            w->loadFile (junk);
            test::check (w->session.isAnalysed() && ! w->isBusy() && w->session.notes.size() == 9 && gErrors.size() == 1, "and a bad file after it leaves the open recording and its notes alone");
            test::check (w->windowTitle() == "Orpheus - phrase.wav", "the window is titled by its file");

            // dirty marker, and asking before edits are lost
            auto& s = w->session;
            test::check (! s.dirty(), "an opened recording is not 'edited'");
            s.correct (1.0f);
            test::check (s.dirty() && w->windowTitle().endsWithChar ('*'), "an edit marks the session unsaved, with a star in the title");
            s.undo();
            test::check (! s.dirty(), "undoing back to where it was is clean again");
            s.correct (1.0f);
            gAsked.clear();
            gAnswer = false;
            w->loadFile (wav);
            test::check (gAsked.size() == 1 && gAsked[0].contains ("not saved") && ! w->isBusy(), "opening a file over unsaved edits asks first; 'cancel' opens nothing");
            gAnswer = true;
            w->loadFile (wav);
            test::check (w->isBusy(), "and agreeing opens it");
            waitIdle (*w);
            gAnswer = false;
            test::check (! s.dirty(), "the new recording starts clean");

            // Save, silently, to the project it has; relative path and fingerprint in the file
            s.correct (1.0f);
            s.notes[3].gainDb = -4.0f;
            s.changedAll();
            const auto project = take.getChildFile ("phrase.orpheus");
            int titles = 0;
            w->onTitleChanged = [&titles] { ++titles; };
            test::check (w->saveTo (project) && ! s.dirty() && titles >= 1, "Save As writes the project, marks it saved and changes the title");
            const auto xml = juce::XmlDocument::parse (project);
            test::check (xml != nullptr && xml->getStringAttribute ("sourceRelative") == "phrase.wav" && xml->getStringAttribute ("fingerprint").isNotEmpty(), "the project holds the path relative to itself and a fingerprint of the audio");
            test::check (take.getNumberOfChildFiles (juce::File::findFiles, "*.orpheus") == 1 && take.getNumberOfChildFiles (juce::File::findFiles, "*") == 2, "a save leaves no temporary file");
            s.notes[2].gainDb = 3.0f;
            s.changedAll();
            test::check (s.dirty(), "an edit after the save is unsaved again");
            test::check (w->keyPressed (character ('s', juce::ModifierKeys (juce::ModifierKeys::commandModifier))) && ! s.dirty(), "Cmd+S saves to the project without asking where");
            const auto saved = juce::XmlDocument::parse (project);
            test::check (saved != nullptr && saved->getNumChildElements() == 9, "and the file has the nine notes");
            double gain2 = 0.0;
            int idx = 0;
            for (const auto* e : saved->getChildWithTagNameIterator ("Note"))
                if (idx++ == 2)
                    gain2 = e->getDoubleAttribute ("gain");
            test::check (std::fabs (gain2 - 3.0) < 1e-6, "Cmd+S wrote the edit made after the first save");

            // the project, with its folder moved: found by the relative path
            const auto moved = dir.getChildFile ("moved");
            test::check (take.moveFileTo (moved), "(the folder with the audio and the project is moved)");
            auto w2 = makeBare();
            gErrors.clear();
            w2->loadFile (moved.getChildFile ("phrase.orpheus"));
            waitIdle (*w2);
            test::check (gErrors.empty() && w2->session.isAnalysed() && w2->session.notes.size() == 9 && std::fabs (w2->session.notes[3].gainDb + 4.0f) < 1e-5f,
                         "a project whose folder has moved is found by its relative path and brings its edits");
            test::check (! w2->session.dirty() && w2->windowTitle() == "Orpheus - phrase.orpheus", "a loaded project is clean, and titled by the project");

            // same name, other audio: refused by the fingerprint, with the open recording left as it was
            {
                auto other = makeWindow (false, 0.3);
                test::check (other->session.exportWav (moved.getChildFile ("phrase.wav"), error), "(the audio file is replaced by another take)");
            }
            gErrors.clear();
            const auto before = w2->session.notes.size();
            w2->loadFile (moved.getChildFile ("phrase.orpheus"));
            test::check (! w2->isBusy() && gErrors.size() == 1 && gErrors[0].contains ("different recording") && w2->session.notes.size() == before, "a project whose audio is another take is refused, and the open recording stays");
            // ... while one from before fingerprints (nothing but the frame count) still loads
            auto old = juce::XmlDocument::parse (moved.getChildFile ("phrase.orpheus"));
            old->removeAttribute ("fingerprint");
            old->removeAttribute ("sourceRelative");
            old->setAttribute ("source", moved.getChildFile ("phrase.wav").getFullPathName());
            const auto oldFile = moved.getChildFile ("old.orpheus");
            old->writeTo (oldFile);
            gErrors.clear();
            w2->loadFile (oldFile);
            waitIdle (*w2);
            test::check (w2->session.isAnalysed() && w2->session.notes.size() > 0, "a project without a fingerprint (an older one) is still opened");
        }

        // applyXml: what a hand-edited or damaged project can do
        {
            auto w = makeWindow();
            auto& s = w->session;
            auto xml = s.toXml();
            test::check (s.applyXml (*xml), "(the project of this recording applies)");
            xml->setAttribute ("fingerprint", "123abc");
            test::check (! s.applyXml (*xml), "a project with another fingerprint is refused by applyXml too");
            xml->removeAttribute ("fingerprint");
            xml->setAttribute ("a4", 1000.0);
            xml->setAttribute ("glide", 5000.0);
            // notes out of order, one overlapping its neighbour, one with NaN and absurd values
            const auto a = *xml->getChildByName ("Note");
            xml->deleteAllChildElementsWithTagName ("Note");
            const auto note = [&] (int start, int end, double target)
            {
                auto* e = xml->createNewChildElement ("Note");
                e->setAttribute ("start", start);
                e->setAttribute ("end", end);
                e->setAttribute ("target", target);
            };
            const int first = a.getIntAttribute ("start");
            note (first + 400, first + 500, 70.0);
            note (first, first + 100, 60.0);
            note (first + 50, first + 150, 61.0);   // overlaps the one before it: dropped
            note (first + 200, first + 300, 9999.0); // absurd: held to the model's range
            auto* nan = xml->createNewChildElement ("Note");
            nan->setAttribute ("start", first + 600);
            nan->setAttribute ("end", first + 700);
            nan->setAttribute ("target", "nan");
            nan->setAttribute ("gain", 400.0);
            test::check (s.applyXml (*xml) && s.notes.size() == 4, test::fmt ("a project's notes are put in order and the overlapping one dropped (%zu notes)", s.notes.size()));
            bool ordered = true;
            for (size_t i = 1; i < s.notes.size(); ++i)
                ordered = ordered && s.notes[i - 1].end <= s.notes[i].start;
            test::check (ordered && s.notes[0].start == first, "... in order and without overlap");
            test::check (std::fabs (s.notes[2].target - s.notes[2].centre) <= kMaxShiftSemitones + 1e-4f && s.notes[3].target == s.notes[3].centre && s.notes[3].gainDb == 12.0f,
                         "an absurd or NaN target is held to the range, an absurd gain to the knob's");
            test::check (s.key.a4Hz == kMaxA4Hz && s.glideMs == 200.0, "the reference pitch and the glide are held to the knobs' ranges (one constant for the A4 range)");
        }

        // the split that cannot happen
        {
            auto w = makeWindow();
            auto& s = w->session;
            s.correct (1.0f);
            s.undo();
            test::check (s.canRedo(), "(undone: there is a redo step)");
            const auto edge = (juce::int64) s.notes[2].start * s.contour.hop + s.contour.hop;
            w->roll.select ({ 2 });
            test::check (! w->roll.splitAtSample (edge) && s.canRedo() && ! s.canUndo(), "a split too close to the edge of a note is refused and leaves undo and redo as they were");
        }

        // limits on the target
        {
            auto w = makeWindow (true);
            auto& s = w->session;
            w->roll.fitToSession();
            w->refreshInspector();
            const auto from = centreOf (*w, 1);
            dragFrom (w->roll, from, from - juce::Point<float> (0.0f, (float) w->roll.rowH * 90.0f));
            test::check (s.notes[1].target <= s.notes[1].centre + kMaxShiftSemitones + 1e-4f && s.notes[1].target > s.notes[1].centre + 10.0f,
                         test::fmt ("dragging a note 90 rows up stops %.1f semitones above its sung pitch", s.notes[1].target - s.notes[1].centre));
            w->roll.select ({ 4 });
            for (int i = 0; i < 40; ++i)
                w->keyPressed (key (juce::KeyPress::upKey));
            test::check (std::fabs (s.notes[4].target - (s.notes[4].centre + kMaxShiftSemitones)) < 1e-3f, "so does the Up key, held down");
            bool finite = true;
            for (int ch = 0; ch < s.output.getNumChannels(); ++ch)
                for (int i = 0; i < s.output.getNumSamples(); ++i)
                    finite = finite && std::isfinite (s.output.getSample (ch, i));
            test::check (finite, "and the audio stays finite");
        }

        // peak, clip warning, export
        {
            auto w = makeBare();
            auto notes = test::outOfTunePhrase();
            const auto voice = test::sing (notes, 7.0);
            juce::AudioBuffer<float> audio (1, (int) voice.audio.size());
            float peak = 0.0f;
            for (const float v : voice.audio)
                peak = std::max (peak, std::fabs (v));
            for (size_t i = 0; i < voice.audio.size(); ++i)
                audio.setSample (0, (int) i, voice.audio[i] * 0.8f / peak);
            w->session.adoptAudio (std::move (audio), voice.rate, juce::File ("loud.wav"));
            w->analyseNow();
            test::check (std::fabs (w->session.outputPeak() - 0.8f) < 1e-4f, test::fmt ("the output peak is read off the audio (%.3f)", w->session.outputPeak()));
            test::check (w->notes().empty(), "a take at -2 dB: nothing to warn about");
            for (auto& n : w->session.notes)
                n.gainDb = 12.0f;
            w->session.changedAll();
            test::check (w->session.outputPeak() > 1.5f && ui::hasNote (w->notes(), "will clip", ui::Severity::warning), test::fmt ("GAIN +12 dB on every note takes it to %.1f: the strip warns that it will clip", w->session.outputPeak()));
            const auto target = dir.getChildFile ("loud_tuned.wav");
            gAsked.clear();
            gAnswer = false;
            w->exportTo (target);
            test::check (gAsked.size() == 1 && gAsked[0].contains ("peaks at +") && ! target.existsAsFile(), "EXPORT of a result that clips asks first; 'cancel' writes nothing");
            gAnswer = true;
            w->exportTo (target);
            test::check (target.existsAsFile(), "and 'export anyway' writes it");
            gAnswer = false;
            w->session.resetNotes();
            gAsked.clear();
            w->exportTo (dir.getChildFile ("quiet.wav"));
            test::check (gAsked.empty() && dir.getChildFile ("quiet.wav").existsAsFile(), "a result that does not clip is exported without a question");
        }

        // the voice range
        {
            test::check (analysisSettingsFor (0).minHz == 65.0 && analysisSettingsFor (0).maxHz == 1100.0 && analysisSettingsFor (1).maxHz == 400.0 && analysisSettingsFor (3).minHz == 200.0,
                         "the voice ranges map to the tracker's limits (wide is the tracker's own)");
            auto w = makeWindow();
            auto& s = w->session;
            const auto highest = [&]
            {
                double top = 0.0;
                for (int f = 0; f < s.contour.numFrames(); ++f)
                    if (s.contour.isVoiced (f))
                        top = std::max<double> (top, s.contour.midi[(size_t) f]);
                return top;
            };
            const double wide = highest();
            s.correct (1.0f);
            gAsked.clear();
            gAnswer = false;
            w->rangeBox.setSelectedId (2, juce::sendNotificationSync);
            test::check (s.voiceRange == 1 && s.analysedRange == 0, "choosing LOW in the box sets the range for the next analysis only");
            w->reanalyseButton.onClick();
            test::check (gAsked.size() == 1 && gAsked[0].contains ("not saved") && ! w->isBusy(), "RE-ANALYSE over edits asks first");
            gAnswer = true;
            w->reanalyseButton.onClick();
            waitIdle (*w);
            gAnswer = false;
            test::check (s.analysedRange == 1 && highest() < hzToMidi (400.0 * 1.06) && wide > 68.5, test::fmt ("re-analysed LOW (65 to 400 Hz) the contour tops out at MIDI %.1f, wide found %.1f", highest(), wide));
            bool clean = ! s.dirty();
            for (const auto& n : s.notes)
                clean = clean && ! n.edited();
            test::check (clean, "and the edits are gone");
            const auto saved = s.toXml();
            test::check (saved->getIntAttribute ("range") == 1, "the range the notes were found with is saved in the project");
            // a project of another range is analysed with it
            const auto lowWav = dir.getChildFile ("low.wav"), lowProject = dir.getChildFile ("low.orpheus");
            s.exportWav (lowWav, error);
            auto w3 = makeBare();
            w3->loadFile (lowWav);
            waitIdle (*w3);
            w3->session.voiceRange = 1;
            w3->reanalyseButton.onClick();
            waitIdle (*w3);
            w3->saveTo (lowProject);
            auto w4 = makeBare();
            w4->loadFile (lowProject);
            waitIdle (*w4);
            test::check (w4->session.voiceRange == 1 && w4->session.analysedRange == 1 && w4->rangeBox.getSelectedId() == 2, "opening that project analyses with its range, and the box shows it");
        }

        // selecting by how far off, zooming to the selection, copy and paste
        {
            auto w = makeWindow();
            auto& s = w->session;
            auto& roll = w->roll;
            int expected = 0;
            for (const auto& n : s.notes)
                expected += std::fabs (s.key.snap (n.target) - n.target) * 100.0 > 15.0 ? 1 : 0;
            const int found = roll.selectOffScale (15.0);
            test::check (found == expected && found > 0 && (int) roll.selectedIndices().size() == found, test::fmt ("SELECT finds the %d notes more than 15 ct off the scale", found));
            for (const auto i : roll.selectedIndices())
                test::check (std::fabs (s.key.snap (s.notes[i].target) - s.notes[i].target) * 100.0 > 15.0, test::fmt ("... note %zu is one of them", i));
            s.correct (1.0f);
            roll.select ({ 1 });
            test::check (roll.selectOffScale (5.0) == 0 && roll.selectedIndices() == std::vector<size_t> { 1 }, "after Correct all none is off, and the selection is left alone");
            w->offBox.setSelectedId (5, juce::dontSendNotification);
            s.resetNotes();
            w->selectOffButton.onClick();
            test::check (! roll.selectedIndices().empty(), "the SELECT button uses the limit in the box");

            roll.fitToSession();
            roll.select ({ 7 });
            const double visible = roll.visibleSeconds();
            w->keyPressed (character ('z'));
            const double t0 = s.notes[7].start * s.contour.hop / s.sampleRate, t1 = s.notes[7].end * s.contour.hop / s.sampleRate;
            test::check (roll.visibleSeconds() < visible * 0.5 && roll.scrollSec <= t0 && roll.scrollSec + roll.visibleSeconds() >= t1, test::fmt ("Z zooms to the selected note (%.1f s shown instead of %.1f)", roll.visibleSeconds(), visible));
            const auto r = roll.noteRect (s.notes[7]);
            test::check (roll.graphArea().toFloat().contains (r), "and the note is on screen, in time and in pitch");
            w->keyPressed (character ('f'));

            // copy and paste of one note's settings
            s.notes[2].target = s.notes[2].centre + 0.6f;
            s.notes[2].drift = 0.5f;
            s.notes[2].vibrato = 0.3f;
            s.notes[2].formant = -2.0f;
            s.notes[2].gainDb = 2.5f;
            s.changedAll();
            roll.select ({ 2, 3 });
            w->keyPressed (character ('c', juce::ModifierKeys (juce::ModifierKeys::commandModifier)));
            test::check (! w->hasCopiedSettings(), "COPY with two notes selected copies nothing");
            roll.select ({ 2 });
            w->copyButton.onClick();
            test::check (w->hasCopiedSettings(), "COPY takes the settings of the one selected note");
            roll.select ({ 4, 5 });
            const bool wasClean = ! s.notes[4].edited();
            w->keyPressed (character ('v', juce::ModifierKeys (juce::ModifierKeys::commandModifier)));
            bool ok = wasClean;
            for (const size_t i : { (size_t) 4, (size_t) 5 })
                ok = ok && std::fabs (s.notes[i].shift() - 0.6f) < 1e-5f && s.notes[i].drift == 0.5f && s.notes[i].vibrato == 0.3f && s.notes[i].formant == -2.0f && s.notes[i].gainDb == 2.5f;
            test::check (ok, "PASTE puts shift (the same distance from each note's own pitch), drift, vibrato, formant and gain on every selected note");
            w->undo();
            test::check (! s.notes[4].edited() && ! s.notes[5].edited() && s.notes[2].edited(), "and one undo takes the paste back from both");
        }

        // recent files, and the last one again
        {
            const auto store = dir.getChildFile ("recent.txt");
            RecentFiles list (store);
            list.add (dir.getChildFile ("moved/phrase.wav"));
            list.add (dir.getChildFile ("low.wav"));
            list.add (dir.getChildFile ("moved/phrase.wav"));
            list.add (dir.getChildFile ("gone.wav"));
            RecentFiles again (store);
            const auto files = again.files();
            test::check (files.size() == 2 && files[0] == dir.getChildFile ("moved/phrase.wav") && files[1] == dir.getChildFile ("low.wav"),
                         "the recent list is newest first, without repeats, without files that are gone, and is kept in its file");
            {
                RecentFiles many (dir.getChildFile ("many.txt"));
                for (int i = 0; i < 12; ++i)
                {
                    const auto f = dir.getChildFile ("f" + juce::String (i) + ".wav");
                    f.replaceWithText ("x");
                    many.add (f);
                }
                test::check (many.files().size() == RecentFiles::kMax && many.last() == dir.getChildFile ("f11.wav"), "the list holds the last eight");
            }
            auto w = makeBare();
            w->recent = RecentFiles (store);
            w->reopenLast();
            test::check (w->isBusy() && w->session.file == again.last(), "REOPEN LAST opens the file used last");
            waitIdle (*w);
            test::check (w->session.isAnalysed(), "(and it analyses)");
            test::check (w->recent.last() == again.last(), "opening a file puts it first");
        }
        dir.deleteRecursively();
    }

    {
        // Advice: the notes that keep the editor from being used for the wrong job
        using ui::Severity;
        const auto has = [] (const std::vector<ui::Note>& n, const char* word, Severity sev) { return ui::hasNote (n, word, sev); };
        AdviceInput fine;
        fine.hasAudio = true; fine.notes = 9; fine.shortestNoteMs = 300.0;
        test::check (adviceFor (fine).empty(), "a recording with notes, nothing edited: nothing to say");
        auto in = fine; in.hasAudio = false;
        test::check (has (adviceFor (in), "Open a recording", Severity::info), "no recording: say what to open and that it is one voice");
        in = fine; in.busy = true;
        test::check (has (adviceFor (in), "Analysing", Severity::info), "while analysing: say so");
        in = fine; in.playingOriginal = true;
        test::check (has (adviceFor (in), "ORIGINAL is lit", Severity::warning), "ORIGINAL lit: a warning");
        in = fine; in.unsaved = true;
        test::check (has (adviceFor (in), "not saved", Severity::warning), "edits not saved: a warning");
        in.unsaved = false;
        test::check (! has (adviceFor (in), "not saved", Severity::warning), "... which saving takes away");
        in = fine; in.outputPeakDb = 2.5;
        test::check (has (adviceFor (in), "peaks at +2.5 dB", Severity::warning), "a result past full scale: it will clip");
        in.outputPeakDb = -0.5;
        test::check (adviceFor (in).empty(), "... and just under it, nothing to say");
        in = fine; in.largestShiftSemitones = 8.0;
        test::check (has (adviceFor (in), "8 semitones", Severity::warning), "a shift of 8 semitones: the voice changes character");
        in = fine; in.shortestNoteMs = 60.0;
        test::check (has (adviceFor (in), "under 100 ms", Severity::info), "a 60 ms note may be a mis-cut");

        size_t longest = 0;
        for (int mask = 0; mask < 128; ++mask)
        {
            AdviceInput all;
            all.hasAudio = true; all.notes = 9; all.playingOriginal = mask & 1; all.unsaved = mask & 2; all.outputPeakDb = (mask & 64) ? 12.3 : -6.0; all.largestShiftSemitones = (mask & 4) ? 12.0 : 0.0;
            all.shortestNoteMs = (mask & 8) ? 50.0 : 300.0; all.busy = mask & 16; all.hasAudio = ! (mask & 32);
            for (auto& n : adviceFor (all))
                longest = std::max (longest, n.text.size());
        }
        test::check ((int) longest <= ui::kMaxNoteChars, test::fmt ("the longest note is %d characters (a line holds %d)", (int) longest, ui::kMaxNoteChars));

        auto w = makeWindow();
        test::check (w->notes().empty(), "a window with the phrase open and nothing done: no warning");
        w->originalButton.setToggleState (true, juce::sendNotification);
        test::check (has (w->notes(), "ORIGINAL is lit", Severity::warning), "ORIGINAL pressed in the window reaches its notes");
        w->originalButton.setToggleState (false, juce::sendNotification);
        w->session.correct (1.0f);
        test::check (has (w->notes(), "not saved", Severity::warning), "Correct all without saving: the edits are not saved");
        test::check (w->guideButton.isVisible() && std::strlen (guideText()) > 400 && std::string (guideText()).find ("NOT FOR") != std::string::npos,
                     "the GUIDE button is there and its text says what the app is not for");
    }

    return test::finish ("OrpheusUiCheck");
}
