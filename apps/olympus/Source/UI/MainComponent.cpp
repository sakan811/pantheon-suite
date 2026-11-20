// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 The Pantheon authors

#include "MainComponent.h"

namespace olympus::gui
{
namespace
{
const char* const kAudioWildcard = "*.wav;*.aif;*.aiff;*.flac;*.mp3;*.m4a;*.ogg;*.caf";
constexpr int kMargin = 16, kHeaderHeight = 84;
constexpr int kTicksPerBeat = 960;

juce::String timeText (juce::int64 sample, double rate)
{
    const double s = rate > 0.0 ? (double) sample / rate : 0.0;
    const int minutes = (int) (s / 60.0);
    return juce::String (minutes).paddedLeft ('0', 2) + ":" + juce::String (std::fmod (s, 60.0), 2).paddedLeft ('0', 5);
}

// "5.2.480  00:09.50": bar, beat and tick of the position (from the tempo map), then the time.
juce::String positionText (const Session& s, juce::int64 sample)
{
    const auto bb = s.tempo.barBeatAt ((double) sample);
    const int ticks = juce::jlimit (0, kTicksPerBeat - 1, (int) std::floor (bb.fraction * kTicksPerBeat + 1.0e-9));
    return juce::String (bb.bar) + "." + juce::String (bb.beat) + "." + juce::String (ticks).paddedLeft ('0', 3) + "  "
           + timeText (sample, s.sampleRate > 0.0 ? s.sampleRate : 48000.0);
}

// The tempo as the field shows it: a whole number without decimals, else one decimal.
juce::String bpmText (double bpm)
{
    if (std::abs (bpm - std::round (bpm)) < 0.05) return juce::String ((int) std::round (bpm));
    return juce::String (bpm, 1);
}

juce::juce_wchar lowerCase (juce::juce_wchar c) { return (c >= 'A' && c <= 'Z') ? c + ('a' - 'A') : c; }

// The tempo field's look: a glass display, amber figures, no outline of its own.
void styleField (juce::TextEditor& field)
{
    field.setFont (ui::lcdFont (14.0f));
    field.setJustification (juce::Justification::centred);
    field.setColour (juce::TextEditor::backgroundColourId, colours::glass);
    field.setColour (juce::TextEditor::textColourId, colours::amber);
    field.setColour (juce::TextEditor::outlineColourId, juce::Colours::transparentBlack);
    field.setColour (juce::TextEditor::focusedOutlineColourId, colours::amber);
    field.setColour (juce::TextEditor::highlightColourId, colours::amber.withAlpha (0.35f));
    field.setColour (juce::TextEditor::highlightedTextColourId, colours::amber);
    field.setInputRestrictions (6, "0123456789.");
    field.setSelectAllWhenFocused (true);
}
} // namespace

// ---- PluginWindow ------------------------------------------------------------------------------------------------------------
PluginWindow::PluginWindow (const juce::String& title, juce::AudioProcessorEditor* editor, std::function<void()> closed)
    : juce::DocumentWindow (title, colours::background, juce::DocumentWindow::closeButton), onClosed (std::move (closed))
{
    setUsingNativeTitleBar (true);
    setContentOwned (editor, true);
    setResizable (editor->isResizable(), false);
    setAlwaysOnTop (false);
    centreAroundComponent (nullptr, getWidth(), getHeight());
    setVisible (true);
}

// ---- audio -------------------------------------------------------------------------------------------------------------------
// The callback plays the session block by block. A block never crosses the loop's end: it is cut there, and the rest comes from the
// loop's start. Without a loop the playback stops at the end of the audio (the longest track, and the latency of its chains).
void MainComponent::Source::getNextAudioBlock (const juce::AudioSourceChannelInfo& info)
{
    info.clearActiveBufferRegion();
    if (! playing.load())
        return;
    const juce::int64 start = loopStart.load(), end = loopEnd.load(), audioEnd = endOfAudio.load();
    const bool wrap = loopOn.load() && end > start;
    juce::int64 pos = position.load();
    int done = 0;
    while (done < info.numSamples)
    {
        if (wrap && pos >= end)
            pos = start;
        else if (! wrap && pos >= audioEnd)
        {
            playing = false;
            break;
        }
        const juce::int64 room = wrap ? end - pos : audioEnd - pos;
        const int n = (int) juce::jmin ((juce::int64) juce::jmin (kBlockSize, info.numSamples - done), room);
        chunk.setSize (2, n, false, false, true);
        owner.session.processBlock (chunk, pos);
        for (int ch = 0; ch < info.buffer->getNumChannels(); ++ch)
            info.buffer->copyFrom (ch, info.startSample + done, chunk, juce::jmin (ch, 1), 0, n);
        pos += n;
        done += n;
    }
    position = pos;
}

// ---- the window --------------------------------------------------------------------------------------------------------------
MainComponent::MainComponent() : MainComponent (Options {}) {}

MainComponent::MainComponent (Options options) : opts (options)
{
    setLookAndFeel (&look);
    setWantsKeyboardFocus (true);
    settings = Settings::load (opts.settingsFile);

    ask = [] (const juce::String& message, const juce::String& button, std::function<void()> proceed)
    {
        juce::AlertWindow::showOkCancelBox (juce::MessageBoxIconType::QuestionIcon, "Olympus", message, button, "Cancel", nullptr,
                                            juce::ModalCallbackFunction::create ([proceed] (int result) { if (result == 1) proceed(); }));
    };
    reportError = [] (const juce::String& message) { juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, "Olympus", message); };
    makeEditorWindow = [] (const juce::String& title, juce::AudioProcessorEditor* pluginEditor, std::function<void()> closed) -> std::unique_ptr<juce::Component>
    {
        return std::make_unique<PluginWindow> (title, pluginEditor, std::move (closed));
    };

    // the arrangement reports what the user did; the window owns the transport, the files, the menus and the plugin windows
    arrange.onSeek = [this] (juce::int64 s) { seek (s); };
    arrange.onStructure = [this] { syncTransport(); updateTitle(); updateTransport(); };
    arrange.onTrackMenu = [this] (int track) { showTrackMenu (track); };
    arrange.onRemoveTrack = [this] (int track) { removeTrack (track); };
    arrange.onFilesDropped = [this] (const juce::StringArray& files, int track, juce::int64 at) { importFiles (files, track, at); };
    arrange.reportError = [this] (const juce::String& message) { showError (message); };
    addAndMakeVisible (arrange);

    for (auto* b : { &newButton, &openButton, &saveButton, &saveAsButton, &addTrackButton, &importButton, &pluginsButton, &renderButton,
                     &rewindButton, &undoButton, &redoButton, &zoomOutButton, &zoomInButton, &zoomFitButton })
        addAndMakeVisible (*b);
    for (auto* b : { &playButton, &loopButton, &metronomeButton, &snapButton })
    {
        b->setClickingTogglesState (false);   // the session says what state they are in: see updateTransport
        addAndMakeVisible (*b);
    }
    addAndMakeVisible (guideButton);
    addAndMakeVisible (bpmField);
    addAndMakeVisible (signatureBox);
    addAndMakeVisible (gridBox);
    // a focused button would take Space and Return for itself: the keys stay with the window
    for (auto* c : std::initializer_list<juce::Component*> { &newButton, &openButton, &saveButton, &saveAsButton, &addTrackButton, &importButton, &pluginsButton,
                                                             &renderButton, &rewindButton, &undoButton, &redoButton, &zoomOutButton, &zoomInButton, &zoomFitButton,
                                                             &playButton, &loopButton, &metronomeButton, &snapButton, &guideButton, &signatureBox, &gridBox })
        c->setWantsKeyboardFocus (false);

    newButton.onClick = [this] { askToDiscard ("Start a new project and lose them?", [this] { newProject(); }); };
    openButton.onClick = [this] { askToDiscard ("Open another project and lose them?", [this] { chooseAndOpen(); }); };
    saveButton.onClick = [this] { saveProject(); };
    saveAsButton.onClick = [this] { saveProjectAs(); };
    addTrackButton.onClick = [this] { addEmptyTrack(); };
    importButton.onClick = [this] { chooseAndImport(); };
    pluginsButton.onClick = [this] { showPluginsMenu(); };
    renderButton.onClick = [this] { chooseAndRender(); };
    rewindButton.onClick = [this] { rewind(); updateTransport(); };
    playButton.onClick = [this] { togglePlay(); updateTransport(); };
    loopButton.onClick = [this] { toggleLoop(); };
    metronomeButton.onClick = [this] { toggleMetronome(); };
    snapButton.onClick = [this] { editor.setSnap (! session.snapOn); updateTransport(); };
    undoButton.onClick = [this] { undoEdit(); };
    redoButton.onClick = [this] { redoEdit(); };
    zoomOutButton.onClick = [this] { zoomBy (1.0 / 1.5); };
    zoomInButton.onClick = [this] { zoomBy (1.5); };
    zoomFitButton.onClick = [this] { arrange.zoomToFit(); arrange.repaint(); };

    newButton.setTooltip ("Empty the session.");
    openButton.setTooltip ("Open a saved project (.olympus).");
    saveButton.setTooltip ("Save the project: the tracks, clips, chains, tempo, markers and loop, and every plugin's settings. Cmd+S.");
    addTrackButton.setTooltip ("Add an empty track.");
    importButton.setTooltip ("Audio files: each one becomes a track with its clip at the playhead. You can also drop them on the window or a lane.");
    pluginsButton.setTooltip ("Where plugins (VST3, and Audio Units on macOS) are looked for, which were found, and a rescan.");
    renderButton.setTooltip ("Write one WAV per audible track, after its clips, chain, fader and pan, and the mix.");
    playButton.setTooltip ("Play from the playhead, or stop (Space).");
    rewindButton.setTooltip ("Back to the loop's start when the loop is on, else to the start (Return).");
    loopButton.setTooltip ("Loop the region between the loop markers (L). A loop with no range yet covers the first four bars.");
    metronomeButton.setTooltip ("A click on every beat, a higher one on the bar. Not in the stems. (Undoable.)");
    snapButton.setTooltip ("Snap moves and edges to the grid.");
    undoButton.setTooltip ("Undo the last edit (Cmd+Z). Adding or removing a track or a plugin cannot be undone.");
    redoButton.setTooltip ("Redo (Shift+Cmd+Z).");
    zoomOutButton.setTooltip ("Zoom out (-).");
    zoomInButton.setTooltip ("Zoom in (+).");
    zoomFitButton.setTooltip ("Zoom so the whole session fits.");
    bpmField.setTooltip ("The tempo in beats per minute (20 to 300). Type a number and press Return. With tempo changes in the project, the field shows the tempo at the playhead.");
    signatureBox.setTooltip ("The time signature of the project.");
    gridBox.setTooltip ("The grid that snap and the lines of the ruler follow.");

    styleField (bpmField);
    bpmField.onReturnKey = [this] { applyBpm(); grabKeyboardFocus(); };
    bpmField.onFocusLost = [this] { applyBpm(); };

    for (int i = 0; i < signatures.size(); ++i)
        signatureBox.addItem (signatures[i], i + 1);
    signatureBox.setColour (juce::ComboBox::textColourId, colours::amber);
    signatureBox.onChange = [this]
    {
        const int index = signatureBox.getSelectedItemIndex();
        if (! juce::isPositiveAndBelow (index, signatures.size())) return;
        const auto parts = juce::StringArray::fromTokens (signatures[index], "/", "");
        editor.setSignature (parts[0].getIntValue(), parts[1].getIntValue());
        updateTransport();
        arrange.repaint();
    };
    for (int g = 0; g <= (int) Grid::ThirtySecond; ++g)
        gridBox.addItem (gridName ((Grid) g), g + 1);
    gridBox.onChange = [this]
    {
        const int index = gridBox.getSelectedItemIndex();
        if (index >= 0) editor.setGrid ((Grid) index);
        updateTransport();
        arrange.repaint();
    };

    if (opts.audioDevice)
    {
        deviceManager.initialiseWithDefaultDevices (0, 2);
        player.setSource (&source);
        deviceManager.addAudioCallback (&player);
    }
    if (opts.scanPlugins)
    {
        // the last scan is loaded; the folders are read again only when there is none, or when the user asks (PLUGINS > Rescan plugins)
        session.catalog.setSuiteOnly (settings.scanSuiteOnly);
        const bool cached = session.catalog.loadCache (Settings::pluginCacheFile (opts.settingsFile));
        if (! cached || session.catalog.names().isEmpty())
            juce::MessageManager::callAsync ([safe = juce::Component::SafePointer<MainComponent> (this)] { if (safe != nullptr) safe->scanPlugins (false); });
    }
    syncTransport();
    updateTransport();
    startTimerHz (30);
}

MainComponent::~MainComponent()
{
    stopTimer();
    cancelPendingUpdate();
    if (opts.audioDevice)
    {
        deviceManager.removeAudioCallback (&player);
        player.setSource (nullptr);
    }
    closeEditors();
    setLookAndFeel (nullptr);
}

void MainComponent::scanPlugins (bool rescan)
{
    if (scanning) return;
    scanning = true;
    session.catalog.setSuiteOnly (settings.scanSuiteOnly);
    ScanOptions options;
    options.rescan = rescan;
    options.deadMansPedal = Settings::scanPedalFile (opts.settingsFile);
    session.catalog.scan (settings.searchPath(), options);
    session.catalog.saveCache (Settings::pluginCacheFile (opts.settingsFile));
    scanning = false;
    // a project opened before the scan finished: give its missing plugins another try
    repaint();
}

void MainComponent::updateTitle()
{
    const auto title = windowTitle();
    if (title != shownTitle) { shownTitle = title; if (onTitleChanged) onTitleChanged(); }
}

juce::String MainComponent::windowTitle() const { return "Olympus - " + session.name + (session.dirty ? " *" : ""); }

void MainComponent::showError (const juce::String& message) { if (reportError) reportError (message); }

void MainComponent::askToDiscard (const juce::String& action, std::function<void()> proceed)
{
    if (! session.dirty || session.tracks.empty()) { proceed(); return; }
    if (ask) ask ("The project has changes that are not saved. " + action, "Discard", std::move (proceed));
    else proceed();
}

// Puts the session's transport on the controls and the audio callback's copy of the loop and the end. The timer calls it; so does every
// action, so a button shows what it did at once.
void MainComponent::updateTransport()
{
    const bool playing = isPlaying();
    playButton.setToggleState (playing, juce::dontSendNotification);
    if (playButton.getButtonText() != (playing ? "Stop" : "Play")) playButton.setButtonText (playing ? "Stop" : "Play");
    loopButton.setToggleState (session.loopOn, juce::dontSendNotification);
    metronomeButton.setToggleState (session.metronomeOn, juce::dontSendNotification);
    snapButton.setToggleState (session.snapOn, juce::dontSendNotification);
    if (gridBox.getSelectedItemIndex() != (int) session.grid) gridBox.setSelectedItemIndex ((int) session.grid, juce::dontSendNotification);
    showSignature (session.tempo.numerator, session.tempo.denominator);
    undoButton.setEnabled (editor.canUndo());
    redoButton.setEnabled (editor.canRedo());

    if (! bpmField.hasKeyboardFocus (true))
    {
        // one tempo: the field sets it. Tempo changes: the field shows the tempo at the playhead, and the ruler's flags are where they are edited.
        const bool single = session.tempo.points.size() <= 1;
        const double bpm = single ? session.tempo.bpmAtBeat (0.0) : session.tempo.bpmAtSample ((double) position());
        const auto text = bpmText (bpm);
        if (bpmField.getText() != text) bpmField.setText (text, juce::dontSendNotification);
        bpmField.setReadOnly (! single);
    }
    repaint (positionBox);
    updateTitle();
}

void MainComponent::showSignature (int numerator, int denominator)
{
    const auto text = juce::String (numerator) + "/" + juce::String (denominator);
    if (! signatures.contains (text))
    {
        signatures.add (text);   // a signature set elsewhere (an agent, a project) is listed too
        signatureBox.clear (juce::dontSendNotification);
        for (int i = 0; i < signatures.size(); ++i)
            signatureBox.addItem (signatures[i], i + 1);
    }
    const int index = signatures.indexOf (text);
    if (signatureBox.getSelectedItemIndex() != index) signatureBox.setSelectedItemIndex (index, juce::dontSendNotification);
}

// Reads the tempo field: a number from 20 to 300 sets the tempo at beat 0 (one undo step). Anything else is refused and the field goes back.
void MainComponent::applyBpm()
{
    if (session.tempo.points.size() > 1) { updateTransport(); return; }   // read-only: the tempo at the playhead
    const auto text = bpmField.getText().trim();
    const double bpm = text.getDoubleValue();
    if (! text.containsOnly ("0123456789.") || text.isEmpty() || bpm < TempoMap::kMinBpm || bpm > TempoMap::kMaxBpm)
    {
        showError ("The tempo must be a number from 20 to 300 bpm.");
        bpmField.setText (bpmText (session.tempo.bpmAtBeat (0.0)), juce::dontSendNotification);
        return;
    }
    juce::String error;
    if (! editor.setTempo (0.0, bpm, error)) showError (error);
    bpmField.setText (bpmText (session.tempo.bpmAtBeat (0.0)), juce::dontSendNotification);
    updateTransport();
    arrange.repaint();
}

// Everything the arrangement shows is taken from the session again: its headers, its meters, the transport and the title.
void MainComponent::refreshNow()
{
    cancelPendingUpdate();
    syncTransport();
    arrange.refresh();
    updateTransport();
    repaint();
}

// ---- actions -----------------------------------------------------------------------------------------------------------------
void MainComponent::newProject()
{
    closeEditors();
    source.playing = false;
    session.newProject ("Untitled");
    editor.clearUndo();
    seek (0);
    arrange.selected = {};
    arrange.zoom = ArrangeView::kDefaultZoom;
    arrange.scrollTo (0);
    refreshNow();
}

void MainComponent::openProject (const juce::File& file)
{
    closeEditors();
    source.playing = false;
    juce::String error;
    if (! session.load (file, error))
    {
        showError (error);
        return;
    }
    editor.clearUndo();
    arrange.selected = {};
    seek (0);
    settings.lastProject = file.getFullPathName();
    settings.save (opts.settingsFile);
    refreshNow();
    arrange.zoomToFit();
    arrange.repaint();
    if (session.warnings.size() > 0)
        showError ("Opened with notes:\n\n" + session.warnings.joinIntoString ("\n"));
}

bool MainComponent::saveProject()
{
    if (session.projectFile == juce::File()) { saveProjectAs(); return false; }
    return saveTo (session.projectFile);
}

void MainComponent::saveProjectAs()
{
    chooser = std::make_unique<juce::FileChooser> ("Save the project", session.projectFile == juce::File() ? juce::File::getSpecialLocation (juce::File::userDocumentsDirectory) : session.projectFile, "*.olympus");
    chooser->launchAsync (juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles | juce::FileBrowserComponent::warnAboutOverwriting,
                          [this] (const juce::FileChooser& fc) { if (fc.getResult() != juce::File()) saveTo (fc.getResult().withFileExtension ("olympus")); });
}

bool MainComponent::saveTo (const juce::File& file)
{
    juce::String error;
    if (! session.save (file, error)) { showError (error); return false; }
    settings.lastProject = file.getFullPathName();
    settings.save (opts.settingsFile);
    updateTitle();
    return true;
}

void MainComponent::addTracks (const juce::StringArray& files) { importFiles (files, -1, 0); }

void MainComponent::importFiles (const juce::StringArray& files, int track, juce::int64 at)
{
    juce::StringArray problems;
    juce::int64 next = juce::jmax ((juce::int64) 0, at);
    for (const auto& f : files)
    {
        const juce::File file (f);
        const bool fresh = track < 0;
        int target = track;
        if (fresh)
        {
            // each file is a track of its own, its clip at `at`
            target = session.addEmptyTrack (file.getFileNameWithoutExtension());
            editor.clearUndo();   // the track is not undone; the clip is (below)
            next = juce::jmax ((juce::int64) 0, at);
        }
        juce::String error;
        const auto clip = editor.addClip (target, file, next, 0, 0, error);
        if (! clip.valid())
        {
            problems.add (error);
            if (fresh)
            {
                juce::String ignored;
                session.removeTrack (target, ignored);
                editor.clearUndo();
            }
            continue;
        }
        // files dropped on one track follow each other
        const juce::ScopedLock sl (session.lock);
        next = session.tracks[(size_t) target]->clips[(size_t) clip.clip].end();
    }
    refreshNow();
    if (problems.size() > 0)
        showError (problems.joinIntoString ("\n"));
}

void MainComponent::addEmptyTrack()
{
    session.addEmptyTrack ({});
    editor.clearUndo();
    refreshNow();
}

void MainComponent::undoEdit()
{
    editor.undo();
    refreshNow();
}

void MainComponent::redoEdit()
{
    editor.redo();
    refreshNow();
}

// Switches the loop on or off. Turned on with no range yet, the loop covers the first four bars or the whole audio, whichever is longer.
void MainComponent::toggleLoop()
{
    const bool on = ! session.loopOn;
    juce::int64 start = session.loopStart, end = session.loopEnd;
    if (on && end <= start)
    {
        start = 0;
        end = (juce::int64) std::llround (juce::jmax (session.tempo.sampleAtBar (5), (double) session.length()));
    }
    editor.setLoop (start, end, on);
    syncTransport();
    updateTransport();
    arrange.repaint();
}

void MainComponent::toggleMetronome()
{
    editor.setMetronome (! session.metronomeOn);
    updateTransport();
}

void MainComponent::zoomBy (double factor)
{
    arrange.zoom = juce::jlimit (0.05, 4000.0, arrange.zoom * factor);
    arrange.repaint();
}

void MainComponent::removeTrack (int index)
{
    closeEditors();
    juce::String error;
    if (session.removeTrack (index, error))
    {
        editor.clearUndo();
        arrange.selected = {};
    }
    else showError (error);
    requestRefresh();
}

juce::String MainComponent::renderTo (const juce::File& folder, const std::function<bool (float)>& progress)
{
    source.playing = false;
    closeEditors();
    RenderOptions options;
    options.folder = folder;
    RenderResult result;
    juce::String error;
    if (! session.renderStems (options, result, error, progress))
        return error.isNotEmpty() ? error : juce::String ("The render failed.");
    return {};
}

void MainComponent::togglePlay()
{
    if (source.playing.load()) { source.playing = false; return; }
    if (session.tracks.empty()) return;
    syncTransport();
    // at the end of the audio (and not looping), playing starts again from the beginning
    if (! session.loopOn && position() >= endOfPlay()) seek (0);
    session.prepare (false);
    source.playing = true;
}

void MainComponent::rewind()
{
    seek (session.loopOn && session.loopEnd > session.loopStart ? session.loopStart : 0);
}

// The playhead may be put anywhere from 0; playing stops at the end of the audio (or wraps at the loop's end).
void MainComponent::seek (juce::int64 sample) { source.position = juce::jmax ((juce::int64) 0, sample); }

void MainComponent::syncTransport()
{
    const juce::ScopedLock sl (session.lock);
    source.loopOn = session.loopOn;
    source.loopStart = session.loopStart;
    source.loopEnd = session.loopEnd;
    source.endOfAudio = session.length() + session.maxLatency();
}

void MainComponent::filesDropped (const juce::StringArray& files, int, int)
{
    // dropped outside the arrangement: a project opens, audio goes on new tracks at the playhead
    if (files.size() == 1 && juce::File (files[0]).hasFileExtension ("olympus"))
        askToDiscard ("Open it and lose them?", [this, f = files[0]] { openProject (juce::File (f)); });
    else
        importFiles (files, -1, position());
}

// ---- file choosers -----------------------------------------------------------------------------------------------------------
void MainComponent::chooseAndOpen()
{
    chooser = std::make_unique<juce::FileChooser> ("Open a project", juce::File::getSpecialLocation (juce::File::userDocumentsDirectory), "*.olympus");
    chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                          [this] (const juce::FileChooser& fc) { if (fc.getResult() != juce::File()) openProject (fc.getResult()); });
}

void MainComponent::chooseAndImport()
{
    const auto at = position();
    chooser = std::make_unique<juce::FileChooser> ("Import audio", juce::File::getSpecialLocation (juce::File::userMusicDirectory), kAudioWildcard);
    chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles | juce::FileBrowserComponent::canSelectMultipleItems,
                          [this, at] (const juce::FileChooser& fc)
                          {
                              juce::StringArray files;
                              for (auto& f : fc.getResults()) files.add (f.getFullPathName());
                              if (! files.isEmpty()) importFiles (files, -1, at);
                          });
}

void MainComponent::chooseFolder()
{
    chooser = std::make_unique<juce::FileChooser> ("The folder with the built plugins (.vst3), or the repository's plugins folder", juce::File::getSpecialLocation (juce::File::userHomeDirectory));
    chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories,
                          [this] (const juce::FileChooser& fc)
                          {
                              if (fc.getResult() == juce::File()) return;
                              settings.pluginFolders.addIfNotAlreadyThere (fc.getResult().getFullPathName());
                              settings.save (opts.settingsFile);
                              scanPlugins();
                          });
}

void MainComponent::chooseAndRender()
{
    if (session.tracks.empty()) { showError ("There are no tracks to render."); return; }
    const auto start = session.projectFile != juce::File() ? session.projectFile.getParentDirectory().getChildFile (session.name + " stems") : juce::File::getSpecialLocation (juce::File::userDocumentsDirectory);
    chooser = std::make_unique<juce::FileChooser> ("Render the stems into this folder", start);
    chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories,
                          [this] (const juce::FileChooser& fc)
                          {
                              const auto folder = fc.getResult();
                              if (folder == juce::File()) return;
                              struct Job : juce::ThreadWithProgressWindow
                              {
                                  Job (MainComponent& m, juce::File f) : juce::ThreadWithProgressWindow ("Rendering the stems", true, true), owner (m), folder (std::move (f)) {}
                                  void run() override
                                  {
                                      error = owner.renderTo (folder, [this] (float p) { setProgress ((double) p); return ! threadShouldExit(); });
                                  }
                                  MainComponent& owner;
                                  juce::File folder;
                                  juce::String error;
                              } job (*this, folder);
                              job.runThread();
                              if (job.error.isNotEmpty()) showError (job.error);
                              else folder.revealToUser();
                          });
}

// ---- plugins and tracks ------------------------------------------------------------------------------------------------------
void MainComponent::showPluginsMenu()
{
    juce::PopupMenu menu;
    menu.addSectionHeader ("Found: " + juce::String (session.catalog.names().size()) + " plugins");
    for (auto& n : session.catalog.names()) menu.addItem (n, false, false, nullptr);
    menu.addSeparator();
    menu.addItem ("Look in a folder...", [this] { chooseFolder(); });
    menu.addItem ("Rescan plugins", [this] { scanPlugins (true); requestRefresh(); });
    if (! settings.pluginFolders.isEmpty())
    {
        menu.addSeparator();
        menu.addSectionHeader ("Folders");
        for (auto& f : settings.pluginFolders)
            menu.addItem ("Forget " + juce::File (f).getFileName(), [this, f] { settings.pluginFolders.removeString (f); settings.save (opts.settingsFile); scanPlugins(); });
    }
    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&pluginsButton));
}

void MainComponent::showAddPluginMenu (int track)
{
    juce::PopupMenu menu;
    const auto names = session.catalog.names();
    if (names.isEmpty())
        menu.addItem ("No plugin found: use PLUGINS", false, false, nullptr);
    for (auto& n : names)
        menu.addItem (n, [this, track, n]
        {
            juce::String error;
            if (! session.addPlugin (track, n, -1, error)) showError (error);
            editor.clearUndo();
            requestRefresh();
        });
    menu.showMenuAsync (juce::PopupMenu::Options());
}

// The FX menu of a track: one submenu per plugin (its first item opens the plugin's window, then bypass, move and remove), and Add plugin.
void MainComponent::showTrackMenu (int track)
{
    if (! juce::isPositiveAndBelow (track, (int) session.tracks.size())) return;
    const auto& chain = session.tracks[(size_t) track]->chain;
    const int count = (int) chain.size();

    juce::PopupMenu menu;
    if (count == 0)
        menu.addItem ("No plugins on this track yet", false, false, nullptr);
    for (int s = 0; s < count; ++s)
    {
        const bool missing = chain[(size_t) s]->instance == nullptr;
        const bool bypassed = chain[(size_t) s]->bypass;
        juce::PopupMenu sub;
        sub.addItem ("Open", ! missing, false, [this, track, s] { openEditor (track, s); });
        sub.addItem (bypassed ? "Switch on" : "Bypass", [this, track, s, bypassed]
        {
            juce::String error;
            if (! session.setBypass (track, s, ! bypassed, error)) showError (error);
            requestRefresh();
        });
        sub.addItem ("Move earlier", s > 0, false, [this, track, s] { closeEditors(); juce::String e; session.movePlugin (track, s, s - 1, e); requestRefresh(); });
        sub.addItem ("Move later", s < count - 1, false, [this, track, s] { closeEditors(); juce::String e; session.movePlugin (track, s, s + 1, e); requestRefresh(); });
        sub.addSeparator();
        sub.addItem ("Remove", [this, track, s]
        {
            closeEditors();
            juce::String error;
            if (session.removePlugin (track, s, error)) editor.clearUndo(); else showError (error);
            requestRefresh();
        });
        menu.addSubMenu (chain[(size_t) s]->plugin + (missing ? "  (not installed)" : bypassed ? "  (bypassed)" : ""), sub);
    }
    menu.addSeparator();
    menu.addItem ("Add plugin...", [this, track] { showAddPluginMenu (track); });
    menu.showMenuAsync (juce::PopupMenu::Options());
}

void MainComponent::openEditor (int track, int slot)
{
    juce::String error;
    auto* s = session.slotAt (track, slot, error);
    if (s == nullptr || s->instance == nullptr)
    {
        showError (s == nullptr ? error : "That plugin is not installed here, so there is nothing to open.");
        return;
    }
    if (auto it = windows.find (s); it != windows.end()) { it->second->toFront (true); return; }
    juce::AudioProcessorEditor* pluginEditor = s->instance->hasEditor() ? s->instance->createEditorAndMakeActive() : new juce::GenericAudioProcessorEditor (*s->instance);
    if (pluginEditor == nullptr) return;
    windows[s] = makeEditorWindow (session.tracks[(size_t) track]->name + " - " + s->plugin, pluginEditor,
                                   [this, s, safe = juce::Component::SafePointer<MainComponent> (this)]
                                   { juce::MessageManager::callAsync ([safe, s] { if (safe != nullptr) safe->windows.erase (s); }); });
}

void MainComponent::closeEditors() { windows.clear(); }

// ---- timer, keys, layout -----------------------------------------------------------------------------------------------------
void MainComponent::timerCallback()
{
    // The audio copy of the transport is not taken here: every change that moves it calls syncTransport (refreshNow, the loop, the arrangement's
    // edits). A timer that took the session's lock would make the audio callback's try-lock miss a block now and then.
    syncPlayhead();
    arrange.refresh();
    updateTransport();
    // A plugin edited in its own window changes the project: notice it by what the plugins would save.
    if (! windows.empty() && ++tick % 15 == 0)
    {
        juce::int64 hash = 0;
        for (auto& t : session.tracks)
            for (auto& s : t->chain)
                hash = hash * 31 + session.stateOf (*s).toBase64Encoding().hashCode64();
        if (lastEditorHash != 0 && hash != lastEditorHash) session.dirty = true;
        lastEditorHash = hash;
    }
    else if (windows.empty())
        lastEditorHash = 0;
}

// The keys. The window owns the transport and the clip keys; the arrangement takes only its selection and nudges (arrows, Escape).
// Keys typed into the tempo field never get here (the field takes them).
bool MainComponent::keyPressed (const juce::KeyPress& key)
{
    const auto code = key.getKeyCode();
    const auto text = lowerCase (key.getTextCharacter());
    const auto mods = key.getModifiers();
    const bool cmd = mods.isCommandDown(), shift = mods.isShiftDown();
    const auto is = [text, code] (juce::juce_wchar c) { return text == c || lowerCase ((juce::juce_wchar) code) == c; };

    if (cmd)
    {
        if (is ('z')) { if (shift) redoEdit(); else undoEdit(); return true; }
        if (is ('s')) { if (shift) saveProjectAs(); else saveProject(); return true; }
        if (is ('o')) { openButton.triggerClick(); return true; }
        if (is ('n')) { newButton.triggerClick(); return true; }
        if (is ('d')) { arrange.duplicateSelected(); return true; }
        return false;
    }
    if (code == juce::KeyPress::spaceKey) { togglePlay(); updateTransport(); return true; }
    if (code == juce::KeyPress::returnKey) { rewind(); updateTransport(); return true; }
    if (code == juce::KeyPress::deleteKey || code == juce::KeyPress::backspaceKey) { arrange.deleteSelected(); return true; }
    if (is ('s')) { syncPlayhead(); arrange.splitAtPlayhead(); return true; }
    if (is ('m')) { syncPlayhead(); arrange.addMarkerAtPlayhead(); return true; }
    if (is ('l')) { toggleLoop(); return true; }
    if (is ('+') || is ('=')) { zoomBy (1.5); return true; }
    if (is ('-')) { zoomBy (1.0 / 1.5); return true; }
    return false;
}

void MainComponent::resized()
{
    auto r = getLocalBounds();
    const int w = r.getWidth();
    const int rowA = 12, rowB = 46, h = 26, hb = 30;

    // row A: the project and the tracks
    int x = 290;
    for (auto* b : { &newButton, &openButton, &saveButton, &saveAsButton })
    {
        b->setBounds (x, rowA, 70, h);
        x += 76;
    }
    x += 6;
    addTrackButton.setBounds (x, rowA, 92, h);
    x += 98;
    importButton.setBounds (x, rowA, 104, h);
    x += 110;
    pluginsButton.setBounds (x, rowA, 80, h);
    x += 86;
    renderButton.setBounds (x, rowA, 112, h);
    guideButton.setBounds (w - kMargin - 70, rowA, 70, h);

    // row B: the transport, then the zoom, then the position
    x = 290;
    rewindButton.setBounds (x, rowB, 34, hb);       x += 40;
    playButton.setBounds (x, rowB, 66, hb);         x += 72;
    loopButton.setBounds (x, rowB, 60, hb);         x += 66;
    metronomeButton.setBounds (x, rowB, 64, hb);    x += 70;
    bpmField.setBounds (x, rowB, 70, hb);           x += 76;
    signatureBox.setBounds (x, rowB, 66, hb);       x += 72;
    snapButton.setBounds (x, rowB, 56, hb);         x += 62;
    gridBox.setBounds (x, rowB, 84, hb);            x += 90;
    undoButton.setBounds (x, rowB, 54, hb);         x += 60;
    redoButton.setBounds (x, rowB, 54, hb);         x += 60;
    zoomOutButton.setBounds (x, rowB, 30, hb);      x += 36;
    zoomInButton.setBounds (x, rowB, 30, hb);       x += 36;
    zoomFitButton.setBounds (x, rowB, 44, hb);
    positionBox = { w - kMargin - 184, rowB, 184, hb };

    noteStrip = { kMargin, r.getHeight() - kMargin - ui::kNoteStripHeight, w - 2 * kMargin, ui::kNoteStripHeight };
    bodyArea = { kMargin, kHeaderHeight + 12, w - 2 * kMargin, noteStrip.getY() - 10 - (kHeaderHeight + 12) };
    arrange.setBounds (bodyArea);
}

void MainComponent::paint (juce::Graphics& g)
{
    g.fillAll (colours::background);
    ui::hw::drawHeaderPlate (g, juce::Rectangle<float> (0.0f, 0.0f, (float) getWidth(), (float) kHeaderHeight - 6.0f));
    ui::hw::drawNameplate (g, { kMargin, 8, 250, 66 }, "Olympus", "Basic DAW", "Pantheon plugins", ! session.tracks.empty(), 24.0f);

    // the position read-out, on glass
    ui::hw::drawGlass (g, positionBox.toFloat(), 3.0f, colours::amber);
    g.setColour (colours::amber);
    g.setFont (ui::lcdFont (14.0f));
    g.drawText (positionText (session, position()), positionBox.reduced (8, 0), juce::Justification::centredRight);
    ui::hw::drawGlassOverlay (g, positionBox.toFloat(), 3.0f, false);

    const auto tracks = juce::String ((int) session.tracks.size()) + (session.tracks.size() == 1 ? " track" : " tracks")
                        + (session.sampleRate > 0.0 ? "  " + juce::String (session.sampleRate / 1000.0, 1) + " kHz" : juce::String());
    ui::hw::drawLabel (g, tracks, { 1026, 12, 160, 26 }, juce::Justification::centredLeft, colours::dim, 10.5f);
    ui::drawNoteStrip (g, noteStrip, notes());
}
}
