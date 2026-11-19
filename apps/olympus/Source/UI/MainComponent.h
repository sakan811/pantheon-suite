// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 The Pantheon authors

#pragma once

#include "Advice.h"
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

// A push switch whose lamp has the colour it is given (the kit's look-and-feel lights every switch in one colour): red for a mute, green for a solo, red for a
// plugin that is not installed.
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

// One plugin of a chain: click opens it, shift-click bypasses it, right-click (or ctrl-click) for the menu.
class SlotButton : public LampButton
{
public:
    std::function<void()> onMenu;
    void mouseDown (const juce::MouseEvent& e) override { if (e.mods.isPopupMenu()) { if (onMenu) onMenu(); return; } LampButton::mouseDown (e); }
    void mouseUp (const juce::MouseEvent& e) override { if (! e.mods.isPopupMenu()) LampButton::mouseUp (e); }
};

// The waveform of a track on glass, with the playhead; a click puts the playhead there.
class WaveStrip : public juce::Component
{
public:
    std::vector<float> envelope;
    double fraction = 0.0; // playhead, 0..1
    std::function<void (double)> onSeek;
    void paint (juce::Graphics& g) override;
    void mouseDown (const juce::MouseEvent& e) override { if (onSeek) onSeek (juce::jlimit (0.0, 1.0, (double) e.x / (double) juce::jmax (1, getWidth()))); }
    void mouseDrag (const juce::MouseEvent& e) override { mouseDown (e); }
};

// One track: name, file, mute and solo, fader, waveform and the chain.
class TrackRow : public juce::Component
{
public:
    TrackRow (MainComponent& owner, int trackIndex);
    int heightFor (int width) const;
    void resized() override;
    void paint (juce::Graphics& g) override;
    void refresh();
    int index;
    WaveStrip wave;
    juce::Label nameLabel;
    LampButton muteButton { "M" }, soloButton { "S" };
    juce::TextButton removeButton { "X" }, addPluginButton { "+" };
    juce::Slider gainKnob;
    juce::OwnedArray<SlotButton> slotButtons;

private:
    MainComponent& main;
};

// The tracks, one under the other.
class TrackList : public juce::Component
{
public:
    explicit TrackList (MainComponent& m) : main (m) {}
    void rebuild();
    void resized() override;
    juce::OwnedArray<TrackRow> rows;

private:
    MainComponent& main;
};

class PluginWindow : public juce::DocumentWindow
{
public:
    PluginWindow (const juce::String& title, juce::AudioProcessorEditor* editor, std::function<void()> closed);
    void closeButtonPressed() override { if (onClosed) onClosed(); }
    std::function<void()> onClosed;
};

// The whole window: the controls along the top, the tracks, and the notes at the foot. It owns the session and the audio device.
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

    // ---- what the buttons do (public so the checks can press them) -------------------------------------------
    void newProject();
    void openProject (const juce::File& file);
    bool saveProject();
    void saveProjectAs();
    bool saveTo (const juce::File& file);
    void addTracks (const juce::StringArray& files);
    // Writes the stems and the mix. Returns the reason when it fails, an empty string when it worked.
    juce::String renderTo (const juce::File& folder, const std::function<bool (float)>& progress = {});
    void togglePlay();
    void seek (juce::int64 sample);
    juce::int64 position() const { return source.position.load(); }
    bool isPlaying() const { return source.playing.load(); }
    void scanPlugins();
    void openEditor (int track, int slot);
    int openEditors() const { return (int) windows.size(); }
    void closeEditors();
    void removeTrack (int index);
    // Rebuilds the rows now (the structure of the tracks or a chain changed).
    void rebuildNow();
    void requestRebuild() { triggerAsyncUpdate(); }

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

    bool keyPressed (const juce::KeyPress& key) override;
    void paint (juce::Graphics&) override;
    void resized() override;
    bool isInterestedInFileDrag (const juce::StringArray& files) override { return ! files.isEmpty(); }
    void filesDropped (const juce::StringArray& files, int, int) override;

    ui::GuideButton guideButton { guideTitle(), guideText() };
    juce::TextButton newButton { "New" }, openButton { "Open" }, saveButton { "Save" }, saveAsButton { "Save as" }, addButton { "Add track" }, pluginsButton { "Plugins" },
        renderButton { "Render stems" }, rewindButton { "|<" }, playButton { "Play" };
    juce::Viewport viewport;
    TrackList trackList { *this };

    // for the rows
    void showAddPluginMenu (int track);
    void showSlotMenu (int track, int slot);
    void toggleEditor (int track, int slot);

private:
    struct Source : juce::AudioSource
    {
        explicit Source (MainComponent& m) : owner (m) {}
        void prepareToPlay (int, double rate) override { owner.deviceRate = rate; }
        void releaseResources() override {}
        void getNextAudioBlock (const juce::AudioSourceChannelInfo& info) override;
        MainComponent& owner;
        std::atomic<juce::int64> position { 0 };
        std::atomic<bool> playing { false };
        juce::AudioBuffer<float> chunk { 2, kBlockSize };
    };

    void timerCallback() override;
    void handleAsyncUpdate() override { rebuildNow(); }
    void updateTitle();
    void showError (const juce::String& message);
    void chooseAndOpen();
    void chooseAndAdd();
    void chooseFolder();
    void chooseAndRender();
    void showPluginsMenu();
    void setDeviceRate();
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
    juce::Rectangle<int> noteStrip, timeBox, bodyArea;
};
}
