// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 The Pantheon authors

#pragma once

#include "Advice.h"
#include "Arrange.h"
#include "LampButton.h"
#include "PianoRoll.h"
#include "../Edit.h"
#include "../Record.h"
#include "LevelMeter.h"
#include "Session.h"
#include "Settings.h"

#include <juce_audio_utils/juce_audio_utils.h>

#include <cstdint>
#include <map>
#include <optional>

#include <pantheon/ui/Advice.h>
#include <pantheon/ui/Aspect.h>
#include <pantheon/ui/HardwareLookAndFeel.h>

namespace olympus::gui
{
class MainComponent;

// A plugin switch: click opens it, shift-click bypasses it, right-click (or ctrl-click) for the menu. Kept for the chains' buttons; the
// window's FX menu lists a track's plugins instead.
class SlotButton : public LampButton
{
public:
    std::function<void()> onMenu;
    void mouseDown (const juce::MouseEvent& e) override { if (e.mods.isPopupMenu()) { if (onMenu) onMenu(); return; } LampButton::mouseDown (e); }
    void mouseUp (const juce::MouseEvent& e) override { if (! e.mods.isPopupMenu()) LampButton::mouseUp (e); }
};

// A plugin of the monitor chain on the MONITOR strip: click opens it, shift-click bypasses it, right-click (or ctrl-click) is its menu. A plugin
// that is not installed is drawn with its name in red.
class MonitorSlotButton : public SlotButton
{
public:
    std::function<void()> onShiftClick;
    bool missing = false;
    void mouseDown (const juce::MouseEvent& e) override
    {
        if (e.mods.isShiftDown() && ! e.mods.isPopupMenu()) { shiftDown = true; return; }
        SlotButton::mouseDown (e);
    }
    void mouseUp (const juce::MouseEvent& e) override
    {
        if (shiftDown)
        {
            shiftDown = false;
            if (contains (e.getPosition()) && onShiftClick) onShiftClick();
            return;
        }
        SlotButton::mouseUp (e);
    }
    void paintButton (juce::Graphics& g, bool highlighted, bool down) override
    {
        ui::hw::drawPushButton (g, getLocalBounds().toFloat(), getToggleState(), lamp, down, highlighted, isEnabled());
        g.setFont (ui::labelFont (10.0f));
        g.setColour (missing ? colours::bad : (getToggleState() && lamp.getPerceivedBrightness() > 0.5f ? colours::lampInk : colours::text));
        g.drawFittedText (getButtonText().toUpperCase(), getLocalBounds().reduced (8, 0).translated (0, down ? 1 : 0), juce::Justification::centred, 1);
    }

private:
    bool shiftDown = false;
};

// The CLIP lamp of the master section: lit red once the master has reached 0 dBFS, until it is clicked (or a new project).
class ClipLamp : public juce::Button
{
public:
    ClipLamp() : juce::Button ("Clip") {}
    bool lit = false;
    void paintButton (juce::Graphics& g, bool, bool) override
    {
        ui::hw::drawLamp (g, { 7.0f, (float) getHeight() * 0.5f }, 4.0f, colours::bad, lit);
        g.setFont (ui::labelFont (10.0f));
        g.setColour (lit ? colours::bad : colours::dim);
        g.drawText ("CLIP", getLocalBounds().withTrimmedLeft (16), juce::Justification::centredLeft);
    }
};

// The audio device's settings dialog: a window that stays open beside the arrangement (no modal loop).
class AudioDialog : public juce::DialogWindow
{
public:
    explicit AudioDialog (std::unique_ptr<juce::Component> content, bool addToDesktop = true)
        : juce::DialogWindow ("Audio devices", colours::background, true, addToDesktop)
    {
        setUsingNativeTitleBar (true);
        if (content != nullptr)
            content->setSize (540, 440);
        setContentOwned (content.release(), true);
        setResizable (true, true);
        setResizeLimits (420, 320, 1200, 1000);
    }
    void closeButtonPressed() override { setVisible (false); }
};

class PluginWindow : public juce::DocumentWindow
{
public:
    PluginWindow (const juce::String& title, juce::AudioProcessorEditor* editor, std::function<void()> closed,
                  bool addToDesktop = true, juce::Component* centreAround = nullptr);
    ~PluginWindow() override;
    void closeButtonPressed() override { if (onClosed) onClosed(); }
    void childBoundsChanged (juce::Component* child) override;
    void moved() override;
    void clearPendingInitialCenter() { pendingInitialCenter = false; }
    std::function<void()> onClosed;

private:
    struct FrameConstrainer;
    void centreNow();

    std::unique_ptr<FrameConstrainer> frameConstrainer;
    juce::Component::SafePointer<juce::Component> centerTarget;
    bool pendingInitialCenter = false;
    bool centeringNow = false;
};

// The whole window: the project buttons and the transport along the top, the arrangement (ArrangeView) in the body, and the notes at the
// foot. It owns the session, the editor that changes it, and the audio device.
class MainComponent : public juce::Component, public juce::FileDragAndDropTarget, private juce::Timer, private juce::AsyncUpdater,
                      private juce::ChangeListener
{
public:
    static constexpr int kDesignWidth = 1280, kDesignHeight = ui::editorHeightFor (kDesignWidth);

    struct Options
    {
        bool audioDevice = true;           // false for the checks and the snapshots
        bool scanPlugins = true;           // false when the caller fills the catalog
        bool includeSystemPlugins = true;  // false in headless checks so a test scan looks only in pluginFolders
        juce::File settingsFile = Settings::defaultFile();
        juce::File recordings;             // where takes go when set (the checks); else the project's folder, or ~/Music
        // the plugins the catalog holds, filled before the monitor chain is read (the checks; the app scans instead)
        std::function<void (Catalog&)> fillCatalog;
    };

    // The audio device as the window last read it (on the message thread): what the summary, REC and the notes need.
    struct DeviceState
    {
        bool running = false;                      // the device runs and calls back
        juce::String inputName, outputName;
        double rate = 0.0;
        int bufferSize = 0;
        int inputs = 0, outputs = 0;               // the active channels
        int inputLatency = 0, outputLatency = 0;   // samples
        juce::BigInteger activeInputs;             // which of the device's inputs are active (absolute numbers)
        juce::StringArray inputNames;              // the device's name for each of its inputs (absolute numbers), may be empty
    };

    // One entry of the IN menu: a device input alone (right -1) or a pair of neighbouring inputs.
    struct InputChoice
    {
        juce::String label;
        int left = 0, right = -1;
    };

    // The audio path of one device block, called by the device (audioDeviceIOCallbackWithContext) and by the checks, which pass buffers of
    // their own. The inputs go to the recorder first (its meters always, its takes while one runs); the transport then fills the outputs.
    class DeviceCallback : public juce::AudioIODeviceCallback
    {
    public:
        explicit DeviceCallback (MainComponent& m) : owner (m) {}
        void deviceBlock (const float* const* in, int numIn, float* const* out, int numOut, int n);
        void audioDeviceIOCallbackWithContext (const float* const* inputChannelData, int numInputChannels, float* const* outputChannelData,
                                               int numOutputChannels, int numSamples, const juce::AudioIODeviceCallbackContext&) override
        {
            deviceBlock (inputChannelData, numInputChannels, outputChannelData, numOutputChannels, numSamples);
        }
        void audioDeviceAboutToStart (juce::AudioIODevice* device) override;
        void audioDeviceStopped() override;
        // Message thread, before the callbacks run: the scratch a device with no outputs runs the transport into, for blocks of `samples`.
        void prepareScratch (int samples)
        {
            const juce::SpinLock::ScopedLockType sl (scratchLock);
            scratch.setSize (2, juce::jmax (1, samples), false, true, false);
        }

    private:
        MainComponent& owner;
        juce::SpinLock scratchLock;
        juce::AudioBuffer<float> scratch;   // silent, never heard: the transport's output on a device with no outputs
    };

    MainComponent();
    explicit MainComponent (Options options);
    ~MainComponent() override;

    Session session;
    Settings settings;
    Editor editor { session };          // every timeline edit, and the undo of it, goes through here

    // ---- what the buttons do (public so the checks can press them) ----------------------------------------------------------------
    void newProject();
    bool openProject (const juce::File& file);
    void openProjectRequested (const juce::File& file);
    bool saveProject();
    // `done` (optional) gets whether the file was written; it runs after the chooser, which is asynchronous.
    void saveProjectAs (std::function<void (bool)> done = {});
    bool saveTo (const juce::File& file);
    // Autosave (every kAutosaveInterval seconds while the project has changes): the project is written beside the project file as
    // `<name>.olympus.autosave`, or as `autosave-untitled.olympus` beside settings.xml, and the session's name, file and dirty flag stay.
    // Returns false when nothing was written (no changes, or a render is running). A test calls it directly.
    static constexpr double kAutosaveInterval = 120.0;   // seconds
    bool autosaveNow();
    juce::File autosaveFile() const;
    // At start, after the last project: unsaved work of the last session (an autosave newer than its project, or an untitled one) is offered;
    // "Open it" loads it as the session with changes, its project file kept, and "Ignore" deletes it. Returns true when an autosave was found.
    bool offerRecovery();
    // Notices when the project file on disk was saved outside this window (by the MCP server, say): reloads it at once when this window has
    // no unsaved changes, or asks (`askReload`) when it has. Returns true when the project was reloaded. Called by the timer, and by checks.
    bool checkExternalProjectChange();
    // One new track per file, its clip at sample 0 (the checks' way in; the buttons use importFiles at the playhead).
    void addTracks (const juce::StringArray& files);
    // Audio files as clips: with `track` -1 each file is a new track named after it, else the files go on that track one after another.
    // Every file starts at `at`, or after the previous clip on the track. Errors are shown together.
    void importFiles (const juce::StringArray& files, int track, juce::int64 at);
    void addEmptyTrack();
    void addMidiTrack();
    void undoEdit();
    void redoEdit();
    void toggleLoop();
    void toggleMetronome();
    void zoomBy (double factor);
    void removeTrack (int index);
    // Writes the stems and the mix. Returns the reason when it fails, an empty string when it worked.
    // Called on the message thread (the checks) it closes the tracks' plugin windows first; the progress window's worker passes
    // editorsClosed = true, after chooseAndRender closed them on the message thread.
    // `options` says what is written (its folder is `folder`); `result` gets what was written, and its `stopped` says a cancel (no message).
    juce::String renderTo (const juce::File& folder, const std::function<bool (float)>& progress = {}, bool editorsClosed = false,
                           RenderOptions options = {}, RenderResult* result = nullptr);
    // After a render, as the window shows it: a cancel is quiet, a failure is shown, a success is notified and its folder revealed.
    void finishRender (const juce::File& folder, const RenderResult& result, const juce::String& error);
    void togglePlay();
    void rewind();                                  // to the loop's start when the loop is on, else to 0
    void jumpToMarker (bool forward);               // , and . : the marker before or after the playhead
    void seek (juce::int64 sample);
    juce::int64 position() const { return source.position.load(); }
    bool isPlaying() const { return source.playing.load(); }
    // Copies what the audio thread needs (the loop, the click, the end of the audio) from the session. Message thread; the timer does it too.
    void syncTransport();
    // What the audio callback does for one block: the same code, so the checks can drive the loop and the end of the audio.
    void renderAudioBlock (juce::AudioBuffer<float>& buffer) { juce::AudioSourceChannelInfo info (&buffer, 0, buffer.getNumSamples()); source.getNextAudioBlock (info); }
    void scanPlugins (bool rescan = false, bool suiteOnlyScan = false);
    juce::File defaultPluginFolder() const;
    bool addPluginFolder (const juce::File& folder);
    bool isScanQueued() const { return scanQueued; }
    bool isScanning() const { return scanning; }
    // Opens the editor of a plugin: track `track`'s chain slot `slot`, or with track -1 a plugin of the monitor chain. One window per plugin.
    void openEditor (int track, int slot);
    int openEditors() const { return (int) windows.size(); }
    void closeEditors();
    // Closes the editors of the tracks' plugins, not those of the monitor chain (which stay open across NEW and OPEN).
    void closeTrackEditors();
    // Brings the arrangement, the transport and the title up to date now.
    void refreshNow();
    // The same, after the current event: for a change made from inside a control that the change may remove.
    void requestRefresh() { triggerAsyncUpdate(); }

    // ---- recording (Record.h) ------------------------------------------------------------------------------------------------------
    // REC (or R): starts the takes of the armed tracks from the playhead, with playback; again stops them and places the takes.
    bool isRecording() const { return recorder.isRecording(); }
    bool startRecording();
    void toggleRecord();
    // Ends the takes: the clips go on their tracks as one undo step. `problem` is shown after (a device that stopped, say).
    void stopRecording (const juce::String& problem = {});
    // Where the takes of this project go: "<project> Recordings" beside the project file, else a folder in ~/Music (Options::recordings wins).
    juce::File recordingFolder() const;

    // ---- the audio device ----------------------------------------------------------------------------------------------------------
    // The IN menu's entries for a device: each active input alone, then each pair of neighbouring active inputs.
    static std::vector<InputChoice> inputChoices (const DeviceState& state);
    // The top bar's text: "IN: <device> / OUT: <device> / 48.0 kHz / 256 smp", or "no audio device".
    juce::String deviceSummary() const;
    // The device's round trip (input and output latency) in ms, 0 when there is no device.
    double roundTripMs() const;
    void showInputMenu (int track);
    void showAudioSettings();
    // Checks and the snapshot: the device the window shows, without a device. Prepares the recorder as a real device start would.
    void setDeviceForTest (const DeviceState& state);
    DeviceCallback io { *this };        // the device's callback (the checks call its deviceBlock)
    DeviceState device;
    double deviceRate = 0.0;            // the device's rate while it runs, else 0

    // Runs `proceed` now if nothing would be lost, else after the user has chosen: Save (a project without a file gets the Save As chooser,
    // and `proceed` runs only once the file is written), Don't save, or Cancel (`proceed` does not run). A take that runs is placed first.
    void askToDiscard (const juce::String& action, std::function<void()> proceed);
    // True (and shows the reason when a render runs) when NEW, OPEN or Quit must wait: a render is running or a dialog is open.
    bool refuseWhileBusy();
    bool isDialogOpen() const { return dialogOpen; }
    // Quit with the changes dropped: deletes this session's autosave (the action that drops it goes through only after the question).
    void dropAutosave() { deleteAutosaves(); }
    // The questions, replaceable so the checks answer them without a dialog (the defaults show the real ones).
    enum class Unsaved { cancel, save, discard };
    std::function<Unsaved (const juce::String&)> askUnsaved;
    std::function<void (const juce::String& title, const juce::File& start, std::function<void (const juce::File&)> done)> chooseSaveFile;
    std::function<void (const juce::File& start, std::function<void (const juce::StringArray&)> done)> chooseImportFiles;
    // Before the render's folder chooser: the bit depth and the mix-only choice in `options` (false when cancelled). Remembered in settings.
    std::function<bool (RenderOptions&)> chooseRenderOptions;
    // Folder chooser for render/export (replaceable so the checks can supply a folder without a native dialog).
    std::function<void (const juce::String& title, const juce::File& start, std::function<void (const juce::File&)> done)> chooseRenderFolder;
    // "Olympus found unsaved work from the last session. Open it?": true to open it.
    std::function<bool (const juce::String&)> askRecover;
    // "The project file was changed on disk (for example by the MCP server). Reload it and lose the unsaved changes in this window?": true to reload.
    std::function<bool (const juce::String&)> askReload;
    // A message that is not a problem (the render's result): a box by default.
    std::function<void (const juce::String&)> notify;
    std::function<void (const juce::File&)> reveal;   // the render's folder, shown in the file manager (replaceable: the checks show nothing)
    std::function<void (const juce::String&)> reportError;
    // What holds a plugin's editor on screen (a PluginWindow by default); replaceable, so the checks open no native window.
    std::function<std::unique_ptr<juce::Component> (const juce::String&, juce::AudioProcessorEditor*, std::function<void()>)> makeEditorWindow;
    juce::String windowTitle() const;
    std::function<void()> onTitleChanged;
    std::vector<ui::Note> notes() const;

    // The menus of a track: its plugins (open, bypass, move, remove) and the plugins to add.
    void showAddPluginMenu (int track);
    void showTrackMenu (int track);

    // ---- the monitor chain (Session::monitor): playback only, saved in monitor.xml, not in the project and not an undo step ----------------
    // Each change is saved at once. The chain is read once, after the catalog is filled (at start, or after the first scan).
    bool addMonitorPlugin (const juce::String& name);           // at the end of the chain
    void removeMonitorPlugin (int index);                       // closes its editor
    void moveMonitorPlugin (int index, int to);
    void setMonitorPluginBypass (int index, bool bypass);
    void setMonitorBypassed (bool bypass);                      // CORRECTION / BYPASS: the whole chain
    void showMonitorMenu (int index);
    void showAddMonitorMenu();
    void refreshMonitorStrip();                                 // the strip's buttons and read-outs, from the session
    bool monitorChainLoaded() const { return monitorLoaded; }
    // Safe start (Settings.h): the last start died loading the chain, so it is skipped and PLUGINS > Load saved monitor chain reads it now.
    bool monitorChainSkipped() const { return monitorSkipped && ! monitorLoaded; }
    void loadSkippedMonitorChain();
    // The last project opens at start, unless the last start died opening it (then a note says so and OPEN opens it).
    void openLastProjectAtStart();
    double monitorLatencyMs() const;                           // the chain's latency in ms at the device's rate (0 when bypassed)
    mutable int monitorLatencyCacheSamples = 0;                // the last latency read with the lock (kept while a render or a busy lock)
    mutable juce::uint64 monitorLatencyCacheRevision = 0;
    mutable std::vector<ui::Note> notesCache;                  // the last notes read with the lock (kept while a render or a busy lock)
    mutable juce::uint64 cachedNotesRevision = 0;
    mutable AdviceDevice cachedNotesAdvice;
    mutable bool cachedNotesSuiteOnly = false;
    mutable int cachedNotesCatalogCount = -1;

    bool keyPressed (const juce::KeyPress& key) override;
    void paint (juce::Graphics&) override;
    void resized() override;
    bool isInterestedInFileDrag (const juce::StringArray& files) override { return ! files.isEmpty(); }
    void filesDropped (const juce::StringArray& files, int, int) override;

    // ---- the controls (public so the checks can find and press them) ---------------------------------------------------------------
    ui::GuideButton guideButton { guideTitle(), guideText() };
    juce::TextButton newButton { "New" }, openButton { "Open" }, saveButton { "Save" }, saveAsButton { "Save as" }, addTrackButton { "Add track" },
        addMidiButton { "+ MIDI" }, importButton { "Import audio" }, pluginsButton { "Plugins" }, renderButton { "Render stems" }, exportButton { "Export" }, audioButton { "Audio" };
    juce::TextButton rewindButton { "|<" }, undoButton { "Undo" }, redoButton { "Redo" }, zoomOutButton { "-" }, zoomInButton { "+" }, zoomFitButton { "Fit" };
    LampButton playButton { "Play" }, recordButton { "Rec" }, loopButton { "Loop" }, metronomeButton { "Click" }, snapButton { "Snap" };
    juce::TextEditor bpmField;                       // the tempo, when the project has one; the tempo at the playhead when it has more
    juce::ComboBox signatureBox, gridBox;
    juce::Label deviceLabel;                         // the device summary, under the buttons
    // the master section, in row C left of the device line: the MASTER fader (one undo step a drag), a stereo meter, the peak read-out and CLIP
    juce::Slider masterFader;
    juce::Label masterPeakLabel;
    ClipLamp masterClipLamp;
    // The master section's read-outs. updateMaster (the timer) reads the session's peaks, the only reader; the checks call it with their own times.
    static constexpr double kMasterHoldMs = 2000.0;  // the peak read-out holds the highest peak this long
    void updateMaster (double nowMs);
    float masterPeakDb() const { return masterHoldDb; }   // the held peak, dBFS (-100 when silent)
    bool masterClipLatched() const { return clipLatched; }
    void clearMasterClip();                               // CLIP's click: the lamp goes out
    juce::Rectangle<float> masterMeterArea() const { return masterBarLeft.getUnion (masterBarRight); }
    // Checks only: the recorder, so a check can make a disk problem happen (Recorder::lostForTest, failedForTest).
    Recorder& recorderForChecks() { return recorder; }
    // Checks only: pretend the take's files are still opening (Recorder::start does the folder and the headers on the message thread), so a
    // check can feed the callback the blocks that arrive then and see what it makes of them.
    void setFilesOpeningForChecks (bool opening) { recordStarting = opening; }
    // Checks only: the scan's state, so the empty-scan note can be checked while a scan is queued or running.
    void setScanStateForChecks (bool queued, bool running) { scanQueued = queued; scanning = running; }
    void runTimerForChecks() { timerCallback(); }
    void expireAutosaveForChecks() { lastAutosaveMs = -1.0e9; }
    juce::Rectangle<int> positionBox;                // the position read-out (bars.beats.ticks and mm:ss.ss)
    // the MONITOR strip: one button per plugin of the monitor chain, '+', the CORRECTION switch and the chain's latency
    juce::Rectangle<int> monitorStrip;
    juce::OwnedArray<MonitorSlotButton> monitorButtons;
    juce::TextButton monitorAddButton { "+" };
    LampButton monitorBypassButton { "Correction" };
    ArrangeView arrange { session, editor };
    PianoRollView pianoRoll { session, editor };

private:
    // The audio callback's copy of the transport: the playhead, the loop, the click and the end of the audio. The callback reads and writes
    // only these. (Where a take began is the recorder's, Recorder::push decides it with the write itself.)
    struct Source
    {
        explicit Source (MainComponent& m) : owner (m) {}
        void getNextAudioBlock (const juce::AudioSourceChannelInfo& info);
        MainComponent& owner;
        std::atomic<juce::int64> position { 0 };
        std::atomic<bool> playing { false };
        std::atomic<bool> loopOn { false };
        std::atomic<bool> metronomeOn { false };
        std::atomic<juce::int64> loopStart { 0 }, loopEnd { 0 }, endOfAudio { 0 };
        juce::AudioBuffer<float> chunk { 2, kBlockSize };
    };

    void timerCallback() override;
    void changeListenerCallback (juce::ChangeBroadcaster*) override;
    void handleAsyncUpdate() override { refreshNow(); }
    void updateTitle();
    void updateTransport();
    void showError (const juce::String& message);
    void chooseAndOpen();
    void chooseAndImport();
    void chooseFolder();
    void chooseAndRender();
    void chooseAndExport();
    void launchRenderChooser (const juce::String& chooserTitle, const juce::String& progressTitle, const juce::File& start, RenderOptions choice);
    void runRenderJob (const juce::File& folder, RenderOptions choice, const juce::String& title);
    // The render dialog's default: bit depth and mix-only, as AlertWindow (modal). Returns false when cancelled.
    static bool askRenderDialog (RenderOptions& options);
    // The export dialog's default: bit depth only, as AlertWindow (modal). Returns false when cancelled.
    static bool askExportDialog (RenderOptions& options);
    // Autosave and recovery (the file names are in autosaveFile and offerRecovery).
    juce::File autosaveFileFor (const juce::File& project) const;
    juce::File untitledAutosaveFile() const;
    void deleteAutosaves();                         // after a save, NEW, or a quit / open that drops the changes
    void recoverFrom (const juce::File& autosave, const juce::File& project);
    void showPluginsMenu();
    void populatePluginMenu (juce::PopupMenu& menu, std::function<void (const juce::String&)> onPick);
    void showPluginSearch (std::function<void (const juce::String&)> onPick);
    void showSignature (int numerator, int denominator);
    void applyBpm();
    void syncPlayhead() { arrange.playhead = (double) position(); }
    juce::int64 endOfPlay() const { return session.length() + session.maxLatency(); }
    void readDevice();                              // the device as it is now, into `device`
    void adoptDeviceRate();                         // the session takes the device's rate (converting a project opened at another one); message thread
    void deviceChanged();                           // a change of the device: read it, save its state, end a take that cannot go on
    // What audioDeviceAboutToStart described, kept as a value so the message thread can apply it whenever it is safe to wait for the
    // session's lock (see DeviceCallback::audioDeviceAboutToStart and applyDeviceStart).
    struct DeviceStart
    {
        std::uint64_t seq = 0;
        bool hasDevice = false, refresh = false;
        double rate = 0.0;
        int bufSize = 0;
        juce::BigInteger activeInputs;
    };
    // Message thread: end a take that cannot go on, prepare the recorder and the scratch, adopt the rate. Deferred while a render holds the
    // session's lock (its own wait would freeze the progress window), and retried by the timer once the render is over.
    void applyDeviceStart (const DeviceStart&);
    void saveAudioState();
    void updateRecording();                         // the timer's part of a take: its strip, its time, a device that stopped
    void finishRecordingTakes (const std::vector<Recorder::Finished>& takes, juce::int64 heard, juce::int64 headSkipped, const juce::String& problem);
    void setTakeLatency();
    void loadMonitorChain();                        // reads monitor.xml once the catalog is filled
    void saveMonitorChain();                        // writes monitor.xml; nothing before the chain was read (never overwrites an unread file)
    void monitorEditorClosed (const Slot* slot);    // a monitor plugin's window was closed: its state may have changed
    bool hasOpenTrackEditor() const;
    juce::int64 trackPluginStateHash() const;
    Slot* slotOf (int track, int slot, juce::String& error);  // a track's chain slot, or with track -1 the monitor chain's

    Options opts;
    bool monitorLoaded = false;                     // the chain is read: until then no save writes monitor.xml
    bool monitorSkipped = false;                    // a pedal found at start: the chain is not read until PLUGINS reads it
    bool projectSkipped = false;                    // a pedal found at start: the last project is not opened again
    Source source { *this };
    Recorder recorder;
    std::unique_ptr<juce::FileChooser> chooser;
    bool autosaveFailing = false;              // the last autosave failed: its error was shown, the next failure is not shown again
    bool dialogOpen = false;                   // a chooser or a question is open: no second one is started, and NEW/OPEN/Quit wait
    // Sets dialogOpen for its scope and restores the value it found.
    struct DialogBusy { bool& flag; bool was; explicit DialogBusy (bool& f) : flag (f), was (f) { flag = true; } ~DialogBusy() { flag = was; } };
    juce::AudioDeviceManager deviceManager;
    juce::TooltipWindow tooltips { this, 600 };
    HardwareLookAndFeel look;
    std::unique_ptr<AudioDialog> audioDialog;
    juce::Component::SafePointer<juce::AlertWindow> searchWindow;
    std::map<const Slot*, std::unique_ptr<juce::Component>> windows;
    juce::String shownTitle;
    juce::String lastPositionText;
    bool lastRecordingState = false;
    double lastMonitorLatencyMs = -1.0;
    juce::int64 lastEditorHash = 0;
    int tick = 0;
    double lastAutosaveMs = 0.0;                    // juce::Time::getMillisecondCounterHiRes at the last autosave (or at start)
    juce::Time loadedProjectMtime;                  // the project file's modification time when this window last loaded, saved or checked it
    int externalCheckTick = 0;
    bool scanning = false;                          // the scan runs (scanPlugins)
    bool scanQueued = false;                        // the start's scan is queued and has not begun
    juce::StringArray signatures { "4/4", "3/4", "2/4", "5/4", "6/8", "7/8", "12/8" };
    juce::Rectangle<int> noteStrip, bodyArea;
    // a take in progress: the latency its clips are placed earlier by (fixed at REC), and the playhead it began at (the fallback when the
    // callback never ran); the device stopped (set by the device thread, read by the timer)
    juce::int64 takeLatency = 0, takeFrom = 0;
    std::atomic<bool> deviceStopped { false };
    std::atomic<bool> deviceStarting { false };
    std::atomic<bool> recordStarting { false };
    std::atomic<std::uint64_t> deviceStartSeq { 0 };
    // A device start that found a render holding the session's lock: kept for the timer, which applies it once the render lets go. Message
    // thread only, and `deviceStarting` stays set while it waits (the callback holds the output silent until the new device is prepared).
    std::optional<DeviceStart> pendingDeviceStart;
    juce::String monitorProblem;                    // why monitor.xml could not be read (shown on the foot), empty when it was
    // the master section's state: the meter's two bars, the held peak and its time, the latched CLIP, and the fader's one undo step while dragged
    MeterBallistics masterLeft, masterRight;
    juce::Rectangle<float> masterBarLeft, masterBarRight;   // where the meter's bars are drawn (set by resized)
    float masterHoldDb = -100.0f;
    double masterHoldMs = -1.0e9;
    bool clipLatched = false;
    juce::uint64 lastMasterRevision = 0;
    std::unique_ptr<Editor::Gesture> masterGesture;
    void updateMasterFader();                       // the fader follows the session (undo, a project): not while it is dragged
};
}
