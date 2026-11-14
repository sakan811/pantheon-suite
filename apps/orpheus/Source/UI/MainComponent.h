// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 The Pantheon authors

#pragma once

#include "Advice.h"
#include "Navigator.h"
#include "Recent.h"

#include <optional>

#include <pantheon/ui/Advice.h>

namespace orpheus::gui
{
// The whole window: the transport along the top, the pitch display, and two panels underneath, one for the scale and one for the
// notes that are selected. It owns the session, the player and the audio device, and is the only place that knows keys and files.
class MainComponent : public juce::Component, public juce::FileDragAndDropTarget, private juce::Timer
{
public:
    // The window's design size: the notes strip at the foot is part of it, so the display keeps the height it had.
    static constexpr int kDesignWidth = 1300, kDesignHeight = 796 + 6 + ui::kNoteStripHeight + 8;

    explicit MainComponent (bool withAudioDevice = true);
    ~MainComponent() override;

    Session session;
    Playback playback { session };
    PitchRoll roll { session, playback };
    Navigator navigator { session, roll, playback };

    // Reads an audio file or a saved project and analyses it in the background. If edits would be lost it asks first (`ask`); a file that
    // cannot be read, or a project that is not for that recording, is refused with the open recording left as it was.
    void loadFile (const juce::File& f);
    // The last file opened or saved, again (what the app does at launch).
    void reopenLast();
    // Saves to the project file the session has (silently), or asks where if it has none yet. Save As always asks.
    bool save();
    void saveAs();
    bool saveTo (const juce::File& f);
    // Writes the corrected WAV; warns first (through `ask`) when the result is past full scale and the file would clip.
    void exportTo (const juce::File& f);
    // Runs `proceed` now if nothing would be lost, else after the user agrees (for Open, a drop, Re-analyse and Quit).
    void askToDiscard (const juce::String& action, std::function<void()> proceed);
    // How the question is put: message, the text of the button that goes ahead, what to do then. Replaceable, so the checks need no dialog.
    std::function<void (const juce::String&, const juce::String&, std::function<void()>)> ask;
    // How a refusal or a failure is told to the user (a message box by default); replaceable for the checks.
    std::function<void (const juce::String&)> reportError;
    // The window title: the file, and a star while there are edits that are not saved. `onTitleChanged` is called when it changes.
    juce::String windowTitle() const;
    std::function<void()> onTitleChanged;
    RecentFiles recent;
    // Finds the notes again with the chosen voice range (this takes every edit back).
    void reanalyse();
    // The settings of the one selected note (shift, drift, vibrato, formant, gain), put onto the notes selected now.
    void copySettings();
    void pasteSettings();
    bool hasCopiedSettings() const { return copied.has_value(); }
    // Analyses the loaded audio on this thread (tests and screenshots).
    void analyseNow();

    bool isBusy() const { return job != nullptr; }
    std::vector<ui::Note> notes() const { return adviceFor (adviceInput()); } // what the strip at the foot says, for the checks
    void refreshInspector();
    void refreshScalePanel();
    void togglePlay();
    void seek (juce::int64 sample);
    void undo();
    void redo();

    // Keys: the whole list is in the README; the window takes them wherever the focus is, except while a number is being typed.
    bool keyPressed (const juce::KeyPress& key) override;

    void paint (juce::Graphics&) override;
    void resized() override;
    bool isInterestedInFileDrag (const juce::StringArray& files) override { return ! files.isEmpty(); }
    void filesDropped (const juce::StringArray& files, int, int) override { loadFile (juce::File (files[0])); }

    // Controls, public so the checks can press them.
    ui::GuideButton guideButton { guideTitle(), guideText() };
    juce::TextButton openButton { "Open" }, saveButton { "Save" }, saveAsButton { "Save as" }, exportButton { "Export" }, undoButton { "Undo" }, redoButton { "Redo" };
    juce::TextButton zoomOutButton { "-" }, zoomInButton { "+" }, fitButton { "Fit" }, zoomSelButton { "Sel" };
    juce::TextButton playButton { "Play" }, rewindButton { "|<" }, loopButton { "Loop" }, originalButton { "Original" }, snapButton { "Snap" };
    juce::ComboBox keyBox, scaleBox, rangeBox, offBox;
    juce::TextButton reanalyseButton { "Re-analyse" }, copyButton { "Copy" }, pasteButton { "Paste" }, selectOffButton { "Select" };
    ui::ValueKnob tuningKnob, glideKnob, amountKnob;
    juce::TextButton correctAllButton { "Correct all" }, detectKeyButton { "Detect key" }, detectTuneButton { "Detect A4" }, resetAllButton { "Reset all" };
    ui::ValueKnob correctKnob, shiftKnob, driftKnob, vibratoKnob, formantKnob, gainKnob;
    juce::TextButton snapNoteButton { "Snap" }, straightenButton { "Straighten" }, splitButton { "Split" }, mergeButton { "Merge" }, resetNoteButton { "Reset" };

private:
    struct Job;
    void timerCallback() override;
    void startAnalysis();
    void analysisDone (Session::Analysis&& result);
    void applyToSelection (const std::function<void (Note&)>& change);
    void updateLoop();
    void openNow (const juce::File& f);
    void showOpenMenu();
    void chooseAndOpen();
    void chooseAndExport();
    void updateTitle();
    void showError (const juce::String& message);
    AdviceInput adviceInput() const;
    void say (const juce::String& message);  // a line on the glass of the note panel for a few seconds
    void setupKnob (ui::ValueKnob& knob, const juce::String& caption, double lo, double hi, double step, double reset, const juce::String& suffix, int decimals,
                    const juce::String& tooltip);

    std::unique_ptr<Job> job;
    std::unique_ptr<juce::XmlElement> pendingProject;
    std::unique_ptr<juce::FileChooser> chooser;
    juce::AudioDeviceManager deviceManager;
    juce::AudioSourcePlayer player;
    juce::AudioTransportSource transport;
    juce::TooltipWindow tooltips { this, 600 };
    HardwareLookAndFeel look;
    struct NoteSettings
    {
        float shift, drift, vibrato, formant, gainDb;
    };
    std::optional<NoteSettings> copied;
    juce::File projectFile;
    juce::String lastTitle;
    juce::String infoLine, audioError, notice;
    juce::uint32 noticeUntil = 0;
    double correctAmount = 100.0;
    bool glidePending = false, audioOn = false, reanalysing = false;
    int lastVersion = -1, analysisSerial = 0;
    juce::Rectangle<int> headerArea, scalePanel, notePanel, timeBox, nameplate, noteStrip, offLabelArea, peakBox;
};
} // namespace orpheus::gui
