// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 The Pantheon authors

// The window driven without a screen: the project buttons, the transport (play, the loop's wrap, the end of the audio, the metronome, the
// tempo, the time signature, the grid, snap, undo and redo, zoom), the keys, import at the playhead, tracks and chains, projects, renders,
// the notes and the layout; and the arrangement's geometry, drags and menus through its methods.
//
//   OlympusUiCheck

#include "Check.h"
#include "TestPlugin.h"
#include "UI/Arrange.h"
#include "UI/MainComponent.h"

using namespace olympus;
using namespace olympus::gui;

namespace test
{
inline void check (bool ok, const char* what) { check (ok, std::string (what)); }
inline void check (bool ok, const juce::String& what) { check (ok, what.toStdString()); }
}

namespace
{
std::vector<juce::String> gErrors, gAsked;
bool gAnswer = false;

juce::File tempFolder()
{
    static auto folder = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("olympus-uicheck-" + juce::String (juce::Time::currentTimeMillis()));
    folder.createDirectory();
    return folder;
}

juce::File makeWav (const juce::String& name, int length = 24000)
{
    juce::AudioBuffer<float> b (2, length);
    for (int ch = 0; ch < 2; ++ch)
        for (int i = 0; i < length; ++i)
            b.setSample (ch, i, 0.4f * (float) std::sin (2.0 * juce::MathConstants<double>::pi * 330.0 * i / 48000.0));
    auto file = tempFolder().getChildFile (name);
    juce::WavAudioFormat wav;
    std::unique_ptr<juce::OutputStream> stream (file.createOutputStream());
    auto writer = wav.createWriterFor (stream, juce::AudioFormatWriter::Options {}.withSampleRate (48000.0).withNumChannels (2).withBitsPerSample (24));
    writer->writeFromAudioSampleBuffer (b, 0, length);
    return file;
}

std::unique_ptr<MainComponent> makeWindow (bool withPlugins = true)
{
    MainComponent::Options options;
    options.audioDevice = false;
    options.scanPlugins = false;
    options.settingsFile = tempFolder().getChildFile ("settings.xml");
    auto w = std::make_unique<MainComponent> (options);
    w->setSize (MainComponent::kDesignWidth, MainComponent::kDesignHeight);
    if (withPlugins) test::addTestPlugins (w->session.catalog);
    w->reportError = [] (const juce::String& m) { gErrors.push_back (m); };
    w->makeEditorWindow = [] (const juce::String&, juce::AudioProcessorEditor* editor, std::function<void()>) -> std::unique_ptr<juce::Component>
    {
        auto holder = std::make_unique<juce::Component>(); // no native window under test
        holder->addAndMakeVisible (editor);
        holder->setSize (editor->getWidth(), editor->getHeight());
        return holder;
    };
    w->ask = [] (const juce::String& message, const juce::String&, std::function<void()> onOk)
    {
        gAsked.push_back (message);
        if (gAnswer) onOk();
    };
    return w;
}

// A click, and the message loop for the moment the button takes to act on it (JUCE answers triggerClick asynchronously).
void press (juce::Button& b)
{
    b.triggerClick();
    juce::MessageManager::getInstance()->runDispatchLoopUntil (60);
}

bool noteMentions (MainComponent& w, const char* word, ui::Severity sev) { return ui::hasNote (w.notes(), word, sev); }

bool inside (const juce::Component& child, const juce::Component& parent)
{
    return parent.getLocalBounds().contains (child.getBounds());
}

juce::ModifierKeys cmd() { return juce::ModifierKeys::commandModifier; }
juce::ModifierKeys cmdShift() { return juce::ModifierKeys::commandModifier | juce::ModifierKeys::shiftModifier; }

// Every control of the window inside it, below the arrangement's top, and no two of them (or the position read-out) over each other.
void layoutCheck (MainComponent& w, const char* label)
{
    std::vector<juce::Component*> controls { &w.newButton, &w.openButton, &w.saveButton, &w.saveAsButton, &w.addTrackButton, &w.importButton,
                                             &w.pluginsButton, &w.renderButton, &w.guideButton, &w.rewindButton, &w.playButton, &w.loopButton,
                                             &w.metronomeButton, &w.bpmField, &w.signatureBox, &w.snapButton, &w.gridBox, &w.undoButton, &w.redoButton,
                                             &w.zoomOutButton, &w.zoomInButton, &w.zoomFitButton };
    bool ok = inside (w.arrange, w) && w.arrange.getBounds().getWidth() > 0;
    for (size_t i = 0; i < controls.size(); ++i)
    {
        ok = ok && inside (*controls[i], w) && controls[i]->getBottom() <= w.arrange.getY();
        ok = ok && ! controls[i]->getBounds().intersects (w.positionBox);
        for (size_t j = i + 1; j < controls.size(); ++j)
            ok = ok && ! controls[i]->getBounds().intersects (controls[j]->getBounds());
    }
    ok = ok && w.positionBox.getRight() <= w.getWidth() - 16 && w.positionBox.getY() >= 0;
    test::check (ok, juce::String ("layout at ") + label + ": every control inside the window and above the arrangement, none over another");
}

// The arrangement's own mouse event, at a point of its own coordinates.
juce::MouseEvent mouseAt (ArrangeView& view, juce::Point<int> p)
{
    const auto now = juce::Time::getCurrentTime();
    return juce::MouseEvent (juce::Desktop::getInstance().getMainMouseSource(), p.toFloat(), juce::ModifierKeys {}, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f,
                             &view, &view, now, p.toFloat(), now, 1, false);
}
} // namespace

int main()
{
    juce::ScopedJuceInitialiser_GUI gui;

    std::printf ("[1] Window shape\n");
    test::check (ui::hasEditorAspect (MainComponent::kDesignWidth, MainComponent::kDesignHeight) && MainComponent::kDesignWidth % 8 == 0, "16:10, the design width a multiple of 8");

    std::printf ("[2] Empty window\n");
    {
        auto w = makeWindow();
        test::check (w->windowTitle() == "Olympus - Untitled", "the title: " + w->windowTitle());
        test::check (noteMentions (*w, "No audio yet", ui::Severity::info), "it says what to do first");
        auto none = makeWindow (false);
        test::check (noteMentions (*none, "No plugin found", ui::Severity::warning), "with no plugin found it says where to look");
        for (auto& n : none->notes()) test::check ((int) n.text.size() <= ui::kMaxNoteChars, "a note fits its line: " + juce::String (n.text));
        test::check (w->arrange.getBounds().getHeight() > 300, "the arrangement fills the body");
        layoutCheck (*w, "the design size");
        w->setSize (1600, ui::editorHeightFor (1600));
        w->resized();
        layoutCheck (*w, "a wider window");
        w->setSize (MainComponent::kDesignWidth, MainComponent::kDesignHeight);
        w->resized();
    }

    std::printf ("[3] Import and tracks\n");
    {
        auto w = makeWindow();
        const auto a = makeWav ("vocal.wav"), b = makeWav ("drums.wav");
        w->addTracks ({ a.getFullPathName(), b.getFullPathName() });
        test::check (w->session.tracks.size() == 2, "two files, two tracks");
        test::check (w->session.tracks[0]->name == "vocal" && w->session.tracks[0]->clips.size() == 1, "a track is named after its file, with one clip");
        test::check (w->session.tracks[0]->clips[0].start == 0 && w->session.tracks[0]->clips[0].length == 24000, "addTracks puts the clip at 0, the whole file");
        test::check (w->windowTitle().endsWith ("*"), "a change puts a star in the title");
        test::check (w->editor.undoLabel() == "Add clip", "the clip of an import is an undo step (the track is not)");

        // IMPORT AUDIO: at the playhead, a new track
        w->seek (12000);
        const auto c = makeWav ("third.wav");
        w->importFiles ({ c.getFullPathName() }, -1, w->position());
        test::check (w->session.tracks.size() == 3 && w->session.tracks[2]->clips[0].start == 12000, "IMPORT AUDIO: a new track, its clip at the playhead");

        // a file dropped on a lane: a clip on that track, at the drop; the arrangement takes the drop itself
        const auto d = makeWav ("dropped.wav");
        const int y = ArrangeView::kRulerHeight + ArrangeView::kLoopHeight + ArrangeView::kMarkerHeight + 10;
        const int clipsBefore = (int) w->session.tracks[0]->clips.size();
        w->arrange.filesDropped ({ d.getFullPathName() }, 600, y);
        test::check ((int) w->session.tracks[0]->clips.size() == clipsBefore + 1 && w->session.tracks.size() == 3, "a file dropped on a lane is a clip on that track");
        w->undoEdit();
        test::check ((int) w->session.tracks[0]->clips.size() == clipsBefore && w->session.tracks.size() == 3, "the clip's import is undone; the track stays");
        w->redoEdit();

        // a file that is not audio is told, and changes nothing
        gErrors.clear();
        const int before = (int) w->session.tracks.size();
        w->addTracks ({ tempFolder().getChildFile ("missing.wav").getFullPathName() });
        test::check (gErrors.size() == 1 && (int) w->session.tracks.size() == before, "a file that is not audio is told, not silently skipped, and leaves no empty track");

        // ADD TRACK: an empty track, and no undo step for it
        const int tracksNow = (int) w->session.tracks.size();
        press (w->addTrackButton);
        test::check ((int) w->session.tracks.size() == tracksNow + 1 && w->session.tracks.back()->clips.empty(), "ADD TRACK makes an empty track");
        test::check (! w->editor.canUndo(), "adding a track clears the undo history (it is not undone)");
        w->filesDropped ({ makeWav ("dropped2.wav").getFullPathName() }, 0, 0);
        test::check ((int) w->session.tracks.size() == tracksNow + 2, "a file dropped on the window becomes a track, at the playhead");
    }

    std::printf ("[4] Chains and plugin windows\n");
    {
        auto w = makeWindow();
        w->addTracks ({ makeWav ("chain.wav").getFullPathName(), makeWav ("chain2.wav").getFullPathName() });
        juce::String error;
        w->session.addPlugin (0, "test gain", -1, error);
        w->session.addPlugin (0, "test delay", -1, error);
        w->refreshNow();
        test::check (w->session.tracks[0]->chain.size() == 2 && w->session.tracks[1]->chain.empty(), "a chain holds the plugins of its track");
        test::check (noteMentions (*w, "delay it by 64 samples", ui::Severity::info), "a latency is mentioned, with the alignment promise");

        w->openEditor (0, 0);
        test::check (w->openEditors() == 1, "a plugin opens its window");
        w->openEditor (0, 0);
        test::check (w->openEditors() == 1, "opening one that is open brings it to the front, not a second window");
        w->closeEditors();
        test::check (w->openEditors() == 0, "closing the windows");

        // removing a track closes its plugin windows first
        w->openEditor (0, 0);
        w->removeTrack (0);
        test::check (w->openEditors() == 0 && w->session.tracks.size() == 1, "removing a track closes its plugin windows first");
        test::check (! w->editor.canUndo(), "and it is not an undo step");
    }

    std::printf ("[5] Transport\n");
    {
        auto w = makeWindow();
        w->addTracks ({ makeWav ("transport.wav").getFullPathName() });
        w->refreshNow();

        // play and stop: the Space key, the button
        w->keyPressed (juce::KeyPress (juce::KeyPress::spaceKey));
        test::check (w->isPlaying() && w->playButton.getButtonText() == "Stop", "the space bar plays");
        w->keyPressed (juce::KeyPress (juce::KeyPress::spaceKey));
        test::check (! w->isPlaying(), "and stops");
        press (w->playButton);
        test::check (w->isPlaying(), "PLAY");
        press (w->playButton);
        test::check (! w->isPlaying(), "STOP");

        // Return: back to the start, or to the loop's start when the loop is on
        w->seek (5000);
        w->keyPressed (juce::KeyPress (juce::KeyPress::returnKey));
        test::check (w->position() == 0, "Return rewinds to the start");

        // the loop wraps: the callback's own block, cut at the loop's end and continued from its start
        w->editor.setLoop (1000, 2000, true);
        w->syncTransport();
        w->seek (1900);
        w->togglePlay();
        juce::AudioBuffer<float> block (2, 512);
        w->renderAudioBlock (block);
        test::check (w->position() == 1412 && w->isPlaying(), "the loop wraps: 100 samples to the end, then 412 from the loop's start: " + juce::String ((int) w->position()));
        w->togglePlay();
        w->keyPressed (juce::KeyPress (juce::KeyPress::returnKey));
        test::check (w->position() == 1000, "Return with the loop on goes to the loop's start");
        w->editor.setLoop (0, 0, false);
        w->syncTransport();

        // without a loop, playing stops at the end of the audio (the longest track and its latency)
        const auto end = w->session.length() + w->session.maxLatency();
        w->seek (end - 100);
        w->togglePlay();
        w->renderAudioBlock (block);
        test::check (! w->isPlaying() && w->position() == end, "without a loop, playing stops at the end of the audio");
        w->togglePlay();
        test::check (w->position() == 0 && w->isPlaying(), "and playing again starts from the beginning");
        w->togglePlay();

        // the seek while playing is safe (atomic): the callback reads the new position
        w->togglePlay();
        w->seek (3000);
        w->renderAudioBlock (block);
        test::check (w->position() == 3512, "a seek while playing moves the playhead");
        w->togglePlay();

        // the loop, the metronome, snap and the grid, through the controls; the loop and the metronome are undone
        press (w->loopButton);
        test::check (w->session.loopOn && w->session.loopEnd > w->session.loopStart, "LOOP turns on with a range");
        press (w->metronomeButton);
        test::check (w->session.metronomeOn && w->metronomeButton.getToggleState(), "the metronome is on");
        w->editor.undo();
        test::check (! w->session.metronomeOn && w->session.loopOn, "undo takes the metronome back");
        w->editor.undo();
        test::check (! w->session.loopOn, "and the loop");

        const auto label = w->editor.undoLabel();
        press (w->snapButton);
        test::check (! w->session.snapOn && ! w->snapButton.getToggleState(), "SNAP off");
        test::check (w->editor.undoLabel() == label, "snap is a view setting: not an undo step");
        press (w->snapButton);
        test::check (w->session.snapOn, "SNAP on again");

        w->gridBox.setSelectedItemIndex (3, juce::sendNotificationSync);
        test::check (w->session.grid == Grid::Eighth, "the grid, from its list: 1/8");
        test::check (w->editor.undoLabel() == label, "the grid is not an undo step either");

        // the time signature and the tempo: through the Editor, undoable
        w->signatureBox.setSelectedItemIndex (1, juce::sendNotificationSync);
        test::check (w->session.tempo.numerator == 3 && w->session.tempo.denominator == 4, "the time signature 3/4");
        w->editor.undo();
        test::check (w->session.tempo.numerator == 4, "and undo brings back 4/4");

        gErrors.clear();
        w->bpmField.setText ("90");
        w->bpmField.onReturnKey();
        test::check (w->session.tempo.points.size() == 1 && std::abs (w->session.tempo.bpmAtBeat (0.0) - 90.0) < 1e-9, "a tempo typed in the field sets the project's tempo");
        w->editor.undo();
        test::check (std::abs (w->session.tempo.bpmAtBeat (0.0) - 120.0) < 1e-9, "and undo brings back 120");
        w->bpmField.setText ("900");
        w->bpmField.onReturnKey();
        test::check (! gErrors.empty() && gErrors.back().contains ("20 to 300") && std::abs (w->session.tempo.bpmAtBeat (0.0) - 120.0) < 1e-9, "an out-of-range tempo is refused, and told");

        // with tempo changes the field shows the tempo at the playhead and cannot set it
        juce::String tempoError;
        test::check (w->editor.setTempo (16.0, 140.0, tempoError), "a tempo change at beat 16");
        w->refreshNow();
        test::check (w->bpmField.isReadOnly() && w->bpmField.getText() == "120", "with tempo changes, the field shows the tempo at the playhead, read-only");
        w->editor.undo();
        w->refreshNow();
        test::check (! w->bpmField.isReadOnly(), "one tempo again: the field is editable");

        // undo and redo buttons
        w->editor.setMetronome (true);
        w->refreshNow();
        test::check (w->undoButton.isEnabled() && ! w->redoButton.isEnabled(), "undo is enabled, redo is not");
        press (w->undoButton);
        test::check (! w->session.metronomeOn && w->redoButton.isEnabled(), "UNDO takes the metronome back, REDO is enabled");
        press (w->redoButton);
        test::check (w->session.metronomeOn, "REDO brings it back");
        w->editor.setMetronome (false);

        // zoom
        const double zoom = w->arrange.zoom;
        press (w->zoomInButton);
        test::check (w->arrange.zoom > zoom, "zoom in");
        w->keyPressed (juce::KeyPress ('-'));
        test::check (std::abs (w->arrange.zoom - zoom) < 1e-6, "the - key zooms out again");
        w->keyPressed (juce::KeyPress ('+'));
        test::check (w->arrange.zoom > zoom, "the + key zooms in");
        w->keyPressed (juce::KeyPress ('-'));
        press (w->zoomFitButton);
        test::check (w->arrange.zoom > 0.0, "fit");
    }

    std::printf ("[6] Keys\n");
    {
        auto w = makeWindow();
        w->addTracks ({ makeWav ("keys.wav", 48000).getFullPathName() });
        w->refreshNow();
        auto& arrange = w->arrange;
        const auto clipsOn0 = [&w] { return (int) w->session.tracks[0]->clips.size(); };

        // the clip keys go to the window: the arrangement does not take them again
        test::check (! arrange.keyPressed (juce::KeyPress ('s')) && ! arrange.keyPressed (juce::KeyPress (juce::KeyPress::deleteKey))
                     && ! arrange.keyPressed (juce::KeyPress ('d', cmd(), 'd')), "the arrangement leaves S, Delete and Cmd+D to the window");

        w->seek (12000);
        w->keyPressed (juce::KeyPress ('s'));
        test::check (clipsOn0() == 2, "S splits at the playhead");
        w->keyPressed (juce::KeyPress ('z', cmd(), 'z'));
        test::check (clipsOn0() == 1, "Cmd+Z undoes the split");
        w->keyPressed (juce::KeyPress ('z', cmdShift(), 'Z'));
        test::check (clipsOn0() == 2, "Shift+Cmd+Z redoes it");
        w->keyPressed (juce::KeyPress ('z', cmd(), 'z'));

        arrange.selected = { 0, 0 };
        w->keyPressed (juce::KeyPress (juce::KeyPress::deleteKey));
        test::check (clipsOn0() == 0, "Delete removes the selected clip");
        w->keyPressed (juce::KeyPress ('z', cmd(), 'z'));
        test::check (clipsOn0() == 1, "and Cmd+Z brings it back");

        arrange.selected = { 0, 0 };
        w->keyPressed (juce::KeyPress ('d', cmd(), 'd'));
        test::check (clipsOn0() == 2 && w->session.tracks[0]->clips.back().start == 48000, "Cmd+D duplicates the clip after its end");
        w->keyPressed (juce::KeyPress ('z', cmd(), 'z'));

        const auto markers = w->session.markers.size();
        w->seek (6000);
        w->keyPressed (juce::KeyPress ('m'));
        test::check (w->session.markers.size() == markers + 1 && w->session.markers.back().start == 6000, "M adds a marker at the playhead");

        const bool loopBefore = w->session.loopOn;
        w->keyPressed (juce::KeyPress ('l'));
        test::check (w->session.loopOn != loopBefore, "L switches the loop");
        w->keyPressed (juce::KeyPress ('l'));
        test::check (w->session.loopOn == loopBefore, "and back");
        test::check (! w->keyPressed (juce::KeyPress ('q')), "a key that means nothing is not taken");
    }

    std::printf ("[7] Project and render\n");
    {
        auto w = makeWindow();
        const auto a = makeWav ("keys.wav");
        w->addTracks ({ a.getFullPathName() });
        juce::String error;
        w->session.addPlugin (0, "test gain", -1, error);
        w->editor.setTempo (0.0, 100.0, error);
        w->editor.addMarker ("Intro", 0);
        w->editor.setLoop (1000, 5000, true);
        w->refreshNow();
        const auto file = tempFolder().getChildFile ("song.olympus");
        test::check (w->saveTo (file) && ! w->session.dirty && w->windowTitle() == "Olympus - song", "save: the star goes: " + w->windowTitle());
        test::check (Settings::load (tempFolder().getChildFile ("settings.xml")).lastProject == file.getFullPathName(), "the last project is remembered");
        w->editor.setGrid (Grid::Half);
        w->keyPressed (juce::KeyPress ('s', cmd(), 's'));
        test::check (! w->session.dirty, "Cmd+S saves the project");

        gAsked.clear();
        gAnswer = false;
        w->session.dirty = true;
        w->askToDiscard ("Quit?", [] {});
        test::check (gAsked.size() == 1, "with unsaved changes it asks before going on");
        gAnswer = true;
        bool went = false;
        w->askToDiscard ("Quit?", [&] { went = true; });
        test::check (went, "and goes on when told to");
        w->session.dirty = false;
        went = false;
        w->askToDiscard ("Quit?", [&] { went = true; });
        test::check (went && gAsked.size() == 2, "with nothing unsaved it does not ask");

        // tempo, markers and loop come back with the project; the view is zoomed to fit
        auto other = makeWindow (false);
        other->openProject (file);
        test::check (other->session.tracks.size() == 1, "the project opens in another window");
        test::check (noteMentions (*other, "not installed here", ui::Severity::warning), "a plugin that is not installed is said");
        test::check (! gErrors.empty() && gErrors.back().contains ("not installed"), "and told in a message too");
        test::check (std::abs (other->session.tempo.bpmAtBeat (0.0) - 100.0) < 1e-9 && other->bpmField.getText() == "100", "the tempo comes back");
        test::check (other->session.markers.size() == 1 && other->session.markers[0].name == "Intro", "the markers come back");
        test::check (other->session.loopOn && other->session.loopEnd == 5000 && other->loopButton.getToggleState(), "the loop comes back");
        test::check (other->session.grid == Grid::Half, "the grid comes back");
        test::check (other->arrange.zoom != ArrangeView::kDefaultZoom, "after opening, the view is zoomed to fit");
        test::check (! other->editor.canUndo(), "opening clears the undo history");

        w->newProject();
        test::check (w->session.tracks.empty() && w->windowTitle() == "Olympus - Untitled" && ! w->editor.canUndo(), "NEW empties the session");
        w->openProject (file);
        const auto stems = tempFolder().getChildFile ("out");
        test::check (w->renderTo (stems).isEmpty() && stems.getChildFile ("01-keys.wav").existsAsFile() && stems.getChildFile ("mix.wav").existsAsFile(), "RENDER STEMS writes the stem and the mix");

        w->filesDropped ({ makeWav ("dropped.wav").getFullPathName() }, 0, 0);
        test::check (w->session.tracks.size() == 2, "a dropped file becomes a track");
    }

    std::printf ("[8] Missing audio\n");
    {
        auto w = makeWindow();
        const auto a = makeWav ("gone.wav");
        w->addTracks ({ a.getFullPathName() });
        const auto file = tempFolder().getChildFile ("gone.olympus");
        w->saveTo (file);
        a.deleteFile();
        auto other = makeWindow();
        other->openProject (file);
        test::check (noteMentions (*other, "audio file is missing", ui::Severity::warning), "a missing audio file is said, and the track stays");
    }

    std::printf ("[9] Arrange view\n");
    {
        olympus::Session session;
        test::addTestPlugins (session.catalog);
        olympus::Editor editor (session);
        juce::String error;
        const auto a = makeWav ("arrange-a.wav"), b = makeWav ("arrange-b.wav");
        test::check (session.addTrack (a, "Vocal", error) && session.addTrack (b, "Drums", error), "two tracks from two files");
        test::check (session.addClip (0, a, 30000, error) == 1, "a second clip on the first track, at 30000");
        session.addPlugin (0, "test gain", -1, error);

        ArrangeView view (session, editor);
        view.setSize (ArrangeView::kDesignWidth, ArrangeView::kDesignHeight);
        view.zoomToFit();
        view.refresh();
        const double rate = session.sampleRate;
        auto same = [] (ClipRef r, int t, int c) { return r.track == t && r.clip == c; };
        auto clipCount = [&session] (int t) { return (int) session.tracks[(size_t) t]->clips.size(); };

        // geometry: after zoomToFit everything is across the lanes, and clipAt and clipBounds agree with the model
        {
            bool fits = true, agrees = true, scaled = true;
            const double pps = view.zoom / rate;
            for (int t = 0; t < 2; ++t)
                for (int c = 0; c < clipCount (t); ++c)
                {
                    const auto r = view.clipBounds ({ t, c });
                    const auto& clip = session.tracks[(size_t) t]->clips[(size_t) c];
                    fits = fits && r.getRight() <= view.getWidth() && r.getX() >= ArrangeView::kHeaderWidth;
                    agrees = agrees && same (view.clipAt (r.getCentre()), t, c);
                    scaled = scaled && std::abs (r.getWidth() - (double) clip.length * pps) <= 1.5;
                }
            test::check (fits, "zoomToFit: every clip is across the lanes, inside the window");
            test::check (agrees, "clipAt of a clip's centre is that clip (clipBounds)");
            test::check (scaled, "a clip's width is its length at the zoom");
            test::check (! view.clipAt ({ 10, 10 }).valid() && view.clipBounds ({ 0, 9 }).isEmpty() && ! view.clipAt ({ 400, 2 }).valid(), "nothing in the header column, the ruler or past the last clip");
        }

        // a drag moves the clip, snapped to the grid (a quarter note, 24000 samples at 120 bpm); undo puts it back
        {
            const auto p = view.clipBounds ({ 0, 0 }).getCentre();
            view.mouseDown (mouseAt (view, p));
            view.mouseDrag (mouseAt (view, p + juce::Point<int> (400, 0)));
            view.mouseUp (mouseAt (view, p + juce::Point<int> (400, 0)));
            const auto moved = session.tracks[0]->clips[0].start;
            test::check (moved == 24000, "a drag moves the clip, snapped to the grid: " + juce::String (moved));
            test::check (editor.undo() && session.tracks[0]->clips[0].start == 0, "undo restores the clip's place");
            view.refresh();
        }

        // a drag of the right edge trims the clip (snap off so the new length is not moved to the grid)
        {
            editor.setSnap (false);
            const auto r = view.clipBounds ({ 1, 0 });
            const auto p = juce::Point<int> (r.getRight() - 2, r.getCentreY());
            view.mouseDown (mouseAt (view, p));
            view.mouseDrag (mouseAt (view, p + juce::Point<int> (-200, 0)));
            view.mouseUp (mouseAt (view, p + juce::Point<int> (-200, 0)));
            const auto length = session.tracks[1]->clips[0].length;
            test::check (length > 0 && length < 24000, "an edge drag trims the clip: " + juce::String (length));
            test::check (editor.undo() && session.tracks[1]->clips[0].length == 24000, "undo restores the length");
            editor.setSnap (true);
            view.refresh();
        }

        // split at the playhead gives two clips (the window calls this from S)
        {
            view.playhead = 12000;
            view.selected = { 0, 0 };
            view.splitAtPlayhead();
            const auto& t0 = session.tracks[0]->clips;
            test::check (t0.size() == 3 && t0[0].length == 12000 && t0[1].start == 12000 && t0[1].length == 12000, "split at the playhead gives two clips");
            test::check (editor.undo() && clipCount (0) == 2, "undo joins them again");
        }

        // delete and duplicate through the methods the window calls
        {
            view.selected = { 1, 0 };
            view.deleteSelected();
            test::check (clipCount (1) == 0, "deleteSelected removes the clip");
            test::check (editor.undo() && clipCount (1) == 1, "undo brings it back");
            view.selected = { 0, 0 };
            view.duplicateSelected();
            test::check (clipCount (0) == 3 && session.tracks[0]->clips.back().start == 24000, "duplicateSelected: the copy starts where the original ends");
            editor.undo();
            view.refresh();
        }

        // snapping: the nearest grid line, halfway going up
        test::check (editor.snap (12345) == 24000 && editor.snap (11000) == 0, "snap: the nearest quarter note");

        // painting, at several zooms, to an image
        {
            bool ok = true;
            for (double z : { 0.05, 80.0, 4000.0 })
            {
                view.zoom = z;
                view.refresh();
                auto img = view.createComponentSnapshot (view.getLocalBounds());
                ok = ok && ! img.isNull() && img.getWidth() == ArrangeView::kDesignWidth;
            }
            test::check (ok, "painting to an image does not crash, at the smallest, a normal and the largest zoom");
        }
    }

    std::printf ("[10] Session: empty tracks\n");
    {
        olympus::Session session;
        const int index = session.addEmptyTrack ("Empty");
        test::check (index == 0 && session.tracks[0]->clips.empty() && session.tracks[0]->name == "Empty", "an empty track is a valid track");
        test::check (session.length() == 0, "and it has no length");
    }

    tempFolder().deleteRecursively();
    return test::finish ("OlympusUiCheck");
}
