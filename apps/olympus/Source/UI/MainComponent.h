// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 The Pantheon authors

#pragma once

#include "Advice.h"
#include "Arrange.h"
#include "../Edit.h"
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

class PluginWindow : public juce::DocumentWindow
{
public:
    PluginWindow (const juce::String& title, juce::AudioProcessorEditor* editor, std::function<void()> closed);
    void closeButtonPressed() override { if (onClosed) onClosed(); }
    std::function<void()> onClosed;
};

// The whole window: the project buttons and the transport along the top, the arrangement (ArrangeView) in the body, and the notes at the
// foot. It owns the session, the editor that changes it, and the audio device.
class MainComponent : public juce::Component, public juce::FileDragAndDropTarget, private juce::Timer, private juce::AsyncUpdater
{
public:
    static constexpr int kDesignWidth = 1280, kDesignHeight = ui::editorHeightFor (kDesignWidth);

    struct Options
    {
        bool audioDevice = true;           // false for the checks and the snapshots
        bool scanPlugins = true;           // false when the caller fills the catalog
        juce::File settingsFile = Settings::defaultFile();
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
    void openEditor (int track, int slot);
    int openEditors() const { return (int) windows.size(); }
    void closeEditors();
    // Brings the arrangement, the transport and the title up to date now.
    void refreshNow();
    // The same, after the current event: for a change made from inside a control that the change may remove.
    void requestRefresh() { triggerAsyncUpdate(); }

    // Runs `proceed` now if nothing would be lost, else after the user agrees.
    void askToDiscard (const juce::String& action, std::function<void()> proceed);
    // How a question is put: message, the button that goes ahead, what to do then. Replaceable, so the checks need no dialog.
    std::function<void (const juce::String&, const juce::String&, std::function<void()>)> ask;
    std::function<void (const juce::String&)> reportError;
    // What holds a plugin's editor on screen (a PluginWindow by default); replaceable, so the checks open no native window.
    std::function<std::unique_ptr<juce::Component> (const juce::String&, juce::AudioProcessorEditor*, std::function<void()>)> makeEditorWindow;
    juce::String windowTitle() const;
    std::function<void()> onTitleChanged;
    std::vector<ui::Note> notes() const { return adviceFor (session, deviceRate); }
    double deviceRate = 0.0;

    // The menus of a track: its plugins (open, bypass, move, remove) and the plugins to add.
    void showAddPluginMenu (int track);
    void showTrackMenu (int track);

    bool keyPressed (const juce::KeyPress& key) override;
    void paint (juce::Graphics&) override;
    void resized() override;
    bool isInterestedInFileDrag (const juce::StringArray& files) override { return ! files.isEmpty(); }
    void filesDropped (const juce::StringArray& files, int, int) override;

    // ---- the controls (public so the checks can find and press them) ---------------------------------------------------------------
    ui::GuideButton guideButton { guideTitle(), guideText() };
    juce::TextButton newButton { "New" }, openButton { "Open" }, saveButton { "Save" }, saveAsButton { "Save as" }, addTrackButton { "Add track" },
        importButton { "Import audio" }, pluginsButton { "Plugins" }, renderButton { "Render stems" };
    juce::TextButton rewindButton { "|<" }, undoButton { "Undo" }, redoButton { "Redo" }, zoomOutButton { "-" }, zoomInButton { "+" }, zoomFitButton { "Fit" };
    LampButton playButton { "Play" }, loopButton { "Loop" }, metronomeButton { "Click" }, snapButton { "Snap" };
    juce::TextEditor bpmField;                       // the tempo, when the project has one; the tempo at the playhead when it has more
    juce::ComboBox signatureBox, gridBox;
    ArrangeView arrange { session, editor };
    juce::Rectangle<int> positionBox;                // the position read-out (bars.beats.ticks and mm:ss.ss)

private:
    // The audio callback's copy of the transport: the playhead, the loop and the end of the audio. The callback reads only these.
    struct Source : juce::AudioSource
    {
        explicit Source (MainComponent& m) : owner (m) {}
        void prepareToPlay (int, double rate) override { owner.deviceRate = rate; }
        void releaseResources() override {}
        void getNextAudioBlock (const juce::AudioSourceChannelInfo& info) override;
        MainComponent& owner;
        std::atomic<juce::int64> position { 0 };
        std::atomic<bool> playing { false };
        std::atomic<bool> loopOn { false };
        std::atomic<juce::int64> loopStart { 0 }, loopEnd { 0 }, endOfAudio { 0 };
        juce::AudioBuffer<float> chunk { 2, kBlockSize };
    };

    void timerCallback() override;
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

    Options opts;
    Source source { *this };
    std::unique_ptr<juce::FileChooser> chooser;
    juce::AudioDeviceManager deviceManager;
    juce::AudioSourcePlayer player;
    juce::TooltipWindow tooltips { this, 600 };
    HardwareLookAndFeel look;
    std::map<const Slot*, std::unique_ptr<juce::Component>> windows;
    juce::String shownTitle;
    juce::int64 lastEditorHash = 0;
    int tick = 0;
    bool scanning = false;
    juce::StringArray signatures { "4/4", "3/4", "2/4", "5/4", "6/8", "7/8", "12/8" };
    juce::Rectangle<int> noteStrip, bodyArea;
};
}
