// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 The Pantheon authors

#include "MainComponent.h"

namespace olympus::gui
{
namespace
{
const char* const kAudioWildcard = "*.wav;*.aif;*.aiff;*.flac;*.mp3;*.m4a;*.ogg;*.caf";
constexpr int kMargin = 16, kHeaderHeight = 116, kRowC = 80; // kRowC: the top bar's third row (the device, the position)
constexpr int kMonitorTop = kHeaderHeight + 2, kMonitorHeight = 28; // the MONITOR strip, under the header
constexpr int kBodyTop = kMonitorTop + kMonitorHeight + 8;
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

// The strip's read-out (the chain's latency) and the '+' button, from the strip's right end: the CORRECTION switch takes the last 176 px.
juce::Rectangle<int> monitorLatencyArea (juce::Rectangle<int> strip) { return { strip.getRight() - 176 - 8 - 120, strip.getY(), 120, strip.getHeight() }; }
int monitorPlusX (juce::Rectangle<int> strip) { return monitorLatencyArea (strip).getX() - 8 - 28; }

// The maker of a plugin, from the catalog's own description (the labels names() gives are "Name" or, for two of one name, "Name (Maker)").
juce::String makerOf (const Catalog& catalog, const juce::String& label)
{
    for (const auto& d : catalog.descriptions())
    {
        const auto maker = d.manufacturerName.isNotEmpty() ? d.manufacturerName : juce::String ("Other");
        if (label == d.name || label == d.name + " (" + (d.manufacturerName.isNotEmpty() ? d.manufacturerName : d.pluginFormatName) + ")")
            return maker;
    }
    return "Other";
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

// The audio device's settings dialog: a window that stays open beside the arrangement (no modal loop).
class AudioDialog : public juce::DialogWindow
{
public:
    explicit AudioDialog (std::unique_ptr<juce::Component> content)
        : juce::DialogWindow ("Audio devices", colours::background, true, true)
    {
        setUsingNativeTitleBar (true);
        setContentOwned (content.release(), true);
        setResizable (false, false);
    }
    void closeButtonPressed() override { setVisible (false); }
};

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
// loop's start. Without a loop the playback stops at the end of the audio (the longest track, and the latency of its chains). While a take
// runs, neither happens: the loop does not wrap and the playhead goes on past the end of the audio.
void MainComponent::Source::getNextAudioBlock (const juce::AudioSourceChannelInfo& info)
{
    info.clearActiveBufferRegion();
    if (! playing.load())
        return;
    const bool recording = owner.recorder.isRecording();
    const juce::int64 start = loopStart.load(), end = loopEnd.load(), audioEnd = endOfAudio.load();
    const bool wrap = ! recording && loopOn.load() && end > start;
    juce::int64 pos = position.load();
    int done = 0;
    while (done < info.numSamples)
    {
        if (wrap && pos >= end)
            pos = start;
        else if (! wrap && ! recording && pos >= audioEnd)
        {
            playing = false;
            break;
        }
        const juce::int64 room = wrap ? end - pos : recording ? (juce::int64) info.numSamples : audioEnd - pos;
        const int n = (int) juce::jmin ((juce::int64) juce::jmin (kBlockSize, info.numSamples - done), room);
        chunk.setSize (2, n, false, false, true);
        owner.session.processBlock (chunk, pos);
        owner.session.processMonitor (chunk); // the headphones' chain: playback only (Session::processMonitor)
        for (int ch = 0; ch < info.buffer->getNumChannels(); ++ch)
            info.buffer->copyFrom (ch, info.startSample + done, chunk, juce::jmin (ch, 1), 0, n);
        pos += n;
        done += n;
    }
    position = pos;
}

// Audio thread. One device block: the inputs to the recorder, then the transport into the outputs. The take's first sample is the playhead
// of its first block, so the position is read here, before the block plays.
void MainComponent::DeviceCallback::deviceBlock (const float* const* in, int numIn, float* const* out, int numOut, int n)
{
    // the take's first sample is the playhead of the first block the recorder wrote: push says which block that was, so the start and the
    // write are one decision. The position is read before this block plays.
    if (owner.recorder.push (in, numIn, n) && owner.source.recordStart.load() < 0)
        owner.source.recordStart = owner.source.position.load();
    if (out == nullptr || numOut <= 0)
    {
        // no outputs: nothing is heard, but the transport still advances, so a take on an input-only device runs and the playhead with it
        if (n <= scratch.getNumSamples())
        {
            juce::AudioSourceChannelInfo info (&scratch, 0, n);
            owner.source.getNextAudioBlock (info);
        }
        return;
    }
    juce::AudioBuffer<float> buffer (out, numOut, n);
    juce::AudioSourceChannelInfo info (&buffer, 0, n);
    owner.source.getNextAudioBlock (info);
}

// Message thread, before the device's callbacks start (also when the device changes). A take cannot go on across a change of rate or
// block size, so it ends here with what it has; the recorder is then prepared for the new device.
void MainComponent::DeviceCallback::audioDeviceAboutToStart (juce::AudioIODevice* started)
{
    if (owner.recorder.isRecording())
        owner.stopRecording ("The audio device changed while recording: the take was kept up to where it changed.");
    if (started == nullptr)
        return;
    owner.deviceRate = started->getCurrentSampleRate();
    prepareScratch (started->getCurrentBufferSizeSamples());
    owner.recorder.prepare (started->getCurrentSampleRate(), started->getCurrentBufferSizeSamples(), started->getActiveInputChannels());
    // the session follows the device's rate; its plugins are set up on the message thread
    if (juce::MessageManager::getInstance()->isThisTheMessageThread())
        owner.adoptDeviceRate();
    else
        juce::MessageManager::callAsync ([safe = juce::Component::SafePointer<MainComponent> (&owner)] { if (safe != nullptr) safe->adoptDeviceRate(); });
}

// Device thread. Only a flag: the timer ends a take that the device stopped under (on the message thread).
void MainComponent::DeviceCallback::audioDeviceStopped()
{
    owner.deviceStopped = true;
}

// ---- the window --------------------------------------------------------------------------------------------------------------
MainComponent::MainComponent() : MainComponent (Options {}) {}

MainComponent::MainComponent (Options options) : opts (options)
{
    setLookAndFeel (&look);
    setWantsKeyboardFocus (true);
    settings = Settings::load (opts.settingsFile);
    // a pedal the last start left: it died loading the monitor chain or the last project, which are skipped this time (Settings.h)
    monitorSkipped = Settings::takePedal (Settings::monitorPedalFile (opts.settingsFile));
    projectSkipped = Settings::takePedal (Settings::projectPedalFile (opts.settingsFile));

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
    arrange.onInputMenu = [this] (int track) { showInputMenu (track); };
    // an armed track's meter shows its input: the larger of its one or two device inputs, since the last read
    arrange.inputLevel = [this] (int track)
    {
        if (! juce::isPositiveAndBelow (track, (int) session.tracks.size())) return 0.0f;
        const auto& t = *session.tracks[(size_t) track];
        float peak = recorder.inputPeak (t.inputLeft);
        if (t.inputRight >= 0) peak = juce::jmax (peak, recorder.inputPeak (t.inputRight));
        return peak;
    };
    addAndMakeVisible (arrange);

    for (auto* b : { &newButton, &openButton, &saveButton, &saveAsButton, &addTrackButton, &importButton, &pluginsButton, &renderButton, &audioButton,
                     &rewindButton, &undoButton, &redoButton, &zoomOutButton, &zoomInButton, &zoomFitButton })
        addAndMakeVisible (*b);
    for (auto* b : { &playButton, &recordButton, &loopButton, &metronomeButton, &snapButton })
    {
        b->setClickingTogglesState (false);   // the session says what state they are in: see updateTransport
        addAndMakeVisible (*b);
    }
    recordButton.lamp = colours::bad;
    addAndMakeVisible (guideButton);
    addAndMakeVisible (bpmField);
    addAndMakeVisible (signatureBox);
    addAndMakeVisible (gridBox);
    addAndMakeVisible (deviceLabel);
    deviceLabel.setFont (ui::font (10.5f));
    deviceLabel.setColour (juce::Label::textColourId, colours::dim);
    // a focused button would take Space and Return for itself: the keys stay with the window
    for (auto* c : std::initializer_list<juce::Component*> { &newButton, &openButton, &saveButton, &saveAsButton, &addTrackButton, &importButton, &pluginsButton,
                                                             &renderButton, &audioButton, &rewindButton, &undoButton, &redoButton, &zoomOutButton, &zoomInButton,
                                                             &zoomFitButton, &playButton, &recordButton, &loopButton, &metronomeButton, &snapButton, &guideButton,
                                                             &signatureBox, &gridBox })
        c->setWantsKeyboardFocus (false);

    newButton.onClick = [this] { askToDiscard ("Start a new project and lose them?", [this] { newProject(); }); };
    openButton.onClick = [this] { askToDiscard ("Open another project and lose them?", [this] { chooseAndOpen(); }); };
    saveButton.onClick = [this] { saveProject(); };
    saveAsButton.onClick = [this] { saveProjectAs(); };
    addTrackButton.onClick = [this] { addEmptyTrack(); };
    importButton.onClick = [this] { chooseAndImport(); };
    pluginsButton.onClick = [this] { showPluginsMenu(); };
    renderButton.onClick = [this] { chooseAndRender(); };
    audioButton.onClick = [this] { showAudioSettings(); };
    rewindButton.onClick = [this] { rewind(); updateTransport(); };
    playButton.onClick = [this] { togglePlay(); updateTransport(); };
    recordButton.onClick = [this] { toggleRecord(); };
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
    audioButton.setTooltip ("The audio input and output devices, the sample rate and the buffer size.");
    playButton.setTooltip ("Play from the playhead, or stop (Space). While a take runs, this stops it.");
    recordButton.setTooltip ("Record every armed track from the playhead, with playback. Again stops, and the takes go on their tracks (R).");
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
        // the device the user chose last time, else the default one (initialise falls back to it)
        const auto saved = juce::parseXML (settings.audioDeviceState);
        const auto error = deviceManager.initialise (32, 2, saved.get(), true);
        deviceManager.addAudioCallback (&io);
        deviceManager.addChangeListener (this);
        if (error.isNotEmpty())
            showError ("The audio device could not be opened: " + error);
        readDevice();
    }
    if (opts.scanPlugins)
    {
        // the last scan is loaded; the folders are read again only when there is none, or when the user asks (PLUGINS > Rescan plugins)
        session.catalog.setSuiteOnly (settings.scanSuiteOnly);
        const bool cached = session.catalog.loadCache (Settings::pluginCacheFile (opts.settingsFile));
        if (! cached || session.catalog.names().isEmpty())
            juce::MessageManager::callAsync ([safe = juce::Component::SafePointer<MainComponent> (this)] { if (safe != nullptr) safe->scanPlugins (false); });
    }
    // the checks fill the catalog here, before the monitor chain is read (the app's scan does it above, or after the first scan)
    if (opts.fillCatalog)
        opts.fillCatalog (session.catalog);
    if (! session.catalog.names().isEmpty())
        loadMonitorChain();
    adoptDeviceRate(); // a session without a rate takes the device's; one with a project at another rate is converted

    addAndMakeVisible (monitorAddButton);
    addAndMakeVisible (monitorBypassButton);
    monitorAddButton.setWantsKeyboardFocus (false);
    monitorBypassButton.setWantsKeyboardFocus (false);
    monitorAddButton.setTooltip ("Add a plugin to the monitor chain, after the others: the headphones hear it. Playback only; not in a render.");
    monitorAddButton.onClick = [this] { showAddMonitorMenu(); };
    monitorBypassButton.setClickingTogglesState (false);
    monitorBypassButton.lamp = colours::good;
    monitorBypassButton.setTooltip ("CORRECTION: the monitor chain is heard. BYPASS: the headphones hear the uncorrected mix. Not saved in the project.");
    monitorBypassButton.onClick = [this] { setMonitorBypassed (! session.monitorBypass); };

    syncTransport();
    updateTransport();
    startTimerHz (30);
}

MainComponent::~MainComponent()
{
    stopTimer();
    cancelPendingUpdate();
    saveMonitorChain();   // the state of a monitor plugin whose window is still open is kept
    // a take still running keeps its files (Record.h: the takes are never deleted); the project is going away with the window
    if (recorder.isRecording())
        recorder.stop();
    if (opts.audioDevice)
    {
        deviceManager.removeChangeListener (this);
        deviceManager.removeAudioCallback (&io);
    }
    audioDialog.reset();
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
    loadMonitorChain();   // the first scan fills the catalog the chain's plugins are made from
    // a project opened before the scan finished, or a chain read before it: their missing plugins get another try now. Adding one is not an
    // undo step (Editor::clearUndo), and the notes and the strip are read again by refreshNow.
    if (session.retryMissing() > 0)
        editor.clearUndo();
    refreshNow();
}

void MainComponent::updateTitle()
{
    const auto title = windowTitle();
    if (title != shownTitle) { shownTitle = title; if (onTitleChanged) onTitleChanged(); }
}

juce::String MainComponent::windowTitle() const { return "Olympus - " + session.name + (session.dirty ? " *" : ""); }

void MainComponent::showError (const juce::String& message) { if (reportError) reportError (message); }

std::vector<ui::Note> MainComponent::notes() const
{
    AdviceDevice advice;
    advice.running = device.running;
    advice.rate = device.running ? device.rate : 0.0;
    advice.inputs = device.inputs;
    advice.roundTripMs = roundTripMs();
    advice.monitorProblem = monitorProblem;
    advice.monitorSkipped = monitorSkipped && ! monitorLoaded;
    advice.projectSkipped = projectSkipped;
    return adviceFor (session, advice);
}

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
    recordButton.setToggleState (recorder.isRecording(), juce::dontSendNotification);
    loopButton.setToggleState (session.loopOn, juce::dontSendNotification);
    metronomeButton.setToggleState (session.metronomeOn, juce::dontSendNotification);
    snapButton.setToggleState (session.snapOn, juce::dontSendNotification);
    if (gridBox.getSelectedItemIndex() != (int) session.grid) gridBox.setSelectedItemIndex ((int) session.grid, juce::dontSendNotification);
    showSignature (session.tempo.numerator, session.tempo.denominator);
    undoButton.setEnabled (editor.canUndo());
    redoButton.setEnabled (editor.canRedo());

    // the device, as the top bar names it and its tooltip says what it means for the takes
    const auto summary = deviceSummary();
    if (deviceLabel.getText() != summary) deviceLabel.setText (summary, juce::dontSendNotification);
    const auto tip = device.running
        ? "Round trip about " + juce::String (roundTripMs(), 1) + " ms (input " + juce::String (device.inputLatency) + " + output "
              + juce::String (device.outputLatency) + " samples). Each take is placed earlier by that, and by the extra latency in the settings."
        : juce::String ("No audio device is running. Click Audio to choose the input and the output.");
    if (deviceLabel.getTooltip() != tip) deviceLabel.setTooltip (tip);

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
    refreshMonitorStrip();
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

// ---- the audio device ----------------------------------------------------------------------------------------------------------
std::vector<MainComponent::InputChoice> MainComponent::inputChoices (const DeviceState& state)
{
    std::vector<InputChoice> out;
    const int highest = state.activeInputs.getHighestBit();
    for (int i = 0; i <= highest; ++i)
    {
        if (! state.activeInputs[i])
            continue;
        // the device's own name for the input, when it has one ("In 3: Guitar"); the number is the device's, as the take records it
        const auto name = i < state.inputNames.size() ? state.inputNames[i].trim() : juce::String();
        out.push_back ({ "In " + juce::String (i + 1) + (name.isNotEmpty() ? ": " + name : juce::String()), i, -1 });
        if (state.activeInputs[i + 1])
            out.push_back ({ "In " + juce::String (i + 1) + "+" + juce::String (i + 2), i, i + 1 });
    }
    return out;
}

juce::String MainComponent::deviceSummary() const
{
    if (! device.running)
        return "no audio device";
    return "IN: " + (device.inputs > 0 && device.inputName.isNotEmpty() ? device.inputName : juce::String ("none"))
           + " / OUT: " + (device.outputs > 0 && device.outputName.isNotEmpty() ? device.outputName : juce::String ("none"))
           + " / " + juce::String (device.rate / 1000.0, 1) + " kHz / " + juce::String (device.bufferSize) + " smp";
}

double MainComponent::roundTripMs() const
{
    if (! device.running || device.rate <= 0.0)
        return 0.0;
    return 1000.0 * (double) (device.inputLatency + device.outputLatency) / device.rate;
}

// The device as the device manager has it now: running, its names, rate, buffer and active channels, and its latencies.
void MainComponent::readDevice()
{
    if (! opts.audioDevice)
        return; // the checks set their device (setDeviceForTest); there is no device manager to read
    DeviceState state;
    if (auto* current = deviceManager.getCurrentAudioDevice())
    {
        const auto setup = deviceManager.getAudioDeviceSetup();
        state.running = current->isPlaying();
        state.inputName = setup.inputDeviceName;
        state.outputName = setup.outputDeviceName;
        state.rate = current->getCurrentSampleRate();
        state.bufferSize = current->getCurrentBufferSizeSamples();
        state.activeInputs = current->getActiveInputChannels();
        state.inputNames = current->getInputChannelNames();
        state.inputs = state.activeInputs.countNumberOfSetBits();
        state.outputs = current->getActiveOutputChannels().countNumberOfSetBits();
        state.inputLatency = current->getInputLatencyInSamples();
        state.outputLatency = current->getOutputLatencyInSamples();
    }
    device = state;
    deviceRate = state.running ? state.rate : 0.0;
}

// A change of the device (the AUDIO dialog, or the device going away): the new state is read and kept, and a take that cannot go on ends.
// Message thread. The session takes the device's rate (Session::adoptRate): nothing when no device runs. A project's audio is converted once
// and the playhead keeps its place in seconds; the undo steps are cleared, since they hold the old rate.
void MainComponent::adoptDeviceRate()
{
    if (deviceRate <= 0.0)
        return;
    const double before = session.sampleRate;
    const juce::int64 playhead = position();
    juce::String error;
    if (! session.adoptRate (deviceRate, error))
    {
        showError (error);
        return;
    }
    if (session.sampleRate == before)
        return;
    editor.clearUndo();
    if (before > 0.0)
        source.position = (juce::int64) std::llround ((double) playhead * session.sampleRate / before);
    refreshNow();
}

void MainComponent::deviceChanged()
{
    readDevice();
    saveAudioState();
    updateTransport();
    repaint();
}

void MainComponent::changeListenerCallback (juce::ChangeBroadcaster*)
{
    deviceChanged();
}

// The device's settings as XML text, saved with the other settings, so the device comes back as it was chosen.
void MainComponent::saveAudioState()
{
    if (auto xml = deviceManager.createStateXml())
        settings.audioDeviceState = xml->toString();
    settings.save (opts.settingsFile);
}

void MainComponent::setDeviceForTest (const DeviceState& state)
{
    device = state;
    deviceRate = state.running ? state.rate : 0.0;
    io.prepareScratch (state.bufferSize);
    if (state.running && state.rate > 0.0)
        recorder.prepare (state.rate, state.bufferSize, state.activeInputs);
    updateTransport();
}

void MainComponent::showAudioSettings()
{
    if (! audioDialog)
    {
        audioDialog = std::make_unique<AudioDialog> (std::make_unique<juce::AudioDeviceSelectorComponent> (deviceManager, 0, 32, 0, 2, false, false, false, false));
        audioDialog->setLookAndFeel (&look);
        audioDialog->centreAroundComponent (this, audioDialog->getWidth(), audioDialog->getHeight());
    }
    audioDialog->setVisible (true);
    audioDialog->toFront (true);
}

// The IN menu of a track: the device's inputs, one at a time or in pairs, and the choice goes to the track (not an undo step).
void MainComponent::showInputMenu (int track)
{
    if (! juce::isPositiveAndBelow (track, (int) session.tracks.size())) return;
    const auto& current = *session.tracks[(size_t) track];
    juce::PopupMenu menu;
    const auto choices = inputChoices (device);
    if (! device.running)
        menu.addItem ("No audio device: choose the input with Audio", false, false, nullptr);
    else if (choices.empty())
        menu.addItem ("The device has no active input", false, false, nullptr);
    for (const auto& c : choices)
        menu.addItem (c.label, true, c.left == current.inputLeft && c.right == current.inputRight, [this, track, c]
        {
            juce::String error;
            if (! session.setTrackInput (track, c.left, c.right, error)) showError (error);
            requestRefresh();
        });
    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&arrange));
}

// ---- recording ------------------------------------------------------------------------------------------------------------------
juce::File MainComponent::recordingFolder() const
{
    if (opts.recordings != juce::File())
        return opts.recordings;
    const auto name = juce::File::createLegalFileName (session.name);
    if (session.projectFile != juce::File())
        return session.projectFile.getParentDirectory().getChildFile (name + " Recordings");
    const auto stamp = juce::Time::getCurrentTime().formatted ("%Y-%m-%d %H.%M.%S");
    return juce::File::getSpecialLocation (juce::File::userMusicDirectory).getChildFile ("Olympus Recordings").getChildFile (name + " " + stamp);
}

// The latency the takes are placed earlier by: the device's input and output latency, the extra latency in the settings, the monitor
// chain's (what is heard late) and the longest track chain's (the tracks are delayed to line up with it, Session::updateLatenciesLocked,
// so the mix is heard that much late). Fixed at REC, so the take lines up with what was heard then.
void MainComponent::setTakeLatency()
{
    int chains = 0;
    {
        const juce::ScopedLock sl (session.lock);
        chains = session.maxLatency();
    }
    takeLatency = (juce::int64) device.inputLatency + device.outputLatency + settings.extraLatencySamples + session.monitorLatency() + chains;
}

bool MainComponent::startRecording()
{
    if (recorder.isRecording())
        return true;
    juce::String error;
    if (! device.running)
        error = "No audio device is running: choose the input and output device with Audio.";
    else if (device.inputs == 0)
        error = "The audio device has no input channels: choose an input device with Audio.";
    if (error.isNotEmpty())
    {
        showError (error);
        return false;
    }
    source.recordStart = -1;   // set by the first device block the recorder runs in
    if (! beginRecording (recorder, session, recordingFolder(), device.rate, error))
    {
        showError (error);
        return false;
    }
    takeFrom = position();
    setTakeLatency();
    {
        const juce::ScopedLock sl (session.lock);
        arrange.recordingTracks.clear();
        for (int i = 0; i < (int) session.tracks.size(); ++i)
            if (session.tracks[(size_t) i]->armed)
                arrange.recordingTracks.push_back (i);
    }
    arrange.recordingFrom = arrange.recordingTo = (double) takeFrom;
    syncTransport();
    session.prepare (false);
    source.playing = true;
    updateTransport();
    return true;
}

void MainComponent::toggleRecord()
{
    if (recorder.isRecording()) stopRecording();
    else startRecording();
    updateTransport();
}

// Ends the takes, waits for their files to be written, and places them: one undo step, the first clip selected and the view on it. A take
// that stopped with the device is kept as it is; `problem` is shown after.
void MainComponent::stopRecording (const juce::String& problem)
{
    if (! recorder.isRecording())
        return;
    source.playing = false;
    const auto takes = recorder.stop();
    const juce::int64 heard = source.recordStart.exchange (-1);
    const juce::int64 start = heard >= 0 ? heard : takeFrom;
    const juce::int64 begin = juce::jmax ((juce::int64) 0, start - takeLatency);

    juce::String error;
    if (! finishRecording (takes, start, (int) takeLatency, editor, error) && error.isEmpty())
        error = "A take could not be placed on its track.";
    session.dirty = true;

    ClipRef first;
    {
        const juce::ScopedLock sl (session.lock);
        for (const auto& take : takes)
        {
            if (take.frames <= 0 || first.valid())
                continue;
            const auto& clips = session.tracks[(size_t) take.track]->clips;
            for (int c = (int) clips.size() - 1; c >= 0; --c)
                if (clips[(size_t) c].start == begin) { first = { take.track, c }; break; }
        }
    }
    arrange.recordingFrom = arrange.recordingTo = -1.0;
    arrange.recordingTracks.clear();
    if (first.valid())
        arrange.selected = first;
    arrange.scrollTo (begin);
    syncTransport();
    refreshNow();

    juce::StringArray problems;
    if (error.isNotEmpty()) problems.add (error);
    if (problem.isNotEmpty()) problems.add (problem);
    if (problems.size() > 0)
        showError (problems.joinIntoString ("\n"));
}

// The timer's part of a take: the strip grows from where it began to the playhead, and a device that stopped ends it. Also what the
// arrangement's meters show (the inputs of an armed track while the transport is stopped or recording).
void MainComponent::updateRecording()
{
    if (deviceStopped.exchange (false))
    {
        if (recorder.isRecording())
            stopRecording ("The audio device stopped while recording: the take was kept up to where it stopped.");
        readDevice();
        updateTransport();
    }
    if (! recorder.isRecording())
        return;
    const juce::int64 heard = source.recordStart.load();
    arrange.recordingFrom = (double) (heard >= 0 ? heard : takeFrom);
    arrange.recordingTo = (double) position();
    repaint();
}

// ---- actions -----------------------------------------------------------------------------------------------------------------
void MainComponent::newProject()
{
    stopRecording();
    closeTrackEditors();
    source.playing = false;
    session.newProject ("Untitled");
    editor.clearUndo();
    adoptDeviceRate();
    seek (0);
    arrange.selected = {};
    arrange.zoom = ArrangeView::kDefaultZoom;
    arrange.scrollTo (0);
    refreshNow();
}

void MainComponent::openProject (const juce::File& file)
{
    projectSkipped = false;   // the user opened one: the note about the last start's project is no longer news
    stopRecording();
    closeTrackEditors();
    source.playing = false;
    juce::String error;
    if (! session.load (file, error))
    {
        showError (error);
        return;
    }
    editor.clearUndo();
    adoptDeviceRate(); // a project at another rate is converted to the device's
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
    // a take on this track's index would land on another track: it is stopped first, which is the user's choice to make
    if (recorder.isRecording())
    {
        showError ("Stop the recording before you remove a track.");
        return;
    }
    closeTrackEditors();
    juce::String error;
    if (session.removeTrack (index, error))
    {
        editor.clearUndo();
        arrange.selected = {};
    }
    else showError (error);
    requestRefresh();
}

// editorsClosed: the caller (chooseAndRender) has already closed the tracks' windows on the message thread, so this may run on the progress
// window's worker thread; a direct call (the checks, on the message thread) closes them here.
juce::String MainComponent::renderTo (const juce::File& folder, const std::function<bool (float)>& progress, bool editorsClosed)
{
    source.playing = false;
    if (! editorsClosed)
        closeTrackEditors();
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
    if (recorder.isRecording()) { stopRecording(); return; }
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

// The playhead may be put anywhere from 0; playing stops at the end of the audio (or wraps at the loop's end). While a take runs it stays
// where the take is: the take is a continuous recording from where it began.
void MainComponent::seek (juce::int64 sample)
{
    if (recorder.isRecording())
    {
        showError ("Stop the recording before you move the playhead.");
        return;
    }
    source.position = juce::jmax ((juce::int64) 0, sample);
}

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
    if (recorder.isRecording()) { showError ("Stop the recording before you render the stems."); return; }
    const auto start = session.projectFile != juce::File() ? session.projectFile.getParentDirectory().getChildFile (session.name + " stems") : juce::File::getSpecialLocation (juce::File::userDocumentsDirectory);
    chooser = std::make_unique<juce::FileChooser> ("Render the stems into this folder", start);
    chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories,
                          [this] (const juce::FileChooser& fc)
                          {
                              const auto folder = fc.getResult();
                              if (folder == juce::File()) return;
                              // the message thread stops the transport and closes the tracks' plugin windows before the worker starts:
                              // a window is never destroyed on the worker (renderTo's editorsClosed)
                              source.playing = false;
                              closeTrackEditors();
                              struct Job : juce::ThreadWithProgressWindow
                              {
                                  Job (MainComponent& m, juce::File f) : juce::ThreadWithProgressWindow ("Rendering the stems", true, true), owner (m), folder (std::move (f)) {}
                                  void run() override
                                  {
                                      error = owner.renderTo (folder, [this] (float p) { setProgress ((double) p); return ! threadShouldExit(); }, true);
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
    if (! session.catalog.blacklisted().isEmpty())
        menu.addItem ("Forget blacklisted plugins", [this] { session.catalog.clearBlacklist(); scanPlugins (true); requestRefresh(); });
    if (monitorChainSkipped())
        menu.addItem ("Load saved monitor chain", [this] { loadSkippedMonitorChain(); requestRefresh(); });
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
        sub.addItem ("Move earlier", s > 0, false, [this, track, s] { closeTrackEditors(); juce::String e; session.movePlugin (track, s, s - 1, e); requestRefresh(); });
        sub.addItem ("Move later", s < count - 1, false, [this, track, s] { closeTrackEditors(); juce::String e; session.movePlugin (track, s, s + 1, e); requestRefresh(); });
        sub.addSeparator();
        sub.addItem ("Remove", [this, track, s]
        {
            closeTrackEditors();
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

// A plugin's slot: a track's chain (track >= 0), or the monitor chain with track -1.
Slot* MainComponent::slotOf (int track, int slot, juce::String& error)
{
    if (track >= 0) return session.slotAt (track, slot, error);
    if (track == -1) return session.monitorSlotAt (slot, error);
    error = "No track " + juce::String (track);
    return nullptr;
}

void MainComponent::openEditor (int track, int slot)
{
    juce::String error;
    auto* s = slotOf (track, slot, error);
    if (s == nullptr || s->instance == nullptr)
    {
        showError (s == nullptr ? error : "That plugin is not installed here, so there is nothing to open.");
        return;
    }
    if (auto it = windows.find (s); it != windows.end()) { it->second->toFront (true); return; }
    juce::AudioProcessorEditor* pluginEditor = s->instance->hasEditor() ? s->instance->createEditorAndMakeActive() : new juce::GenericAudioProcessorEditor (*s->instance);
    if (pluginEditor == nullptr)
    {
        showError (s->plugin + " has no editor window");   // a plugin that says it has one but gives none: said, not silent
        return;
    }
    const auto owner = track < 0 ? juce::String ("Monitor") : session.tracks[(size_t) track]->name;
    windows[s] = makeEditorWindow (owner + " - " + s->plugin, pluginEditor,
                                   [this, s, safe = juce::Component::SafePointer<MainComponent> (this)]
                                   {
                                       juce::MessageManager::callAsync ([safe, s]
                                       {
                                           if (safe == nullptr) return;
                                           safe->windows.erase (s);
                                           safe->monitorEditorClosed (s);   // its state may have changed in the window: saved if it is a monitor plugin
                                       });
                                   });
}

void MainComponent::closeEditors() { windows.clear(); }

// The editors of the tracks' plugins only: a monitor plugin's window stays open across NEW and OPEN, and when a track is removed or rendered.
void MainComponent::closeTrackEditors()
{
    for (auto it = windows.begin(); it != windows.end();)
    {
        bool monitorSlot = false;
        for (auto& m : session.monitor)
            monitorSlot = monitorSlot || m.get() == it->first;
        it = monitorSlot ? std::next (it) : windows.erase (it);
    }
}

// ---- the monitor chain ---------------------------------------------------------------------------------------------------------------
// Read once, after the catalog has its plugins. A file that is not a monitor chain (or is newer) is kept as it is: nothing is saved over it.
// Safe start: the pedal is written around the read, so a crash in a plugin's load leaves it for the next start, which then skips the chain
// (monitorSkipped, set in the constructor). A skipped chain is unread: nothing is saved over monitor.xml until PLUGINS loads it.
void MainComponent::loadMonitorChain()
{
    if (monitorLoaded || monitorSkipped)
        return;
    const auto pedal = Settings::monitorPedalFile (opts.settingsFile);
    pedal.replaceWithText ("loading the monitor chain\n");
    juce::StringArray problems;
    monitorLoaded = session.loadMonitor (Settings::monitorFile (opts.settingsFile), problems);
    monitorProblem = monitorLoaded ? juce::String() : problems.joinIntoString (" ");
    refreshMonitorStrip();
    adoptDeviceRate(); // the chain's plugins were made at the session's rate, which may not be the device's yet: they are prepared again here, so the pedal stays until it is done
    pedal.deleteFile();
}

// PLUGINS > Load saved monitor chain: the chain the last start skipped, read now (the pedal around it, as at start).
void MainComponent::loadSkippedMonitorChain()
{
    monitorSkipped = false;
    loadMonitorChain();
    updateTransport();
    repaint();
}

// The last project, at start (the window's own startup, unless the command line named one). Skipped when the last start died opening it.
void MainComponent::openLastProjectAtStart()
{
    const juce::File last (settings.lastProject);
    if (! last.existsAsFile() || projectSkipped)
        return;
    const auto pedal = Settings::projectPedalFile (opts.settingsFile);
    pedal.replaceWithText ("opening the last project\n");
    openProject (last);
    pedal.deleteFile();
}

// Writes the chain beside settings.xml. Never before the chain was read: an unread file is not overwritten.
void MainComponent::saveMonitorChain()
{
    if (! monitorLoaded)
        return;
    juce::String error;
    if (! session.saveMonitor (Settings::monitorFile (opts.settingsFile), error))
        showError (error);
}

void MainComponent::monitorEditorClosed (const Slot* slot)
{
    for (auto& m : session.monitor)
        if (m.get() == slot)
        {
            saveMonitorChain();
            return;
        }
}

double MainComponent::monitorLatencyMs() const
{
    const double rate = device.running && device.rate > 0.0 ? device.rate : (session.sampleRate > 0.0 ? session.sampleRate : 48000.0);
    return 1000.0 * (double) session.monitorLatency() / rate;
}

bool MainComponent::addMonitorPlugin (const juce::String& name)
{
    if (! monitorLoaded)
    {
        showError (monitorProblem.isNotEmpty() ? "The monitor chain could not be read, so it is not changed: " + monitorProblem
                                               : juce::String ("The monitor chain is read after the plugin scan has finished."));
        return false;
    }
    juce::String error;
    if (! session.addMonitorPlugin (name, -1, error)) { showError (error); return false; }
    saveMonitorChain();
    refreshMonitorStrip();
    return true;
}

void MainComponent::removeMonitorPlugin (int index)
{
    if (! monitorLoaded) return;
    juce::String error;
    auto* slot = session.monitorSlotAt (index, error);
    if (slot == nullptr) { showError (error); return; }
    if (auto it = windows.find (slot); it != windows.end()) windows.erase (it);   // its editor closes with it
    if (! session.removeMonitorPlugin (index, error)) { showError (error); return; }
    saveMonitorChain();
    refreshMonitorStrip();
}

void MainComponent::moveMonitorPlugin (int index, int to)
{
    if (! monitorLoaded) return;
    juce::String error;
    if (! session.moveMonitorPlugin (index, to, error)) { showError (error); return; }
    saveMonitorChain();
    refreshMonitorStrip();
}

void MainComponent::setMonitorPluginBypass (int index, bool bypass)
{
    if (! monitorLoaded) return;
    juce::String error;
    if (! session.setMonitorPluginBypass (index, bypass, error)) { showError (error); return; }
    saveMonitorChain();
    refreshMonitorStrip();
}

void MainComponent::setMonitorBypassed (bool bypass)
{
    if (! monitorLoaded) return;
    session.setMonitorBypass (bypass);
    saveMonitorChain();
    refreshMonitorStrip();
}

// The strip's buttons follow the chain: one per plugin. The buttons are rebuilt only when their number changes (a button is never deleted
// by its own click); the names, the lamps and the CORRECTION switch are set each time.
void MainComponent::refreshMonitorStrip()
{
    const int count = (int) session.monitor.size();
    if (monitorButtons.size() != count)
    {
        monitorButtons.clear();
        for (int i = 0; i < count; ++i)
        {
            auto* b = monitorButtons.add (new MonitorSlotButton());
            b->setClickingTogglesState (false);
            b->setWantsKeyboardFocus (false);
            addAndMakeVisible (b);
            b->onClick = [this, b] { if (const int at = monitorButtons.indexOf (b); at >= 0) openEditor (-1, at); };
            b->onShiftClick = [this, b] { if (const int at = monitorButtons.indexOf (b); at >= 0) setMonitorPluginBypass (at, ! session.monitor[(size_t) at]->bypass); };
            b->onMenu = [this, b] { if (const int at = monitorButtons.indexOf (b); at >= 0) showMonitorMenu (at); };
        }
        resized();
    }
    for (int i = 0; i < count; ++i)
    {
        auto* b = monitorButtons[i];
        const auto& slot = *session.monitor[(size_t) i];
        if (b->getButtonText() != slot.plugin) b->setButtonText (slot.plugin);
        b->missing = slot.instance == nullptr;
        // lit when it is installed and on; a bypassed or missing plugin is dark (a missing one reads in red)
        b->setToggleState (slot.instance != nullptr && ! slot.bypass, juce::dontSendNotification);
        const auto tip = slot.instance == nullptr ? juce::String ("Not installed on this computer: its name is in red. It passes the audio through and keeps its settings.")
                                                 : juce::String ("Click: open its window. Shift-click: bypass. Right-click: the menu.");
        b->setTooltip (tip);
    }
    const auto correction = juce::String (session.monitorBypass ? "Bypass" : "Correction on");
    if (monitorBypassButton.getButtonText() != correction) monitorBypassButton.setButtonText (correction);
    monitorBypassButton.setToggleState (! session.monitorBypass, juce::dontSendNotification);
    repaint (monitorStrip);
}

// The menu of a monitor plugin: its window, its bypass, its place, and its removal (the chain is saved after each).
void MainComponent::showMonitorMenu (int index)
{
    juce::String error;
    auto* slot = session.monitorSlotAt (index, error);
    if (slot == nullptr) return;
    const bool missing = slot->instance == nullptr, bypassed = slot->bypass;
    const int count = (int) session.monitor.size();
    juce::PopupMenu menu;
    menu.addItem ("Open", ! missing, false, [this, slot]
    {
        for (int i = 0; i < (int) session.monitor.size(); ++i)
            if (session.monitor[(size_t) i].get() == slot) { openEditor (-1, i); return; }
    });
    menu.addItem (bypassed ? "Switch on" : "Bypass", [this, slot, bypassed]
    {
        for (int i = 0; i < (int) session.monitor.size(); ++i)
            if (session.monitor[(size_t) i].get() == slot) { setMonitorPluginBypass (i, ! bypassed); return; }
    });
    menu.addSeparator();
    menu.addItem ("Move left", index > 0, false, [this, index] { moveMonitorPlugin (index, index - 1); });
    menu.addItem ("Move right", index < count - 1, false, [this, index] { moveMonitorPlugin (index, index + 1); });
    menu.addSeparator();
    menu.addItem ("Remove", [this, slot]
    {
        for (int i = 0; i < (int) session.monitor.size(); ++i)
            if (session.monitor[(size_t) i].get() == slot) { removeMonitorPlugin (i); return; }
    });
    menu.showMenuAsync (juce::PopupMenu::Options());
}

// The plugins to add to the monitor chain: one list; over about 25 plugins, one sub-menu per maker.
void MainComponent::showAddMonitorMenu()
{
    if (! monitorLoaded)
    {
        showError (monitorProblem.isNotEmpty() ? "The monitor chain could not be read, so it is not changed: " + monitorProblem
                                               : juce::String ("The monitor chain is read after the plugin scan has finished."));
        return;
    }
    const auto names = session.catalog.names();
    juce::PopupMenu menu;
    if (names.isEmpty())
        menu.addItem ("No plugin found: use PLUGINS", false, false, nullptr);
    else if (names.size() <= 25)
        for (const auto& n : names)
            menu.addItem (n, [this, n] { addMonitorPlugin (n); });
    else
    {
        std::map<juce::String, juce::PopupMenu> makers;
        for (const auto& n : names)
            makers[makerOf (session.catalog, n)].addItem (n, [this, n] { addMonitorPlugin (n); });
        for (auto& [maker, sub] : makers)
            menu.addSubMenu (maker, sub);
    }
    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&monitorAddButton));
}

// ---- timer, keys, layout -----------------------------------------------------------------------------------------------------
void MainComponent::timerCallback()
{
    // The audio copy of the transport is not taken here: every change that moves it calls syncTransport (refreshNow, the loop, the arrangement's
    // edits). A timer that took the session's lock would make the audio callback's try-lock miss a block now and then.
    syncPlayhead();
    arrange.showInputLevels = ! isPlaying() || recorder.isRecording();
    updateRecording();
    // a plugin reported a new latency (its listener only sets a flag): the tracks' delays follow it, and the read-outs below show it
    if (session.takeLatencyChange())
    {
        session.updateLatencies();
        syncTransport(); // the end of the audio includes the longest chain's latency: the callback's copy follows it
    }
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
    if (is ('r'))
    {
        if (bpmField.hasKeyboardFocus (true)) return false;   // typing a tempo: the R is a letter there
        toggleRecord();
        return true;
    }
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
    const int rowA = 12, rowB = 46, h = 26, hb = 30, hc = 28;

    // row A: the project and the tracks, the audio devices
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
    audioButton.setBounds (x + 118, rowA, 76, h);
    guideButton.setBounds (w - kMargin - 70, rowA, 70, h);

    // row B: the transport, then the zoom
    x = 290;
    rewindButton.setBounds (x, rowB, 34, hb);       x += 40;
    playButton.setBounds (x, rowB, 66, hb);         x += 72;
    recordButton.setBounds (x, rowB, 60, hb);       x += 66;
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

    // row C: the device's summary, the take's time or the track count, and the position
    deviceLabel.setBounds (290, kRowC, 620, hc);
    positionBox = { w - kMargin - 184, kRowC, 184, hc };

    noteStrip = { kMargin, r.getHeight() - kMargin - ui::kNoteStripHeight, w - 2 * kMargin, ui::kNoteStripHeight };
    bodyArea = { kMargin, kBodyTop, w - 2 * kMargin, noteStrip.getY() - 10 - kBodyTop };
    arrange.setBounds (bodyArea);

    // the MONITOR strip: the label on the left, the chain's plugins, '+', the chain's latency and the CORRECTION switch on the right
    monitorStrip = { kMargin, kMonitorTop, w - 2 * kMargin, kMonitorHeight };
    monitorBypassButton.setBounds (monitorStrip.getRight() - 176, monitorStrip.getY(), 176, kMonitorHeight);
    monitorAddButton.setBounds (monitorPlusX (monitorStrip), monitorStrip.getY(), 28, kMonitorHeight);
    const int first = monitorStrip.getX() + 96, last = monitorPlusX (monitorStrip) - 8;
    const int n = monitorButtons.size(), gap = 6;
    const int each = n > 0 ? juce::jlimit (64, 160, (last - first - gap * (n - 1)) / n) : 0;
    for (int i = 0; i < n; ++i)
        monitorButtons[i]->setBounds (first + i * (each + gap), monitorStrip.getY(), each, kMonitorHeight);
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

    // beside the position: the time of the take in progress in red, else the track count and the sample rate
    juce::String readout;
    juce::Colour colour = colours::dim;
    if (recorder.isRecording())
    {
        readout = "REC  " + timeText ((juce::int64) recorder.framesRecorded(), device.rate);
        colour = colours::bad;
    }
    else
    {
        readout = juce::String ((int) session.tracks.size()) + (session.tracks.size() == 1 ? " track" : " tracks")
                  + (session.sampleRate > 0.0 ? "  " + juce::String (session.sampleRate / 1000.0, 1) + " kHz" : juce::String());
    }
    ui::hw::drawLabel (g, readout, { 930, kRowC, 140, 28 }, juce::Justification::centredRight, colour, 10.5f);
    // the MONITOR strip: its label, a hint while the chain is empty, and the chain's latency beside the CORRECTION switch
    ui::hw::drawLabel (g, "MONITOR", { monitorStrip.getX(), monitorStrip.getY(), 90, monitorStrip.getHeight() }, juce::Justification::centredLeft, colours::dim, 10.5f);
    if (monitorButtons.isEmpty())
        ui::hw::drawLabel (g, "no plugin: the headphones hear the mix as it is", { monitorStrip.getX() + 96, monitorStrip.getY(), 400, monitorStrip.getHeight() },
                           juce::Justification::centredLeft, colours::dim, 10.0f);
    ui::hw::drawLabel (g, "latency " + juce::String (monitorLatencyMs(), 1) + " ms", monitorLatencyArea (monitorStrip), juce::Justification::centredRight,
                       colours::dim, 10.0f);
    ui::drawNoteStrip (g, noteStrip, notes());
}
}
