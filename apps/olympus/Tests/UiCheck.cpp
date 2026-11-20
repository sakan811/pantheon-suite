// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 The Pantheon authors

// The window driven without a screen: the project buttons, the transport (play, the loop's wrap, the end of the audio, the metronome, the
// tempo, the time signature, the grid, snap, undo and redo, zoom), the keys, import at the playhead, tracks and chains, projects, renders,
// the notes and the layout; and the arrangement's geometry, drags and menus through its methods.
//
//   OlympusUiCheck

#include "Check.h"
#include "TestPlugin.h"
#include <thread>
#include "UI/Arrange.h"
#include "UI/MainComponent.h"
#include <atomic>

using namespace olympus;
using namespace olympus::gui;

namespace test
{
inline void check (bool ok, const char* what) { check (ok, std::string (what)); }
inline void check (bool ok, const juce::String& what) { check (ok, what.toStdString()); }
}

namespace
{
std::vector<juce::String> gErrors, gAsked, gNotes;
std::vector<std::function<void()>> gClosers;   // the "closed" callbacks of the plugin windows the checks opened
bool gAnswer = false;                          // the answer the checks give to the questions (Discard / Open it / the save target)
juce::File gSaveTarget;                        // what the Save As chooser of a check returns (empty: cancelled)
bool gRecoverAnswer = false;                   // "Open it" when true

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

// Each window's takes go to a folder of their own, so the files a check finds are the ones it made.
juce::File takesFolder()
{
    static int windows = 0;
    return tempFolder().getChildFile ("takes-" + juce::String (++windows));
}

// The window of the checks. Its plugins are filled before its monitor chain is read (Options::fillCatalog), as the app's scan does. The
// settings (and so monitor.xml) are the given file, else the folder's shared one.
std::unique_ptr<MainComponent> makeWindow (bool withPlugins = true, const juce::File& settings = {})
{
    MainComponent::Options options;
    options.audioDevice = false;
    options.scanPlugins = false;
    options.settingsFile = settings != juce::File() ? settings : tempFolder().getChildFile ("settings.xml");
    options.recordings = takesFolder();
    if (withPlugins)
        options.fillCatalog = [] (olympus::Catalog& catalog) { test::addTestPlugins (catalog); };
    auto w = std::make_unique<MainComponent> (options);
    w->setSize (MainComponent::kDesignWidth, MainComponent::kDesignHeight);
    w->reportError = [] (const juce::String& m) { gErrors.push_back (m); };
    w->makeEditorWindow = [] (const juce::String&, juce::AudioProcessorEditor* editor, std::function<void()> closed) -> std::unique_ptr<juce::Component>
    {
        gClosers.push_back (closed);
        auto holder = std::make_unique<juce::Component>(); // no native window under test
        holder->addAndMakeVisible (editor);
        holder->setSize (editor->getWidth(), editor->getHeight());
        return holder;
    };
    // the questions: Discard when gAnswer, else Cancel (the checks of the unsaved changes); the save chooser returns gSaveTarget
    w->askUnsaved = [] (const juce::String& message)
    {
        gAsked.push_back (message);
        return gAnswer ? MainComponent::Unsaved::discard : MainComponent::Unsaved::cancel;
    };
    w->chooseSaveFile = [] (const juce::String&, const juce::File&, std::function<void (const juce::File&)> done) { done (gSaveTarget); };
    w->chooseRenderOptions = [] (RenderOptions&) { return true; };   // the dialog is answered by the checks: the options as they are
    w->askRecover = [] (const juce::String&) { return gRecoverAnswer; };
    w->notify = [] (const juce::String& message) { gNotes.push_back (message); };
    w->reveal = [] (const juce::File&) {};
    return w;
}

// A running device the checks describe: four inputs (the tests record on the first and the second pair), the given rate and latencies.
MainComponent::DeviceState fakeDevice (double rate = 48000.0, int inLatency = 100, int outLatency = 50)
{
    MainComponent::DeviceState d;
    d.running = true;
    d.inputName = "Interface";
    d.outputName = "Headphones";
    d.rate = rate;
    d.bufferSize = 256;
    d.inputs = 4;
    d.outputs = 2;
    d.inputLatency = inLatency;
    d.outputLatency = outLatency;
    d.activeInputs.setRange (0, 4, true);
    return d;
}

// What the device does for `frames` frames, in odd block sizes: input 0 carries a 440 Hz sine at 0.5, inputs 2 and 3 the same tone at 0.25.
// `fed` counts the frames the device has run (the sine's phase). Returns the peak of the outputs.
float feedDevice (MainComponent& w, juce::int64& fed, double rate, int frames)
{
    constexpr int kSizes[] = { 100, 512, 37, 1100, 256, 999, 3, 2048, 777 };
    float peak = 0.0f;
    for (int block = 0; frames > 0; ++block)
    {
        const int n = juce::jmin (frames, kSizes[block % 9]);
        std::vector<std::vector<float>> in (4, std::vector<float> ((size_t) n, 0.0f));
        std::vector<std::vector<float>> out (2, std::vector<float> ((size_t) n, 0.0f));
        for (int i = 0; i < n; ++i)
        {
            const double phase = 2.0 * juce::MathConstants<double>::pi * 440.0 * (double) (fed + i) / rate;
            in[0][(size_t) i] = 0.5f * (float) std::sin (phase);
            in[2][(size_t) i] = 0.25f * (float) std::sin (phase);
            in[3][(size_t) i] = in[2][(size_t) i];
        }
        const float* inPtr[4] = { in[0].data(), in[1].data(), in[2].data(), in[3].data() };
        float* outPtr[2] = { out[0].data(), out[1].data() };
        w.io.deviceBlock (inPtr, 4, outPtr, 2, n);
        for (auto& channel : out)
            for (float v : channel)
                peak = juce::jmax (peak, std::abs (v));
        fed += n;
        frames -= n;
    }
    return peak;
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

// The peak the device gets from one pass of the playback, from its start: the monitor chain is in it when the chain is on.
float playPeak (MainComponent& w)
{
    w.seek (0);
    if (! w.isPlaying())
        w.togglePlay();
    juce::int64 fed = 0;
    return feedDevice (w, fed, 48000.0, 8000);
}

// What a file holds, byte for byte (an empty result when there is no file).
juce::MemoryBlock bytesOf (const juce::File& f)
{
    juce::MemoryBlock data;
    f.loadFileAsData (data);
    return data;
}

juce::ModifierKeys cmd() { return juce::ModifierKeys::commandModifier; }
juce::ModifierKeys cmdShift() { return juce::ModifierKeys::commandModifier | juce::ModifierKeys::shiftModifier; }

// Every control of the window inside it, below the arrangement's top, and no two of them (or the position read-out) over each other.
void layoutCheck (MainComponent& w, const char* label)
{
    std::vector<juce::Component*> controls { &w.newButton, &w.openButton, &w.saveButton, &w.saveAsButton, &w.addTrackButton, &w.importButton,
                                             &w.pluginsButton, &w.renderButton, &w.guideButton, &w.rewindButton, &w.playButton, &w.loopButton,
                                             &w.metronomeButton, &w.bpmField, &w.signatureBox, &w.snapButton, &w.gridBox, &w.undoButton, &w.redoButton,
                                             &w.zoomOutButton, &w.zoomInButton, &w.zoomFitButton, &w.audioButton, &w.recordButton, &w.deviceLabel };
    bool ok = inside (w.arrange, w) && w.arrange.getBounds().getWidth() > 0;
    for (size_t i = 0; i < controls.size(); ++i)
    {
        ok = ok && inside (*controls[i], w) && controls[i]->getBottom() <= w.arrange.getY();
        ok = ok && ! controls[i]->getBounds().intersects (w.positionBox);
        for (size_t j = i + 1; j < controls.size(); ++j)
            ok = ok && ! controls[i]->getBounds().intersects (controls[j]->getBounds());
    }
    ok = ok && w.positionBox.getRight() <= w.getWidth() - 16 && w.positionBox.getY() >= 0;
    // the master section (row C, left of the device line): its controls and its meter inside the window, over no other control
    const std::vector<juce::Component*> master { &w.masterFader, &w.masterPeakLabel, &w.masterClipLamp };
    for (auto* m : master)
    {
        ok = ok && inside (*m, w) && m->getBottom() <= w.arrange.getY() && ! m->getBounds().intersects (w.deviceLabel.getBounds());
        for (auto* c : controls)
            ok = ok && ! m->getBounds().intersects (c->getBounds());
    }
    ok = ok && w.masterMeterArea().getRight() <= (float) w.deviceLabel.getX() && w.masterMeterArea().getBottom() <= (float) w.arrange.getY()
         && w.masterMeterArea().getY() >= 0.0f;
    test::check (ok, juce::String ("layout at ") + label + ": every control inside the window and above the arrangement, none over another");
}

// A one-second stereo WAV of a 330 Hz sine at `level` (its peak is `level`): a clip at a known level for the master's meter.
juce::File makeLevelWav (const juce::String& name, float level, int length = 48000)
{
    juce::AudioBuffer<float> b (2, length);
    for (int ch = 0; ch < 2; ++ch)
        for (int i = 0; i < length; ++i)
            b.setSample (ch, i, level * (float) std::sin (2.0 * juce::MathConstants<double>::pi * 330.0 * i / 48000.0));
    auto file = tempFolder().getChildFile (name);
    file.deleteFile();
    juce::WavAudioFormat wav;
    std::unique_ptr<juce::OutputStream> stream (file.createOutputStream());
    auto writer = wav.createWriterFor (stream, juce::AudioFormatWriter::Options {}.withSampleRate (48000.0).withNumChannels (2).withBitsPerSample (24));
    writer->writeFromAudioSampleBuffer (b, 0, length);
    return file;
}

// One block of playback through the transport's own path (renderAudioBlock, the code the device's callback runs), then the master's meter
// read at `ms`. The transport must be playing for the block to carry sound.
void masterBlock (MainComponent& w, double ms, int frames = 4800)
{
    juce::AudioBuffer<float> block (2, frames);
    block.clear();
    w.renderAudioBlock (block);
    w.updateMaster (ms);
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

    std::printf ("[11] Devices and the notes\n");
    {
        auto w = makeWindow();
        test::check (w->deviceSummary() == "no audio device", "with no device the top bar says so");
        w->setDeviceForTest (fakeDevice());
        test::check (w->deviceSummary() == "IN: Interface / OUT: Headphones / 48.0 kHz / 256 smp", "the top bar: " + w->deviceSummary());
        test::check (std::abs (w->roundTripMs() - 150.0 * 1000.0 / 48000.0) < 1e-9, "the round trip: input and output latency, in ms");
        test::check (w->deviceLabel.getText() == w->deviceSummary(), "the device label shows the summary");

        // the IN menu's entries: each active input, then each neighbouring pair of active inputs
        const auto all = MainComponent::inputChoices (fakeDevice());
        bool ok = all.size() == 7 && all[0].label == "In 1" && all[0].right == -1 && all[1].label == "In 1+2" && all[1].left == 0 && all[1].right == 1;
        ok = ok && all[2].label == "In 2" && all[3].label == "In 2+3" && all[5].label == "In 3+4" && all[5].left == 2 && all[5].right == 3 && all[6].label == "In 4";
        test::check (ok, "the IN menu: each input alone, then each pair of neighbours");
        auto gap = fakeDevice();
        gap.activeInputs.clearBit (1);
        test::check (MainComponent::inputChoices (gap).size() == 4 && MainComponent::inputChoices (MainComponent::DeviceState {}).empty(),
                     "an inactive input is not offered, and with no device the menu is empty");

        // the notes that fire for an armed track and the device
        juce::String error;
        w->addTracks ({ makeWav ("notes.wav").getFullPathName() });
        w->session.setTrackArm (0, true, error);
        w->setDeviceForTest (MainComponent::DeviceState {});
        w->refreshNow();
        test::check (noteMentions (*w, "no audio input is running", ui::Severity::warning), "an armed track with no device input is said");
        w->setDeviceForTest (fakeDevice (44100.0, 0, 0));
        test::check (noteMentions (*w, "Takes are recorded at 44.1 kHz", ui::Severity::info), "a take at another rate than the session's is said");
        w->setDeviceForTest (fakeDevice (48000.0, 1000, 1000));
        test::check (noteMentions (*w, "round trip is 42 ms", ui::Severity::warning), "a round trip above 30 ms says to lower the buffer size");
        w->setDeviceForTest (fakeDevice());
        test::check (! noteMentions (*w, "round trip", ui::Severity::warning) && ! noteMentions (*w, "no audio input", ui::Severity::warning),
                     "with a device and the same rate, neither note fires");

        // the device's settings come back from the settings file as they were saved
        const juce::String state = "<DEVICESETUP deviceType=\"Test\" audioInputDeviceName=\"Interface\" audioOutputDeviceName=\"Headphones\" audioDeviceRate=\"48000\" audioDeviceBufferSize=\"256\"/>";
        olympus::Settings saved;
        saved.audioDeviceState = state;
        const auto file = tempFolder().getChildFile ("device-settings.xml");
        saved.save (file);
        const auto back = olympus::Settings::load (file);
        const auto parsed = juce::parseXML (back.audioDeviceState);
        test::check (parsed != nullptr && parsed->hasTagName ("DEVICESETUP") && parsed->getStringAttribute ("audioInputDeviceName") == "Interface"
                         && parsed->getIntAttribute ("audioDeviceBufferSize") == 256, "the device's settings round-trip through the settings file");
    }

    std::printf ("[12] Recording\n");
    {
        auto w = makeWindow();
        w->addTracks ({ makeWav ("playback.wav").getFullPathName() });   // a clip on track 0, 24000 frames at 48 kHz
        w->addEmptyTrack();
        juce::String error;

        // refused: with no device, and with no track armed
        gErrors.clear();
        test::check (! w->startRecording() && ! w->isRecording() && gErrors.size() == 1 && gErrors.back().contains ("No audio device"),
                     "REC without a running device is refused, and says so");
        w->setDeviceForTest (fakeDevice());
        gErrors.clear();
        test::check (! w->startRecording() && ! w->isRecording() && gErrors.size() == 1 && gErrors.back().contains ("No track is armed"),
                     "REC with no track armed is refused, and says so");
        auto noInputs = fakeDevice();
        noInputs.inputs = 0;
        noInputs.activeInputs.clear();
        w->setDeviceForTest (noInputs);
        press (*w->arrange.armButton (0));
        gErrors.clear();
        test::check (! w->startRecording() && gErrors.size() == 1 && gErrors.back().contains ("no input channels"), "REC with a device that has no inputs is refused");
        w->setDeviceForTest (fakeDevice());

        // ARM: the header's R button on track 0, the session on track 1; each track its own input, shown on its header
        test::check (w->session.tracks[0]->armed && w->arrange.armButton (0)->getToggleState(), "the R button arms the track");
        test::check (w->session.setTrackArm (1, true, error) && w->session.tracks[1]->armed, "a track is armed through the session too");
        test::check (w->session.setTrackInput (0, 0, -1, error) && w->session.setTrackInput (1, 2, 3, error), "two tracks on different inputs");
        w->refreshNow();
        test::check (w->arrange.inputButton (0)->getButtonText() == "IN 1" && w->arrange.inputButton (1)->getButtonText() == "IN 3+4",
                     "each header shows its input: IN 1 and IN 3+4");
        test::check (w->arrange.armButton (1)->getToggleState(), "and the lamp of an armed track is lit");

        // REC with the R key, from the playhead at 4800: the playback clip is heard while the takes run
        w->seek (4800);
        w->keyPressed (juce::KeyPress ('r'));
        test::check (w->isRecording() && w->isPlaying() && w->recordButton.getToggleState(), "the R key starts the takes; the transport plays and REC is lit");
        juce::int64 fed = 0;
        const float heard = feedDevice (*w, fed, 48000.0, 30000);
        test::check (heard > 0.1f, "the playback clip is heard while recording: peak " + juce::String (heard));
        test::check (w->isRecording() && w->position() == 4800 + 30000, "the take keeps running past the end of the audio");

        // while the take runs, the loop does not wrap: the playhead goes on past the loop's end
        w->editor.setLoop (5000, 6000, true);
        w->syncTransport();
        feedDevice (*w, fed, 48000.0, 1000);
        test::check (w->position() == 4800 + 31000, "no loop wrap while recording: " + juce::String ((int) w->position()));
        w->editor.setLoop (0, 0, false);
        w->syncTransport();

        // the playhead cannot be moved during a take
        gErrors.clear();
        w->seek (0);
        test::check (w->position() == 4800 + 31000 && ! gErrors.empty(), "the playhead does not move during a take, and says so");

        // STOP (the Space key): the takes go on their tracks, minus the device's round trip (100 + 50 samples)
        w->keyPressed (juce::KeyPress (juce::KeyPress::spaceKey));
        test::check (! w->isRecording() && ! w->isPlaying(), "STOP ends the takes and the playback");
        const auto& t0 = w->session.tracks[0]->clips;
        const auto& t1 = w->session.tracks[1]->clips;
        const juce::int64 placed = 4800 - 150;
        test::check (t0.size() == 2 && t1.size() == 1, "each armed track has its take as a clip");
        test::check (t0[1].start == placed && t1[0].start == placed, "the takes start at the record start less the round trip: " + juce::String ((int) placed));
        test::check (t0[1].length == 31000 && t1[0].length == 31000, "a take is as long as it recorded");
        test::check (w->arrange.selected.track == 0 && w->arrange.selected.clip == 1, "the first new clip is selected");
        test::check (w->session.dirty && w->editor.undoLabel() == "Record", "the session is dirty, and the takes are one step called Record");
        test::check (w->arrange.recordingFrom < 0.0 && w->arrange.recordingTracks.empty(), "the recording strip is gone");

        // the takes are the device's sine: track 0 took input 0 (0.5), track 1 inputs 2 and 3 (0.25)
        test::check (t0[1].audio != nullptr && std::abs (t0[1].audio->getSample (0, 1000) - 0.5f * (float) std::sin (2.0 * juce::MathConstants<double>::pi * 440.0 * 1000.0 / 48000.0)) < 1e-4f,
                     "the take on IN 1 is the sine of IN 1");
        test::check (t1[0].audio != nullptr && std::abs (t1[0].audio->getSample (1, 1000) - 0.25f * (float) std::sin (2.0 * juce::MathConstants<double>::pi * 440.0 * 1000.0 / 48000.0)) < 1e-4f,
                     "the take on IN 3+4 is the sine of IN 3 and 4");
        test::check (w->recordingFolder().getNumberOfChildFiles (juce::File::findFiles, "*.wav") == 2, "two take files on disk, in the recordings folder");

        // one undo step takes both out, and redo brings them back
        test::check (w->editor.undo() && w->session.tracks[0]->clips.size() == 1 && w->session.tracks[1]->clips.empty(), "one undo takes both takes out");
        test::check (w->editor.redo() && w->session.tracks[0]->clips.size() == 2 && w->session.tracks[1]->clips.size() == 1, "and redo brings them back");

        // a take that is stopped by the device is kept: the device stops, the timer ends the take with what it has
        const int before = (int) w->session.tracks[0]->clips.size();
        w->seek (0);
        w->toggleRecord();
        juce::int64 more = 0;
        feedDevice (*w, more, 48000.0, 2000);
        w->io.audioDeviceStopped();
        juce::MessageManager::getInstance()->runDispatchLoopUntil (100);
        test::check (! w->isRecording() && (int) w->session.tracks[0]->clips.size() == before + 1
                         && w->session.tracks[0]->clips.back().length == 2000 - 150 && w->session.tracks[0]->clips.back().offset == 150
                         && ! gErrors.empty() && gErrors.back().contains ("stopped while recording"),
                     "a device that stops ends the take, which is kept, and the window says so: the 150 samples of latency before 0 are skipped");

        // a change of the device during a take ends it as well
        w->toggleRecord();
        feedDevice (*w, more, 48000.0, 512);
        w->io.audioDeviceAboutToStart (nullptr);
        test::check (! w->isRecording(), "a device change ends the take before the new device runs");
    }

    std::printf ("[13] Recording in an empty session\n");
    {
        auto w = makeWindow();
        juce::String error;
        w->session.addEmptyTrack ("Mic");
        w->session.setTrackArm (0, true, error);
        w->setDeviceForTest (fakeDevice (44100.0, 0, 0));
        w->toggleRecord();
        juce::int64 fed = 0;
        feedDevice (*w, fed, 44100.0, 4410);
        w->toggleRecord();
        test::check (w->session.sampleRate == 44100.0 && w->session.tracks[0]->clips.size() == 1, "the first take sets the session's rate: " + juce::String (w->session.sampleRate));
    }

    std::printf ("[17] Inputs by their own number: the callback gets only the switched-on inputs; REC refuses one that is off; the take holds its own input\n");
    {
        auto w = makeWindow();
        juce::String error;
        w->session.addEmptyTrack ("Drums");
        w->session.setTrackArm (0, true, error);

        // the device has ONE active input, input 2 (absolute number 1), so the callback passes one channel
        MainComponent::DeviceState only = fakeDevice (48000.0, 0, 0);
        only.inputs = 1;
        only.activeInputs.clear();
        only.activeInputs.setBit (1);
        only.inputNames = { "Mic 1", "Guitar" };
        const auto choices = MainComponent::inputChoices (only);
        test::check (choices.size() == 1 && choices[0].label == "In 2: Guitar" && choices[0].left == 1,
                     "the IN menu lists only the switched-on input, by its number and its name");
        w->setDeviceForTest (only);

        test::check (w->session.setTrackInput (0, 0, -1, error), "Drums is set to input 1, which is switched off");
        gErrors.clear();
        test::check (! w->startRecording() && ! w->isRecording() && gErrors.size() == 1 && gErrors.back() == "Drums records input 1, which is not switched on in AUDIO",
                     "REC refuses it, naming the track and the input");

        test::check (w->session.setTrackInput (0, 1, -1, error) && w->startRecording(), "on input 2, the take starts");
        std::vector<float> sine (4800);
        for (int i = 0; i < (int) sine.size(); ++i)
            sine[(size_t) i] = 0.5f * (float) std::sin (2.0 * juce::MathConstants<double>::pi * 440.0 * i / 48000.0);
        std::vector<float> outL (1200), outR (1200);
        float* outPtr[2] = { outL.data(), outR.data() };
        for (int b = 0; b < (int) sine.size(); b += 1200)
        {
            const float* inPtr[1] = { sine.data() + b };   // one channel: the only active input
            w->io.deviceBlock (inPtr, 1, outPtr, 2, 1200);
        }
        w->stopRecording();
        const auto& clips = w->session.tracks[0]->clips;
        test::check (clips.size() == 1 && clips[0].audio != nullptr && clips[0].length == 4800, "the take is a clip of what the input heard");
        test::check (clips.size() == 1 && clips[0].audio != nullptr && std::abs (clips[0].audio->getSample (0, 1000) - sine[1000]) < 1e-4f
                         && std::abs (clips[0].audio->getSample (1, 1000) - sine[1000]) < 1e-4f,
                     "the take is the sine of input 2, on both channels (mono)");

        // a device with no outputs: nothing is heard, but the transport still runs (the playhead advances over the take)
        w->seek (0);
        w->togglePlay();
        std::vector<float> none (256, 0.0f);
        const float* noneIn[1] = { none.data() };
        w->io.deviceBlock (noneIn, 1, nullptr, 0, 256);
        test::check (w->position() == 256 && w->isPlaying(), "a device with no outputs still advances the playhead: " + juce::String ((int) w->position()));
    }

    std::printf ("[18] A take is placed minus every latency it is heard late by: the device's, the extra, the monitor's and the longest track chain's; a take that begins before 0 keeps its sound\n");
    {
        auto w = makeWindow();
        juce::String error;
        w->session.addEmptyTrack ("Gtr");
        w->session.setTrackArm (0, true, error);
        // a track chain with 64 samples of latency: the tracks are delayed to line up with it, so the mix is heard 64 samples late
        test::check (w->session.setChain (0, { "test delay" }, error) && w->session.maxLatency() == 64, "a chain with 64 samples of latency");
        w->setDeviceForTest (fakeDevice());   // 100 samples in, 50 out
        w->seek (0);                           // the take begins at 0, so 100 + 50 + 64 samples of its head fall before it
        test::check (w->startRecording(), "REC starts on the switched-on input");
        juce::int64 fed = 0;
        feedDevice (*w, fed, 48000.0, 4800);
        w->stopRecording();
        const auto& clips = w->session.tracks[0]->clips;
        test::check (clips.size() == 1 && clips[0].start == 0 && clips[0].offset == 214 && clips[0].length == 4800 - 214,
                     test::fmt ("the take starts at 0 from sample 214 (the latencies), length %d", clips.empty() ? -1 : (int) clips[0].length));
        test::check (clips.size() == 1 && clips[0].audio != nullptr
                         && std::abs (clips[0].audio->getSample (0, (int) clips[0].offset) - 0.5f * (float) std::sin (2.0 * juce::MathConstants<double>::pi * 440.0 * 214.0 / 48000.0)) < 1e-4f,
                     "and the first sample it plays (file sample 214) is the sine there: none of the sound is lost");
    }

    std::printf ("[19] Safe start: a pedal left by a start that died skips the monitor chain or the last project (the files kept, a note says so); PLUGINS loads the chain; no pedal changes nothing. A plugin with no editor is said so\n");
    {
        const auto dir = tempFolder().getChildFile ("safe-start");
        dir.deleteRecursively();
        const auto settings = dir.getChildFile ("settings.xml");
        const auto monitorFile = Settings::monitorFile (settings);
        const auto monitorPedal = Settings::monitorPedalFile (settings);
        const auto projectPedal = Settings::projectPedalFile (settings);
        const auto project = dir.getChildFile ("song.olympus");
        const auto monitorNote = "Last start stopped loading the monitor chain";
        const auto projectNote = "Last start stopped opening the last project";

        {
            auto w = makeWindow (true, settings);
            test::check (w->addMonitorPlugin ("test gain") && monitorFile.existsAsFile(), "a monitor plugin is saved in monitor.xml");
        }
        const auto before = monitorFile.loadFileAsString();

        // the last start died while loading the chain: its pedal is still there
        monitorPedal.replaceWithText ("loading the monitor chain\n");
        {
            auto w = makeWindow (true, settings);
            test::check (! w->monitorChainLoaded() && w->monitorChainSkipped() && w->session.monitor.empty(), "a chain whose pedal is there is not loaded");
            test::check (! monitorPedal.existsAsFile(), "the pedal is removed at this start");
            test::check (noteMentions (*w, monitorNote, ui::Severity::warning), "and a note at the foot says so");
            test::check (monitorFile.loadFileAsString() == before, "monitor.xml is byte-identical");
            w->loadSkippedMonitorChain();
            test::check (w->monitorChainLoaded() && w->session.monitor.size() == 1 && ! w->monitorChainSkipped() && ! monitorPedal.existsAsFile(),
                         "PLUGINS > Load saved monitor chain reads it, and leaves no pedal behind");
            test::check (! noteMentions (*w, monitorNote, ui::Severity::warning), "and the note goes");
        }

        // no pedal: the chain loads as before, and no note
        {
            auto w = makeWindow (true, settings);
            test::check (w->monitorChainLoaded() && w->session.monitor.size() == 1 && ! w->monitorChainSkipped() && ! noteMentions (*w, monitorNote, ui::Severity::warning),
                         "without a pedal the chain loads as before, with no note");
        }
        test::check (! monitorPedal.existsAsFile(), "and no pedal is left");

        // the last project: a pedal means the start died opening it, so it is not opened again
        {
            auto w = makeWindow (true, settings);
            w->addTracks ({ makeWav ("safe.wav").getFullPathName() });
            test::check (w->saveTo (project), "a project to reopen is saved");
        }
        Settings saved = Settings::load (settings);
        saved.lastProject = project.getFullPathName();
        saved.save (settings);
        const auto projectBefore = project.loadFileAsString();
        projectPedal.replaceWithText ("opening the last project\n");
        {
            auto w = makeWindow (true, settings);
            w->openLastProjectAtStart();
            test::check (w->session.projectFile == juce::File() && w->session.tracks.empty() && ! projectPedal.existsAsFile(),
                         "a last project whose pedal is there is not opened again; the pedal is removed");
            test::check (noteMentions (*w, projectNote, ui::Severity::warning), "and a note says so");
            test::check (project.loadFileAsString() == projectBefore, "the project file is byte-identical");
        }
        {
            auto w = makeWindow (true, settings);
            w->openLastProjectAtStart();
            test::check (w->session.projectFile == project && ! w->session.tracks.empty() && ! projectPedal.existsAsFile(),
                         "without a pedal the last project opens as before");
        }

        // a plugin that says it has an editor and gives none: the window says so
        struct NoEditor : test::GainPlugin
        {
            NoEditor() : test::GainPlugin ("Silent Plugin", 0) {}
            bool hasEditor() const override { return true; }
        };
        auto w = makeWindow();
        w->session.catalog.addInternal (test::GainPlugin::description ("Silent Plugin"), [] (double, int) { return std::make_unique<NoEditor>(); });
        juce::String error;
        const int track = w->session.addEmptyTrack ("Ed");
        test::check (w->session.addPlugin (track, "Silent Plugin", -1, error), "a plugin with no editor is added: " + error);
        gErrors.clear();
        w->openEditor (track, 0);
        test::check (gErrors.size() == 1 && gErrors.back() == "Silent Plugin has no editor window", "a plugin that gives no editor is said so, not silent");
    }

    std::printf ("[10] Session: empty tracks\n");
    {
        olympus::Session session;
        const int index = session.addEmptyTrack ("Empty");
        test::check (index == 0 && session.tracks[0]->clips.empty() && session.tracks[0]->name == "Empty", "an empty track is a valid track");
        test::check (session.length() == 0, "and it has no length");
    }

    std::printf ("[14] The monitor chain: the headphones' plugins, on the playback only\n");
    {
        const auto home = tempFolder().getChildFile ("monitor-home").getChildFile ("settings.xml");
        const auto monitorFile = Settings::monitorFile (home);
        auto w = makeWindow (true, home);
        juce::StringArray files;
        files.add (makeWav ("monitor-a.wav").getFullPathName());
        w->addTracks (files);
        w->editor.clearUndo();                                   // the import is the project's: the monitor's changes are checked from here
        w->saveTo (tempFolder().getChildFile ("monitor-start.olympus"));
        const float dry = playPeak (*w);
        test::check (dry > 0.1f && w->monitorButtons.isEmpty(), "a track plays, and the chain is empty: " + juce::String (dry));

        // ADD: a button on the strip; the output is the plugin's (a gain of 0 dB, so the same)
        test::check (w->addMonitorPlugin ("Test Gain") && w->monitorButtons.size() == 1 && w->monitorButtons[0]->getButtonText() == "Test Gain",
                     "ADD puts a button on the MONITOR strip");
        test::check (std::abs (playPeak (*w) - dry) < 1e-6f, "a plugin at 0 dB leaves the playback as it was");
        test::check (! w->editor.canUndo() && ! w->session.dirty, "adding a monitor plugin is not an undo step and does not dirty the project");
        test::check (bytesOf (monitorFile).getSize() > 0, "monitor.xml is written after the change");
        {
            const auto xml = juce::parseXML (monitorFile);
            test::check (xml != nullptr && xml->getNumChildElements() == 1 && xml->getChildByName ("Plugin") != nullptr, "and it holds the one plugin");
        }

        // the plugin's own window: a click on its button opens it; a change made in the window is saved when it closes
        press (*w->monitorButtons[0]);
        test::check (w->openEditors() == 1, "a click on the button opens the plugin's window");
        w->session.monitor[0]->instance->setCurrentProgram (1);   // a +6 dB boost, changed in its own window
        test::check (! gClosers.empty(), "the window has a closed callback to tell the window it closed");
        gClosers.back()();
        juce::MessageManager::getInstance()->runDispatchLoopUntil (60);
        test::check (w->openEditors() == 0, "closing the window");
        const float boosted = playPeak (*w);
        test::check (std::abs (boosted / dry - 1.9953f) < 0.01f, "the plugin's gain is in the playback: " + juce::String (boosted / dry));

        // the CORRECTION lamp: bypassed, the headphones hear the uncorrected mix
        press (w->monitorBypassButton);
        test::check (w->session.monitorBypass && ! w->monitorBypassButton.getToggleState() && w->monitorBypassButton.getButtonText() == "Bypass",
                     "the lamp bypasses the chain: BYPASS");
        test::check (std::abs (playPeak (*w) - dry) < 1e-6f, "bypassed, the output is the unprocessed mix");
        test::check (! w->editor.canUndo() && ! w->session.dirty, "the bypass is not an undo step either");
        press (w->monitorBypassButton);
        test::check (! w->session.monitorBypass && std::abs (playPeak (*w) - boosted) < 1e-6f, "and CORRECTION on again brings the plugin back");

        // MOVE and bypass one plugin, saved at once
        test::check (w->addMonitorPlugin ("Test Delay") && w->monitorButtons.size() == 2, "a second plugin, after the first");
        w->moveMonitorPlugin (1, 0);
        test::check (w->monitorButtons[0]->getButtonText() == "Test Delay" && w->session.monitor[0]->plugin == "Test Delay", "move: the delay is first");
        w->setMonitorPluginBypass (0, true);
        test::check (w->session.monitor[0]->bypass && ! w->monitorButtons[0]->getToggleState(), "a bypassed plugin is dark on the strip");
        {
            const auto xml = juce::parseXML (monitorFile);
            test::check (xml != nullptr && xml->getChildByName ("Plugin") != nullptr && xml->getChildByName ("Plugin")->getStringAttribute ("plugin") == "Test Delay"
                         && xml->getChildByName ("Plugin")->getBoolAttribute ("bypass"), "monitor.xml has the order and the bypass after each change");
        }

        // the state of the chain as a second window reads it back from the same settings
        const auto boostedState = w->session.stateOf (*w->session.monitor[1]);
        w->openEditor (-1, 1);   // a window left open at the end: it is closed with the window, and its state saved
        w->session.monitor[1]->instance->setCurrentProgram (1);
        w.reset();   // the window goes: its plugins' windows close and the chain is saved
        auto back = makeWindow (true, home);
        test::check (back->session.monitor.size() == 2 && back->monitorChainLoaded(), "a second window reads the chain from monitor.xml");
        test::check (back->session.monitor[0]->plugin == "Test Delay" && back->session.monitor[0]->bypass && back->session.monitor[1]->plugin == "Test Gain",
                     "with its order and bypass");
        test::check (back->session.stateOf (*back->session.monitor[1]) == boostedState, "the boosted state of a plugin is read back");
        test::check (! back->session.monitorBypass, "and the chain is on");
        juce::StringArray clip;
        clip.add (makeWav ("monitor-a.wav").getFullPathName());
        back->addTracks (clip);
        test::check (std::abs (playPeak (*back) - boosted) < 0.01f * boosted, "its playback is the same as the window that saved it");

        // NEW and OPEN keep the chain, and the plugin window that is open stays open
        back->openEditor (-1, 0);
        const auto project = tempFolder().getChildFile ("monitor-keep.olympus");
        test::check (back->saveTo (project), "a project is saved");
        back->newProject();
        test::check (back->session.tracks.empty() && back->session.monitor.size() == 2 && back->monitorButtons.size() == 2, "NEW keeps the monitor chain");
        test::check (back->openEditors() == 1, "and the monitor window stays open across NEW");
        back->openProject (project);
        test::check (back->session.monitor.size() == 2 && back->session.monitor[0]->plugin == "Test Delay" && back->openEditors() == 1,
                     "OPEN keeps the monitor chain and its window");
        test::check (! back->editor.canUndo(), "and clears no undo step of its own");

        // a track's window closes with its track; the monitor's does not
        back->removeMonitorPlugin (0);
        test::check (back->session.monitor.size() == 1 && back->monitorButtons.size() == 1 && back->openEditors() == 0,
                     "REMOVE takes the plugin off the strip and closes its window");
        test::check (bytesOf (monitorFile).getSize() > 0 && juce::parseXML (monitorFile)->getNumChildElements() == 1, "and monitor.xml is saved after the removal");

        // the notes: correction bypassed; a latency above 30 ms; a plugin that is not installed
        back->setMonitorBypassed (true);
        test::check (noteMentions (*back, "headphone correction is bypassed", ui::Severity::warning), "a note: the correction is bypassed");
        back->setMonitorBypassed (false);
        test::check (! noteMentions (*back, "headphone correction is bypassed", ui::Severity::warning), "and it goes when the correction is on");
        back->session.catalog.addInternal (test::GainPlugin::description ("Test Long"), [] (double, int) { return std::make_unique<test::GainPlugin> ("Test Long", 2000); });
        test::check (back->addMonitorPlugin ("Test Long"), "a plugin of 2000 samples of latency");
        test::check (noteMentions (*back, "linear-phase correction adds delay", ui::Severity::warning), "a note: a monitor latency above 30 ms");
        back->removeMonitorPlugin (1);
        test::check (! noteMentions (*back, "linear-phase", ui::Severity::warning), "and it goes with the latency");
        test::check (back->session.monitorLatency() == 0 && back->monitorLatencyMs() == 0.0, "the chain's latency is read out on the strip (0 with no delay)");
        back.reset();

        // a monitor plugin that is not installed: red, a note; and a file that is not one is kept as it is
        const auto gone = tempFolder().getChildFile ("monitor-gone").getChildFile ("settings.xml");
        {
            juce::XmlElement xml ("OlympusMonitor");
            xml.setAttribute ("version", 1);
            xml.setAttribute ("bypass", false);
            auto* plugin = xml.createNewChildElement ("Plugin");
            plugin->setAttribute ("plugin", "Gone Plugin");
            plugin->setAttribute ("bypass", false);
            plugin->setAttribute ("state", "");
            Settings::monitorFile (gone).getParentDirectory().createDirectory();
            xml.writeTo (Settings::monitorFile (gone));
        }
        auto missing = makeWindow (true, gone);
        test::check (missing->session.monitor.size() == 1 && missing->monitorButtons.size() == 1 && missing->monitorButtons[0]->missing,
                     "a plugin that is not installed is on the strip, in red");
        test::check (noteMentions (*missing, "not installed here", ui::Severity::warning), "and a note says so");
        test::check (missing->session.monitor[0]->savedState.getSize() == 0 && missing->session.monitor[0]->plugin == "Gone Plugin",
                     "its saved settings are kept");
        missing.reset();

        const auto newer = tempFolder().getChildFile ("monitor-newer").getChildFile ("settings.xml");
        {
            juce::XmlElement xml ("OlympusMonitor");
            xml.setAttribute ("version", 99);
            Settings::monitorFile (newer).getParentDirectory().createDirectory();
            xml.writeTo (Settings::monitorFile (newer));
        }
        const auto before = bytesOf (Settings::monitorFile (newer));
        auto other = makeWindow (true, newer);
        test::check (! other->monitorChainLoaded() && noteMentions (*other, "Monitor chain not read", ui::Severity::warning), "a monitor.xml from a newer Olympus is not read, and says so");
        test::check (! other->addMonitorPlugin ("Test Gain") && other->session.monitor.empty(), "and a change is refused, not saved over it");
        other.reset();
        test::check (bytesOf (Settings::monitorFile (newer)) == before, "the newer file is kept as it is");
    }

    std::printf ("[15] The monitor chain and the render\n");
    {
        // the same project rendered with a monitor chain (a boosted plugin and a delay) and without: the stems and the mix are the same bytes
        const auto clipFile = makeWav ("monitor-render.wav");
        juce::StringArray files;
        files.add (clipFile.getFullPathName());
        const auto homeWith = tempFolder().getChildFile ("render-with").getChildFile ("settings.xml");
        const auto homeWithout = tempFolder().getChildFile ("render-without").getChildFile ("settings.xml");
        auto with = makeWindow (true, homeWith);
        auto without = makeWindow (true, homeWithout);
        with->addTracks (files);
        without->addTracks (files);
        with->addMonitorPlugin ("Test Gain");
        with->session.monitor[0]->instance->setCurrentProgram (1);
        with->addMonitorPlugin ("Test Delay");
        const auto folderWith = tempFolder().getChildFile ("stems-with"), folderWithout = tempFolder().getChildFile ("stems-without");
        folderWith.deleteRecursively();
        folderWithout.deleteRecursively();
        test::check (with->renderTo (folderWith).isEmpty() && without->renderTo (folderWithout).isEmpty(), "both windows render");
        int compared = 0;
        bool same = true;
        for (auto& f : folderWithout.findChildFiles (juce::File::findFiles, false, "*.wav"))
        {
            ++compared;
            same = same && bytesOf (folderWith.getChildFile (f.getFileName())) == bytesOf (f) && bytesOf (f).getSize() > 1000;
        }
        test::check (compared >= 2 && same, "the stems and the mix are the same bytes with and without the monitor chain: " + juce::String (compared) + " files");
    }

    std::printf ("[16] The MONITOR strip and the header\n");
    {
        auto w = makeWindow();
        w->addMonitorPlugin ("Test Gain");
        w->addMonitorPlugin ("Test Delay");
        const auto area = w->monitorStrip;
        bool ok = area.getY() >= 110 && w->arrange.getY() >= area.getBottom();
        for (auto* b : { static_cast<juce::Component*> (&w->monitorAddButton), static_cast<juce::Component*> (&w->monitorBypassButton) })
            ok = ok && inside (*b, *w) && b->getBottom() <= w->arrange.getY() && ! b->getBounds().intersects (w->positionBox);
        for (auto* b : w->monitorButtons)
            ok = ok && inside (*b, *w) && b->getBottom() <= w->arrange.getY() && b->getBounds().getRight() < w->monitorAddButton.getX();
        test::check (ok, "the strip sits under the header and above the arrangement, the plugins left of '+', CORRECTION on the right");
        test::check (w->monitorButtons[0]->getBounds().getRight() <= w->monitorButtons[1]->getX(), "the plugin buttons do not overlap");
    }

    std::printf ("[20] Render: a cancel is quiet and leaves no stem of its run; a success names the folder and the files and reveals it; mix only writes exactly mix.wav, the same bytes as the full mix; 16 and 24 bit; the choice is remembered\n");
    {
        auto w = makeWindow();
        w->addTracks ({ makeWav ("r-a.wav").getFullPathName(), makeWav ("r-b.wav").getFullPathName() });
        const auto home = tempFolder().getChildFile ("render-options");
        home.deleteRecursively();

        const auto cancelled = home.getChildFile ("cancelled");
        RenderResult cres;
        gErrors.clear();
        gNotes.clear();
        // the cancel comes from the progress callback, and only once a stem of this run is on disk: the test proves the files existed
        bool stemExisted = false;
        const auto cerr = w->renderTo (cancelled, [&] (float)
        {
            stemExisted = stemExisted || cancelled.findChildFiles (juce::File::findFiles, false, "*.wav").size() > 0;
            return ! stemExisted;
        }, false, {}, &cres);
        w->finishRender (cancelled, cres, cerr);
        test::check (stemExisted, "the cancel came after a stem was written: the files existed when it was asked");
        test::check (cres.stopped && cerr == "Stopped." && gErrors.empty() && gNotes.empty(), "a cancelled render is quiet: no message, no note");
        test::check (cancelled.findChildFiles (juce::File::findFiles, false, "*.wav").isEmpty(), "and the stems it had written are deleted");

        const auto full = home.getChildFile ("full");
        juce::File shown;
        w->reveal = [&shown] (const juce::File& f) { shown = f; };
        RenderResult fres;
        const auto ferr = w->renderTo (full, {}, false, {}, &fres);
        w->finishRender (full, fres, ferr);
        test::check (ferr.isEmpty() && fres.stems.size() == 2 && ! fres.mixFile.isEmpty(), "the full render writes two stems and the mix");
        test::check (gNotes.size() == 1 && gNotes.back() == "Rendered 2 stems and mix.wav into " + full.getFullPathName(), "the success names the folder and the files: " + (gNotes.empty() ? juce::String() : gNotes.back()));
        test::check (shown == full, "and the folder is revealed");
        w->reveal = [] (const juce::File&) {};

        RenderOptions mixOnly;
        mixOnly.writeStems = false;
        const auto only = home.getChildFile ("mix-only");
        test::check (w->renderTo (only, {}, false, mixOnly).isEmpty(), "mix only renders");
        const auto written = only.findChildFiles (juce::File::findFiles, false, "*");
        test::check (written.size() == 1 && written[0].getFileName() == "mix.wav", "mix only writes exactly one file, mix.wav");
        test::check (bytesOf (only.getChildFile ("mix.wav")) == bytesOf (full.getChildFile ("mix.wav")) && bytesOf (full.getChildFile ("mix.wav")).getSize() > 1000,
                     "and its bytes are those of the full render's mix");

        RenderOptions nothing;
        nothing.writeStems = false;
        nothing.writeMix = false;
        test::check (! w->renderTo (home.getChildFile ("nothing"), {}, false, nothing).isEmpty(), "with neither stems nor mix it is refused");

        auto bitsOf = [] (const juce::File& f)
        {
            juce::WavAudioFormat wav;
            std::unique_ptr<juce::InputStream> in (f.createInputStream());
            std::unique_ptr<juce::AudioFormatReader> reader (wav.createReaderFor (in.release(), true));
            return reader != nullptr ? (int) reader->bitsPerSample : 0;
        };
        for (int bits : { 16, 24 })
        {
            RenderOptions o;
            o.bits = bits;
            const auto folder = home.getChildFile ("bits-" + juce::String (bits));
            test::check (w->renderTo (folder, {}, false, o).isEmpty(), juce::String (bits) + "-bit renders");
            const auto files = folder.findChildFiles (juce::File::findFiles, false, "*.wav");
            bool all = files.size() == 3;
            for (auto& f : files) all = all && bitsOf (f) == bits;
            test::check (all, "every stem and the mix are " + juce::String (bits) + "-bit: " + juce::String (files.size()) + " files");
        }

        const auto settingsFile = tempFolder().getChildFile ("render-choice.xml");
        Settings chosen;
        chosen.renderBits = 24;
        chosen.renderStems = false;
        chosen.save (settingsFile);
        const auto back = Settings::load (settingsFile);
        test::check (back.renderBits == 24 && ! back.renderStems, "the render choice is remembered in settings.xml");
        const auto older = tempFolder().getChildFile ("render-older.xml");
        older.replaceWithText ("<OlympusSettings lastProject=\"\"/>");
        const auto old = Settings::load (older);
        test::check (old.renderBits == 32 && old.renderStems, "an older settings file has the defaults: 32-bit float, stems on");
    }

    std::printf ("[21] A render holds the session's lock: the message thread does not wait for it (the arrangement shows its last model, the timer runs), and renderRunning is true while it runs\n");
    {
        auto w = makeWindow();
        w->addTracks ({ makeWav ("nb.wav").getFullPathName() });
        w->arrange.refresh();

        std::atomic<bool> held { false }, release { false };
        std::thread holder ([&]
        {
            w->session.renderBusy = true;                  // what a render does, then holds the lock for its whole run
            const juce::ScopedLock sl (w->session.lock);
            held = true;
            while (! release)
                std::this_thread::sleep_for (std::chrono::milliseconds (2));
            w->session.renderBusy = false;
        });
        while (! held)
            std::this_thread::sleep_for (std::chrono::milliseconds (1));
        const auto t0 = juce::Time::getMillisecondCounterHiRes();
        w->arrange.refresh();
        juce::MessageManager::getInstance()->runDispatchLoopUntil (200);   // the window's timer runs meanwhile
        const double took = juce::Time::getMillisecondCounterHiRes() - t0;
        release = true;
        holder.join();
        test::check (took < 1000.0, test::fmt ("refresh and the timer return while the lock is held: %d ms", (int) took));

        bool sawBusy = false;
        w->renderTo (tempFolder().getChildFile ("nb-render"), [&] (float) { sawBusy = sawBusy || w->session.renderRunning(); return true; });
        test::check (sawBusy && ! w->session.renderRunning(), "renderRunning is true during the render and false after it");
    }

    std::printf ("[22] A take that runs when the project is quit or NEW is placed first (a clip, the project dirty), then the question is asked\n");
    {
        auto w = makeWindow();
        juce::String error;
        w->session.addEmptyTrack ("Gtr");
        w->session.setTrackArm (0, true, error);
        w->setDeviceForTest (fakeDevice (48000.0, 0, 0));
        w->seek (0);
        test::check (w->startRecording(), "REC starts");
        juce::int64 fed = 0;
        feedDevice (*w, fed, 48000.0, 4800);
        gAsked.clear();
        gAnswer = false;
        bool quit = false;
        w->askToDiscard ("Quit and lose them?", [&] { quit = true; });
        test::check (! w->isRecording() && w->session.tracks[0]->clips.size() == 1 && w->session.dirty, "quit: the take is placed as a clip and the project is dirty");
        test::check (gAsked.size() == 1 && ! quit, "then it is asked, and Cancel keeps the window open");
        w->editor.undo();
        test::check (w->session.tracks[0]->clips.empty(), "the take is one undo step");

        auto n = makeWindow();
        juce::String err2;
        n->session.addEmptyTrack ("Gtr");
        n->session.setTrackArm (0, true, err2);
        n->setDeviceForTest (fakeDevice (48000.0, 0, 0));
        n->seek (0);
        test::check (n->startRecording(), "REC starts in a second window");
        feedDevice (*n, fed, 48000.0, 2400);
        gAnswer = true;   // Discard
        n->askToDiscard ("Start a new project and lose them?", [&] { n->newProject(); });
        test::check (! n->isRecording() && n->session.tracks.empty() && gAsked.size() == 2, "NEW: the take is stopped first, then the question is asked, and Discard starts the new project");
    }

    std::printf ("[23] Quit, NEW and OPEN with changes ask Save, Don't save or Cancel: Save writes a project with a file; one without gets the Save As chooser and goes on only once it is written; a cancelled chooser cancels the whole action\n");
    {
        const auto folder = tempFolder().getChildFile ("save-cases");
        folder.deleteRecursively();
        folder.createDirectory();
        auto w = makeWindow (true, folder.getChildFile ("settings.xml"));
        w->addTracks ({ makeWav ("sv.wav").getFullPathName() });
        const auto named = folder.getChildFile ("named.olympus");
        test::check (w->saveTo (named) && ! w->session.dirty, "a project with a file");

        bool went = false;
        w->askUnsaved = [] (const juce::String&) { return MainComponent::Unsaved::save; };
        w->session.dirty = true;
        w->askToDiscard ("Quit?", [&] { went = true; });
        test::check (went && ! w->session.dirty && named.existsAsFile(), "Save writes the project with its file, then goes on");

        w->askUnsaved = [] (const juce::String&) { return MainComponent::Unsaved::cancel; };
        w->session.dirty = true;
        went = false;
        w->askToDiscard ("Quit?", [&] { went = true; });
        test::check (! went && w->session.dirty, "Cancel goes on with nothing, and the changes stay");

        w->askUnsaved = [] (const juce::String&) { return MainComponent::Unsaved::discard; };
        went = false;
        w->askToDiscard ("Quit?", [&] { went = true; });
        test::check (went, "Don't save goes on without writing");

        auto u = makeWindow (true, folder.getChildFile ("settings.xml"));
        u->addTracks ({ makeWav ("sv2.wav").getFullPathName() });
        const auto target = folder.getChildFile ("untitled-saved.olympus");
        gSaveTarget = target;
        u->askUnsaved = [] (const juce::String&) { return MainComponent::Unsaved::save; };
        went = false;
        u->askToDiscard ("Quit?", [&] { went = true; });
        juce::MessageManager::getInstance()->runDispatchLoopUntil (50);   // the action runs on the next message, after the chooser's callback (F2)
        test::check (went && target.existsAsFile() && u->session.projectFile == target && ! u->session.dirty,
                     "Save on a project with no file runs the Save As chooser, and goes on once the file is written");

        auto v = makeWindow (true, folder.getChildFile ("settings.xml"));
        v->addTracks ({ makeWav ("sv3.wav").getFullPathName() });
        gSaveTarget = juce::File();   // the chooser is cancelled
        v->askUnsaved = [] (const juce::String&) { return MainComponent::Unsaved::save; };
        went = false;
        v->askToDiscard ("Quit?", [&] { went = true; });
        test::check (! went && v->session.dirty && v->session.projectFile == juce::File(), "a cancelled chooser cancels the whole action");
        gSaveTarget = juce::File();
    }

    std::printf ("[24] Autosave: autosaveNow writes beside the project without changing the session; a save deletes it; NEW deletes the untitled one; recovery offers a newer autosave, opens it with changes and its project file, and Ignore deletes it\n");
    {
        const auto folder = tempFolder().getChildFile ("autosave-case");
        folder.deleteRecursively();
        folder.createDirectory();
        const auto settings = folder.getChildFile ("settings.xml");
        auto w = makeWindow (true, settings);
        w->addTracks ({ makeWav ("au.wav").getFullPathName() });
        const auto song = folder.getChildFile ("song.olympus");
        const auto autosave = folder.getChildFile ("song.olympus.autosave");
        test::check (w->saveTo (song) && ! w->session.dirty, "the project is saved");
        song.setLastModificationTime (juce::Time::getCurrentTime() - juce::RelativeTime::seconds (60));
        w->editor.setGrid (Grid::Half);
        const auto name = w->session.name;
        const auto file = w->session.projectFile;
        test::check (w->autosaveNow() && autosave.existsAsFile(), "autosaveNow writes song.olympus.autosave beside the project");
        test::check (w->session.dirty && w->session.projectFile == file && w->session.name == name, "and leaves the session as it was: dirty, its file, its name");
        Session copy;
        juce::String err;
        test::check (copy.load (autosave, err) && copy.tracks.size() == 1 && copy.grid == Grid::Half, "the autosave is a project: it loads with the changes");

        test::check (w->saveProject() && ! autosave.existsAsFile() && ! w->session.dirty, "a save deletes the autosave");

        auto u = makeWindow (true, settings);
        u->addTracks ({ makeWav ("au2.wav").getFullPathName() });
        const auto untitled = folder.getChildFile ("autosave-untitled.olympus");
        test::check (u->autosaveNow() && untitled.existsAsFile() && u->session.projectFile == juce::File() && u->session.dirty,
                     "an untitled session autosaves as autosave-untitled.olympus beside settings.xml");
        u->newProject();
        test::check (! untitled.existsAsFile(), "NEW deletes the untitled autosave");

        // recovery: the project is older than its autosave, so the next start offers it
        auto a = makeWindow (true, settings);
        a->addTracks ({ makeWav ("au3.wav").getFullPathName() });
        a->openProject (song);
        a->editor.setGrid (Grid::Quarter);
        song.setLastModificationTime (juce::Time::getCurrentTime() - juce::RelativeTime::seconds (60));
        a->editor.setGrid (Grid::Half);
        test::check (a->autosaveNow() && autosave.existsAsFile(), "a changed project has its autosave again");
        auto r = makeWindow (true, settings);
        r->openProject (song);
        gRecoverAnswer = true;
        test::check (r->offerRecovery() && r->session.dirty && r->session.projectFile == song && r->session.grid == Grid::Half,
                     "recovery opens the autosave with changes, and the project file it belongs to");
        test::check (r->session.tracks.size() == 1 && r->session.tracks[0]->name == "au", "the recovered session holds what the autosave held: its one track");

        auto ig = makeWindow (true, settings);
        ig->openProject (song);
        gRecoverAnswer = false;
        test::check (ig->offerRecovery() && ! autosave.existsAsFile() && ! ig->session.dirty, "Ignore deletes the autosave, and the project stays as it was");

        auto ut = makeWindow (true, settings);
        ut->addTracks ({ makeWav ("au4.wav").getFullPathName() });
        ut->session.dirty = true;
        ut->autosaveNow();
        auto ur = makeWindow (true, settings);
        gRecoverAnswer = true;
        test::check (ur->offerRecovery() && ur->session.projectFile == juce::File() && ur->session.dirty && ur->session.tracks.size() == 1,
                     "an untitled autosave is offered and opens as an untitled session with changes");
        gRecoverAnswer = false;
        ur->newProject();
    }

    std::printf ("[25] While a render holds the session's lock, the window paints, updates its transport and runs its timer without waiting for it (a watchdog releases the lock after 3 s, so a regression fails and does not hang)\n");
    {
        auto w = makeWindow();
        w->addTracks ({ makeWav ("pl.wav").getFullPathName() });
        w->arrange.refresh();

        std::atomic<bool> held { false }, release { false }, done { false };
        std::thread holder ([&]
        {
            w->session.renderBusy = true;                  // what a render does, then holds the lock for its whole run
            const juce::ScopedLock sl (w->session.lock);
            held = true;
            while (! release)
                std::this_thread::sleep_for (std::chrono::milliseconds (2));
            w->session.renderBusy = false;
        });
        std::thread watchdog ([&]
        {
            const auto limit = std::chrono::steady_clock::now() + std::chrono::seconds (3);
            while (! done && std::chrono::steady_clock::now() < limit)
                std::this_thread::sleep_for (std::chrono::milliseconds (5));
            release = true;
        });
        while (! held)
            std::this_thread::sleep_for (std::chrono::milliseconds (1));

        const auto t0 = juce::Time::getMillisecondCounterHiRes();
        const auto snap = w->createComponentSnapshot (w->getLocalBounds());   // paint of the whole window
        juce::MessageManager::getInstance()->runDispatchLoopUntil (200);     // the timer (which updates the transport) and the paints run meanwhile
        const double took = juce::Time::getMillisecondCounterHiRes() - t0;
        done = true;
        release = true;
        holder.join();
        watchdog.join();
        test::check (snap.isValid() && took < 1000.0,
                     test::fmt ("paint, updateTransport and the timer return while a render holds the lock: %d ms", (int) took));
    }

    std::printf ("[27] Autosave, negative: an autosave older than its project is not offered; saving another project leaves this one's autosave; a failed open keeps the autosave\n");
    {
        const auto folder = tempFolder().getChildFile ("autosave-negative");
        folder.deleteRecursively();
        folder.createDirectory();
        const auto settings = folder.getChildFile ("settings.xml");
        const auto a = folder.getChildFile ("a.olympus"), b = folder.getChildFile ("b.olympus"), c = folder.getChildFile ("c.olympus");

        auto w = makeWindow (true, settings);
        w->addTracks ({ makeWav ("na.wav").getFullPathName() });
        test::check (w->saveTo (a), "project A is saved");
        w->editor.setGrid (Grid::Half);
        test::check (w->autosaveNow() && a.getSiblingFile ("a.olympus.autosave").existsAsFile(), "A has an autosave");
        a.getSiblingFile ("a.olympus.autosave").setLastModificationTime (juce::Time::getCurrentTime() - juce::RelativeTime::seconds (120));
        a.setLastModificationTime (juce::Time::getCurrentTime());
        auto r = makeWindow (true, settings);
        r->openProject (a);
        gRecoverAnswer = true;
        test::check (! r->offerRecovery() && ! r->session.dirty, "an autosave older than its project is not offered");
        test::check (a.getSiblingFile ("a.olympus.autosave").existsAsFile(), "and it is kept");

        auto wb = makeWindow (true, settings);
        wb->addTracks ({ makeWav ("nb2.wav").getFullPathName() });
        test::check (wb->saveTo (b), "project B is saved");
        wb->editor.setGrid (Grid::Half);
        test::check (wb->autosaveNow() && b.getSiblingFile ("b.olympus.autosave").existsAsFile(), "B has an autosave");
        auto wc = makeWindow (true, settings);
        wc->addTracks ({ makeWav ("nc.wav").getFullPathName() });
        test::check (wc->saveTo (c) && b.getSiblingFile ("b.olympus.autosave").existsAsFile(),
                     "saving another project (C) leaves B's autosave");

        wb->openProject (folder.getChildFile ("missing.olympus"));
        test::check (b.getSiblingFile ("b.olympus.autosave").existsAsFile() && wb->session.projectFile == b,
                     "a failed open keeps the autosave of the session that stays");
    }

    std::printf ("[28] NEW, OPEN and Quit wait while a render runs: they are refused with the reason, and nothing proceeds\n");
    {
        auto w = makeWindow();
        w->addTracks ({ makeWav ("rb.wav").getFullPathName() });
        w->session.dirty = true;
        bool proceeded = false;
        gErrors.clear();
        w->session.renderBusy = true;
        w->askToDiscard ("Start a new project and lose them?", [&] { proceeded = true; });
        w->session.renderBusy = false;
        test::check (! proceeded && gErrors.size() == 1 && gErrors[0] == "A render is running: wait for it or cancel it first.",
                     "a NEW, OPEN or Quit while a render runs is refused with the reason");
    }

    std::printf ("[29] Master section: the meter and the peak read-out follow the master; CLIP latches at 0 dBFS until clicked; a fader drag is one undo step\n");
    {
        auto w = makeWindow();
        w->addTracks ({ makeLevelWav ("master-6.wav", 0.5f).getFullPathName() });   // a peak of 0.5: -6.0 dBFS
        w->seek (0);
        if (! w->isPlaying()) w->togglePlay();
        masterBlock (*w, 0.0);
        test::check (std::abs (w->masterPeakDb() + 6.0f) < 0.3f && ! w->masterClipLatched(), "a signal at -6 dBFS reads about -6 dB on the master, and does not clip");

        w->editor.setMasterDb (12.0f);
        w->seek (0);
        if (! w->isPlaying()) w->togglePlay();
        masterBlock (*w, 100.0);
        test::check (w->masterClipLatched() && w->masterPeakDb() > 5.0f, "with the master at +12 dB the CLIP lamp latches");
        if (w->isPlaying()) w->togglePlay();
        masterBlock (*w, 200.0, 512);   // the signal has stopped
        test::check (w->masterClipLatched() && w->masterPeakDb() > 5.0f, "the lamp stays latched after the signal stops, and the read-out holds the peak");
        masterBlock (*w, 2500.0, 512);  // two seconds on: the hold has run out
        test::check (w->masterPeakDb() <= -99.9f && w->masterClipLatched(), "after two seconds the read-out falls back to the silence; the lamp is still lit");
        layoutCheck (*w, "the master section at the design size");
        press (w->masterClipLamp);
        test::check (! w->masterClipLatched(), "a click on CLIP clears the lamp");

        // the fader: one drag is one undo step, and undo puts back the gain the drag started from
        w->updateMaster (2600.0);
        test::check (std::abs (w->masterFader.getValue() - 12.0) < 0.01, "the fader shows the master's gain (+12 dB)");
        const float before = w->session.masterDb;
        w->masterFader.onDragStart();
        w->masterFader.setValue (-6.0, juce::sendNotificationSync);
        w->masterFader.setValue (-9.0, juce::sendNotificationSync);
        w->masterFader.onDragEnd();
        test::check (std::abs (w->session.masterDb + 9.0f) < 0.01f && w->editor.undoLabel() == "Master gain", "a drag sets the gain and is the step Master gain");
        test::check (w->editor.undo() && std::abs (w->session.masterDb - before) < 0.01f, "one undo restores the gain from before the drag");
        w->updateMaster (2700.0);
        test::check (std::abs (w->masterFader.getValue() - (double) before) < 0.01, "and the fader follows the restored gain");
    }

    std::printf ("[30] Output ceiling: the device's output is held at full scale at +12 dB; the master's peak is read before it; the mix of a render keeps its samples over 1.0\n");
    {
        auto w = makeWindow();
        w->addTracks ({ makeLevelWav ("ceiling.wav", 0.5f).getFullPathName() });
        w->editor.setMasterDb (12.0f);
        w->seek (0);
        if (! w->isPlaying()) w->togglePlay();
        juce::AudioBuffer<float> through (2, 4800);
        w->renderAudioBlock (through);   // the transport's own path: no ceiling
        test::check (through.getMagnitude (0, 0, 4800) > 1.5f, "the transport's samples go over full scale at +12 dB (about 2.0)");
        const float devicePeak = playPeak (*w);   // the device's path: held
        test::check (devicePeak <= 1.0f && devicePeak > 0.999f, "the device's output is held at full scale");
        w->updateMaster (0.0);
        test::check (w->masterClipLatched() && w->masterPeakDb() > 5.0f, "the master's peak is read before the ceiling, so CLIP lights although the device is held");

        const auto folder = takesFolder().getChildFile ("ceiling-render");
        test::check (w->renderTo (folder).isEmpty(), "the render of the same session is written");
        juce::AudioFormatManager formats;
        formats.registerBasicFormats();
        std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (folder.getChildFile ("mix.wav")));
        juce::AudioBuffer<float> mix (2, reader != nullptr ? (int) reader->lengthInSamples : 0);
        if (reader != nullptr) reader->read (&mix, 0, mix.getNumSamples(), 0, true, true);
        const float mixPeak = juce::jmax (mix.getMagnitude (0, 0, mix.getNumSamples()), mix.getMagnitude (1, 0, mix.getNumSamples()));
        test::check (mixPeak > 1.5f, "mix.wav keeps its samples over full scale: the ceiling is the device's alone");
    }

    std::printf ("[31] Disk problems of a take: the frames the disk could not take, and a file whose writes failed, are reported when the take stops; a clean take says nothing\n");
    {
        auto w = makeWindow();
        w->addTracks ({ makeWav ("disk.wav").getFullPathName() });
        juce::String error;
        w->setDeviceForTest (fakeDevice());   // 48 kHz
        test::check (w->session.setTrackArm (0, true, error) && w->session.setTrackInput (0, 0, -1, error), "the track is armed on input 1");
        juce::int64 fed = 0;
        gErrors.clear();
        test::check (w->startRecording(), "REC starts a take");
        feedDevice (*w, fed, 48000.0, 4800);
        w->recorderForChecks().lostForTest (0, 480);   // the disk refused 480 frames (10 ms)
        w->stopRecording();
        test::check (gErrors.size() == 1 && gErrors[0].contains ("480 samples (10 ms) of the take were lost because the disk could not keep up: the take is shorter by that much and what follows the loss sits that much early."),
                     "a take the disk could not keep up with says how much is lost");

        gErrors.clear();
        test::check (w->startRecording(), "a second take starts");
        feedDevice (*w, fed, 48000.0, 1024);
        w->recorderForChecks().failedForTest (0);      // its file's writes failed
        w->stopRecording();
        test::check (gErrors.size() == 1 && gErrors[0].startsWith ("The take could not be written: ") && gErrors[0].contains ("-take-"),
                     "a take whose file could not be written is said, with its file");

        gErrors.clear();
        test::check (w->startRecording(), "a third take starts");
        feedDevice (*w, fed, 48000.0, 1024);
        w->stopRecording();
        test::check (gErrors.empty(), "a clean take says nothing");
    }

    std::printf ("[32] The empty scan: with no plugin found the foot says where to look, unless the scan is suite-only (then it says nothing about the scan)\n");
    {
        auto none = makeWindow (false);
        test::check (noteMentions (*none, "PLUGINS > Look in a folder", ui::Severity::warning), "no plugin found: the note says where to look");
        none->session.catalog.setSuiteOnly (true);
        test::check (! noteMentions (*none, "No plugin found", ui::Severity::warning), "suite only, with none of the suite found: no note about the scan");
        auto found = makeWindow (true);
        test::check (! noteMentions (*found, "No plugin found", ui::Severity::warning), "with plugins found there is no note");
        // a scan queued or running (the start's scan, before the first one finished) says nothing about the plugins yet
        auto busy = makeWindow (false);
        busy->setScanStateForChecks (true, false);
        test::check (! noteMentions (*busy, "No plugin found", ui::Severity::warning), "a scan queued at the start: no note yet");
        busy->setScanStateForChecks (false, true);
        test::check (! noteMentions (*busy, "No plugin found", ui::Severity::warning), "a scan running: no note yet");
        busy->setScanStateForChecks (false, false);
        test::check (noteMentions (*busy, "PLUGINS > Look in a folder", ui::Severity::warning), "the scan finished with none: the note says where to look");
    }

    std::printf ("[33] The device's output, the master's load and the CLIP latch: NaN is 0 and an infinity the rail; a master above +12 dB is set to +12 dB with a warning and the fader is set once; its tooltip shows the value; OPEN clears CLIP; two takes that lose samples name their tracks\n");
    {
        // a float clip with a NaN, +inf and -inf: the device's output has none of them and the NaN is 0
        const auto file = tempFolder().getChildFile ("nonfinite.wav");
        file.deleteFile();
        {
            juce::WavAudioFormat wav;
            std::unique_ptr<juce::OutputStream> stream (file.createOutputStream());
            auto writer = wav.createWriterFor (stream, juce::AudioFormatWriterOptions {}
                                                           .withSampleRate (48000.0)
                                                           .withNumChannels (2)
                                                           .withBitsPerSample (32)
                                                           .withSampleFormat (juce::AudioFormatWriterOptions::SampleFormat::floatingPoint));
            juce::AudioBuffer<float> b (2, 4800);
            b.clear();
            for (int ch = 0; ch < 2; ++ch)
            {
                b.setSample (ch, 100, std::numeric_limits<float>::quiet_NaN());
                b.setSample (ch, 200, std::numeric_limits<float>::infinity());
                b.setSample (ch, 300, -std::numeric_limits<float>::infinity());
            }
            writer->writeFromAudioSampleBuffer (b, 0, 4800);
        }
        auto w = makeWindow();
        w->addTracks ({ file.getFullPathName() });
        w->seek (0);
        if (! w->isPlaying()) w->togglePlay();
        std::vector<std::vector<float>> in (4, std::vector<float> (1000, 0.0f));
        std::vector<std::vector<float>> out (2, std::vector<float> (1000, 0.0f));
        const float* inPtr[4] = { in[0].data(), in[1].data(), in[2].data(), in[3].data() };
        float* outPtr[2] = { out[0].data(), out[1].data() };
        w->io.deviceBlock (inPtr, 4, outPtr, 2, 1000);
        bool clean = true;
        for (auto& channel : out)
            for (float v : channel)
                clean = clean && std::isfinite (v) && v >= -1.0f && v <= 1.0f;
        test::check (clean, "the device's output has no NaN and no value outside [-1, 1] when the clip holds a NaN and infinities");
        // the clip's frames come out 64 samples later than the file's (the transport's own offset), so the rails are found, then the NaN is
        // 100 samples before the +inf and the -inf 100 after it: a NaN that the old clip turned into +1.0 would show here
        int plus = -1;
        for (int i = 0; i < 1000 && plus < 0; ++i)
            if (out[0][(size_t) i] == 1.0f) plus = i;
        const bool rails = plus >= 100 && plus + 100 < 1000 && out[0][(size_t) (plus + 100)] == -1.0f && out[1][(size_t) plus] == 1.0f;
        test::check (rails, "+inf and -inf are the rails: +1 at one sample, -1 a hundred samples later");
        test::check (rails && out[0][(size_t) (plus - 100)] == 0.0f && out[1][(size_t) (plus - 100)] == 0.0f, "the NaN sample is 0");

        // two takes that lose samples: the message names the largest loss and the other track
        auto disk = makeWindow();
        disk->addTracks ({ makeWav ("lossA.wav").getFullPathName(), makeWav ("lossB.wav").getFullPathName() });
        juce::String error;
        disk->setDeviceForTest (fakeDevice());
        test::check (disk->session.setTrackArm (0, true, error) && disk->session.setTrackInput (0, 0, -1, error)
                         && disk->session.setTrackArm (1, true, error) && disk->session.setTrackInput (1, 0, -1, error), "both tracks are armed");
        juce::int64 fed = 0;
        gErrors.clear();
        test::check (disk->startRecording(), "REC starts a take on two tracks");
        feedDevice (*disk, fed, 48000.0, 4800);
        disk->recorderForChecks().lostForTest (0, 480);
        disk->recorderForChecks().lostForTest (1, 240);
        disk->stopRecording();
        const auto expected = "480 samples (10 ms) of the take on track '" + disk->session.tracks[0]->name + "' (the largest loss; the takes on "
                              + disk->session.tracks[1]->name + " lost some too) were lost";
        test::check (gErrors.size() == 1 && gErrors[0].contains (expected), "two takes that lost samples: the largest is named, and the other track too");

        // a project with +18 dB opens at +12 dB with a warning; the fader shows 12 and is not set again on the next tick
        const auto project = tempFolder().getChildFile ("master18.olympus");
        {
            Session saved;
            saved.masterDb = 18.0f;
            juce::String saveError;
            test::check (saved.save (project, saveError), "a project with +18 dB is saved: " + saveError);
        }
        auto m = makeWindow();
        m->openProject (project);
        test::check (std::abs (m->session.masterDb - 12.0f) < 0.01f, "the project opens at +12 dB");
        test::check (m->session.warnings.joinIntoString ("\n").contains ("The master gain was above +12 dB and is set to +12 dB."), "and a warning says so");
        m->updateMaster (2600.0);
        test::check (std::abs (m->masterFader.getValue() - 12.0) < 0.01, "the fader shows +12");
        test::check (m->masterFader.getTooltip() == "Master gain +12.0 dB (double-click for 0 dB)", "the fader's tooltip shows +12.0 dB");
        m->masterFader.setValue (11.97, juce::dontSendNotification);   // within 0.05 dB of the session: the next tick leaves it alone
        m->updateMaster (2700.0);
        test::check (std::abs (m->masterFader.getValue() - 11.97) < 1.0e-6, "a tick within 0.05 dB of the session does not set the fader again");
        m->masterFader.setValue (3.0, juce::sendNotificationSync);
        test::check (m->masterFader.getTooltip() == "Master gain +3.0 dB (double-click for 0 dB)", "a change of the fader updates its tooltip");

        // CLIP latches on a sample at full scale and OPEN clears it, as NEW does
        m->session.masterPeakL = 1.0f;
        m->updateMaster (2800.0);
        test::check (m->masterClipLatched(), "a sample at full scale latches CLIP");
        m->openProject (project);
        test::check (! m->masterClipLatched(), "OPEN of a project clears the CLIP latch");
    }

    std::printf ("[34] Clipboard and navigation keys: Cmd+C, Cmd+X and Cmd+V on the selected clip (the paste at the playhead, the new clip selected, one undo step); Home, End, comma and full stop move the playhead (no marker, one marker, three markers); the tempo field keeps its keys; a paste during a take is refused\n");
    {
        auto w = makeWindow();
        w->addTracks ({ makeWav ("clip34.wav", 48000).getFullPathName() });
        w->refreshNow();
        auto& arrange = w->arrange;
        const auto clipsOn0 = [&w] { return (int) w->session.tracks[0]->clips.size(); };

        arrange.selected = {};
        w->keyPressed (juce::KeyPress ('c', cmd(), 'c'));
        test::check (! w->editor.hasClipboard(), "Cmd+C with no clip selected copies nothing");
        arrange.selected = { 0, 0 };
        w->keyPressed (juce::KeyPress ('c', cmd(), 'c'));
        test::check (w->editor.hasClipboard() && clipsOn0() == 1, "Cmd+C copies the selected clip and leaves it in place");

        w->seek (12000);
        arrange.selected = { 0, 0 };
        w->keyPressed (juce::KeyPress ('v', cmd(), 'v'));
        test::check (clipsOn0() == 2 && w->session.tracks[0]->clips.back().start == 12000, "Cmd+V pastes at the playhead on the selected clip's track");
        test::check (arrange.selected.track == 0 && arrange.selected.clip == 1, "and the new clip is selected");
        w->keyPressed (juce::KeyPress ('z', cmd(), 'z'));
        test::check (clipsOn0() == 1, "one Cmd+Z takes the paste back");

        arrange.selected = { 0, 0 };
        w->keyPressed (juce::KeyPress ('x', cmd(), 'x'));
        test::check (clipsOn0() == 0 && ! arrange.selected.valid(), "Cmd+X cuts the selected clip and clears the selection");
        w->keyPressed (juce::KeyPress ('z', cmd(), 'z'));
        test::check (clipsOn0() == 1, "one Cmd+Z brings the cut clip back");

        // nothing selected: the paste goes to the clipboard's track, at the playhead
        arrange.selected = {};
        w->seek (24000);
        w->keyPressed (juce::KeyPress ('v', cmd(), 'v'));
        test::check (clipsOn0() == 2 && w->session.tracks[0]->clips.back().start == 24000 && arrange.selected.clip == 1,
                     "with no clip selected, Cmd+V pastes on the clipboard's track");
        w->keyPressed (juce::KeyPress ('z', cmd(), 'z'));

        // an empty clipboard: a paste changes nothing
        w->editor.clearClipboard();
        const auto label = w->editor.undoLabel();
        w->keyPressed (juce::KeyPress ('v', cmd(), 'v'));
        test::check (clipsOn0() == 1 && w->editor.undoLabel() == label, "Cmd+V with an empty clipboard changes nothing");

        // Home, End, comma and full stop: the markers before and after the playhead (none, one, three)
        auto nav = makeWindow();
        nav->addTracks ({ makeWav ("nav34.wav", 48000).getFullPathName() });
        nav->refreshNow();
        const auto pos = [&nav] { return nav->position(); };
        nav->seek (12000);
        nav->keyPressed (juce::KeyPress (juce::KeyPress::endKey));
        test::check (pos() == nav->session.length() && pos() == 48000, "End goes to the end of the audio");
        nav->keyPressed (juce::KeyPress (juce::KeyPress::homeKey));
        test::check (pos() == 0, "Home goes to 0");
        nav->seek (12000);
        nav->keyPressed (juce::KeyPress (','));
        nav->keyPressed (juce::KeyPress ('.'));
        test::check (pos() == 12000, "with no marker, comma and full stop do nothing");
        nav->editor.addMarker ("A", 4000);
        nav->seek (0);
        nav->keyPressed (juce::KeyPress ('.'));
        test::check (pos() == 4000, "with one marker, full stop goes to it from before it");
        nav->keyPressed (juce::KeyPress (','));
        test::check (pos() == 4000, "and comma does nothing before the first marker");
        nav->editor.addMarker ("B", 20000);
        nav->editor.addMarker ("C", 36000);
        nav->seek (12000);
        nav->keyPressed (juce::KeyPress ('.'));
        test::check (pos() == 20000, "with three markers, full stop goes to the next one");
        nav->keyPressed (juce::KeyPress (','));
        test::check (pos() == 4000, "and comma to the previous one");
        nav->seek (36000);
        nav->keyPressed (juce::KeyPress ('.'));
        test::check (pos() == 36000, "at the last marker, full stop does nothing");

        // The tempo field's guard (keys typed there never jump or copy) is not checked here: a headless window has no peer, so the
        // field cannot take the keyboard focus. keyPressed reads it with hasKeyboardFocus (true), as the R key does.

        // a paste while a take runs is refused with the reason, and the take is placed as usual when it stops
        auto rec = makeWindow();
        rec->addTracks ({ makeWav ("rec34.wav", 48000).getFullPathName() });
        juce::String recError;
        rec->setDeviceForTest (fakeDevice());
        test::check (rec->session.setTrackArm (0, true, recError) && rec->session.setTrackInput (0, 0, -1, recError), "a track is armed for a take");
        test::check (rec->editor.copyClip ({ 0, 0 }, recError), "a clip is copied before the take");
        rec->arrange.selected = { 0, 0 };
        juce::int64 fed = 0;
        gErrors.clear();
        test::check (rec->startRecording(), "REC starts a take");
        feedDevice (*rec, fed, 48000.0, 4800);
        const auto clipsBefore = rec->session.tracks[0]->clips.size();
        rec->keyPressed (juce::KeyPress ('v', cmd(), 'v'));
        test::check (rec->session.tracks[0]->clips.size() == clipsBefore && gErrors.size() == 1 && gErrors[0].contains ("Stop the recording before you paste."),
                     "a paste during a take is refused, and says so");
        rec->stopRecording();
    }

    tempFolder().deleteRecursively();
    return test::finish ("OlympusUiCheck");
}
