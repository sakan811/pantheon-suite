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
bool gReloadAnswer = false;                    // "Reload" when true

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
    options.includeSystemPlugins = false;
    options.settingsFile = settings != juce::File() ? settings : tempFolder().getChildFile ("settings.xml");
    options.recordings = takesFolder();
    if (withPlugins)
        options.fillCatalog = [] (olympus::Catalog& catalog) { test::addTestPlugins (catalog); };
    auto w = std::make_unique<MainComponent> (options);
    w->setSize (MainComponent::kDesignWidth, MainComponent::kDesignHeight);
    w->arrange.headlessDialogs = true;   // no native AlertWindow: CI has no display
    w->reportError =[] (const juce::String& m) { gErrors.push_back (m); };
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
    w->askReload = [] (const juce::String&) { return gReloadAnswer; };
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

// A device the checks hand to audioDeviceAboutToStart: the given rate and block size, four active inputs and two active outputs, with the
// test window's latencies. Nothing is opened or started: the callback only reads what the device says it is.
struct DummyAudioDevice : juce::AudioIODevice
{
    DummyAudioDevice (double r, int b) : juce::AudioIODevice ("Dummy", "Test"), rate (r), buf (b) {}
    juce::StringArray getOutputChannelNames() override { return { "Out L", "Out R" }; }
    juce::StringArray getInputChannelNames() override { return { "In 1", "In 2", "In 3", "In 4" }; }
    juce::Array<double> getAvailableSampleRates() override { return { rate }; }
    juce::Array<int> getAvailableBufferSizes() override { return { buf }; }
    int getDefaultBufferSize() override { return buf; }
    juce::String open (const juce::BigInteger&, const juce::BigInteger&, double, int) override { return {}; }
    void close() override {}
    bool isOpen() override { return true; }
    void start (juce::AudioIODeviceCallback*) override {}
    void stop() override {}
    bool isPlaying() override { return true; }
    juce::String getLastError() override { return {}; }
    int getCurrentBufferSizeSamples() override { return buf; }
    double getCurrentSampleRate() override { return rate; }
    int getCurrentBitDepth() override { return 24; }
    juce::BigInteger getActiveOutputChannels() const override { juce::BigInteger ch; ch.setRange (0, 2, true); return ch; }
    juce::BigInteger getActiveInputChannels() const override { juce::BigInteger ch; ch.setRange (0, 4, true); return ch; }
    int getOutputLatencyInSamples() override { return 50; }
    int getInputLatencyInSamples() override { return 100; }
    double rate;
    int buf;
};

// Every control of the window inside it, below the arrangement's top, and no two of them (or the position read-out) over each other.
void layoutCheck (MainComponent& w, const char* label)
{
    std::vector<juce::Component*> controls { &w.newButton, &w.openButton, &w.saveButton, &w.saveAsButton, &w.addTrackButton, &w.importButton,
                                             &w.pluginsButton, &w.renderButton, &w.exportButton, &w.guideButton, &w.rewindButton, &w.playButton, &w.loopButton,
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
    std::setvbuf (stdout, nullptr, _IOLBF, 0);   // a crash then leaves the last line printed in the CI log, not the last 4 KiB lost
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

    std::printf ("[34] Clipboard and navigation keys: Cmd+C, Cmd+X and Cmd+V on the selected clip (the paste at the playhead, the new clip selected, one undo step); Home, End, comma and full stop move the playhead (no marker, one marker, three markers); removing a track clears the clipboard; a paste during a take is refused\n");
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
        test::check (pos() == 4000, "and comma does nothing with the playhead on the first marker");
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

        // a removed track would leave the clipboard pointing at the track that moved into its place: removing a track clears it
        {
            auto two = makeWindow();
            two->addTracks ({ makeWav ("rm34a.wav", 48000).getFullPathName(), makeWav ("rm34b.wav", 48000).getFullPathName() });
            two->refreshNow();
            juce::String err;
            test::check (two->session.tracks.size() == 2 && two->editor.copyClip ({ 1, 0 }, err), "two tracks, a clip of the second is copied");
            two->removeTrack (0);
            test::check (! two->editor.hasClipboard(), "removing a track clears the clipboard");
            two->arrange.selected = {};
            const auto before = two->session.tracks[0]->clips.size();
            gErrors.clear();
            two->keyPressed (juce::KeyPress ('v', cmd(), 'v'));
            test::check (two->session.tracks[0]->clips.size() == before, "and a paste with nothing selected adds nothing to the track that took its place");
            gErrors.clear();
        }

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

    std::printf ("[35] UI, lifecycle, threading and advice safeguards (R2.1, R2.3, R2.4, R2.5, R2.6, R3.9, R3.10, R3.11)\n");
    {
        // R2.1: IMPORT AUDIO opens the chooser (dialogOpen = true while open, blocking concurrent askToDiscard), and finishing/cancelling resets it
        {
            auto w = makeWindow();
            std::function<void (const juce::StringArray&)> pendingImport;
            w->chooseImportFiles = [&] (const juce::File&, std::function<void (const juce::StringArray&)> done)
            {
                pendingImport = std::move (done);
            };
            w->seek (12000);
            press (w->importButton);
            test::check (w->isDialogOpen() && (bool) pendingImport, "IMPORT AUDIO opens the chooser and sets dialogOpen true");
            bool discardedWhileOpen = false;
            w->askToDiscard ("Start a new project and lose them?", [&] { discardedWhileOpen = true; });
            test::check (! discardedWhileOpen, "askToDiscard is refused while the import chooser is open");
            const auto importedFile = makeWav ("r21.wav", 24000);
            pendingImport ({ importedFile.getFullPathName() });
            pendingImport = nullptr;
            test::check (! w->isDialogOpen() && w->session.tracks.size() == 1 && w->session.tracks[0]->clips.size() == 1
                             && w->session.tracks[0]->clips[0].start == 12000,
                         "completing the import chooser resets dialogOpen and imports at the playhead");
            // cancelling the import chooser also resets dialogOpen and leaves askToDiscard working
            press (w->importButton);
            test::check (w->isDialogOpen() && (bool) pendingImport, "IMPORT AUDIO opens a second time");
            pendingImport ({});
            pendingImport = nullptr;
            test::check (! w->isDialogOpen(), "cancelling the import chooser resets dialogOpen to false");
            gAnswer = true;
            bool proceededAfterClose = false;
            w->askToDiscard ("Start a new project and lose them?", [&] { proceededAfterClose = true; });
            test::check (proceededAfterClose, "askToDiscard works normally after the import chooser closes");
        }

        // R2.3: audioDeviceAboutToStart called from a background thread while a take is recording marshals to the message thread
        {
            auto w = makeWindow();
            w->addTracks ({ makeWav ("r23.wav", 24000).getFullPathName() });
            juce::String err;
            w->setDeviceForTest (fakeDevice (48000.0, 100, 50));
            test::check (w->session.setTrackArm (0, true, err) && w->session.setTrackInput (0, 0, -1, err), "track 0 is armed for R2.3");
            gErrors.clear();
            test::check (w->startRecording(), "recording starts at 48 kHz");
            juce::int64 fed = 0;
            feedDevice (*w, fed, 48000.0, 4800);

            std::atomic<bool> stillRecordingOnWorker { false }, rateUnchangedOnWorker { false };
            std::thread worker ([&]
            {
                DummyAudioDevice dev (44100.0, 512);
                w->io.audioDeviceAboutToStart (&dev);
                stillRecordingOnWorker = w->isRecording();
                rateUnchangedOnWorker = (w->deviceRate == 48000.0 && w->session.sampleRate == 48000.0);
            });
            worker.join();
            test::check (stillRecordingOnWorker && rateUnchangedOnWorker,
                         "background audioDeviceAboutToStart defers stopping the take and adopting the new rate to the message thread");
            juce::MessageManager::getInstance()->runDispatchLoopUntil (100);
            test::check (! w->isRecording() && w->deviceRate == 44100.0 && w->session.sampleRate == 44100.0
                             && w->session.tracks[0]->clips.size() == 2 && ! gErrors.empty() && gErrors.back().contains ("changed while recording"),
                         "once the message loop runs, the take is stopped and placed and the new 44.1 kHz rate is adopted");
        }

        // R2.4: startRecording prepares the session and sets takeFrom / takeLatency / syncTransport before the recorder opens, and isPlaying() is true immediately
        {
            struct PrepareSpyPlugin : test::GainPlugin
            {
                std::function<void()> onPrepare;
                explicit PrepareSpyPlugin (std::function<void()> cb) : test::GainPlugin ("Test Spy", 32), onPrepare (std::move (cb)) {}
                void prepareToPlay (double rate, int block) override
                {
                    test::GainPlugin::prepareToPlay (rate, block);
                    if (onPrepare) onPrepare();
                }
            };

            auto w = makeWindow();
            bool recorderWasActiveDuringPrepare = true;
            int prepareCount = 0;
            w->session.catalog.addInternal (test::GainPlugin::description ("Test Spy"), [&] (double, int)
            {
                return std::make_unique<PrepareSpyPlugin> ([&]
                {
                    ++prepareCount;
                    recorderWasActiveDuringPrepare = w->isRecording();
                });
            });
            w->addTracks ({ makeWav ("r24.wav", 24000).getFullPathName() });
            juce::String err;
            test::check (w->session.setChain (0, { "Test Spy" }, err), "Test Spy added to track 0");
            w->setDeviceForTest (fakeDevice (48000.0, 100, 50));
            test::check (w->session.setTrackArm (0, true, err) && w->session.setTrackInput (0, 0, -1, err), "track 0 armed for R2.4");
            w->seek (4800);
            prepareCount = 0;
            recorderWasActiveDuringPrepare = true;
            test::check (w->startRecording() && w->isRecording() && w->isPlaying(), "startRecording starts both recording and playback");
            test::check (prepareCount > 0 && ! recorderWasActiveDuringPrepare,
                         "session.prepare(false) ran before beginRecording opened the recorder");
            juce::int64 fed = 0;
            feedDevice (*w, fed, 48000.0, 2400);
            w->stopRecording();
            // takeLatency = 100 + 50 + 32 + monitorLatency = 182 + session.monitorLatency() samples
            const juce::int64 expectedStart = 4800 - (182 + w->session.monitorLatency());
            test::check (w->session.tracks[0]->clips.size() == 2 && w->session.tracks[0]->clips[1].start == expectedStart,
                         test::fmt ("takeLatency was computed before recording began and placed the take at %d (expected %d)",
                                    (int) (w->session.tracks[0]->clips.size() == 2 ? w->session.tracks[0]->clips[1].start : -1), (int) expectedStart));
        }

        // R2.5: ArrangeView::askText sets dialogOpen, validates clip/marker identity on confirm, and cleans up safely on destruction
        {
            auto w = makeWindow();
            w->addTracks ({ makeWav ("r25a.wav", 24000).getFullPathName(), makeWav ("r25b.wav", 24000).getFullPathName() });
            w->refreshNow();

            // 1) Clip gain dialog sets dialogOpen, refuses NEW/OPEN/Quit while open, and applies when clip is unchanged
            w->arrange.openClipActionForTest ({ 0, 0 }, 6);
            test::check (w->isDialogOpen() && w->arrange.hasOpenDialog() && w->refuseWhileBusy(),
                         "opening Clip gain sets dialogOpen true and refuseWhileBusy returns true");
            w->arrange.finishOpenDialogForTest (true, "-6.0");
            test::check (! w->isDialogOpen() && ! w->arrange.hasOpenDialog() && std::abs (w->session.tracks[0]->clips[0].gainDb + 6.0f) < 0.01f,
                         "confirming Clip gain resets dialogOpen and updates the clip's gain");

            // 2) Stale clip: deleting/replacing the target clip while Rename clip is open ignores the callback instead of mutating the replacement clip
            w->arrange.openClipActionForTest ({ 0, 0 }, 7);
            test::check (w->isDialogOpen(), "Rename clip sets dialogOpen true");
            juce::String err;
            test::check (w->editor.deleteClip (0, 0, err) && w->editor.moveClipToTrack (1, 0, 0, 1200, err).valid(),
                         "clip 0 on track 0 is deleted and replaced by another clip while Rename clip is open");
            const auto replacementName = w->session.tracks[0]->clips[0].name;
            w->arrange.finishOpenDialogForTest (true, "Wrongly Renamed");
            test::check (! w->isDialogOpen() && w->session.tracks[0]->clips[0].name == replacementName,
                         "confirming Rename clip after the target clip was replaced does not rename the replacement clip");

            // 3) Stale marker: deleting a marker while Rename marker is open does not rename the marker that shifted into its index
            w->editor.addMarker ("Verse", 1000);
            w->editor.addMarker ("Chorus", 8000);
            w->arrange.openMarkerActionForTest (0, 1);
            test::check (w->isDialogOpen(), "Rename marker sets dialogOpen true");
            test::check (w->editor.removeMarker (0, err), "marker 0 is removed while Rename marker is open");
            w->arrange.finishOpenDialogForTest (true, "Stale Marker Name");
            test::check (! w->isDialogOpen() && w->session.markers.size() == 1 && w->session.markers[0].name == "Chorus",
                         "confirming Rename marker after marker 0 was removed leaves the shifted marker untouched");

            // 4) Destroying an ArrangeView while askText is open resets dialogOpen and does not use-after-free
            bool sharedDialogOpen = false;
            {
                ArrangeView tempArrange (w->session, w->editor);
                tempArrange.dialogOpen = &sharedDialogOpen;
                tempArrange.headlessDialogs = true;
                tempArrange.openMarkerActionForTest (0, 1);
                test::check (sharedDialogOpen && tempArrange.hasOpenDialog(), "tempArrange opened askText and set sharedDialogOpen");
            }
            test::check (! sharedDialogOpen, "destroying ArrangeView while askText is open resets dialogOpen cleanly");
        }

        // R2.6: adviceFor and w->notes() do not block or race on track/plugin state while a render holds session.lock and mutates plugins
        {
            auto w = makeWindow();
            w->addTracks ({ makeWav ("r26.wav", 24000).getFullPathName() });
            juce::String err;
            test::check (w->session.addPlugin (0, "Test Delay", -1, err), "Test Delay added to track 0");
            const auto cachedNotes = w->notes();
            test::check (! cachedNotes.empty(), "notes() populates notesCache when unlocked");

            std::atomic<bool> held { false }, stopWorker { false };
            std::thread worker ([&]
            {
                w->session.renderBusy = true;
                const juce::ScopedLock sl (w->session.lock);
                held = true;
                auto* gain = dynamic_cast<test::GainPlugin*> (w->session.tracks[0]->chain[0]->instance.get());
                int step = 0;
                while (! stopWorker)
                {
                    if (gain != nullptr) gain->changeLatency ((++step % 4) * 32);
                    std::this_thread::sleep_for (std::chrono::milliseconds (1));
                }
                w->session.renderBusy = false;
            });
            while (! held)
                std::this_thread::sleep_for (std::chrono::milliseconds (1));

            const auto t0 = juce::Time::getMillisecondCounterHiRes();
            const auto duringRenderNotes = w->notes();
            const auto directAdvice = adviceFor (w->session);
            const auto snap = w->createComponentSnapshot (w->getLocalBounds());
            const double elapsedMs = juce::Time::getMillisecondCounterHiRes() - t0;
            stopWorker = true;
            worker.join();
            test::check (snap.isValid() && ! duringRenderNotes.empty() && ! directAdvice.empty() && elapsedMs < 500.0,
                         test::fmt ("notes(), adviceFor() and paint return immediately during render without racing: %d ms", (int) elapsedMs));
        }

        // R3.9: openProjectRequested refuses while busy and asks to discard unsaved changes before opening another project
        {
            const auto folder = tempFolder().getChildFile ("r39");
            folder.deleteRecursively();
            folder.createDirectory();
            const auto settings = folder.getChildFile ("settings.xml");
            const auto projA = folder.getChildFile ("a.olympus"), projB = folder.getChildFile ("b.olympus");
            {
                auto setup = makeWindow (true, settings);
                setup->addTracks ({ makeWav ("r39b.wav", 12000).getFullPathName() });
                test::check (setup->saveTo (projB), "project B saved for R3.9");
            }
            auto w = makeWindow (true, settings);
            w->addTracks ({ makeWav ("r39a.wav", 24000).getFullPathName() });
            test::check (w->saveTo (projA), "project A saved for R3.9");
            w->editor.setGrid (Grid::Half);   // dirty project A
            test::check (w->autosaveNow() && w->autosaveFile().existsAsFile(), "project A has an autosave");

            // Refused while a render is running
            gErrors.clear();
            gAsked.clear();
            w->session.renderBusy = true;
            w->openProjectRequested (projB);
            w->session.renderBusy = false;
            test::check (w->session.projectFile == projA && gAsked.empty() && gErrors.size() == 1,
                         "openProjectRequested refuses while a render runs");

            // Refused when the user cancels the unsaved prompt
            gAsked.clear();
            gAnswer = false;
            w->openProjectRequested (projB);
            test::check (w->session.projectFile == projA && gAsked.size() == 1 && w->autosaveFile().existsAsFile(),
                         "openProjectRequested prompts when dirty and keeps the current project and autosave on Cancel");

            // Discards, opens project B, and deletes project A's autosave when the user chooses Discard
            const auto oldAutosave = w->autosaveFile();
            gAnswer = true;
            w->openProjectRequested (projB);
            test::check (w->session.projectFile == projB && ! w->session.dirty && ! oldAutosave.existsAsFile(),
                         "openProjectRequested opens the new project and removes the discarded project's autosave on Discard");
        }

        // R3.10: an open monitor plugin editor across saveTo, newProject, openProject, and recoverFrom does not mark a clean project dirty on the timer
        {
            const auto folder = tempFolder().getChildFile ("r310");
            folder.deleteRecursively();
            folder.createDirectory();
            const auto settings = folder.getChildFile ("settings.xml");
            const auto proj = folder.getChildFile ("clean.olympus");

            auto w = makeWindow (true, settings);
            test::check (w->addMonitorPlugin ("Test Gain"), "monitor plugin added for R3.10");
            w->openEditor (-1, 0);
            test::check (w->openEditors() == 1, "monitor plugin editor is open");

            w->addTracks ({ makeWav ("r310.wav", 24000).getFullPathName() });
            juce::String err;
            test::check (w->session.addPlugin (0, "Test Gain", -1, err), "track plugin added for R3.10");
            // let the timer hash the current track plugin state
            juce::MessageManager::getInstance()->runDispatchLoopUntil (600);
            test::check (w->saveTo (proj) && ! w->session.dirty, "project saved cleanly with monitor editor open");
            juce::MessageManager::getInstance()->runDispatchLoopUntil (600);
            test::check (! w->session.dirty, "timer ticks after saveTo do not mark the saved project dirty");

            w->newProject();
            test::check (! w->session.dirty && w->openEditors() == 1, "newProject is clean while monitor editor stays open");
            juce::MessageManager::getInstance()->runDispatchLoopUntil (600);
            test::check (! w->session.dirty, "timer ticks after newProject do not mark the new project dirty");

            w->openProject (proj);
            test::check (! w->session.dirty && w->openEditors() == 1, "openProject is clean while monitor editor stays open");
            juce::MessageManager::getInstance()->runDispatchLoopUntil (600);
            test::check (! w->session.dirty, "timer ticks after openProject do not mark the opened project dirty");

            // recovery resets lastEditorHash too (if then saved, timer ticks keep it clean)
            w->editor.setGrid (Grid::Half);
            proj.setLastModificationTime (juce::Time::getCurrentTime() - juce::RelativeTime::seconds (60));
            test::check (w->autosaveNow(), "autosave written before recovery");
            gRecoverAnswer = true;
            test::check (w->offerRecovery() && w->session.dirty, "offerRecovery loads the autosave");
            gRecoverAnswer = false;
            test::check (w->saveTo (proj) && ! w->session.dirty, "saving after recovery clears dirty");
            juce::MessageManager::getInstance()->runDispatchLoopUntil (600);
            test::check (! w->session.dirty, "timer ticks after recoverFrom + saveTo keep the project clean");
        }

        // R3.11: out-of-bounds ArrangeView::selected does not crash nudgeSelected or splitAtPlayhead
        {
            auto w = makeWindow();
            w->addTracks ({ makeWav ("r311.wav", 48000).getFullPathName() });
            w->refreshNow();

            w->arrange.selected = { 99, 99 };
            w->arrange.keyPressed (juce::KeyPress (juce::KeyPress::rightKey));
            test::check (! w->arrange.selected.valid() && w->session.tracks[0]->clips[0].start == 0,
                         "nudgeSelected with out-of-bounds track clears selection without crashing");

            w->arrange.selected = { 0, 99 };
            w->arrange.keyPressed (juce::KeyPress (juce::KeyPress::downKey));
            test::check (! w->arrange.selected.valid() && w->session.tracks[0]->clips.size() == 1,
                         "nudgeSelected with out-of-bounds clip clears selection without crashing");

            w->seek (24000);
            w->arrange.playhead = 24000.0;
            w->arrange.selected = { 99, 99 };
            w->arrange.splitAtPlayhead();
            test::check (! w->arrange.selected.valid() && w->session.tracks[0]->clips.size() == 2,
                         "splitAtPlayhead with out-of-bounds selected clip safely falls back to splitting clips under the playhead");
        }
    }

    std::printf ("[36] Startup plugin scan, empty cache persistence, and folder safety (R1.1, R1.3, R1.4)\n");
    {
        const auto folder = tempFolder().getChildFile ("r1-uicheck");
        folder.deleteRecursively();
        folder.createDirectory();

        // 1) When plugins.xml already exists with 0 plugins (<KNOWNPLUGINS/>), startup with opts.scanPlugins = true does NOT queue a scan
        {
            const auto dir1 = folder.getChildFile ("existing-empty");
            dir1.createDirectory();
            const auto settingsFile = dir1.getChildFile ("settings.xml");
            const auto cacheFile = Settings::pluginCacheFile (settingsFile);
            cacheFile.replaceWithText ("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<KNOWNPLUGINS/>\n");

            MainComponent::Options options;
            options.audioDevice = false;
            options.scanPlugins = true;
            options.includeSystemPlugins = false;
            options.settingsFile = settingsFile;
            options.recordings = takesFolder();
            auto w = std::make_unique<MainComponent> (options);
            test::check (! w->isScanQueued() && ! w->isScanning(),
                         "startup with an existing 0-plugin plugins.xml does not queue an automatic scan");
        }

        // 2) When plugins.xml is missing, startup queues the suite-only scan, writes plugins.xml, skips third-party bundles,
        //    and the next launch with that plugins.xml present does not queue another scan
        {
            const auto dir2 = folder.getChildFile ("first-launch");
            dir2.createDirectory();
            const auto pluginDir = dir2.getChildFile ("custom-vst3");
            pluginDir.createDirectory();
            // Create a fake third-party .vst3 bundle in pluginDir; suite-only startup scan must ignore it before loading
            const auto thirdParty = pluginDir.getChildFile ("ThirdParty Crash.vst3");
            thirdParty.createDirectory();

            const auto settingsFile = dir2.getChildFile ("settings.xml");
            Settings s;
            s.pluginFolders.add (pluginDir.getFullPathName());
            s.save (settingsFile);
            const auto cacheFile = Settings::pluginCacheFile (settingsFile);
            test::check (! cacheFile.existsAsFile(), "plugins.xml is missing before first launch");

            MainComponent::Options options;
            options.audioDevice = false;
            options.scanPlugins = true;
            options.includeSystemPlugins = false;
            options.settingsFile = settingsFile;
            options.recordings = takesFolder();
            auto w1 = std::make_unique<MainComponent> (options);
            test::check (w1->isScanQueued() && ! w1->settings.scanSuiteOnly,
                         "missing plugins.xml queues startup scan while settings.scanSuiteOnly stays false");

            // Check that candidate filtering for w1's startup scan (onlySuite = true) ignores ThirdParty Crash.vst3
            const auto startupCandidates = w1->session.catalog.candidatesForFormat ("VST3", w1->settings.searchPath(), false, true);
            test::check (startupCandidates.isEmpty(),
                         "suite-only startup scan ignores third-party .vst3 bundles in pluginFolders");

            // Run the queued startup scan (with includeSystem = false via scanPlugins or let scanPlugins run on the temp folder)
            // Note: scanPlugins(false, true) scans system folders for suite names too, and writes plugins.xml
            juce::MessageManager::getInstance()->runDispatchLoopUntil (200);
            test::check (! w1->isScanQueued() && ! w1->isScanning() && cacheFile.existsAsFile(),
                         "queued startup scan completes and writes plugins.xml to disk");
            test::check (! w1->session.catalog.names().contains ("ThirdParty Crash"),
                         "third-party bundle was not loaded by the automatic startup scan");

            // Second launch with the newly written plugins.xml present
            auto w2 = std::make_unique<MainComponent> (options);
            test::check (! w2->isScanQueued() && ! w2->isScanning(),
                         "next window launch with plugins.xml present does not queue another startup scan");
        }

        // 3) defaultPluginFolder() is never userHomeDirectory or a TCC-protected folder, and addPluginFolder refuses forbidden roots
        {
            const auto dir3 = folder.getChildFile ("folder-safety");
            dir3.createDirectory();
            const auto settingsFile = dir3.getChildFile ("settings.xml");
            auto w = makeWindow (true, settingsFile);

            const auto home = juce::File::getSpecialLocation (juce::File::userHomeDirectory);
            const auto def1 = w->defaultPluginFolder();
            test::check (def1 != home && ! Catalog::isForbiddenPluginRoot (def1) && ! Catalog::isTccProtectedDir (def1),
                         "defaultPluginFolder() is never userHomeDirectory or a TCC-protected folder: " + def1.getFullPathName());

            // Even if settings.pluginFolders contains userHomeDirectory, defaultPluginFolder() ignores it
            w->settings.pluginFolders.add (home.getFullPathName());
            const auto def2 = w->defaultPluginFolder();
            test::check (def2 != home && ! Catalog::isForbiddenPluginRoot (def2),
                         "defaultPluginFolder() ignores a forbidden root stored in settings.pluginFolders");
            w->settings.pluginFolders.clear();

            for (const auto& bad : { home,
                                     juce::File::getSpecialLocation (juce::File::userDesktopDirectory),
                                     juce::File::getSpecialLocation (juce::File::userDocumentsDirectory),
                                     juce::File::getSpecialLocation (juce::File::userMusicDirectory),
                                     juce::File ("/") })
            {
                gErrors.clear();
                test::check (! w->addPluginFolder (bad) && w->settings.pluginFolders.isEmpty()
                                 && gErrors.size() == 1 && gErrors[0].contains ("not your home or a protected folder"),
                             "addPluginFolder refuses forbidden root with an error: " + bad.getFullPathName());
            }

            const auto validFolder = dir3.getChildFile ("my-vst3");
            validFolder.createDirectory();
            gErrors.clear();
            test::check (w->addPluginFolder (validFolder) && gErrors.empty()
                             && w->settings.pluginFolders.contains (validFolder.getFullPathName())
                             && w->defaultPluginFolder() == validFolder,
                         "addPluginFolder accepts a valid plugin folder and defaultPluginFolder() returns it");
        }
    }

    std::printf ("[37] External project reload and track fader tooltips\n");
    {
        const auto dir = tempFolder().getChildFile ("ext-reload");
        dir.createDirectory();
        const auto proj = dir.getChildFile ("live.olympus");
        auto w = makeWindow (true, dir.getChildFile ("settings.xml"));
        const auto wav = makeWav ("ext-reload.wav", 24000);
        w->addTracks ({ wav.getFullPathName() });
        juce::String err;
        test::check (w->session.addPlugin (0, "Test Gain", -1, err) && w->saveTo (proj) && ! w->session.dirty,
                     "external reload: project saved with track and plugin");
        test::check (w->arrange.faderSlider (0) != nullptr
                         && w->arrange.faderSlider (0)->getTooltip() == "Fader +0.0 dB (drag, double-click for 0 dB)",
                     "external reload: initial track 0 fader tooltip is +0.0 dB");

        // External process (e.g. OlympusMcp) modifies the project file on disk with a newer mtime.
        Session mcp;
        test::addTestPlugins (mcp.catalog);
        test::check (mcp.load (proj, err), "external reload: MCP loads project");
        Editor mcpEd (mcp);
        test::check (mcpEd.setTrackGain (0, -5.4f, err), "external reload: MCP sets track gain to -5.4 dB");
        mcp.tracks[0]->chain[0]->instance->setCurrentProgram (1);
        test::check (mcp.save (proj, err), "external reload: MCP saves project");
        proj.setLastModificationTime (juce::Time::getCurrentTime() + juce::RelativeTime::seconds (10.0));

        // While playback or render is active, checkExternalProjectChange defers without consuming the mtime change.
        w->togglePlay();
        test::check (w->isPlaying() && ! w->checkExternalProjectChange() && std::abs (w->session.tracks[0]->gainDb) < 0.01f,
                     "external reload: deferred while playing");
        w->togglePlay();
        w->session.renderBusy.store (true);
        test::check (! w->checkExternalProjectChange() && std::abs (w->session.tracks[0]->gainDb) < 0.01f,
                     "external reload: deferred while rendering");
        w->session.renderBusy.store (false);

        // Clean window auto-reloads immediately without asking.
        test::check (w->checkExternalProjectChange()
                         && std::abs (w->session.tracks[0]->gainDb - (-5.4f)) < 0.01f
                         && w->session.tracks[0]->chain[0]->instance->getCurrentProgram() == 1
                         && ! w->session.dirty,
                     "external reload: clean window reloads track gain (-5.4 dB) and plugin preset (1) automatically");
        test::check (w->arrange.faderSlider (0) != nullptr
                         && w->arrange.faderSlider (0)->getTooltip() == "Fader -5.4 dB (drag, double-click for 0 dB)",
                     "external reload: track 0 fader tooltip updated to -5.4 dB after reload");

        // Dirty window prompts via askReload: "Keep mine" (false) keeps local changes and does not prompt again for the same mtime.
        test::check (w->editor.setTrackGain (0, -12.0f, err) && w->session.dirty, "external reload: window makes local edit (-12 dB)");
        test::check (w->autosaveNow() && w->autosaveFile().existsAsFile(), "external reload: autosave written for dirty window");
        test::check (mcpEd.setTrackGain (0, -3.0f, err) && mcp.save (proj, err), "external reload: MCP saves -3.0 dB");
        proj.setLastModificationTime (juce::Time::getCurrentTime() + juce::RelativeTime::seconds (20.0));
        int reloadPrompts = 0;
        w->askReload = [&] (const juce::String&) { ++reloadPrompts; return gReloadAnswer; };
        gReloadAnswer = false;
        test::check (! w->checkExternalProjectChange() && reloadPrompts == 1
                         && std::abs (w->session.tracks[0]->gainDb - (-12.0f)) < 0.01f && w->session.dirty,
                     "external reload: dirty window with Keep mine preserves local -12 dB and dirty flag");
        test::check (! w->checkExternalProjectChange() && reloadPrompts == 1,
                     "external reload: same mtime does not prompt a second time");

        // Newer external save with "Reload" (true) reloads the disk file, clears dirty, and deletes the discarded autosave.
        proj.setLastModificationTime (juce::Time::getCurrentTime() + juce::RelativeTime::seconds (30.0));
        gReloadAnswer = true;
        test::check (w->checkExternalProjectChange() && reloadPrompts == 2
                         && std::abs (w->session.tracks[0]->gainDb - (-3.0f)) < 0.01f
                         && ! w->session.dirty && ! w->autosaveFile().existsAsFile(),
                     "external reload: dirty window with Reload loads external -3.0 dB, clears dirty, and deletes autosave");
        test::check (w->arrange.faderSlider (0) != nullptr
                         && w->arrange.faderSlider (0)->getTooltip() == "Fader -3.0 dB (drag, double-click for 0 dB)",
                     "external reload: track 0 fader tooltip updated to -3.0 dB after second reload");

        // Failed reload (corrupt XML on disk) returns false, keeps current session in w, and does NOT delete w->autosaveFile().
        test::check (w->editor.setTrackGain (0, -9.0f, err) && w->session.dirty && w->autosaveNow() && w->autosaveFile().existsAsFile(),
                     "external reload: dirty session (-9 dB) and autosave prepared before failed reload");
        proj.replaceWithText ("<not-valid-project-xml");
        proj.setLastModificationTime (juce::Time::getCurrentTime() + juce::RelativeTime::seconds (40.0));
        gReloadAnswer = true;
        test::check (! w->checkExternalProjectChange()
                         && std::abs (w->session.tracks[0]->gainDb - (-9.0f)) < 0.01f
                         && w->session.dirty && w->autosaveFile().existsAsFile(),
                     "external reload: failed reload returns false, keeps session state, and preserves autosave");
    }

    std::printf ("[38] Window resizing: AudioDialog, PluginWindow, and MainComponent wide layout\n");
    {
        // 1. AudioDialog has non-zero initial size and is resizable with limits
        AudioDialog dlg (std::make_unique<juce::Component>(), false);
        test::check (dlg.getWidth() > 0 && dlg.getHeight() > 0 && dlg.isResizable(),
                     "AudioDialog has non-zero width/height and isResizable() is true");

        // 2. PluginWindow wraps editor->getConstrainer() and forwards isResizable()
        struct ResizablePlugin : test::GainPlugin
        {
            struct Editor : juce::AudioProcessorEditor
            {
                explicit Editor (juce::AudioProcessor& p) : juce::AudioProcessorEditor (p)
                {
                    setResizeLimits (240, 160, 900, 700);
                    setResizable (true, true);
                    setSize (400, 300);
                }
            };
            bool hasEditor() const override { return true; }
            juce::AudioProcessorEditor* createEditor() override { return new Editor (*this); }
        };
        ResizablePlugin proc;
        auto* editor = proc.createEditor();
        PluginWindow pwin ("Test Plugin", editor, {}, false);
        test::check (pwin.isResizable() && pwin.getConstrainer() != nullptr,
                     "PluginWindow wraps editor->getConstrainer() and forwards isResizable() for a resizable editor");
        {
            const auto b = pwin.getBorderThickness();
            const int extraW = b.getLeftAndRight();
            const int extraH = b.getTopAndBottom() + (pwin.isUsingNativeTitleBar() ? 0 : pwin.getTitleBarHeight());
            const juce::Rectangle<int> limits (0, 0, 2000, 2000);
            const juce::Rectangle<int> prev (150, 200, 400 + extraW, 300 + extraH);
            juce::Rectangle<int> bounds (150, 200, 100, 80);
            pwin.getConstrainer()->checkBounds (bounds, prev, limits, false, false, true, true);
            test::check (bounds.getX() == 150 && bounds.getY() == 200
                             && bounds.getWidth() == 240 + extraW && bounds.getHeight() == 160 + extraH,
                         "PluginWindow constrainer enforces editor limits without corrupting non-zero (x, y)");

            juce::Rectangle<int> leftTopBounds (300, 350, 100, 80);
            pwin.getConstrainer()->checkBounds (leftTopBounds, prev, limits, true, true, false, false);
            test::check (leftTopBounds.getRight() == prev.getRight() && leftTopBounds.getBottom() == prev.getBottom()
                             && leftTopBounds.getWidth() == 240 + extraW && leftTopBounds.getHeight() == 160 + extraH,
                         "PluginWindow constrainer preserves anchored right/bottom corner when stretching top/left");

            editor->getConstrainer()->setMinimumOnscreenAmounts (20, 20, 20, 20);
            const juce::Rectangle<int> offsetLimits (1440, 25, 1920, 1055);
            const juce::Rectangle<int> offsetPrev (1500, 100, 400 + extraW, 300 + extraH);
            juce::Rectangle<int> offsetBounds (1500, 100, 500 + extraW, 400 + extraH);
            pwin.getConstrainer()->checkBounds (offsetBounds, offsetPrev, offsetLimits, false, false, true, true);
            test::check (offsetBounds.getX() == 1500 && offsetBounds.getY() == 100
                             && offsetBounds.getWidth() == 500 + extraW && offsetBounds.getHeight() == 400 + extraH,
                         "PluginWindow constrainer keeps non-zero (x, y) and valid size under non-zero-origin limits");
        }

        // Aspect-ratio + coordinate-overwriting inner constrainer (like VST3PluginWindow) must not jump to negative coordinates
        struct AspectJumpingPlugin : test::GainPlugin
        {
            struct JumpingConstrainer : juce::ComponentBoundsConstrainer
            {
                JumpingConstrainer()
                {
                    setSizeLimits (320, 200, 1280, 800);
                    setFixedAspectRatio (1.6);
                }
                void checkBounds (juce::Rectangle<int>& bounds, const juce::Rectangle<int>& previousBounds,
                                  const juce::Rectangle<int>& limits, bool top, bool left, bool bottom, bool right) override
                {
                    juce::ComponentBoundsConstrainer::checkBounds (bounds, previousBounds, limits, top, left, bottom, right);
                    bounds.setPosition (-120, -90);
                }
            };
            struct Editor : juce::AudioProcessorEditor
            {
                explicit Editor (juce::AudioProcessor& p) : juce::AudioProcessorEditor (p)
                {
                    setConstrainer (&jumping);
                    setResizable (true, true);
                    setSize (640, 400);
                }
                ~Editor() override { setConstrainer (nullptr); }
                JumpingConstrainer jumping;
            };
            bool hasEditor() const override { return true; }
            juce::AudioProcessorEditor* createEditor() override { return new Editor (*this); }
        };
        AspectJumpingPlugin jumpProc;
        auto* jumpEditor = jumpProc.createEditor();
        PluginWindow jumpWin ("Aspect Plugin", jumpEditor, {}, false);
        {
            const auto b = jumpWin.getBorderThickness();
            const int extraW = b.getLeftAndRight();
            const int extraH = b.getTopAndBottom() + (jumpWin.isUsingNativeTitleBar() ? 0 : jumpWin.getTitleBarHeight());
            const juce::Rectangle<int> limits (0, 0, 2000, 2000);
            const juce::Rectangle<int> prev (120, 90, 640 + extraW, 400 + extraH);
            juce::Rectangle<int> bounds (120, 90, 800 + extraW, 650 + extraH);
            jumpWin.getConstrainer()->checkBounds (bounds, prev, limits, false, false, true, true);
            const int contentW = bounds.getWidth() - extraW;
            const int contentH = bounds.getHeight() - extraH;
            test::check (bounds.getX() == 120 && bounds.getY() == 90
                             && contentW >= 320 && contentW <= 1280
                             && std::abs ((double) contentW / (double) contentH - 1.6) < 0.02,
                         "PluginWindow constrainer enforces aspect/limits while ignoring negative (x, y) from inner constrainer");
        }

        struct FixedPlugin : test::GainPlugin
        {
            struct Editor : juce::AudioProcessorEditor
            {
                explicit Editor (juce::AudioProcessor& p) : juce::AudioProcessorEditor (p) { setSize (320, 200); }
            };
            bool hasEditor() const override { return true; }
            juce::AudioProcessorEditor* createEditor() override { return new Editor (*this); }
        };
        FixedPlugin fixedProc;
        auto* fixedEditor = fixedProc.createEditor();
        PluginWindow fixedWin ("Fixed Plugin", fixedEditor, {}, false);
        test::check (! fixedWin.isResizable(),
                     "PluginWindow respects isResizable() == false for a fixed-size editor");

        // 3. MainComponent at 1600x1000 lays out deviceLabel, track/REC readout area, and positionBox without overlap
        auto w = makeWindow();
        w->setSize (1600, 1000);
        w->resized();
        layoutCheck (*w, "1600x1000");
        const int readoutX = w->positionBox.getX() - 148;
        test::check (! w->deviceLabel.getBounds().intersects (w->positionBox)
                         && w->deviceLabel.getWidth() > 620
                         && w->deviceLabel.getRight() <= readoutX
                         && readoutX + 140 <= w->positionBox.getX(),
                     "MainComponent at 1600x1000 anchors deviceLabel and readout before positionBox without overlap");
    }

    std::printf ("[39] Export button: layout at 1280x800, mix-only export, and monitor chain exclusion\n");
    {
        const auto expRoot = tempFolder().getChildFile ("export-check");
        expRoot.deleteRecursively();
        expRoot.createDirectory();
        auto w = makeWindow (true, expRoot.getChildFile ("settings.xml"));
        w->setSize (MainComponent::kDesignWidth, MainComponent::kDesignHeight);
        w->resized();
        test::check (w->exportButton.isVisible() && inside (w->exportButton, *w)
                         && w->exportButton.getButtonText() == "Export"
                         && ! w->exportButton.getBounds().intersects (w->renderButton.getBounds())
                         && ! w->exportButton.getBounds().intersects (w->audioButton.getBounds())
                         && ! w->exportButton.getBounds().intersects (w->guideButton.getBounds())
                         && w->renderButton.getBounds().getRight() <= w->exportButton.getX()
                         && w->exportButton.getBounds().getRight() <= w->audioButton.getX()
                         && w->audioButton.getBounds().getRight() <= w->guideButton.getX(),
                     "exportButton is visible, inside bounds, and sits cleanly between renderButton and audioButton at 1280x800");
        layoutCheck (*w, "Export button at 1280x800");

        w->addTracks ({ makeWav ("exp-a.wav", 24000).getFullPathName(), makeWav ("exp-b.wav", 24000).getFullPathName() });
        juce::String gainErr;
        test::check (w->editor.setTrackGain (0, -12.0f, gainErr) && w->editor.setTrackGain (1, -12.0f, gainErr),
                     "track gains attenuate before monitor boost check");
        const auto baselineDir = expRoot.getChildFile ("baseline");
        RenderOptions baseOpts;
        baseOpts.writeStems = false;
        baseOpts.writeMix = true;
        baseOpts.bits = 32;
        test::check (w->renderTo (baselineDir, {}, false, baseOpts).isEmpty(), "baseline monitor-free mix renders");
        const auto baselineBytes = bytesOf (baselineDir.getChildFile ("mix.wav"));
        test::check (baselineBytes.getSize() > 1000, "baseline mix.wav is non-empty");

        // Add a monitor plugin with +6 dB gain (Boost preset) and verify playback is boosted while Export excludes it.
        const float unmonitoredPeak = playPeak (*w);
        test::check (unmonitoredPeak < 0.5f, "attenuated unmonitored playback stays well below full scale");
        test::check (w->addMonitorPlugin ("Test Gain") && w->session.monitor.size() == 1, "Test Gain added to monitor chain");
        w->session.monitor[0]->instance->setCurrentProgram (1);   // +6 dB
        const float monitoredPeak = playPeak (*w);
        test::check (monitoredPeak > unmonitoredPeak * 1.8f, "active +6 dB monitor plugin boosts playback");

        const auto export32Dir = expRoot.getChildFile ("export-32");
        juce::String seenChooserTitle;
        bool sawMixOnlyDefaults = false;
        w->settings.renderStems = true;
        w->settings.renderBits = 32;
        w->chooseRenderOptions = [&] (RenderOptions& o)
        {
            sawMixOnlyDefaults = (! o.writeStems && o.writeMix);
            o.writeStems = true;   // chooseAndExport must still force writeStems = false
            return true;
        };
        w->chooseRenderFolder = [&] (const juce::String& title, const juce::File&, std::function<void (const juce::File&)> done)
        {
            seenChooserTitle = title;
            done (export32Dir);
        };
        gErrors.clear();
        gNotes.clear();
        press (w->exportButton);
        const auto exportedFiles = export32Dir.findChildFiles (juce::File::findFiles, false, "*");
        test::check (sawMixOnlyDefaults && seenChooserTitle == "Export final audio into this folder" && gErrors.empty(),
                     "Export button opens folder chooser with export title and mix-only options");
        test::check (exportedFiles.size() == 1 && exportedFiles[0].getFileName() == "mix.wav",
                     "Export writes only mix.wav without stems");
        test::check (bytesOf (export32Dir.getChildFile ("mix.wav")) == baselineBytes,
                     "Exported mix.wav excludes the active +6 dB monitor chain");
        test::check (w->settings.renderStems, "Export preserves settings.renderStems");

        // 24-bit export remembers bits without changing renderStems.
        const auto export24Dir = expRoot.getChildFile ("export-24");
        w->chooseRenderOptions = [] (RenderOptions& o) { o.bits = 24; return true; };
        w->chooseRenderFolder = [&] (const juce::String&, const juce::File&, std::function<void (const juce::File&)> done)
        {
            done (export24Dir);
        };
        press (w->exportButton);
        const auto files24 = export24Dir.findChildFiles (juce::File::findFiles, false, "*");
        juce::WavAudioFormat wav;
        std::unique_ptr<juce::InputStream> in24 (export24Dir.getChildFile ("mix.wav").createInputStream());
        std::unique_ptr<juce::AudioFormatReader> reader24 (wav.createReaderFor (in24.release(), true));
        test::check (files24.size() == 1 && reader24 != nullptr && reader24->bitsPerSample == 24
                         && w->settings.renderBits == 24 && w->settings.renderStems,
                     "24-bit Export writes 24-bit mix.wav, remembers renderBits, and keeps renderStems");

        // Export refuses while another render is running.
        gErrors.clear();
        w->session.renderBusy = true;
        press (w->exportButton);
        w->session.renderBusy = false;
        test::check (gErrors.size() == 1 && gErrors[0].contains ("A render is running"),
                     "Export button refuses while a render is running");
    }

    std::printf ("[40] Batch 3 checks: 0-track dirty askToDiscard, wheel clamp / Shift+scroll, and trimStart fade preservation\n");
    {
        // 1) askToDiscard prompts when dirty == true even if tracks.empty() (e.g. tempo/grid edit or deleting last track)
        auto w = makeWindow();
        w->editor.setGrid (Grid::Half);
        test::check (w->session.dirty && w->session.tracks.empty(), "session is dirty with 0 tracks after grid edit");
        gAsked.clear();
        gAnswer = false;
        bool proceeded = false;
        w->askToDiscard ("Start a new project and lose them?", [&] { proceeded = true; });
        test::check (gAsked.size() == 1 && ! proceeded, "askToDiscard prompts when dirty == true and tracks.empty(), and Cancel stops");
        gAnswer = true;
        w->askToDiscard ("Start a new project and lose them?", [&] { proceeded = true; });
        test::check (gAsked.size() == 2 && proceeded, "askToDiscard proceeds when confirmed on 0-track dirty project");

        // 2) Cmd+wheel clamps x >= kHeaderWidth and Shift+scroll prefers deltaY over small deltaX noise
        w->addTracks ({ makeWav ("wheel40.wav", 48000).getFullPathName() });
        w->editor.setSnap (false);
        w->refreshNow();
        const auto now = juce::Time::getCurrentTime();
        auto makeWheelEvent = [&] (juce::Point<int> p, juce::ModifierKeys mods)
        {
            return juce::MouseEvent (juce::Desktop::getInstance().getMainMouseSource(), p.toFloat(), mods, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f,
                                     &w->arrange, &w->arrange, now, p.toFloat(), now, 1, false);
        };
        w->arrange.mouseWheelMove (makeWheelEvent ({ 10, 100 }, cmd()), { 0.0f, -0.5f, false, false, false });
        test::check (w->arrange.zoom < ArrangeView::kDefaultZoom
                         && w->arrange.clipBounds ({ 0, 0 }).getX() == ArrangeView::kHeaderWidth,
                     "Cmd+wheel over header clamps x >= kHeaderWidth so scrollSamples does not drift");
        w->arrange.zoom = 4000.0;   // zoomed in: the content is wider than the view, so a horizontal scroll has somewhere to go (it is bounded now)
        w->arrange.scrollTo (0);
        w->arrange.mouseWheelMove (makeWheelEvent ({ 400, 100 }, juce::ModifierKeys (juce::ModifierKeys::shiftModifier)),
                                   { 0.02f, -1.0f, false, false, false });
        test::check (w->arrange.clipBounds ({ 0, 0 }).getX() < ArrangeView::kHeaderWidth - 100,
                     "Shift+scroll uses deltaY to scroll horizontally even when deltaX has small noise");

        // 3) trimStart drag preserves original fadeIn/fadeOut when dragging inward then back out before mouseUp
        w->arrange.scrollTo (0);
        w->arrange.zoom = ArrangeView::kDefaultZoom;
        w->refreshNow();
        juce::String err;
        test::check (w->editor.setClipFades (0, 0, 8000, 8000, err), "initial clip fades set to 8000 samples");
        const auto bounds = w->arrange.clipBounds ({ 0, 0 });
        const juce::Point<int> leftEdge { bounds.getX() + 2, bounds.getCentreY() };
        const juce::Point<int> farInward { bounds.getRight() - 4, bounds.getCentreY() };
        w->arrange.mouseDown (mouseAt (w->arrange, leftEdge));
        w->arrange.mouseDrag (mouseAt (w->arrange, farInward));
        test::check (w->session.tracks[0]->clips[0].length < 8000 && w->session.tracks[0]->clips[0].fadeIn < 8000,
                     "dragging trimStart inward temporarily shortens clip and fades");
        w->arrange.mouseDrag (mouseAt (w->arrange, leftEdge));
        w->arrange.mouseUp (mouseAt (w->arrange, leftEdge));
        test::check (w->session.tracks[0]->clips[0].start == 0
                         && w->session.tracks[0]->clips[0].length == 48000
                         && w->session.tracks[0]->clips[0].fadeIn == 8000
                         && w->session.tracks[0]->clips[0].fadeOut == 8000,
                     "dragging trimStart back out before mouseUp restores original fadeIn and fadeOut");

        // trimEnd drag preserves original fadeIn/fadeOut when dragging inward then back out before mouseUp
        const juce::Point<int> rightEdge { bounds.getRight() - 2, bounds.getCentreY() };
        const juce::Point<int> farLeft { bounds.getX() + 4, bounds.getCentreY() };
        w->arrange.mouseDown (mouseAt (w->arrange, rightEdge));
        w->arrange.mouseDrag (mouseAt (w->arrange, farLeft));
        test::check (w->session.tracks[0]->clips[0].length < 8000 && w->session.tracks[0]->clips[0].fadeOut < 8000,
                     "dragging trimEnd inward temporarily shortens clip and fades");
        w->arrange.mouseDrag (mouseAt (w->arrange, rightEdge));
        w->arrange.mouseUp (mouseAt (w->arrange, rightEdge));
        test::check (w->session.tracks[0]->clips[0].start == 0
                         && w->session.tracks[0]->clips[0].length == 48000
                         && w->session.tracks[0]->clips[0].fadeIn == 8000
                         && w->session.tracks[0]->clips[0].fadeOut == 8000,
                     "dragging trimEnd back out before mouseUp restores original fadeIn and fadeOut");
    }

    std::printf ("[41] Batch 4 checks: fade handles from 0, cursors, zoomBy anchor, double-click faders, Alt+Click seek, empty togglePlay\n");
    {
        auto w = makeWindow();
        gNotes.clear();
        w->togglePlay();
        test::check (! w->isPlaying() && gNotes.size() == 1, "togglePlay with no tracks notifies instead of playing silently");

        w->addTracks ({ makeWav ("batch4.wav", 192000).getFullPathName() });
        w->refreshNow();

        // 1) faderSlider and masterFader double-click return value is 0.0
        auto* tf = w->arrange.faderSlider (0);
        test::check (w->masterFader.isDoubleClickReturnEnabled() && std::abs (w->masterFader.getDoubleClickReturnValue()) < 1e-9,
                     "masterFader double-click return value is 0.0");
        test::check (tf != nullptr && tf->isDoubleClickReturnEnabled() && std::abs (tf->getDoubleClickReturnValue()) < 1e-9,
                     "track faderSlider double-click return value is 0.0");

        // 2) zoomBy anchors on visible playhead, else on viewport center (zoomed in so the content is wider than the view: scroll is bounded)
        w->arrange.scrollTo (0);
        w->arrange.zoom = 1600.0;
        w->seek (20000);   // inside the view at this zoom
        w->zoomBy (2.0);
        test::check (std::abs (w->arrange.zoom - 3200.0) < 1e-6
                         && std::abs (w->arrange.clipBounds ({ 0, 0 }).getX() - (ArrangeView::kHeaderWidth - juce::roundToInt (10000.0 * 3200.0 / 48000.0))) <= 1,
                     "zoomBy anchors on visible playhead");

        w->arrange.zoom = 1600.0;
        w->arrange.scrollTo (1 << 30);   // clamps to the content's right edge
        w->seek (0);       // off-screen to the left
        const double leftSample = w->arrange.scrollSamplesForTest();
        const double timelineW = (double) (w->arrange.getWidth() - ArrangeView::kHeaderWidth);
        const double centerSample = leftSample + (timelineW * 0.5) / (1600.0 / 48000.0);
        w->zoomBy (2.0);
        const double newPps = 3200.0 / 48000.0;
        const double expectedScroll = centerSample - (timelineW * 0.5) / newPps;
        const int expectedClipX = ArrangeView::kHeaderWidth + juce::roundToInt (-expectedScroll * newPps);
        test::check (std::abs (w->arrange.clipBounds ({ 0, 0 }).getX() - expectedClipX) <= 1,
                     "zoomBy anchors on viewport center when playhead is off-screen");

        // 3) hitFadeIn / hitFadeOut at 0 do not swallow trimStart / trimEnd at the 6 px clip edge, drag-create fades, clipMenu fade actions, and hover cursors
        w->editor.setSnap (false);
        w->arrange.scrollTo (0);
        w->arrange.zoom = ArrangeView::kDefaultZoom;
        w->refreshNow();
        const auto cb = w->arrange.clipBounds ({ 0, 0 });
        const juce::Point<int> topEdgeLeft { cb.getX() + 2, cb.getY() + 2 };
        const juce::Point<int> topEdgeRight { cb.getRight() - 2, cb.getY() + 2 };
        const juce::Point<int> fadeZeroLeft { cb.getX() + 9, cb.getY() + 2 };
        const juce::Point<int> fadeZeroRight { cb.getRight() - 9, cb.getY() + 2 };
        test::check (! w->arrange.hitFadeIn ({ 0, 0 }, topEdgeLeft) && ! w->arrange.hitFadeOut ({ 0, 0 }, topEdgeRight),
                     "hitFadeIn and hitFadeOut do not swallow the 6 px clip edge when fadeIn == 0 and fadeOut == 0");
        test::check (w->arrange.hitFadeIn ({ 0, 0 }, fadeZeroLeft) && w->arrange.hitFadeOut ({ 0, 0 }, fadeZeroRight),
                     "hitFadeIn and hitFadeOut hit inset top corner handles when fadeIn == 0 and fadeOut == 0");

        // Clicking top-left 6 px edge trims start (not fadeIn), and clicking top-right 6 px edge trims end (not fadeOut)
        w->arrange.mouseDown (mouseAt (w->arrange, topEdgeLeft));
        w->arrange.mouseDrag (mouseAt (w->arrange, topEdgeLeft + juce::Point<int> (20, 0)));
        w->arrange.mouseUp (mouseAt (w->arrange, topEdgeLeft + juce::Point<int> (20, 0)));
        test::check (w->session.tracks[0]->clips[0].start > 0 && w->session.tracks[0]->clips[0].fadeIn == 0,
                     "dragging top-left 6 px clip edge trims start instead of creating fadeIn");
        w->editor.undo();
        w->refreshNow();

        const auto origLen = w->session.tracks[0]->clips[0].length;
        w->arrange.mouseDown (mouseAt (w->arrange, topEdgeRight));
        w->arrange.mouseDrag (mouseAt (w->arrange, topEdgeRight - juce::Point<int> (20, 0)));
        w->arrange.mouseUp (mouseAt (w->arrange, topEdgeRight - juce::Point<int> (20, 0)));
        test::check (w->session.tracks[0]->clips[0].length < origLen && w->session.tracks[0]->clips[0].fadeOut == 0,
                     "dragging top-right 6 px clip edge trims end instead of creating fadeOut");
        w->editor.undo();
        w->refreshNow();

        w->arrange.mouseMove (mouseAt (w->arrange, fadeZeroLeft));
        test::check (w->arrange.getMouseCursor() == juce::MouseCursor::LeftRightResizeCursor,
                     "hovering fade handle shows LeftRightResizeCursor");
        w->arrange.mouseMove (mouseAt (w->arrange, cb.getCentre()));
        test::check (w->arrange.getMouseCursor() == juce::MouseCursor::NormalCursor,
                     "hovering clip body shows NormalCursor");

        w->arrange.mouseDown (mouseAt (w->arrange, fadeZeroLeft));
        w->arrange.mouseDrag (mouseAt (w->arrange, fadeZeroLeft + juce::Point<int> (40, 0)));
        w->arrange.mouseUp (mouseAt (w->arrange, fadeZeroLeft + juce::Point<int> (40, 0)));
        test::check (w->session.tracks[0]->clips[0].fadeIn > 0, "dragging inset top-left fade handle from 0 creates fadeIn");

        w->arrange.openClipActionForTest ({ 0, 0 }, 9);
        test::check (w->session.tracks[0]->clips[0].fadeIn == 24000, "clipMenu Fade in (0.5 s) sets 24000 samples at 48 kHz");
        w->arrange.openClipActionForTest ({ 0, 0 }, 10);
        test::check (w->session.tracks[0]->clips[0].fadeOut == 24000, "clipMenu Fade out (0.5 s) sets 24000 samples at 48 kHz");
        w->arrange.openClipActionForTest ({ 0, 0 }, 11);
        test::check (w->session.tracks[0]->clips[0].fadeIn == 0 && w->session.tracks[0]->clips[0].fadeOut == 0,
                     "clipMenu Clear fades resets both fades to 0");

        // 4) Alt+Click in empty lane space seeks the playhead
        w->editor.setSnap (false);
        const auto now = juce::Time::getCurrentTime();
        const juce::Point<int> emptyLane { ArrangeView::kHeaderWidth + 160, ArrangeView::kRulerHeight + ArrangeView::kLoopHeight + ArrangeView::kMarkerHeight + 140 };
        w->arrange.mouseDown (juce::MouseEvent (juce::Desktop::getInstance().getMainMouseSource(), emptyLane.toFloat(),
                                                juce::ModifierKeys (juce::ModifierKeys::altModifier), 1.0f, 0.0f, 0.0f, 0.0f, 0.0f,
                                                &w->arrange, &w->arrange, now, emptyLane.toFloat(), now, 1, false));
        test::check (std::abs (w->position() - 96000) <= 1000, "Alt+Click in empty lane space seeks playhead");

        // 5) trackAt returns -1 in the bottom scroll strip (no hit/drop on occluded tracks)
        const auto scrollWav = makeWav ("strip_drop.wav", 48000).getFullPathName();
        while ((int) w->session.tracks.size() * ArrangeView::kLaneHeight
               <= w->arrange.getHeight() - ArrangeView::kRulerHeight - ArrangeView::kLoopHeight - ArrangeView::kMarkerHeight)
            w->addTracks ({ scrollWav });
        w->refreshNow();
        const int tracksBeforeDrop = (int) w->session.tracks.size();
        const int stripY = w->arrange.getHeight() - ArrangeView::kScrollStripHeight / 2;
        test::check (! w->arrange.clipAt ({ ArrangeView::kHeaderWidth + 20, stripY }).valid(),
                     "clipAt over bottom scroll strip returns invalid even when tracks extend underneath");
        w->arrange.filesDropped ({ scrollWav }, ArrangeView::kHeaderWidth + 20, stripY);
        test::check ((int) w->session.tracks.size() == tracksBeforeDrop + 1,
                     "filesDropped over bottom scroll strip creates a new track instead of targeting occluded track");
    }

    std::printf ("[42] The take's own first written block is its start (with the head the files opening cost); a device change and REC wait for a render, and a late stale start leaves the newer one kept; a plugin edited and closed at once dirties; a seek inside a block stands; the caps agree; a null output channel, the other held silent; the notes read only under the lock\n");
    {
        // The take's start is the playhead of the first block the recorder wrote, not where REC was pressed, and the input its files were
        // still opening costs is counted and said (findings 2 and 7). Its own settings folder: no monitor chain beside it, so the take is
        // placed exactly where it began.
        const auto dir42 = tempFolder().getChildFile ("takes42");
        dir42.deleteRecursively();
        dir42.createDirectory();
        const auto clean42 = dir42.getChildFile ("settings.xml");
        {
            auto w = makeWindow (true, clean42);
            w->session.addEmptyTrack ("Gtr");
            juce::String error;
            test::check (w->session.setTrackArm (0, true, error) && w->session.setTrackInput (0, 0, -1, error), "a track armed on input 1");
            w->setDeviceForTest (fakeDevice (48000.0, 0, 0));   // no latency: the take is placed exactly where it began
            w->seek (4800);
            test::check (w->startRecording(), "REC starts at 4800");
            w->setFilesOpeningForChecks (true);                 // the take's files are still opening (Recorder::start does that on the message thread)
            juce::int64 fed = 0;
            feedDevice (*w, fed, 48000.0, 2000);                // the transport runs on; the input of these two seconds is not written
            w->setFilesOpeningForChecks (false);
            feedDevice (*w, fed, 48000.0, 2400);
            gErrors.clear();
            w->stopRecording();
            const auto& clips = w->session.tracks[0]->clips;
            test::check (clips.size() == 1 && clips[0].start == 6800 && clips[0].length == 2400,
                         test::fmt ("the take begins where its first frame was written (6800, not the 4800 REC was pressed at): %d clips, start %d, length %d",
                                    (int) clips.size(), clips.empty() ? -1 : (int) clips[0].start, clips.empty() ? -1 : (int) clips[0].length));
            test::check (gErrors.size() == 1 && gErrors[0].contains ("2000 samples") && gErrors[0].contains ("were not written before the take could run"),
                         "and the head the opening cost is reported with its length: " + (gErrors.empty() ? juce::String ("<nothing>") : gErrors[0]));
        }

        // The selection is the clip a take actually became: a take that could not be placed leaves the view on the clips, and an older clip
        // that happens to start where the take would have gone does not win the selection (finding 8).
        {
            auto w = makeWindow (true, clean42);
            w->addTracks ({ makeWav ("older-clip.wav", 24000).getFullPathName() });   // track 0, one clip at sample 0
            juce::String error;
            test::check (w->session.setTrackArm (0, true, error), "that track is armed");
            w->setDeviceForTest (fakeDevice (48000.0, 100, 50));   // a round trip of 150: a take from 0 would begin before the timeline
            w->arrange.selected = {};
            w->seek (0);
            test::check (w->startRecording(), "REC starts at 0");
            juce::int64 fed = 0;
            feedDevice (*w, fed, 48000.0, 100);                   // a take shorter than the latency, so it cannot be placed
            gErrors.clear();
            w->stopRecording();
            test::check (w->session.tracks[0]->clips.size() == 1 && ! gErrors.empty() && gErrors.back().contains ("was not placed"),
                         "the take that cannot be placed is said, and nothing was added");
            test::check (! w->arrange.selected.valid(), "and the older clip at its start is not selected for it");
        }

        // A plugin edited in its own window and closed within a tick marks the project dirty (finding 1): the state the timer compares
        // against is taken when the first track window opens, not only at its next 15-tick read.
        {
            auto w = makeWindow();
            w->addTracks ({ makeWav ("editor-dirty.wav", 24000).getFullPathName() });
            juce::String error;
            test::check (w->session.addPlugin (0, "Test Gain", -1, error), "a track plugin for the editor");
            w->refreshNow();
            w->openEditor (0, 0);
            test::check (w->openEditors() == 1, "its window is open");
            w->session.dirty = false;
            if (auto* gain = dynamic_cast<test::GainPlugin*> (w->session.tracks[0]->chain[0]->instance.get()))
                gain->setCurrentProgram (1);                       // what a knob in that window does: the plugin's state changes
            else
                test::check (false, "the track plugin is a GainPlugin");
            w->closeEditors();
            juce::MessageManager::getInstance()->runDispatchLoopUntil (120);   // one tick, well before the periodic read's fifteen
            test::check (w->session.dirty, "closing that window at once marks the project dirty, so a quit asks and the autosave runs");
        }

        // REC waits for a render (finding 5): its set-up waits for the session's lock three times, which the render holds for its whole run.
        {
            auto w = makeWindow();
            w->addEmptyTrack ();
            juce::String error;
            test::check (w->session.setTrackArm (0, true, error), "a track armed");
            w->setDeviceForTest (fakeDevice ());
            w->session.renderBusy = true;
            gErrors.clear();
            test::check (! w->startRecording() && ! w->isRecording() && gErrors.size() == 1 && gErrors.back().contains ("A render is running"),
                         "REC during a render is refused with the reason, and nothing is prepared");
            w->session.renderBusy = false;
        }

        // A device change during a render does not freeze this thread (finding 3): the start is kept, the outputs stay held silent while it
        // waits, and the timer lands it once the render lets the lock go.
        {
            auto w = makeWindow();
            w->addTracks ({ makeWav ("device-render.wav", 48000).getFullPathName() });
            w->setDeviceForTest (fakeDevice (48000.0, 0, 0));
            w->togglePlay();
            std::atomic<bool> held { false }, release { false };
            std::thread holder ([&]
            {
                w->session.renderBusy = true;                    // what a render does: the flag, then the lock for its whole run
                const juce::ScopedLock sl (w->session.lock);
                held = true;
                while (! release)
                    std::this_thread::sleep_for (std::chrono::milliseconds (2));
                w->session.renderBusy = false;
            });
            while (! held)
                std::this_thread::sleep_for (std::chrono::milliseconds (1));

            DummyAudioDevice dev (44100.0, 256);
            std::thread worker ([&] { w->io.audioDeviceAboutToStart (&dev); });   // the device's own thread
            worker.join();
            const auto t0 = juce::Time::getMillisecondCounterHiRes();
            juce::MessageManager::getInstance()->runDispatchLoopUntil (300);   // the deferred start runs here, and the timer tries it too
            const double took = juce::Time::getMillisecondCounterHiRes() - t0;
            test::check (took < 1000.0, test::fmt ("the message thread never waits for the render's lock: %d ms of dispatch", (int) took));
            test::check (w->deviceRate == 48000.0 && w->session.sampleRate == 48000.0, "the new rate is not adopted while the render holds the lock");
            const juce::int64 before = w->position();
            std::vector<std::vector<float>> in (4, std::vector<float> (256, 0.0f));
            std::vector<std::vector<float>> out (2, std::vector<float> (256, 0.0f));
            const float* inPtr[4] = { in[0].data(), in[1].data(), in[2].data(), in[3].data() };
            float* outPtr[2] = { out[0].data(), out[1].data() };
            w->io.deviceBlock (inPtr, 4, outPtr, 2, 256);
            test::check (w->position() == before, "the silent-output guard is still set while the start waits: the block ran nothing");
            release = true;
            holder.join();
            juce::MessageManager::getInstance()->runDispatchLoopUntil (200);   // the next tick applies what was kept
            test::check (w->deviceRate == 44100.0 && w->session.sampleRate == 44100.0, "once the render lets go, the timer adopts the new rate");
            w->io.deviceBlock (inPtr, 4, outPtr, 2, 256);
            test::check (w->position() > before, "and the transport runs again");
        }

        // Two device starts that come quickly, the first one from the device's thread and applied late (review finding 1 of the round after):
        // the late, stale call must not throw away the newer start that is kept for the timer, nor leave the silent-output guard set.
        {
            auto w = makeWindow();
            w->addTracks ({ makeWav ("device-stale.wav", 48000).getFullPathName() });
            w->setDeviceForTest (fakeDevice (48000.0, 0, 0));
            w->togglePlay();
            std::atomic<bool> held { false }, release { false };
            std::thread holder ([&]
            {
                w->session.renderBusy = true;                    // what a render does: the flag, then the lock for its whole run
                const juce::ScopedLock sl (w->session.lock);
                held = true;
                while (! release)
                    std::this_thread::sleep_for (std::chrono::milliseconds (2));
                w->session.renderBusy = false;
            });
            while (! held)
                std::this_thread::sleep_for (std::chrono::milliseconds (1));

            DummyAudioDevice at96000 (96000.0, 256), at44100 (44100.0, 256);
            std::thread worker ([&] { w->io.audioDeviceAboutToStart (&at96000); });   // the device's own thread: its call is only queued
            worker.join();
            w->io.audioDeviceAboutToStart (&at44100);   // this thread applies it at once: it finds the render and keeps it for the timer
            juce::MessageManager::getInstance()->runDispatchLoopUntil (150);   // the first call runs here, and is stale by then
            test::check (w->deviceRate == 48000.0 && w->session.sampleRate == 48000.0, "the render still holds the lock: no rate is adopted");
            release = true;
            holder.join();
            juce::MessageManager::getInstance()->runDispatchLoopUntil (300);   // the next tick applies what was kept
            test::check (w->deviceRate == 44100.0 && w->session.sampleRate == 44100.0,
                         test::fmt ("the newer start survived the stale one, which left what it did not defer alone: the device's rate is %d Hz",
                                    (int) w->deviceRate));
            const juce::int64 before = w->position();
            std::vector<std::vector<float>> in (4, std::vector<float> (256, 0.0f));
            std::vector<std::vector<float>> out (2, std::vector<float> (256, 0.0f));
            const float* inPtr[4] = { in[0].data(), in[1].data(), in[2].data(), in[3].data() };
            float* outPtr[2] = { out[0].data(), out[1].data() };
            w->io.deviceBlock (inPtr, 4, outPtr, 2, 256);
            test::check (w->position() > before, "and the silent-output guard went with the start it waited for, so the transport runs again");
        }

        // A seek that lands inside a block stands (finding 4): the callback advances only from the position it loaded, so the block already
        // built cannot swallow the playhead the message thread put there.
        {
            // A plugin that moves the playhead the first time the audio path runs a block: the store lands inside getNextAudioBlock, between
            // the position it read at its top and the one it writes at its end, which is where a seek from this thread lands in practice.
            struct SeekOncePlugin : test::GainPlugin
            {
                std::function<void()> onFirstBlock;
                explicit SeekOncePlugin (std::function<void()> cb) : test::GainPlugin ("Seek Once", 0), onFirstBlock (std::move (cb)) {}
                void processBlock (juce::AudioBuffer<float>& b, juce::MidiBuffer& m) override
                {
                    test::GainPlugin::processBlock (b, m);
                    if (onFirstBlock) { const auto move = onFirstBlock; onFirstBlock = nullptr; move(); }
                }
            };
            auto w = makeWindow();
            w->addTracks ({ makeWav ("seek-mid.wav", 48000).getFullPathName() });
            auto* window = w.get();
            w->session.catalog.addInternal (test::GainPlugin::description ("Seek Once"), [window] (double, int)
            {
                return std::make_unique<SeekOncePlugin> ([window] { window->seek (30000); });
            });
            juce::String error;
            test::check (w->session.setChain (0, { "Seek Once" }, error), "a plugin that moves the playhead inside its first block");
            w->seek (0);
            w->togglePlay();
            w->syncTransport();
            juce::AudioBuffer<float> block (2, 4800);
            w->renderAudioBlock (block);
            test::check (w->position() == 30000, "the seek that landed inside the block survives it: " + juce::String ((int) w->position()));

            // and the rate's scaled playhead still goes in the other direction: a device start on this thread converts the session and puts
            // the position where the same moment of the new rate is
            auto s = makeWindow();
            s->addTracks ({ makeWav ("scale-mid.wav", 48000).getFullPathName() });
            s->setDeviceForTest (fakeDevice (48000.0, 0, 0));
            s->seek (24000);
            DummyAudioDevice at44100 (44100.0, 512);
            s->io.audioDeviceAboutToStart (&at44100);
            test::check (s->session.sampleRate == 44100.0 && s->position() == 22050,
                         "the playhead keeps its place in seconds at the device's rate: " + juce::String ((int) s->position()));
        }

        // The caps agree (finding 9): the IN menu stops at what the recorder meters and records, which is what the AUDIO dialog offers.
        {
            MainComponent::DeviceState many = fakeDevice (48000.0, 0, 0);
            many.inputs = 70;
            many.activeInputs.clear();
            many.activeInputs.setRange (0, 70, true);
            const auto choices = MainComponent::inputChoices (many);
            bool within = ! choices.empty();
            for (const auto& c : choices)
                within = within && c.left < Recorder::kMaxInputs && (c.right < 0 || c.right < Recorder::kMaxInputs);
            test::check (within && choices.back().label == "In " + juce::String (Recorder::kMaxInputs),
                         test::fmt ("the IN menu stops at the recorder's %d inputs (%d entries, the last %s)",
                                    Recorder::kMaxInputs, (int) choices.size(), choices.empty() ? "" : choices.back().label.toRawUTF8()));
        }

        // A null output channel (JUCE leaves those pointers unspecified; CoreAudio and ALSA compact them) is never walked: the transport runs
        // into the silent scratch instead (finding 10). The channels the device does use are cleared, so its last block is not heard again.
        {
            auto w = makeWindow();
            w->addTracks ({ makeWav ("null-out.wav", 48000).getFullPathName() });
            w->setDeviceForTest (fakeDevice (48000.0, 0, 0));
            w->togglePlay();
            std::vector<std::vector<float>> in (4, std::vector<float> (256, 0.0f));
            std::vector<float> out0 (256, 0.0f);
            const float* inPtr[4] = { in[0].data(), in[1].data(), in[2].data(), in[3].data() };
            float* outPtr[2] = { out0.data(), nullptr };
            const juce::int64 before = w->position();
            w->io.deviceBlock (inPtr, 4, outPtr, 2, 256);
            test::check (w->position() == before + 256, "a device block whose second output channel is null still advances the transport");
            for (float& v : out0)
                v = 1.0f;                                     // what the block before left in the channel the device does use
            w->io.deviceBlock (inPtr, 4, outPtr, 2, 256);
            bool silent = true;
            for (float v : out0)
                silent = silent && v == 0.0f;
            test::check (silent, "and the channel that is not null is cleared, not left to be heard again from the block before");
        }

        // The notes (finding 6): while the lock is busy the window shows what it read under the lock, and never reads the session unlocked.
        {
            auto w = makeWindow();
            std::atomic<bool> held { false }, release { false };
            std::thread holder ([&]
            {
                const juce::ScopedLock sl (w->session.lock);
                held = true;
                while (! release)
                    std::this_thread::sleep_for (std::chrono::milliseconds (2));
            });
            while (! held)
                std::this_thread::sleep_for (std::chrono::milliseconds (1));
            const auto during = w->notes();
            release = true;
            holder.join();
            test::check (during.empty(), "with the lock held and nothing read under it yet, notes() gives nothing rather than an unlocked read");
            test::check (! w->notes().empty(), "and once the lock is free it reads them normally");
        }
    }

    // [43] Arrangement guards: a take refuses a seek at the view (no error box per ruler-drag event); the horizontal scroll stays on the
    // content (the overview strip cannot desync by overscroll); a marker's hit area is no wider than the name is drawn.
    {
        auto w = makeWindow();
        w->addTracks ({ makeWav ("arrange-guards.wav", 48000).getFullPathName() });
        w->editor.setSnap (false);
        w->arrange.recordingFrom = 0.0;   // a take runs
        w->arrange.playhead = 12000.0;
        const auto now = juce::Time::getCurrentTime();
        const juce::Point<int> ruler { ArrangeView::kHeaderWidth + 200, 4 };
        w->arrange.mouseDown (juce::MouseEvent (juce::Desktop::getInstance().getMainMouseSource(), ruler.toFloat(),
                                                juce::ModifierKeys {}, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f,
                                                &w->arrange, &w->arrange, now, ruler.toFloat(), now, 1, false));
        w->arrange.mouseUp (juce::MouseEvent (juce::Desktop::getInstance().getMainMouseSource(), ruler.toFloat(),
                                              juce::ModifierKeys {}, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f,
                                              &w->arrange, &w->arrange, now, ruler.toFloat(), now, 1, false));
        test::check (std::abs (w->arrange.playhead - 12000.0) < 1e-9,
                     "a ruler press during a take does not move the view's playhead (the transport would refuse it with an error box per event)");
        w->arrange.recordingFrom = -1.0;

        // overscroll: zoom in so the content is wider than the view, then wheel far past the end
        w->arrange.zoom = 4000.0;
        w->arrange.scrollTo (0);
        const double pps = 4000.0 / 48000.0;
        const double span = (double) juce::jmax (1, w->arrange.getWidth() - ArrangeView::kHeaderWidth) / pps;
        const double bound = juce::jmax (0.0, 48000.0 - span);
        const juce::Point<int> lane { ArrangeView::kHeaderWidth + 100, ArrangeView::kRulerHeight + ArrangeView::kLoopHeight + ArrangeView::kMarkerHeight + 60 };
        for (int i = 0; i < 6; ++i)
        {
            juce::MouseWheelDetails wheel;
            wheel.deltaX = -10.0f;
            wheel.deltaY = 0.0f;
            w->arrange.mouseWheelMove (juce::MouseEvent (juce::Desktop::getInstance().getMainMouseSource(), lane.toFloat(),
                                                         juce::ModifierKeys {}, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f,
                                                         &w->arrange, &w->arrange, now, lane.toFloat(), now, 1, false), wheel);
        }
        test::check (std::abs (w->arrange.scrollSamplesForTest() - bound) < 1.0,
                     "wheeling past the end stops at the content's edge, so the overview thumb cannot pin right while the lanes scroll into blank space");

        // marker hit area: a long name is drawn in 160 px; a click 200 px past the flag is not on it
        w->editor.addMarker ("A very long marker name that goes well past one hundred and sixty pixels of drawn text", 0);
        w->arrange.scrollTo (0);
        w->arrange.zoom = ArrangeView::kDefaultZoom;
        w->refreshNow();
        const int markerY = ArrangeView::kRulerHeight + ArrangeView::kLoopHeight + 4;
        test::check (w->arrange.markerAtForTest ({ ArrangeView::kHeaderWidth + 10, markerY }) == 0
                         && w->arrange.markerAtForTest ({ ArrangeView::kHeaderWidth + 200, markerY }) == -1,
                     "a marker is grabbable where its name is drawn, not over the blank lane past it");
    }

    // [44] The click with no audio: an empty track plays while the metronome is on (the transport's copy follows the switch), the click is
    // heard, and turning it off stops the playback at the empty audio's end. With audio, the click does not move the end.
    {
        const auto dir44 = tempFolder().getChildFile ("click-only");
        dir44.deleteRecursively();
        dir44.createDirectory();
        auto w = makeWindow (true, dir44.getChildFile ("settings.xml"));
        w->session.addEmptyTrack ("Click");
        press (w->metronomeButton);
        test::check (w->session.metronomeOn, "the click is on");
        w->togglePlay();
        test::check (w->isPlaying(), "an empty track plays with the click on");
        juce::AudioBuffer<float> block (2, 512);
        w->renderAudioBlock (block);
        test::check (w->isPlaying() && w->position() == 512, "the click-only block advances the playhead and keeps playing");
        test::check (block.getMagnitude (0, 0, 512) > 0.1f, "and the click is heard in it");
        w->renderAudioBlock (block);
        test::check (w->isPlaying() && w->position() == 1024, "a second block runs on past where the audio would end");
        press (w->metronomeButton);
        test::check (! w->session.metronomeOn, "the click is off");
        w->renderAudioBlock (block);
        test::check (! w->isPlaying(), "with the click off, the empty audio's end stops the playback");
        w->togglePlay();
        test::check (! w->isPlaying(), "and playing an empty session with no click is refused");
    }
    {
        const auto dir44b = tempFolder().getChildFile ("click-end");
        dir44b.deleteRecursively();
        dir44b.createDirectory();
        auto w = makeWindow (true, dir44b.getChildFile ("settings.xml"));
        w->addTracks ({ makeWav ("click-end.wav").getFullPathName() });
        press (w->metronomeButton);
        const auto end = w->session.length() + w->session.maxLatency();
        w->seek (end - 100);
        w->togglePlay();
        juce::AudioBuffer<float> block (2, 512);
        w->renderAudioBlock (block);
        test::check (! w->isPlaying() && w->position() == end, "with audio and the click on, playing still stops at the end of the audio");
    }

    // [45] A device with no outputs: a block bigger than the prepared scratch still advances the playhead by all of it (in scratch-sized
    // chunks), and the take's start and frames follow; a null output channel on an oversized block advances too, its live channel cleared;
    // the scratch-miss path advances only a playing transport.
    {
        const auto dir45 = tempFolder().getChildFile ("no-out-big");
        dir45.deleteRecursively();
        dir45.createDirectory();
        auto w = makeWindow (true, dir45.getChildFile ("settings.xml"));
        juce::String error;
        w->session.addEmptyTrack ("Mic");
        test::check (w->session.setTrackArm (0, true, error) && w->session.setTrackInput (0, 0, -1, error), "a track armed on input 1");
        w->setDeviceForTest (fakeDevice (48000.0, 0, 0));   // the scratch is 256 samples
        w->seek (0);
        test::check (w->startRecording(), "REC starts");
        std::vector<float> in (1000, 0.25f);
        const float* inPtr[1] = { in.data() };
        w->io.deviceBlock (inPtr, 1, nullptr, 0, 1000);      // four times the scratch
        test::check (w->position() == 1000, "a no-output block bigger than the scratch advances the playhead by all of it: " + juce::String ((int) w->position()));
        w->stopRecording();
        const auto& clips = w->session.tracks[0]->clips;
        test::check (clips.size() == 1 && clips[0].start == 0 && clips[0].length == 1000,
                     "and the take holds every frame of it, from where the playhead was");
    }
    {
        auto w = makeWindow();
        w->addTracks ({ makeWav ("null-big.wav", 48000).getFullPathName() });
        w->setDeviceForTest (fakeDevice (48000.0, 0, 0));
        w->togglePlay();
        std::vector<float> in (1000, 0.0f), out0 (1000, 1.0f);
        const float* inPtr[1] = { in.data() };
        float* outPtr[2] = { out0.data(), nullptr };
        const juce::int64 before = w->position();
        w->io.deviceBlock (inPtr, 1, outPtr, 2, 1000);
        test::check (w->position() == before + 1000, "a null output channel on an oversized block still advances the transport");
        bool silent = true;
        for (float v : out0)
            silent = silent && v == 0.0f;
        test::check (silent, "and the channel that is not null is cleared");
    }
    {
        // the scratch-miss path, forced without contention: no device was set, so the scratch is empty and every chunk misses it. A
        // stopped transport does not advance the playhead there (as in the hit branch), a playing one still does.
        auto w = makeWindow();
        w->addTracks ({ makeWav ("miss-path.wav").getFullPathName() });
        std::vector<float> in (1000, 0.0f);
        const float* inPtr[1] = { in.data() };
        const juce::int64 before = w->position();
        w->io.deviceBlock (inPtr, 1, nullptr, 0, 1000);
        test::check (w->position() == before, "a stopped transport does not advance on the scratch-miss path");
        w->togglePlay();
        w->io.deviceBlock (inPtr, 1, nullptr, 0, 1000);
        test::check (w->position() == before + 1000, "a playing transport still advances on it");
    }

    // [46] A device start while a take runs: the blocks the silent-output guard holds are neither written nor lost uncounted — they are
    // counted as skipped, and the stop the start lands reports them with their length. The same while the take's files are still opening
    // (recording not up yet): the two guards together still count every held block.
    {
        const auto dir46 = tempFolder().getChildFile ("start-skips");
        dir46.deleteRecursively();
        dir46.createDirectory();
        auto w = makeWindow (true, dir46.getChildFile ("settings.xml"));
        juce::String error;
        w->session.addEmptyTrack ("Mic");
        test::check (w->session.setTrackArm (0, true, error) && w->session.setTrackInput (0, 0, -1, error), "a track armed on input 1");
        w->setDeviceForTest (fakeDevice (48000.0, 0, 0));
        test::check (w->startRecording(), "REC starts");
        std::vector<float> in (256, 0.25f), out0 (256, 0.0f), out1 (256, 0.0f);
        const float* inPtr[1] = { in.data() };
        float* outPtr[2] = { out0.data(), out1.data() };
        w->io.deviceBlock (inPtr, 1, outPtr, 2, 256);   // one block written
        // the device's own thread announces a change: the silent guard goes up, the message thread has not applied the start yet
        std::thread worker ([&] { w->io.audioDeviceAboutToStart (nullptr); });
        worker.join();
        test::check (w->isRecording(), "the take still runs while the start waits for the message thread");
        w->io.deviceBlock (inPtr, 1, outPtr, 2, 256);
        w->io.deviceBlock (inPtr, 1, outPtr, 2, 256);   // two blocks the guard holds: not written, counted
        test::check (w->recorderForChecks().samplesSkippedWhileOpening() == 512,
                     "the frames the silent guard held are counted as skipped: " + juce::String ((int) w->recorderForChecks().samplesSkippedWhileOpening()));
        gErrors.clear();
        juce::MessageManager::getInstance()->runDispatchLoopUntil (200);   // the start lands and ends the take
        test::check (! w->isRecording(), "the device change ended the take");
        const auto& clips = w->session.tracks[0]->clips;
        test::check (clips.size() == 1 && clips[0].length == 256, "the take keeps what was written before the change");
        test::check (! gErrors.empty() && gErrors.back().contains ("512 samples") && gErrors.back().contains ("were not written before the take could run"),
                     "and the stop reports the held frames with their length: " + (gErrors.empty() ? juce::String ("<nothing>") : gErrors.back()));
    }
    {
        // a device start while the take's files are still opening: recording is not up yet (Recorder::start sets it only after the files
        // open), so the silent guard must count the held blocks itself, not leave them to the opening guard its early return hides
        const auto dir46b = tempFolder().getChildFile ("start-skips-opening");
        dir46b.deleteRecursively();
        dir46b.createDirectory();
        auto w = makeWindow (true, dir46b.getChildFile ("settings.xml"));
        juce::String error;
        w->session.addEmptyTrack ("Mic");
        test::check (w->session.setTrackArm (0, true, error) && w->session.setTrackInput (0, 0, -1, error), "a track armed on input 1");
        w->setDeviceForTest (fakeDevice (48000.0, 0, 0));
        w->setFilesOpeningForChecks (true);   // the files are still opening: no take runs yet
        std::thread worker ([&] { w->io.audioDeviceAboutToStart (nullptr); });
        worker.join();
        test::check (! w->isRecording(), "no take runs yet while the files open");
        std::vector<float> in (256, 0.25f), out0 (256, 0.0f), out1 (256, 0.0f);
        const float* inPtr[1] = { in.data() };
        float* outPtr[2] = { out0.data(), out1.data() };
        w->io.deviceBlock (inPtr, 1, outPtr, 2, 256);
        w->io.deviceBlock (inPtr, 1, outPtr, 2, 256);   // two blocks both guards hold: counted, not dropped between them
        test::check (w->recorderForChecks().samplesSkippedWhileOpening() == 512,
                     "the frames held while the files open and the device starts are counted as skipped: " + juce::String ((int) w->recorderForChecks().samplesSkippedWhileOpening()));
        w->setFilesOpeningForChecks (false);
        juce::MessageManager::getInstance()->runDispatchLoopUntil (200);   // the start lands (no take to end)
    }

    tempFolder().deleteRecursively();
    return test::finish ("OlympusUiCheck");
}
