// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 The Pantheon authors

#pragma once

#include "Advice.h"
#include "Arrange.h"
#include "../Edit.h"
#include "../Record.h"
#include "Session.h"
#include "Settings.h"

#include <juce_audio_utils/juce_audio_utils.h>

#include <map>

#include <pantheon/ui/Advice.h>
#include <pantheon/ui/Aspect.h>
#include <pantheon/ui/HardwareLookAndFeel.h>

namespace olympus::gui
{
class MainComponent;
class AudioDialog;

// A push switch whose lamp has the colour it is given (the kit's look-and-feel lights every switch in one colour): amber for the transport
// switches, red for a mute, green for a solo, red for a plugin that is not installed.
class LampButton : public juce::TextButton
{
public:
    using juce::TextButton::TextButton;
    juce::Colour lamp = colours::amber;
    void paintButton (juce::Graphics& g, bool highlighted, bool down) override
    {
        ui::hw::drawPushButton (g, getLocalBounds().toFloat(), getToggleState(), lamp, down, highlighted, isEnabled());
        g.setFont (ui::labelFont (juce::jmin (10.5f, (float) getHeight() * 0.5f)));
        g.setColour (getToggleState() && lamp.getPerceivedBrightness() > 0.5f ? colours::lampInk : colours::text);
        g.drawText (getButtonText().toUpperCase(), getLocalBounds().translated (0, down ? 1 : 0), juce::Justification::centred);
    }
};

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

class PluginWindow : public juce::DocumentWindow
{
public:
    PluginWindow (const juce::String& title, juce::AudioProcessorEditor* editor, std::function<void()> closed);
    void closeButtonPressed() override { if (onClosed) onClosed(); }
    std::function<void()> onClosed;
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
        juce::BigInteger activeInputs;             // which of the device's inputs are active
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

    private:
        MainComponent& owner;
    };

    MainComponent();
    explicit MainComponent (Options options);
    ~MainComponent() override;

    Session session;
    Settings settings;
    Editor editor { session };          // every timeline edit, and the undo of it, goes through here

    // ---- what the buttons do (public so the checks can press them) ----------------------------------------------------------------
    void newProject();
    void openProject (const juce::File& file);
    bool saveProject();
    void saveProjectAs();
    bool saveTo (const juce::File& file);
    // One new track per file, its clip at sample 0 (the checks' way in; the buttons use importFiles at the playhead).
    void addTracks (const juce::StringArray& files);
    // Audio files as clips: with `track` -1 each file is a new track named after it, else the files go on that track one after another.
    // Every file starts at `at`, or after the previous clip on the track. Errors are shown together.
    void importFiles (const juce::StringArray& files, int track, juce::int64 at);
    void addEmptyTrack();
    void undoEdit();
    void redoEdit();
    void toggleLoop();
    void toggleMetronome();
    void zoomBy (double factor);
    void removeTrack (int index);
    // Writes the stems and the mix. Returns the reason when it fails, an empty string when it worked.
    juce::String renderTo (const juce::File& folder, const std::function<bool (float)>& progress = {});
    void togglePlay();
    void rewind();                                   // to the loop's start when the loop is on, else to 0
    void seek (juce::int64 sample);
    juce::int64 position() const { return source.position.load(); }
    bool isPlaying() const { return source.playing.load(); }
    // Copies what the audio thread needs (the loop, the end of the audio) from the session. Message thread; the timer does it too.
    void syncTransport();
    // What the audio callback does for one block: the same code, so the checks can drive the loop and the end of the audio.
    void renderAudioBlock (juce::AudioBuffer<float>& buffer) { juce::AudioSourceChannelInfo info (&buffer, 0, buffer.getNumSamples()); source.getNextAudioBlock (info); }
    void scanPlugins (bool rescan = false);
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

    // Runs `proceed` now if nothing would be lost, else after the user agrees.
    void askToDiscard (const juce::String& action, std::function<void()> proceed);
    // How a question is put: message, the button that goes ahead, what to do then. Replaceable, so the checks need no dialog.
    std::function<void (const juce::String&, const juce::String&, std::function<void()>)> ask;
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
    double monitorLatencyMs() const;                           // the chain's latency in ms at the device's rate (0 when bypassed)

    bool keyPressed (const juce::KeyPress& key) override;
    void paint (juce::Graphics&) override;
    void resized() override;
    bool isInterestedInFileDrag (const juce::StringArray& files) override { return ! files.isEmpty(); }
    void filesDropped (const juce::StringArray& files, int, int) override;

    // ---- the controls (public so the checks can find and press them) ---------------------------------------------------------------
    ui::GuideButton guideButton { guideTitle(), guideText() };
    juce::TextButton newButton { "New" }, openButton { "Open" }, saveButton { "Save" }, saveAsButton { "Save as" }, addTrackButton { "Add track" },
        importButton { "Import audio" }, pluginsButton { "Plugins" }, renderButton { "Render stems" }, audioButton { "Audio" };
    juce::TextButton rewindButton { "|<" }, undoButton { "Undo" }, redoButton { "Redo" }, zoomOutButton { "-" }, zoomInButton { "+" }, zoomFitButton { "Fit" };
    LampButton playButton { "Play" }, recordButton { "Rec" }, loopButton { "Loop" }, metronomeButton { "Click" }, snapButton { "Snap" };
    juce::TextEditor bpmField;                       // the tempo, when the project has one; the tempo at the playhead when it has more
    juce::ComboBox signatureBox, gridBox;
    juce::Label deviceLabel;                         // the device summary, under the buttons
    juce::Rectangle<int> positionBox;                // the position read-out (bars.beats.ticks and mm:ss.ss)
    // the MONITOR strip: one button per plugin of the monitor chain, '+', the CORRECTION switch and the chain's latency
    juce::Rectangle<int> monitorStrip;
    juce::OwnedArray<MonitorSlotButton> monitorButtons;
    juce::TextButton monitorAddButton { "+" };
    LampButton monitorBypassButton { "Correction" };
    ArrangeView arrange { session, editor };

private:
    // The audio callback's copy of the transport: the playhead, the loop, the end of the audio and where the take began. The callback reads
    // only these.
    struct Source
    {
        explicit Source (MainComponent& m) : owner (m) {}
        void getNextAudioBlock (const juce::AudioSourceChannelInfo& info);
        MainComponent& owner;
        std::atomic<juce::int64> position { 0 };
        std::atomic<bool> playing { false };
        std::atomic<bool> loopOn { false };
        std::atomic<juce::int64> loopStart { 0 }, loopEnd { 0 }, endOfAudio { 0 };
        // The playhead at the first device block the recorder ran in (set by DeviceCallback::deviceBlock); -1 before it. Cleared at REC.
        std::atomic<juce::int64> recordStart { -1 };
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
    void showPluginsMenu();
    void showSignature (int numerator, int denominator);
    void applyBpm();
    void syncPlayhead() { arrange.playhead = (double) position(); }
    juce::int64 endOfPlay() const { return session.length() + session.maxLatency(); }
    void readDevice();                              // the device as it is now, into `device`
    void deviceChanged();                           // a change of the device: read it, save its state, end a take that cannot go on
    void saveAudioState();
    void updateRecording();                         // the timer's part of a take: its strip, its time, a device that stopped
    void setTakeLatency();
    void loadMonitorChain();                        // reads monitor.xml once the catalog is filled
    void saveMonitorChain();                        // writes monitor.xml; nothing before the chain was read (never overwrites an unread file)
    void monitorEditorClosed (const Slot* slot);    // a monitor plugin's window was closed: its state may have changed
    Slot* slotOf (int track, int slot, juce::String& error);  // a track's chain slot, or with track -1 the monitor chain's

    Options opts;
    bool monitorLoaded = false;                     // the chain is read: until then no save writes monitor.xml
    Source source { *this };
    Recorder recorder;
    std::unique_ptr<juce::FileChooser> chooser;
    juce::AudioDeviceManager deviceManager;
    juce::TooltipWindow tooltips { this, 600 };
    HardwareLookAndFeel look;
    std::unique_ptr<AudioDialog> audioDialog;
    std::map<const Slot*, std::unique_ptr<juce::Component>> windows;
    juce::String shownTitle;
    juce::int64 lastEditorHash = 0;
    int tick = 0;
    bool scanning = false;
    juce::StringArray signatures { "4/4", "3/4", "2/4", "5/4", "6/8", "7/8", "12/8" };
    juce::Rectangle<int> noteStrip, bodyArea;
    // a take in progress: the latency its clips are placed earlier by (fixed at REC), and the playhead it began at (the fallback when the
    // callback never ran); the device stopped (set by the device thread, read by the timer)
    juce::int64 takeLatency = 0, takeFrom = 0;
    std::atomic<bool> deviceStopped { false };
    juce::String monitorProblem;                    // why monitor.xml could not be read (shown on the foot), empty when it was
};
}
