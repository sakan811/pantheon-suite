// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 The Pantheon authors

#pragma once

#include "Advice.h"
#include "Navigator.h"

#include <pantheon/ui/Advice.h>

namespace orpheus::gui
{
// The whole window: the transport along the top, the pitch display, and two panels underneath, one for the scale and one for the
// notes that are selected. It owns the session, the player and the audio device, and is the only place that knows keys and files.
class MainComponent : public juce::Component, public juce::FileDragAndDropTarget, private juce::Timer
{
public:
    // The window's design size: the notes strip at the foot is part of it, so the display keeps the height it had.
    static constexpr int kDesignWidth = 1240, kDesignHeight = 760 + 6 + ui::kNoteStripHeight + 8;

    explicit MainComponent (bool withAudioDevice = true);
    ~MainComponent() override;

    Session session;
    Playback playback { session };
    PitchRoll roll { session, playback };
    Navigator navigator { session, roll, playback };

    // Reads an audio file or a saved project and analyses it in the background.
    void loadFile (const juce::File& f);
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
    juce::TextButton openButton { "Open" }, saveButton { "Save" }, exportButton { "Export" }, undoButton { "Undo" }, redoButton { "Redo" };
    juce::TextButton zoomOutButton { "-" }, zoomInButton { "+" }, fitButton { "Fit" };
    juce::TextButton playButton { "Play" }, rewindButton { "|<" }, loopButton { "Loop" }, originalButton { "Original" }, snapButton { "Snap" };
    juce::ComboBox keyBox, scaleBox;
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
    void chooseAndOpen();
    void chooseAndSave();
    void chooseAndExport();
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
    juce::File projectFile;
    juce::String infoLine, audioError, notice;
    juce::uint32 noticeUntil = 0;
    double correctAmount = 100.0;
    bool glidePending = false, audioOn = false;
    int lastVersion = -1, analysisSerial = 0;
    juce::Rectangle<int> headerArea, scalePanel, notePanel, timeBox, nameplate, noteStrip;
};
} // namespace orpheus::gui
