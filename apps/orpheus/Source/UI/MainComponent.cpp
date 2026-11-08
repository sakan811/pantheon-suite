// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 The Pantheon authors

#include "MainComponent.h"

namespace orpheus::gui
{
// ---- background analysis -------------------------------------------------------------------------------------------------
struct MainComponent::Job : public juce::Thread
{
    Job (MainComponent& o, std::vector<float> samples, double rate, int id) : juce::Thread ("Orpheus analysis"), owner (o), mono (std::move (samples)), sampleRate (rate), serial (id) {}
    ~Job() override { stopThread (20000); }

    void run() override
    {
        auto result = std::make_shared<Session::Analysis> (Session::analyse (mono, sampleRate, [this] (float p)
                                                                              {
                                                                                  progress.store (p);
                                                                                  return ! threadShouldExit();
                                                                              }));
        if (threadShouldExit())
            return;
        // identified by a number, not by address: a newer job can be allocated where this one was
        juce::MessageManager::callAsync ([safe = juce::Component::SafePointer<MainComponent> (&owner), result, id = serial]
                                         {
                                             if (safe != nullptr && safe->job != nullptr && safe->job->serial == id)
                                                 safe->analysisDone (std::move (*result));
                                         });
    }

    MainComponent& owner;
    std::vector<float> mono;
    double sampleRate;
    int serial;
    std::atomic<float> progress { 0.0f };
};

namespace
{
const char* const kAudioWildcard = "*.wav;*.aif;*.aiff;*.flac;*.mp3;*.m4a;*.ogg;*.caf;*.orpheus";

void styleToggle (juce::TextButton& b)
{
    b.setClickingTogglesState (true);
}
} // namespace

MainComponent::MainComponent (bool withAudioDevice)
{
    setLookAndFeel (&look);
    setWantsKeyboardFocus (true);

    addAndMakeVisible (roll);
    addAndMakeVisible (navigator);
    for (auto* b : { &zoomOutButton, &zoomInButton, &fitButton })
        addAndMakeVisible (*b);
    roll.onSelectionChanged = [this] { refreshInspector(); updateLoop(); };
    roll.onEdited = [this] { refreshInspector(); };
    roll.onSeek = [this] (juce::int64 s) { seek (s); };

    for (auto* b : { &openButton, &saveButton, &exportButton, &undoButton, &redoButton, &playButton, &rewindButton, &loopButton, &originalButton, &snapButton, &correctAllButton,
                     &detectKeyButton, &detectTuneButton, &resetAllButton, &snapNoteButton, &straightenButton, &splitButton, &mergeButton, &resetNoteButton })
        addAndMakeVisible (*b);

    styleToggle (loopButton);
    styleToggle (originalButton);
    styleToggle (snapButton);
    snapButton.setToggleState (true, juce::dontSendNotification);

    openButton.setTooltip ("Open an audio file, or a saved project (Cmd/Ctrl+O). You can also drop one on the window.");
    saveButton.setTooltip ("Save the edit as a project next to the audio (Cmd/Ctrl+S). The audio itself is not copied.");
    exportButton.setTooltip ("Write the corrected audio as a 24-bit WAV (Cmd/Ctrl+E).");
    undoButton.setTooltip ("Undo (Cmd/Ctrl+Z)");
    redoButton.setTooltip ("Redo (Shift+Cmd/Ctrl+Z)");
    playButton.setTooltip ("Play / stop (Space)");
    rewindButton.setTooltip ("Back to the start (Home)");
    loopButton.setTooltip ("Loop the selected notes, or the whole recording if none are selected (L)");
    originalButton.setTooltip ("Hear the recording as it was sung, at the same place (Tab). Lit = original.");
    snapButton.setTooltip ("Dragging a note moves it from note to note of the scale. Off: free. (Hold Alt while dragging for the opposite; Shift for fine steps.)");

    zoomOutButton.setTooltip ("Zoom out in time (-). Drag the edges of the window in the bar on the left to zoom as well.");
    zoomInButton.setTooltip ("Zoom in in time (+)");
    fitButton.setTooltip ("Show the first part of the recording at a readable size (F)");
    zoomOutButton.onClick = [this] { roll.zoomTime (1.0 / 1.5, roll.getWidth() * 0.5); };
    zoomInButton.onClick = [this] { roll.zoomTime (1.5, roll.getWidth() * 0.5); };
    fitButton.onClick = [this] { roll.fitToSession(); };
    openButton.onClick = [this] { chooseAndOpen(); };
    saveButton.onClick = [this] { chooseAndSave(); };
    exportButton.onClick = [this] { chooseAndExport(); };
    undoButton.onClick = [this] { undo(); };
    redoButton.onClick = [this] { redo(); };
    playButton.onClick = [this] { togglePlay(); };
    rewindButton.onClick = [this] { seek (0); };
    loopButton.onClick = [this] { updateLoop(); };
    originalButton.onClick = [this] { playback.corrected.store (! originalButton.getToggleState()); };
    snapButton.onClick = [this] { roll.snapToScale = snapButton.getToggleState(); };

    // ---- scale panel --------------------------------------------------------------------------------------------------
    for (int i = 0; i < 12; ++i)
        keyBox.addItem (kNoteNames[i], i + 1);
    for (int i = 0; i < kNumScales; ++i)
        scaleBox.addItem (kScales[i].name, i + 1);
    addAndMakeVisible (keyBox);
    addAndMakeVisible (scaleBox);
    keyBox.setTooltip ("The key: the root note of the scale the notes are pulled to.");
    scaleBox.setTooltip ("The scale. Chromatic lets every semitone through: it tunes each note to the nearest one.");
    keyBox.onChange = [this]
    {
        session.pushUndo();
        session.key.root = keyBox.getSelectedId() - 1;
        ++session.version;
        roll.repaint();
    };
    scaleBox.onChange = [this]
    {
        session.pushUndo();
        session.key.scale = scaleBox.getSelectedId() - 1;
        keyBox.setEnabled (session.key.scale != kChromatic);
        ++session.version;
        roll.repaint();
    };

    for (auto* k : { &tuningKnob, &glideKnob, &amountKnob, &correctKnob, &shiftKnob, &driftKnob, &vibratoKnob, &formantKnob, &gainKnob })
        addAndMakeVisible (*k);

    setupKnob (tuningKnob, "Tuning A4", 415.0, 466.0, 0.1, 440.0, " Hz", 1, "The reference pitch of the song. Detect A4 reads it from the notes (a song recorded at 432 Hz, or a singer who is flat all the way through).");
    tuningKnob.onGestureStart = [this] { session.pushUndo(); };
    tuningKnob.onChange = [this] (double v) { session.key.a4Hz = v; ++session.version; roll.repaint(); };

    setupKnob (glideKnob, "Glide", 0.0, 120.0, 1.0, 30.0, " ms", 0, "How long two neighbouring notes take to slide into each other when they are moved by different amounts. 0 = a step.");
    glideKnob.onGestureStart = [this] { session.pushUndo(); glidePending = false; };
    glideKnob.onChange = [this] (double v) { session.glideMs = v; glidePending = true; };
    glideKnob.onGestureEnd = [this]
    {
        if (glidePending)
            session.changedAll();
        glidePending = false;
        roll.repaint();
    };

    setupKnob (amountKnob, "Amount", 0.0, 100.0, 1.0, 100.0, " %", 0, "How far Correct all pulls every note towards the scale. 100 = onto the note, 50 = halfway.");
    amountKnob.onChange = [this] (double v) { correctAmount = v; };

    correctAllButton.setTooltip ("Pull every note towards the scale by AMOUNT. What was done to individual notes is kept.");
    detectKeyButton.setTooltip ("Find the key of the melody and set KEY and SCALE (major or its relative minor).");
    detectTuneButton.setTooltip ("Find the reference pitch the notes are tuned to and set TUNING A4.");
    resetAllButton.setTooltip ("Take every correction back.");
    correctAllButton.onClick = [this]
    {
        if (! session.isAnalysed())
            return;
        session.correct ((float) (correctAmount / 100.0));
        roll.pruneSelection();
        refreshInspector();
        roll.repaint();
    };
    resetAllButton.onClick = [this]
    {
        if (! session.isAnalysed())
            return;
        session.resetNotes();
        refreshInspector();
        roll.repaint();
    };
    detectKeyButton.onClick = [this]
    {
        if (! session.isAnalysed())
            return;
        const auto guess = detectKeyAuto (session.notes);
        session.pushUndo();
        session.key.root = guess.root;
        session.key.scale = guess.minor ? kMinor : kMajor;
        ++session.version;
        refreshScalePanel();
        say (juce::String ("Key: ") + kNoteNames[guess.root] + (guess.minor ? " minor" : " major") + "  (match " + numberText (guess.score * 100.0) + " %)");
        roll.repaint();
    };
    detectTuneButton.onClick = [this]
    {
        if (! session.isAnalysed())
            return;
        const auto tuning = detectTuning (session.notes);
        if (tuning.strength < kTuningStrengthNeeded)
        {
            say ("No clear reference: the notes are each off in their own way (" + numberText (tuning.strength * 100.0) + " %)");
            return;
        }
        session.pushUndo();
        session.key.a4Hz = std::round (tuning.a4Hz * 10.0) / 10.0;
        ++session.version;
        refreshScalePanel();
        say ("Reference " + numberText (session.key.a4Hz, 1) + " Hz  (" + numberText (tuning.strength * 100.0) + " % sure)");
        roll.repaint();
    };

    // ---- note panel ---------------------------------------------------------------------------------------------------
    setupKnob (correctKnob, "Correct", 0.0, 100.0, 1.0, 100.0, " %", 0, "How far the selected notes are pulled to the nearest note of the scale.");
    setupKnob (shiftKnob, "Shift", -1200.0, 1200.0, 1.0, 0.0, " ct", 0, "Where the note is, in cents from where it was sung. Type an exact value; 100 cents = one semitone. \"+2st\" works too.");
    setupKnob (driftKnob, "Drift", 0.0, 100.0, 1.0, 0.0, " %", 0, "Removes the slow wander of the pitch inside the note (a note that sags or creeps), keeping the vibrato.");
    setupKnob (vibratoKnob, "Vibrato", 0.0, 200.0, 1.0, 100.0, " %", 0, "The fast modulation: 0 = flat, 100 = as sung, 200 = twice as deep.");
    setupKnob (formantKnob, "Formant", -12.0, 12.0, 0.1, 0.0, " st", 1, "Moves the character of the voice (the vowel colour) without moving the pitch.");
    setupKnob (gainKnob, "Gain", -12.0, 12.0, 0.1, 0.0, " dB", 1, "The level of the selected notes.");
    shiftKnob.setup ("Shift", -1200.0, 1200.0, 1.0, 0.0, [] (double v) { return (v > 0.5 ? "+" : "") + numberText (v) + " ct"; },
                     [] (const juce::String& t) -> double
                     {
                         const double v = t.upToFirstOccurrenceOf (" ", false, false).retainCharacters ("+-.0123456789").getDoubleValue();
                         return t.containsIgnoreCase ("st") && ! t.containsIgnoreCase ("ct") ? v * 100.0 : v;
                     },
                     "Where the note is, in cents from where it was sung. Type an exact value; 100 cents = one semitone. \"+2st\" works too.");

    for (auto* k : { &correctKnob, &shiftKnob, &driftKnob, &vibratoKnob, &formantKnob, &gainKnob })
        k->onGestureStart = [this] { session.pushUndo(); };

    correctKnob.onChange = [this] (double v) { applyToSelection ([&, v] (Note& n) { edit::snap (n, session.key, (float) (v / 100.0)); }); };
    shiftKnob.onChange = [this] (double v) { applyToSelection ([v] (Note& n) { n.target = n.centre + (float) (v / 100.0); }); };
    driftKnob.onChange = [this] (double v) { applyToSelection ([v] (Note& n) { n.drift = (float) (v / 100.0); }); };
    vibratoKnob.onChange = [this] (double v) { applyToSelection ([v] (Note& n) { n.vibrato = (float) (v / 100.0); }); };
    formantKnob.onChange = [this] (double v) { applyToSelection ([v] (Note& n) { n.formant = (float) v; }); };
    gainKnob.onChange = [this] (double v) { applyToSelection ([v] (Note& n) { n.gainDb = (float) v; }); };

    snapNoteButton.setTooltip ("Snap the selected notes onto the scale (Enter)");
    straightenButton.setTooltip ("Remove drift and vibrato from the selected notes");
    splitButton.setTooltip ("Cut the selected note in two at the playhead, or at the pointer if it is over the note (S)");
    mergeButton.setTooltip ("Join the selected note with the next one, or the selected notes into one (M)");
    resetNoteButton.setTooltip ("Take back everything done to the selected notes (Backspace)");
    snapNoteButton.onClick = [this] { roll.snapSelected(); };
    straightenButton.onClick = [this] { roll.straightenSelected(); };
    resetNoteButton.onClick = [this] { roll.resetSelected(); };
    splitButton.onClick = [this] { juce::int64 a, b; if (roll.selectionSpan (a, b)) { const auto at = playback.position(); roll.splitAtSample (at > a && at < b ? at : (a + b) / 2); } };
    mergeButton.onClick = [this] { roll.mergeSelected(); };

    // ---- audio --------------------------------------------------------------------------------------------------------
    if (withAudioDevice)
    {
        audioError = deviceManager.initialise (0, 2, nullptr, true);
        audioOn = audioError.isEmpty();
        if (audioOn)
        {
            player.setSource (&transport);
            deviceManager.addAudioCallback (&player);
        }
    }

    refreshInspector();
    refreshScalePanel();
    startTimerHz (30);
    setSize (1240, 760);
}

MainComponent::~MainComponent()
{
    stopTimer();
    job.reset();
    if (audioOn)
    {
        deviceManager.removeAudioCallback (&player);
        player.setSource (nullptr);
    }
    transport.setSource (nullptr);
    setLookAndFeel (nullptr);
}

void MainComponent::setupKnob (ui::ValueKnob& knob, const juce::String& caption, double lo, double hi, double step, double reset, const juce::String& suffix, int decimals,
                               const juce::String& tooltip)
{
    knob.setup (caption, lo, hi, step, reset, [suffix, decimals] (double v) { return numberText (v, decimals) + suffix; },
                [] (const juce::String& t) -> double { return t.retainCharacters ("+-.0123456789").getDoubleValue(); }, tooltip);
}

// ---- files ------------------------------------------------------------------------------------------------------------------
void MainComponent::loadFile (const juce::File& f)
{
    juce::File audio = f;
    std::unique_ptr<juce::XmlElement> project;
    if (f.hasFileExtension ("orpheus"))
    {
        project = juce::XmlDocument::parse (f);
        if (project == nullptr || ! project->hasTagName ("Orpheus"))
        {
            showError (f.getFileName() + " is not an Orpheus project.");
            return;
        }
        audio = juce::File (project->getStringAttribute ("source"));
        if (! audio.existsAsFile())
        {
            showError ("The audio of this project is not where it was:\n" + audio.getFullPathName());
            return;
        }
    }
    job.reset();
    transport.stop();
    juce::String error;
    if (! session.load (audio, error))
    {
        showError (error);
        return;
    }
    pendingProject = std::move (project);
    projectFile = f.hasFileExtension ("orpheus") ? f : juce::File();
    if (audioOn)
        transport.setSource (&playback, 0, nullptr, session.sampleRate);
    playback.setNextReadPosition (0);
    roll.clearSelection();
    startAnalysis();
}

void MainComponent::startAnalysis()
{
    roll.overlayText = "Analysing " + session.file.getFileName();
    roll.overlayProgress = 0.0f;
    job = std::make_unique<Job> (*this, session.mono, session.sampleRate, ++analysisSerial);
    job->startThread();
    roll.repaint();
}

void MainComponent::analysisDone (Session::Analysis&& result)
{
    job.reset();
    if (result.contour.numFrames() == 0)
    {
        roll.overlayText = "Could not analyse this audio";
        roll.overlayProgress = -1.0f;
        return;
    }
    session.adopt (std::move (result));
    if (pendingProject != nullptr)
    {
        if (! session.applyXml (*pendingProject))
            showError ("This project does not belong to that recording. The audio was opened without it.");
        pendingProject.reset();
    }
    roll.overlayText = {};
    roll.overlayProgress = -1.0f;
    roll.fitToSession();
    refreshScalePanel();
    refreshInspector();
    updateLoop();
    repaint();
}

void MainComponent::analyseNow()
{
    job.reset();
    session.adopt (Session::analyse (session.mono, session.sampleRate));
    roll.overlayText = {};
    roll.overlayProgress = -1.0f;
    roll.fitToSession();
    refreshScalePanel();
    refreshInspector();
}

void MainComponent::say (const juce::String& message)
{
    notice = message;
    noticeUntil = juce::Time::getMillisecondCounter() + 5000;
    repaint (notePanel);
}

void MainComponent::showError (const juce::String& message)
{
    juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, "Orpheus", message);
}

void MainComponent::chooseAndOpen()
{
    chooser = std::make_unique<juce::FileChooser> ("Open audio or project", session.file.getParentDirectory(), kAudioWildcard);
    chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                          [this] (const juce::FileChooser& fc)
                          {
                              if (fc.getResult().existsAsFile())
                                  loadFile (fc.getResult());
                          });
}

void MainComponent::chooseAndSave()
{
    if (! session.isAnalysed())
        return;
    const auto start = projectFile != juce::File() ? projectFile : session.file.withFileExtension ("orpheus");
    chooser = std::make_unique<juce::FileChooser> ("Save project", start, "*.orpheus");
    chooser->launchAsync (juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles | juce::FileBrowserComponent::warnAboutOverwriting,
                          [this] (const juce::FileChooser& fc)
                          {
                              auto f = fc.getResult();
                              if (f == juce::File())
                                  return;
                              f = f.withFileExtension ("orpheus");
                              if (session.toXml()->writeTo (f))
                                  projectFile = f;
                              else
                                  showError ("Cannot write " + f.getFullPathName());
                          });
}

void MainComponent::chooseAndExport()
{
    if (! session.isAnalysed())
        return;
    const auto start = session.file.getSiblingFile (session.file.getFileNameWithoutExtension() + "_tuned.wav");
    chooser = std::make_unique<juce::FileChooser> ("Export corrected audio", start, "*.wav");
    chooser->launchAsync (juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles | juce::FileBrowserComponent::warnAboutOverwriting,
                          [this] (const juce::FileChooser& fc)
                          {
                              auto f = fc.getResult();
                              if (f == juce::File())
                                  return;
                              juce::String error;
                              if (! session.exportWav (f.withFileExtension ("wav"), error))
                                  showError (error);
                          });
}

// ---- transport ------------------------------------------------------------------------------------------------------------
void MainComponent::togglePlay()
{
    if (! session.isLoaded() || ! audioOn)
        return;
    if (transport.isPlaying())
    {
        transport.stop();
        return;
    }
    if (playback.position() >= session.numSamples() - 1)
        seek (0);
    updateLoop();
    transport.start();
}

void MainComponent::seek (juce::int64 sample)
{
    sample = juce::jlimit<juce::int64> (0, session.numSamples(), sample);
    if (audioOn)
        transport.setPosition ((double) sample / session.sampleRate);
    else
        playback.setNextReadPosition (sample);
    roll.repaint();
}

void MainComponent::updateLoop()
{
    juce::int64 a = 0, b = session.numSamples();
    if (roll.selectionSpan (a, b))
    {
        const auto pad = (juce::int64) (0.12 * session.sampleRate);
        a = std::max<juce::int64> (0, a - pad);
        b = std::min<juce::int64> (session.numSamples(), b + pad);
    }
    playback.loopStart.store (a);
    playback.loopEnd.store (b);
    playback.looping.store (loopButton.getToggleState());
    roll.repaint();
}

void MainComponent::undo()
{
    if (session.undo())
    {
        roll.pruneSelection();
        refreshInspector();
        refreshScalePanel();
        roll.repaint();
    }
}

void MainComponent::redo()
{
    if (session.redo())
    {
        roll.pruneSelection();
        refreshInspector();
        refreshScalePanel();
        roll.repaint();
    }
}

// ---- the panels ---------------------------------------------------------------------------------------------------------------
void MainComponent::applyToSelection (const std::function<void (Note&)>& change)
{
    const auto which = roll.selectedIndices();
    if (which.empty())
        return;
    int first = std::numeric_limits<int>::max(), last = 0;
    for (const auto i : which)
    {
        change (session.notes[i]);
        first = std::min (first, session.notes[i].start);
        last = std::max (last, session.notes[i].end);
    }
    session.changed (first, last);
    roll.repaint();
    // the knobs that were not touched follow the new state (the shift moves when CORRECT does)
    refreshInspector();
}

void MainComponent::refreshScalePanel()
{
    keyBox.setSelectedId (session.key.root + 1, juce::dontSendNotification);
    scaleBox.setSelectedId (session.key.scale + 1, juce::dontSendNotification);
    keyBox.setEnabled (session.key.scale != kChromatic);
    tuningKnob.setValue (session.key.a4Hz);
    glideKnob.setValue (session.glideMs);
    amountKnob.setValue (correctAmount);
}

void MainComponent::refreshInspector()
{
    const auto which = roll.selectedIndices();
    const bool any = ! which.empty();
    for (auto* k : { &correctKnob, &shiftKnob, &driftKnob, &vibratoKnob, &formantKnob, &gainKnob })
        k->setActive (any);
    for (auto* b : { &snapNoteButton, &straightenButton, &resetNoteButton })
        b->setEnabled (any);
    splitButton.setEnabled (any);
    mergeButton.setEnabled (any);
    undoButton.setEnabled (session.canUndo());
    redoButton.setEnabled (session.canRedo());
    const bool ready = session.isAnalysed() && job == nullptr;
    for (auto* b : { &saveButton, &exportButton, &correctAllButton, &detectKeyButton, &detectTuneButton, &resetAllButton, &playButton, &rewindButton })
        b->setEnabled (ready);

    const auto spread = [&] (auto value, double& lo, double& hi)
    {
        lo = 1.0e9;
        hi = -1.0e9;
        for (const auto i : which)
        {
            const double v = value (session.notes[i]);
            lo = std::min (lo, v);
            hi = std::max (hi, v);
        }
        return std::fabs (hi - lo) > 1.0e-4;
    };
    const auto show = [&] (ui::ValueKnob& knob, auto value, double scale)
    {
        if (! any)
        {
            knob.setValue (knob.getValue(), false);
            return;
        }
        double lo, hi;
        const bool mixed = spread (value, lo, hi);
        knob.setValue (lo * scale, mixed);
    };
    show (correctKnob, [&] (const Note& n) { return std::clamp ((double) edit::correctionOf (n, session.key), 0.0, 1.0); }, 100.0);
    show (shiftKnob, [] (const Note& n) { return (double) n.shift(); }, 100.0);
    show (driftKnob, [] (const Note& n) { return (double) n.drift; }, 100.0);
    show (vibratoKnob, [] (const Note& n) { return (double) n.vibrato; }, 100.0);
    show (formantKnob, [] (const Note& n) { return (double) n.formant; }, 1.0);
    show (gainKnob, [] (const Note& n) { return (double) n.gainDb; }, 1.0);

    if (! any)
        infoLine = session.isAnalysed() ? juce::String (session.notes.size()) + " notes   click a note to select it" : juce::String();
    else if (which.size() == 1)
    {
        const auto& n = session.notes[which[0]];
        const double grid = session.key.gridOffset();
        infoLine = "sung " + noteText (n.centre, grid) + "   ->   " + noteText (n.target, grid) + "   " + timeText (n.start * session.contour.hop / session.sampleRate)
                   + "  " + numberText ((n.end - n.start) * session.contour.hop * 1000.0 / session.sampleRate) + " ms";
    }
    else
        infoLine = juce::String ((int) which.size()) + " notes selected";
    repaint (notePanel);
}

// ---- keys --------------------------------------------------------------------------------------------------------------------
bool MainComponent::keyPressed (const juce::KeyPress& key)
{
    const auto mods = key.getModifiers();
    const bool cmd = mods.isCommandDown() || mods.isCtrlDown();
    const int code = key.getKeyCode();
    const auto character = juce::CharacterFunctions::toLowerCase (key.getTextCharacter());

    if (cmd && character == 'z')
    {
        mods.isShiftDown() ? redo() : undo();
        return true;
    }
    if (cmd && character == 'o') { chooseAndOpen(); return true; }
    if (cmd && character == 's') { chooseAndSave(); return true; }
    if (cmd && character == 'e') { chooseAndExport(); return true; }
    if (cmd && character == 'a') { roll.selectAll(); return true; }
    if (cmd && (character == '+' || character == '=')) { roll.zoomTime (1.4, roll.getWidth() * 0.5); return true; }
    if (cmd && character == '-') { roll.zoomTime (1.0 / 1.4, roll.getWidth() * 0.5); return true; }
    if (cmd)
        return false;
    if (character == '+' || character == '=') { roll.zoomTime (1.5, roll.getWidth() * 0.5); return true; }
    if (character == '-') { roll.zoomTime (1.0 / 1.5, roll.getWidth() * 0.5); return true; }

    if (code == juce::KeyPress::spaceKey) { togglePlay(); return true; }
    if (code == juce::KeyPress::homeKey) { seek (0); return true; }
    if (character == 'f' && ! mods.isAnyModifierKeyDown()) { roll.fitToSession(); return true; }
    if (code == juce::KeyPress::tabKey) { originalButton.setToggleState (! originalButton.getToggleState(), juce::sendNotification); return true; }
    if (code == juce::KeyPress::backspaceKey || code == juce::KeyPress::deleteKey) { roll.resetSelected(); return true; }
    if (code == juce::KeyPress::returnKey) { roll.snapSelected(); return true; }
    if (code == juce::KeyPress::upKey || code == juce::KeyPress::downKey)
    {
        const double step = mods.isShiftDown() ? 0.1 : mods.isAltDown() ? 0.01 : 1.0;
        roll.nudgeSelected (code == juce::KeyPress::upKey ? step : -step);
        return true;
    }
    if (code == juce::KeyPress::leftKey || code == juce::KeyPress::rightKey)
    {
        // the previous / next note
        const auto which = roll.selectedIndices();
        if (session.notes.empty())
            return true;
        size_t target = 0;
        if (! which.empty())
            target = code == juce::KeyPress::rightKey ? std::min (session.notes.size() - 1, which.back() + 1) : (which.front() > 0 ? which.front() - 1 : 0);
        else
        {
            const int frame = session.contour.frameAtSample ((double) playback.position());
            target = 0;
            while (target + 1 < session.notes.size() && session.notes[target].end <= frame)
                ++target;
        }
        roll.select ({ target });
        const auto& n = session.notes[target];
        roll.showSample ((double) n.start * session.contour.hop);
        seek ((juce::int64) n.start * session.contour.hop);
        return true;
    }
    if (character == 's' && ! mods.isAnyModifierKeyDown())
    {
        juce::int64 a, b;
        if (roll.selectionSpan (a, b))
        {
            const auto at = playback.position();
            roll.splitAtSample (at > a && at < b ? at : (a + b) / 2);
        }
        return true;
    }
    if (character == 'm') { roll.mergeSelected(); return true; }
    if (character == 'l') { loopButton.setToggleState (! loopButton.getToggleState(), juce::sendNotification); return true; }
    return false;
}

// ---- timer ---------------------------------------------------------------------------------------------------------------------
void MainComponent::timerCallback()
{
    if (job != nullptr)
    {
        roll.overlayProgress = job->progress.load();
        roll.repaint();
    }
    const bool playing = audioOn && transport.isPlaying();
    if (playing)
        roll.followPlayhead();
    if (playButton.getToggleState() != playing)
        playButton.setToggleState (playing, juce::dontSendNotification);
    if (notice.isNotEmpty() && juce::Time::getMillisecondCounter() >= noticeUntil)
    {
        notice = {};
        repaint (notePanel);
    }
    if (playing || lastVersion != session.version.load())
    {
        lastVersion = session.version.load();
        repaint (timeBox);
        roll.repaint();
        undoButton.setEnabled (session.canUndo());
        redoButton.setEnabled (session.canRedo());
    }
}

// ---- layout & painting -----------------------------------------------------------------------------------------------------------
void MainComponent::resized()
{
    auto r = getLocalBounds();
    headerArea = r.removeFromTop (66);
    auto h = headerArea.reduced (10, 8);
    nameplate = h.removeFromLeft (190);
    h.removeFromLeft (14);

    const auto place = [&] (juce::Component& c, int w, int gap = 6)
    {
        c.setBounds (h.removeFromLeft (w).withSizeKeepingCentre (w, 30));
        h.removeFromLeft (gap);
    };
    place (rewindButton, 40);
    place (playButton, 74);
    place (loopButton, 64);
    place (originalButton, 84, 14);
    timeBox = h.removeFromLeft (130).withSizeKeepingCentre (130, 34);
    h.removeFromLeft (14);
    place (snapButton, 64, 22);
    // right-aligned: file and history
    auto right = h.removeFromRight (400);
    for (auto* b : { &openButton, &saveButton, &exportButton })
        b->setBounds (right.removeFromLeft (76).withSizeKeepingCentre (72, 30)), right.removeFromLeft (4);
    right.removeFromLeft (10);
    undoButton.setBounds (right.removeFromLeft (60).withSizeKeepingCentre (56, 30));
    redoButton.setBounds (right.removeFromLeft (60).withSizeKeepingCentre (56, 30));

    auto bottom = r.removeFromBottom (168).reduced (8, 6);
    scalePanel = bottom.removeFromLeft (500);
    bottom.removeFromLeft (8);
    notePanel = bottom;
    auto navRow = r.removeFromBottom (46).reduced (8, 4);
    fitButton.setBounds (navRow.removeFromRight (56).withSizeKeepingCentre (52, 28));
    zoomInButton.setBounds (navRow.removeFromRight (40).withSizeKeepingCentre (36, 28));
    zoomOutButton.setBounds (navRow.removeFromRight (40).withSizeKeepingCentre (36, 28));
    navRow.removeFromRight (4);
    navigator.setBounds (navRow);
    roll.setBounds (r.reduced (8, 2));

    // scale panel
    {
        auto p = scalePanel.reduced (18, 12);
        p.removeFromTop (12);
        auto row = p.removeFromTop (28);
        keyBox.setBounds (row.removeFromLeft (64));
        row.removeFromLeft (6);
        scaleBox.setBounds (row.removeFromLeft (150));
        row.removeFromLeft (10);
        detectKeyButton.setBounds (row.removeFromLeft (94));
        row.removeFromLeft (4);
        detectTuneButton.setBounds (row.removeFromLeft (94));
        p.removeFromTop (8);
        auto knobs = p.removeFromTop (84);
        tuningKnob.setBounds (knobs.removeFromLeft (92));
        glideKnob.setBounds (knobs.removeFromLeft (92));
        amountKnob.setBounds (knobs.removeFromLeft (92));
        knobs.removeFromLeft (8);
        auto col = knobs.reduced (0, 6);
        correctAllButton.setBounds (col.removeFromTop (30));
        col.removeFromTop (6);
        resetAllButton.setBounds (col.removeFromTop (30));
    }
    // note panel
    {
        auto p = notePanel.reduced (18, 12);
        p.removeFromTop (12);
        auto info = p.removeFromTop (20);
        (void) info;
        p.removeFromTop (6);
        auto buttons = p.removeFromRight (96);
        auto knobs = p;
        const int w = knobs.getWidth() / 6;
        for (auto* k : { &correctKnob, &shiftKnob, &driftKnob, &vibratoKnob, &formantKnob, &gainKnob })
            k->setBounds (knobs.removeFromLeft (w).withTrimmedBottom (0));
        const int bh = (buttons.getHeight() - 4 * 4) / 5;
        for (auto* b : { &snapNoteButton, &straightenButton, &splitButton, &mergeButton, &resetNoteButton })
            b->setBounds (buttons.removeFromTop (bh)), buttons.removeFromTop (4);
    }
}

void MainComponent::paint (juce::Graphics& g)
{
    g.fillAll (colours::background);
    ui::hw::drawHeaderPlate (g, headerArea.toFloat());
    ui::hw::drawNameplate (g, nameplate, kProjectWord, kSecondWord, "Offline pitch editor", session.isAnalysed() && job == nullptr, 24.0f);

    // the clock
    ui::hw::drawGlass (g, timeBox.toFloat(), 3.0f, colours::amber);
    g.setColour (colours::amber);
    g.setFont (ui::lcdFont (19.0f, juce::Font::bold));
    g.drawText (timeText ((double) playback.position() / std::max (1.0, session.sampleRate)), timeBox.reduced (8, 0), juce::Justification::centred);
    ui::hw::drawGlassOverlay (g, timeBox.toFloat(), 3.0f, false);

    ui::hw::drawModule (g, scalePanel.toFloat());
    ui::hw::drawModule (g, notePanel.toFloat());
    ui::hw::drawLabel (g, "Scale", scalePanel.reduced (20, 10).withHeight (14), juce::Justification::left, colours::text, 10.5f);
    ui::hw::drawLabel (g, "Note", notePanel.reduced (20, 10).withHeight (14), juce::Justification::left, colours::text, 10.5f);

    // what the selection is (or what the last button found), on the glass in the corner of the note panel
    const bool noticeShown = notice.isNotEmpty() && juce::Time::getMillisecondCounter() < noticeUntil;
    if (infoLine.isNotEmpty() || noticeShown)
    {
        const auto box = notePanel.reduced (84, 9).withHeight (20).withTrimmedRight (20);
        ui::hw::drawGlass (g, box.toFloat(), 3.0f, colours::amber);
        g.setColour (colours::amber);
        g.setFont (ui::lcdFont (11.5f));
        g.drawText (noticeShown ? notice : infoLine, box.reduced (8, 0), juce::Justification::centredLeft, true);
        ui::hw::drawGlassOverlay (g, box.toFloat(), 3.0f, false);
    }
    if (audioError.isNotEmpty())
    {
        g.setColour (colours::bad);
        g.setFont (ui::labelFont (9.5f));
        g.drawText ("NO AUDIO OUTPUT: " + audioError, getLocalBounds().removeFromTop (10), juce::Justification::centred);
    }
}
} // namespace orpheus::gui
