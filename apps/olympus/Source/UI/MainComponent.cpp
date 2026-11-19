// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 The Pantheon authors

#include "MainComponent.h"

namespace olympus::gui
{
namespace
{
const char* const kAudioWildcard = "*.wav;*.aif;*.aiff;*.flac;*.mp3;*.m4a;*.ogg;*.caf";
constexpr int kMargin = 16, kHeaderHeight = 84, kRowGap = 8, kSlotWidth = 116, kSlotHeight = 28;

juce::String timeText (juce::int64 sample, double rate)
{
    const double s = rate > 0.0 ? (double) sample / rate : 0.0;
    const int minutes = (int) (s / 60.0);
    return juce::String (minutes).paddedLeft ('0', 2) + ":" + juce::String (std::fmod (s, 60.0), 2).paddedLeft ('0', 5);
}

// The peaks of a track's clips merged on its timeline, `buckets` of them (what the row draws).
std::vector<float> envelopeOf (const Track& track, int buckets)
{
    std::vector<float> env ((size_t) buckets, 0.0f);
    const juce::int64 length = track.length();
    if (length <= 0) return env;
    juce::AudioBuffer<float> chunk (2, kBlockSize);
    for (juce::int64 start = 0; start < length; start += kBlockSize)
    {
        const int n = (int) juce::jmin ((juce::int64) kBlockSize, length - start);
        Session::mixClips (track, start, n, chunk);
        for (int i = 0; i < n; ++i)
        {
            const auto b = (size_t) juce::jmin ((juce::int64) buckets - 1, (start + i) * buckets / length);
            for (int ch = 0; ch < 2; ++ch)
                env[b] = juce::jmax (env[b], std::abs (chunk.getSample (ch, i)));
        }
    }
    return env;
}
} // namespace

// ---- WaveStrip ---------------------------------------------------------------------------------------------------------------
void WaveStrip::paint (juce::Graphics& g)
{
    const auto area = getLocalBounds().toFloat();
    ui::hw::drawGlass (g, area, 3.0f, colours::accent);
    const auto inner = area.reduced (3.0f, 3.0f);
    g.setColour (colours::accent.withAlpha (0.8f));
    const int n = (int) envelope.size();
    for (int i = 0; i < n; ++i)
    {
        const float x = inner.getX() + inner.getWidth() * (float) i / (float) juce::jmax (1, n);
        const float h = juce::jlimit (0.0f, 1.0f, std::sqrt (envelope[(size_t) i])) * inner.getHeight() * 0.5f;
        g.fillRect (x, inner.getCentreY() - h, juce::jmax (1.0f, inner.getWidth() / (float) juce::jmax (1, n)), juce::jmax (1.0f, h * 2.0f));
    }
    if (n == 0)
    {
        g.setColour (colours::dim);
        g.setFont (ui::labelFont (10.0f));
        g.drawText ("NO AUDIO", getLocalBounds(), juce::Justification::centred);
    }
    g.setColour (colours::amber);
    g.fillRect (inner.getX() + inner.getWidth() * (float) fraction - 0.5f, inner.getY(), 1.5f, inner.getHeight());
    ui::hw::drawGlassOverlay (g, area, 3.0f, false);
}

// ---- TrackRow ----------------------------------------------------------------------------------------------------------------
TrackRow::TrackRow (MainComponent& owner, int trackIndex) : index (trackIndex), main (owner)
{
    auto& track = *owner.session.tracks[(size_t) trackIndex];
    addAndMakeVisible (wave);
    wave.envelope = envelopeOf (track, 240);
    wave.onSeek = [this] (double f) { main.seek ((juce::int64) (f * (double) main.session.tracks[(size_t) index]->length())); };

    nameLabel.setText (track.name, juce::dontSendNotification);
    nameLabel.setEditable (false, true);
    nameLabel.setFont (ui::font (15.0f, juce::Font::bold));
    nameLabel.setColour (juce::Label::textColourId, colours::text);
    nameLabel.onTextChange = [this]
    {
        const auto text = nameLabel.getText().trim();
        if (text.isNotEmpty()) { main.session.tracks[(size_t) index]->name = text; main.session.dirty = true; }
    };
    addAndMakeVisible (nameLabel);

    for (auto* b : { &muteButton, &soloButton })
    {
        b->setClickingTogglesState (true);
        addAndMakeVisible (*b);
    }
    muteButton.lamp = colours::bad;
    soloButton.lamp = colours::good;
    muteButton.setToggleState (track.mute, juce::dontSendNotification);
    soloButton.setToggleState (track.solo, juce::dontSendNotification);
    muteButton.setTooltip ("Mute: this track is not played, and no stem is written for it.");
    soloButton.setTooltip ("Solo: only soloed tracks are played and rendered.");
    muteButton.onClick = [this] { main.session.tracks[(size_t) index]->mute = muteButton.getToggleState(); main.session.dirty = true; };
    soloButton.onClick = [this] { main.session.tracks[(size_t) index]->solo = soloButton.getToggleState(); main.session.dirty = true; };
    removeButton.setTooltip ("Remove this track from the project (the audio file is not touched).");
    removeButton.onClick = [this] { main.removeTrack (index); };
    addAndMakeVisible (removeButton);

    gainKnob.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
    gainKnob.setTextBoxStyle (juce::Slider::NoTextBox, true, 0, 0);
    gainKnob.setRange (-60.0, 12.0, 0.1);
    gainKnob.setDoubleClickReturnValue (true, 0.0);
    gainKnob.setValue ((double) track.gainDb, juce::dontSendNotification);
    gainKnob.setRotaryParameters (juce::MathConstants<float>::pi * 1.2f, juce::MathConstants<float>::pi * 2.8f, true);
    gainKnob.setTooltip ("Fader of the track, after its chain. Double-click for 0 dB.");
    gainKnob.onValueChange = [this] { main.session.tracks[(size_t) index]->gainDb = (float) gainKnob.getValue(); main.session.dirty = true; repaint(); };
    addAndMakeVisible (gainKnob);

    auto& chain = track.chain;
    for (int s = 0; s < (int) chain.size(); ++s)
    {
        auto* b = slotButtons.add (new SlotButton());
        b->setButtonText (chain[(size_t) s]->plugin);
        const bool missing = chain[(size_t) s]->instance == nullptr;
        b->lamp = missing ? colours::bad : colours::amber;
        b->setToggleState (missing || ! chain[(size_t) s]->bypass, juce::dontSendNotification);
        b->setTooltip (missing ? "This plugin is not installed here. It passes the audio through and keeps its settings."
                               : "Click: open the plugin. Shift-click: bypass. Right-click: more.");
        b->onClick = [this, s]
        {
            if (juce::ModifierKeys::currentModifiers.isShiftDown())
            {
                juce::String error;
                main.session.setBypass (index, s, ! main.session.tracks[(size_t) index]->chain[(size_t) s]->bypass, error);
                main.requestRebuild();
            }
            else
                main.toggleEditor (index, s);
        };
        b->onMenu = [this, s] { main.showSlotMenu (index, s); };
        addAndMakeVisible (*b);
    }
    addPluginButton.setTooltip ("Put a plugin of the suite at the end of this chain.");
    addPluginButton.onClick = [this] { main.showAddPluginMenu (index); };
    addAndMakeVisible (addPluginButton);
}

namespace
{
constexpr int kChainX = 640;
int slotsPerLine (int width) { return juce::jmax (1, (width - kChainX - 16) / (kSlotWidth + 6)); }
}

int TrackRow::heightFor (int width) const
{
    const int items = slotButtons.size() + 1;
    const int lines = (items + slotsPerLine (width) - 1) / slotsPerLine (width);
    return juce::jmax (84, 14 + lines * (kSlotHeight + 6) + 8);
}

void TrackRow::resized()
{
    auto r = getLocalBounds().reduced (14, 10);
    nameLabel.setBounds (r.getX(), r.getY(), 190, 24);
    removeButton.setBounds (r.getX() + 196, r.getY() + 1, 22, 22);
    muteButton.setBounds (r.getX() + 236, r.getY(), 30, 24);
    soloButton.setBounds (r.getX() + 270, r.getY(), 30, 24);
    gainKnob.setBounds (r.getX() + 312, r.getY() - 2, 46, 46);
    wave.setBounds (r.getX() + 372, r.getY(), kChainX - 14 - 372 - 12, 52);
    const int perLine = slotsPerLine (getWidth());
    int i = 0;
    for (auto* b : slotButtons)
    {
        b->setBounds (kChainX + (i % perLine) * (kSlotWidth + 6), r.getY() + (i / perLine) * (kSlotHeight + 6), kSlotWidth, kSlotHeight);
        ++i;
    }
    addPluginButton.setBounds (kChainX + (i % perLine) * (kSlotWidth + 6), r.getY() + (i / perLine) * (kSlotHeight + 6), 30, kSlotHeight);
}

void TrackRow::paint (juce::Graphics& g)
{
    ui::hw::drawModule (g, getLocalBounds().toFloat().reduced (1.0f), true);
    if (index >= (int) main.session.tracks.size()) return; // the track was removed and the rows are about to be rebuilt
    auto& track = *main.session.tracks[(size_t) index];
    const auto r = getLocalBounds().reduced (14, 10);
    const auto line2 = juce::Rectangle<int> (r.getX(), r.getY() + 28, 300, 14);
    // the first clip's file, and how many more there are
    const juce::String files = track.clips.empty() ? juce::String ("NO CLIPS")
                                                   : track.clips[0].file.getFileName() + (track.clips.size() > 1 ? "  +" + juce::String ((int) track.clips.size() - 1) : juce::String());
    ui::hw::drawLabel (g, files, line2, juce::Justification::left, colours::dim, 10.0f);
    juce::String detail = timeText (track.length(), main.session.sampleRate);
    if (track.latency() > 0) detail += "   LATENCY " + juce::String (track.latency()) + " SMP";
    ui::hw::drawLabel (g, detail, line2.translated (0, 15), juce::Justification::left, colours::dim, 10.0f);
    ui::hw::drawLabel (g, juce::String (track.gainDb, 1) + " dB", juce::Rectangle<int> (r.getX() + 300, r.getY() + 42, 70, 12), juce::Justification::centred, colours::amber, 9.5f);
}

void TrackRow::refresh()
{
    if (auto* t = index < (int) main.session.tracks.size() ? main.session.tracks[(size_t) index].get() : nullptr)
        wave.fraction = t->length() > 0 ? juce::jlimit (0.0, 1.0, (double) main.position() / (double) t->length()) : 0.0;
    wave.repaint();
}

// ---- TrackList ---------------------------------------------------------------------------------------------------------------
void TrackList::rebuild()
{
    rows.clear();
    for (int i = 0; i < (int) main.session.tracks.size(); ++i)
        addAndMakeVisible (rows.add (new TrackRow (main, i)));
    resized();
}

void TrackList::resized()
{
    int y = 0;
    for (auto* row : rows)
    {
        const int h = row->heightFor (getWidth());
        row->setBounds (0, y, getWidth(), h);
        y += h + kRowGap;
    }
    // the height is the rows', but never less than the viewport's, so the empty list still has a body to draw
    setSize (getWidth(), juce::jmax (y, getParentHeight()));
}

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
void MainComponent::Source::getNextAudioBlock (const juce::AudioSourceChannelInfo& info)
{
    info.clearActiveBufferRegion();
    if (! playing.load())
        return;
    int done = 0;
    auto pos = position.load();
    while (done < info.numSamples)
    {
        const int n = juce::jmin (kBlockSize, info.numSamples - done);
        chunk.setSize (2, n, false, false, true);
        owner.session.processBlock (chunk, pos);
        for (int ch = 0; ch < info.buffer->getNumChannels(); ++ch)
            info.buffer->copyFrom (ch, info.startSample + done, chunk, juce::jmin (ch, 1), 0, n);
        pos += n;
        done += n;
    }
    position = pos;
    if (pos >= owner.endOfPlay())
        playing = false;
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
    makeEditorWindow = [] (const juce::String& title, juce::AudioProcessorEditor* editor, std::function<void()> closed) -> std::unique_ptr<juce::Component>
    {
        return std::make_unique<PluginWindow> (title, editor, std::move (closed));
    };

    addAndMakeVisible (viewport);
    viewport.setViewedComponent (&trackList, false);
    viewport.setScrollBarsShown (true, false);
    for (auto* b : { &newButton, &openButton, &saveButton, &saveAsButton, &addButton, &pluginsButton, &renderButton, &rewindButton, &playButton })
        addAndMakeVisible (*b);
    addAndMakeVisible (guideButton);
    playButton.setClickingTogglesState (false);
    newButton.onClick = [this] { askToDiscard ("Start a new project and lose them?", [this] { newProject(); }); };
    openButton.onClick = [this] { askToDiscard ("Open another project and lose them?", [this] { chooseAndOpen(); }); };
    saveButton.onClick = [this] { saveProject(); };
    saveAsButton.onClick = [this] { saveProjectAs(); };
    addButton.onClick = [this] { chooseAndAdd(); };
    pluginsButton.onClick = [this] { showPluginsMenu(); };
    renderButton.onClick = [this] { chooseAndRender(); };
    rewindButton.onClick = [this] { seek (0); };
    playButton.onClick = [this] { togglePlay(); };
    newButton.setTooltip ("Empty the session.");
    openButton.setTooltip ("Open a saved project (.olympus).");
    saveButton.setTooltip ("Save the project: the tracks, their chains and every plugin's settings. Cmd+S.");
    addButton.setTooltip ("Add stems: audio files, one track each. You can also drop them on the window.");
    pluginsButton.setTooltip ("Where plugins (VST3, and Audio Units on macOS) are looked for, which were found, and a rescan.");
    renderButton.setTooltip ("Write one WAV per audible track, after its chain and fader, and the mix.");
    playButton.setTooltip ("Play from the playhead (space).");

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

void MainComponent::rebuildNow()
{
    cancelPendingUpdate();
    trackList.rebuild();
    resized();
    repaint();
}

// ---- actions -----------------------------------------------------------------------------------------------------------------
void MainComponent::newProject()
{
    closeEditors();
    source.playing = false;
    session.newProject ("Untitled");
    seek (0);
    rebuildNow();
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
    seek (0);
    settings.lastProject = file.getFullPathName();
    settings.save (opts.settingsFile);
    rebuildNow();
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

void MainComponent::addTracks (const juce::StringArray& files)
{
    juce::StringArray problems;
    for (auto& f : files)
    {
        juce::String error;
        if (! session.addTrack (juce::File (f), {}, error))
            problems.add (error);
    }
    rebuildNow();
    if (problems.size() > 0)
        showError (problems.joinIntoString ("\n"));
}

void MainComponent::removeTrack (int index)
{
    closeEditors();
    juce::String error;
    session.removeTrack (index, error);
    requestRebuild();
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
    if (source.position.load() >= endOfPlay()) source.position = 0;
    session.prepare (false);
    source.playing = true;
}

void MainComponent::seek (juce::int64 sample) { source.position = juce::jlimit ((juce::int64) 0, juce::jmax ((juce::int64) 0, endOfPlay()), sample); }

void MainComponent::filesDropped (const juce::StringArray& files, int, int)
{
    if (files.size() == 1 && juce::File (files[0]).hasFileExtension ("olympus"))
        askToDiscard ("Open it and lose them?", [this, f = files[0]] { openProject (juce::File (f)); });
    else
        addTracks (files);
}

// ---- file choosers -----------------------------------------------------------------------------------------------------------
void MainComponent::chooseAndOpen()
{
    chooser = std::make_unique<juce::FileChooser> ("Open a project", juce::File::getSpecialLocation (juce::File::userDocumentsDirectory), "*.olympus");
    chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                          [this] (const juce::FileChooser& fc) { if (fc.getResult() != juce::File()) openProject (fc.getResult()); });
}

void MainComponent::chooseAndAdd()
{
    chooser = std::make_unique<juce::FileChooser> ("Add stems", juce::File::getSpecialLocation (juce::File::userMusicDirectory), kAudioWildcard);
    chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles | juce::FileBrowserComponent::canSelectMultipleItems,
                          [this] (const juce::FileChooser& fc)
                          {
                              juce::StringArray files;
                              for (auto& f : fc.getResults()) files.add (f.getFullPathName());
                              if (! files.isEmpty()) addTracks (files);
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

// ---- plugins -----------------------------------------------------------------------------------------------------------------
void MainComponent::showPluginsMenu()
{
    juce::PopupMenu menu;
    menu.addSectionHeader ("Found: " + juce::String (session.catalog.names().size()) + " plugins");
    for (auto& n : session.catalog.names()) menu.addItem (n, false, false, nullptr);
    menu.addSeparator();
    menu.addItem ("Look in a folder...", [this] { chooseFolder(); });
    menu.addItem ("Rescan plugins", [this] { scanPlugins (true); requestRebuild(); });
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
            requestRebuild();
        });
    menu.showMenuAsync (juce::PopupMenu::Options());
}

void MainComponent::showSlotMenu (int track, int slot)
{
    if (! juce::isPositiveAndBelow (track, (int) session.tracks.size())) return;
    const int count = (int) session.tracks[(size_t) track]->chain.size();
    const bool bypassed = session.tracks[(size_t) track]->chain[(size_t) slot]->bypass;
    juce::PopupMenu menu;
    menu.addItem ("Open", [this, track, slot] { openEditor (track, slot); });
    menu.addItem (bypassed ? "Switch on" : "Bypass", [this, track, slot, bypassed] { juce::String e; session.setBypass (track, slot, ! bypassed, e); requestRebuild(); });
    menu.addItem ("Move earlier", slot > 0, false, [this, track, slot] { closeEditors(); juce::String e; session.movePlugin (track, slot, slot - 1, e); requestRebuild(); });
    menu.addItem ("Move later", slot < count - 1, false, [this, track, slot] { closeEditors(); juce::String e; session.movePlugin (track, slot, slot + 1, e); requestRebuild(); });
    menu.addSeparator();
    menu.addItem ("Remove", [this, track, slot] { closeEditors(); juce::String e; session.removePlugin (track, slot, e); requestRebuild(); });
    menu.showMenuAsync (juce::PopupMenu::Options());
}

void MainComponent::toggleEditor (int track, int slot)
{
    juce::String error;
    if (auto* s = session.slotAt (track, slot, error))
    {
        if (windows.count (s) > 0) windows.erase (s);
        else openEditor (track, slot);
    }
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
    juce::AudioProcessorEditor* editor = s->instance->hasEditor() ? s->instance->createEditorAndMakeActive() : new juce::GenericAudioProcessorEditor (*s->instance);
    if (editor == nullptr) return;
    windows[s] = makeEditorWindow (session.tracks[(size_t) track]->name + " - " + s->plugin, editor,
                                   [this, s, safe = juce::Component::SafePointer<MainComponent> (this)]
                                   { juce::MessageManager::callAsync ([safe, s] { if (safe != nullptr) safe->windows.erase (s); }); });
}

void MainComponent::closeEditors() { windows.clear(); }

// ---- timer, keys, layout -----------------------------------------------------------------------------------------------------
void MainComponent::timerCallback()
{
    playButton.setToggleState (source.playing.load(), juce::dontSendNotification);
    playButton.setButtonText (source.playing.load() ? "Stop" : "Play");
    for (auto* r : trackList.rows) r->refresh();
    repaint (timeBox);
    updateTitle();
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

bool MainComponent::keyPressed (const juce::KeyPress& key)
{
    const auto cmd = juce::ModifierKeys::commandModifier;
    if (key == juce::KeyPress (juce::KeyPress::spaceKey)) { togglePlay(); return true; }
    if (key == juce::KeyPress ('s', cmd, 0)) { saveProject(); return true; }
    if (key == juce::KeyPress ('s', cmd | juce::ModifierKeys::shiftModifier, 0)) { saveProjectAs(); return true; }
    if (key == juce::KeyPress ('o', cmd, 0)) { openButton.triggerClick(); return true; }
    if (key == juce::KeyPress ('n', cmd, 0)) { newButton.triggerClick(); return true; }
    if (key == juce::KeyPress (juce::KeyPress::returnKey)) { seek (0); return true; }
    return false;
}

void MainComponent::resized()
{
    auto r = getLocalBounds();
    const int w = r.getWidth();
    int x = 290;
    const int by = 14, bh = 26;
    for (auto* b : { &newButton, &openButton, &saveButton, &saveAsButton })
    {
        b->setBounds (x, by, 70, bh);
        x += 76;
    }
    x += 12;
    addButton.setBounds (x, by, 90, bh);
    pluginsButton.setBounds (x + 96, by, 80, bh);
    renderButton.setBounds (x + 182, by, 112, bh);
    guideButton.setBounds (w - kMargin - 70, by, 70, bh);
    rewindButton.setBounds (290, 50, 56, bh);
    playButton.setBounds (352, 50, 84, bh);
    timeBox = { 448, 48, 150, 30 };
    noteStrip = { kMargin, r.getHeight() - kMargin - ui::kNoteStripHeight, w - 2 * kMargin, ui::kNoteStripHeight };
    bodyArea = { kMargin, kHeaderHeight + 12, w - 2 * kMargin, noteStrip.getY() - 10 - (kHeaderHeight + 12) };
    viewport.setBounds (bodyArea);
    trackList.setSize (bodyArea.getWidth() - viewport.getScrollBarThickness(), trackList.getHeight());
    trackList.resized();
}

void MainComponent::paint (juce::Graphics& g)
{
    g.fillAll (colours::background);
    ui::hw::drawHeaderPlate (g, juce::Rectangle<float> (0.0f, 0.0f, (float) getWidth(), (float) kHeaderHeight - 6.0f));
    ui::hw::drawNameplate (g, { kMargin, 8, 250, 66 }, "Olympus", "Session host", "Pantheon stems", ! session.tracks.empty(), 24.0f);
    ui::hw::drawGlass (g, timeBox.toFloat(), 3.0f, colours::amber);
    g.setColour (colours::amber);
    g.setFont (ui::lcdFont (17.0f));
    g.drawText (timeText (position(), session.sampleRate > 0.0 ? session.sampleRate : 48000.0), timeBox.reduced (8, 0), juce::Justification::centredRight);
    ui::hw::drawGlassOverlay (g, timeBox.toFloat(), 3.0f, false);
    ui::hw::drawLabel (g, juce::String ((int) session.tracks.size()) + (session.tracks.size() == 1 ? " track" : " tracks") + (session.sampleRate > 0.0 ? "  " + juce::String (session.sampleRate / 1000.0, 1) + " kHz" : juce::String()),
                       { 612, 50, 220, 26 }, juce::Justification::centredLeft, colours::dim, 10.5f);
    if (session.tracks.empty())
    {
        ui::hw::drawModule (g, bodyArea.toFloat(), true);
        g.setColour (colours::dim);
        g.setFont (ui::font (18.0f));
        g.drawText ("Drop the stems here", bodyArea.withTrimmedBottom (30), juce::Justification::centred);
        ui::hw::drawLabel (g, "Audio files, one track each. Then put plugins on them.", bodyArea.withTrimmedTop (30), juce::Justification::centred, colours::dim, 11.0f);
    }
    ui::drawNoteStrip (g, noteStrip, notes());
}
}
