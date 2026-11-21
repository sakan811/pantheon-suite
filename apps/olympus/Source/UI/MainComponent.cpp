// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 The Pantheon authors

#include "MainComponent.h"

namespace olympus::gui
{
namespace
{
const char* const kAudioWildcard = "*.wav;*.aif;*.aiff;*.flac;*.mp3;*.m4a;*.ogg;*.caf";

// The master fader's tooltip: its current value, so the fader can be read without the mouse on the scale.
juce::String masterTooltip (double db)
{
    const juce::String value = db <= -100.0 ? juce::String ("-inf") : (db >= 0.0 ? "+" : "") + juce::String (db, 1);
    return "Master gain " + value + " dB (double-click for 0 dB)";
}
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

// True when a device's output array holds a channel it does not use: JUCE leaves those pointers unspecified (CoreAudio and ALSA compact
// them, so this is hardening), and nothing may read or write through one (DeviceCallback::deviceBlock).
bool hasNullChannel (float* const* channels, int numChannels)
{
    for (int ch = 0; ch < numChannels; ++ch)
        if (channels[ch] == nullptr)
            return true;
    return false;
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

// ---- PluginWindow ------------------------------------------------------------------------------------------------------------
struct PluginWindow::FrameConstrainer : public juce::ComponentBoundsConstrainer
{
    FrameConstrainer (PluginWindow& o, juce::ComponentBoundsConstrainer* i) : owner (o), inner (i)
    {
        syncLimits (0, 0);
    }

    void syncLimits (int extraW, int extraH)
    {
        if (inner == nullptr)
            return;
        constexpr int kInfiniteLimit = 0x3fffffff;
        const int minW = inner->getMinimumWidth() < kInfiniteLimit ? inner->getMinimumWidth() + extraW : inner->getMinimumWidth();
        const int minH = inner->getMinimumHeight() < kInfiniteLimit ? inner->getMinimumHeight() + extraH : inner->getMinimumHeight();
        const int maxW = inner->getMaximumWidth() < kInfiniteLimit ? inner->getMaximumWidth() + extraW : inner->getMaximumWidth();
        const int maxH = inner->getMaximumHeight() < kInfiniteLimit ? inner->getMaximumHeight() + extraH : inner->getMaximumHeight();
        setSizeLimits (minW, minH, maxW, maxH);
    }

    void checkBounds (juce::Rectangle<int>& bounds,
                      const juce::Rectangle<int>& previousBounds,
                      const juce::Rectangle<int>& limits,
                      bool isStretchingTop,
                      bool isStretchingLeft,
                      bool isStretchingBottom,
                      bool isStretchingRight) override
    {
        if (inner == nullptr)
            return;

        int extraW = 0, extraH = 0;
        if (auto* peer = owner.getPeer())
        {
            const auto f = peer->getFrameSize();
            extraW = f.getLeftAndRight();
            extraH = f.getTopAndBottom();
        }
        else
        {
            const auto b = owner.getBorderThickness();
            extraW = b.getLeftAndRight();
            extraH = b.getTopAndBottom() + (owner.isUsingNativeTitleBar() ? 0 : owner.getTitleBarHeight());
        }

        syncLimits (extraW, extraH);

        juce::Rectangle<int> contentBounds (0, 0,
                                            juce::jmax (1, bounds.getWidth() - extraW),
                                            juce::jmax (1, bounds.getHeight() - extraH));
        const juce::Rectangle<int> prevContent (0, 0,
                                                juce::jmax (1, previousBounds.getWidth() - extraW),
                                                juce::jmax (1, previousBounds.getHeight() - extraH));
        const juce::Rectangle<int> contentLimits (0, 0,
                                                  juce::jmax (1, limits.getWidth() - extraW),
                                                  juce::jmax (1, limits.getHeight() - extraH));

        inner->checkBounds (contentBounds, prevContent, contentLimits,
                            isStretchingTop, isStretchingLeft, isStretchingBottom, isStretchingRight);

        const int newW = contentBounds.getWidth() + extraW;
        const int newH = contentBounds.getHeight() + extraH;
        if (isStretchingLeft)
            bounds.setX (previousBounds.getRight() - newW);
        if (isStretchingTop)
            bounds.setY (previousBounds.getBottom() - newH);
        bounds.setSize (newW, newH);
    }

    void resizeStart() override
    {
        owner.clearPendingInitialCenter();
        if (inner != nullptr)
            inner->resizeStart();
    }
    void resizeEnd() override { if (inner != nullptr) inner->resizeEnd(); }

    PluginWindow& owner;
    juce::ComponentBoundsConstrainer* inner = nullptr;
};

PluginWindow::PluginWindow (const juce::String& title, juce::AudioProcessorEditor* editor, std::function<void()> closed,
                            bool addToDesktop, juce::Component* centreAround)
    : juce::DocumentWindow (title, colours::background, juce::DocumentWindow::closeButton, addToDesktop),
      onClosed (std::move (closed)),
      centerTarget (centreAround),
      pendingInitialCenter (addToDesktop)
{
    setUsingNativeTitleBar (true);
    setContentOwned (editor, true);
    if (editor != nullptr && editor->getConstrainer() != nullptr)
    {
        frameConstrainer = std::make_unique<FrameConstrainer> (*this, editor->getConstrainer());
        setConstrainer (frameConstrainer.get());
    }
    setResizable (editor != nullptr && editor->isResizable(), false);
    setAlwaysOnTop (false);
    // Only a window put on the desktop is placed and shown. Centring asks the displays for the primary one, which a bare X server
    // (CI) does not have: getPrimaryDisplay() is null there and the checks, which build windows with addToDesktop = false, crashed.
    if (addToDesktop)
    {
        centreNow();
        {
            const juce::ScopedValueSetter<bool> guard (centeringNow, true);
            setVisible (true);
        }
        juce::Component::SafePointer<PluginWindow> safeThis (this);
        juce::Timer::callAfterDelay (400, [safeThis]
        {
            if (safeThis != nullptr)
                safeThis->pendingInitialCenter = false;
        });
    }
}

PluginWindow::~PluginWindow()
{
    setConstrainer (nullptr);
}

void PluginWindow::centreNow()
{
    const juce::ScopedValueSetter<bool> guard (centeringNow, true);
    centreAroundComponent (centerTarget.getComponent(), getWidth(), getHeight());
}

void PluginWindow::childBoundsChanged (juce::Component* child)
{
    juce::DocumentWindow::childBoundsChanged (child);
    if (pendingInitialCenter && child == getContentComponent() && getWidth() > 0 && getHeight() > 0)
        centreNow();
}

void PluginWindow::moved()
{
    juce::DocumentWindow::moved();
    if (! centeringNow)
        pendingInitialCenter = false;
}

// ---- audio -------------------------------------------------------------------------------------------------------------------
// The callback plays the session block by block. A block never crosses the loop's end: it is cut there, and the rest comes from the
// loop's start. Without a loop the playback stops at the end of the audio (the longest track, and the latency of its chains). While a take
// runs, neither happens: the loop does not wrap and the playhead goes on past the end of the audio. The click with no audio has no end to
// stop at either: it runs free until the click goes off.
void MainComponent::Source::getNextAudioBlock (const juce::AudioSourceChannelInfo& info)
{
    if (! playing.load())
    {
        info.clearActiveBufferRegion();
        return;
    }
    const bool recording = owner.recorder.isRecording();
    const juce::int64 start = loopStart.load(), end = loopEnd.load(), audioEnd = endOfAudio.load();
    const bool clickOnly = metronomeOn.load() && audioEnd <= 0;
    const bool wrap = ! recording && loopOn.load() && end > start;
    juce::int64 pos = position.load();
    const juce::int64 loaded = pos;
    int done = 0;
    while (done < info.numSamples)
    {
        if (wrap && pos >= end)
            pos = start;
        else if (! wrap && ! recording && ! clickOnly && pos >= audioEnd)
        {
            playing = false;
            break;
        }
        const juce::int64 room = wrap ? end - pos : (recording || clickOnly) ? (juce::int64) info.numSamples : audioEnd - pos;
        const int n = (int) juce::jmin ((juce::int64) juce::jmin (kBlockSize, info.numSamples - done), room);
        chunk.setSize (2, n, false, false, true);
        owner.session.processBlock (chunk, pos);
        owner.session.processMonitor (chunk); // the headphones' chain: playback only (Session::processMonitor)
        for (int ch = 0; ch < info.buffer->getNumChannels(); ++ch)
            info.buffer->copyFrom (ch, info.startSample + done, chunk, juce::jmin (ch, 1), 0, n);
        pos += n;
        done += n;
    }
    if (done < info.numSamples)
        info.buffer->clear (info.startSample + done, info.numSamples - done);
    // The playhead moves on from where this block started, and only from there: a seek or a rate's scaled write that landed while the block
    // was being built is the newer decision, so it stands and this one is dropped (a plain store here would swallow it).
    juce::int64 expected = loaded;
    position.compare_exchange_strong (expected, pos);
}

// Audio thread. One device block: the inputs to the recorder, then the transport into the outputs. The take's first sample is the playhead
// of its first block, so the position is read here, before the block plays.
void MainComponent::DeviceCallback::deviceBlock (const float* const* in, int numIn, float* const* out, int numOut, int n)
{
    if (owner.deviceStarting.load())
    {
        // a take still runs (or its files are still opening) until the message thread ends it (applyDeviceStart): these frames are not
        // written, so they are counted as skipped, like the ones that arrive while the take's files open, and the stop reports them with
        // the rest of the missing head
        if (owner.recorder.isRecording() || owner.recordStarting.load())
            owner.recorder.countSkippedWhileOpening (n);
        if (out != nullptr && numOut > 0)
            for (int ch = 0; ch < numOut; ++ch)
                if (out[ch] != nullptr)
                    juce::FloatVectorOperations::clear (out[ch], n);
        return;
    }
    // the take's first sample and its first write are one event: push() is given this block's playhead (read here, before the block plays)
    // and latches it itself, inside the busy scope Recorder::stop() waits for, so a stop cannot come between the write and its start.
    // The blocks that arrive while the take's files open are not written at all; they are counted, so the stop says how much of the head
    // is missing (Recorder::start makes the folder and the headers on this thread's caller, the message thread).
    if (owner.recordStarting.load())
        owner.recorder.countSkippedWhileOpening (n);
    else
        owner.recorder.push (in, numIn, n, owner.source.position.load());
    if (out == nullptr || numOut <= 0 || hasNullChannel (out, numOut))
    {
        // no outputs to write (none at all, or one the device left null): the channels the device can play are cleared, like the silent
        // guard above does (a channel left to the last block would be heard again); the transport still advances, into the scratch, so a
        // take on an input-only device runs and the playhead with it
        if (out != nullptr)
            for (int ch = 0; ch < numOut; ++ch)
                if (out[ch] != nullptr)
                    juce::FloatVectorOperations::clear (out[ch], n);
        // the transport runs in chunks of the prepared scratch, so a block bigger than it still advances the playhead (and the take's
        // start with it). A chunk whose scratch lock is missed advances the position alone while playing, with the same compare-exchange
        // the block's end uses: a seek that landed meanwhile stands and this advance is dropped. No allocation, and the lock is never held
        // across chunks.
        for (int offset = 0; offset < n;)
        {
            const juce::SpinLock::ScopedTryLockType sl (scratchLock);
            const int cap = sl.isLocked() ? scratch.getNumSamples() : 0;
            if (cap > 0)
            {
                const int chunk = juce::jmin (n - offset, cap);
                juce::AudioSourceChannelInfo info (&scratch, 0, chunk);
                owner.source.getNextAudioBlock (info);   // advances the position itself
                offset += chunk;
            }
            else
            {
                const int chunk = juce::jmin (n - offset, kBlockSize);
                if (owner.source.playing.load())   // a stopped transport does not advance, as in the hit branch's getNextAudioBlock
                {
                    juce::int64 expected = owner.source.position.load();
                    owner.source.position.compare_exchange_strong (expected, expected + chunk);
                }
                offset += chunk;
            }
        }
        return;
    }
    juce::AudioBuffer<float> buffer (out, numOut, n);
    juce::AudioSourceChannelInfo info (&buffer, 0, n);
    owner.source.getNextAudioBlock (info);
    // the device's output ceiling: no sample leaves above full scale (the mix is over 1.0 when the master is raised). Only this path is held:
    // the stems and mix.wav are rendered by Session::renderStems and stay float. The master's peak was read before this (Session::processBlock).
    // FloatVectorOperations::clip does not define NaN (SSE turns it into +1.0, full-scale DC), so each sample is limited here: NaN becomes 0,
    // an infinity the rail. No allocation, no lock.
    for (int ch = 0; ch < numOut; ++ch)
        for (int i = 0; i < n; ++i)
        {
            const float x = out[ch][i];
            out[ch][i] = juce::jlimit (-1.0f, 1.0f, x == x ? x : 0.0f);
        }
}

// Before the device's callbacks start (also when the device changes). A take cannot go on across a change of rate or block size, so it
// ends and the audio buffers (scratch, recorder) are prepared on the message thread (marshalled with callAsync if called from another thread).
void MainComponent::DeviceCallback::audioDeviceAboutToStart (juce::AudioIODevice* started)
{
    auto* mm = juce::MessageManager::getInstanceWithoutCreating();
    const bool onMessageThread = mm == nullptr || mm->isThisTheMessageThread();
    const bool hasDevice = started != nullptr;
    MainComponent::DeviceStart start;
    start.seq = ++owner.deviceStartSeq;
    start.hasDevice = hasDevice;
    start.refresh = ! onMessageThread;
    if (hasDevice)
    {
        start.rate = started->getCurrentSampleRate();
        start.bufSize = started->getCurrentBufferSizeSamples();
        start.activeInputs = started->getActiveInputChannels();
    }
    if (onMessageThread)
    {
        owner.applyDeviceStart (start);
        return;
    }
    owner.deviceStarting = true;   // silent outputs until the new device is prepared (applyDeviceStart lets go of it, or keeps it while it waits)
    const auto safe = juce::Component::SafePointer<MainComponent> (&owner);
    juce::MessageManager::callAsync ([safe, start]
    {
        if (safe != nullptr)
            safe->applyDeviceStart (start);
    });
}

// The message thread's part of a device start (see audioDeviceAboutToStart). Nothing here waits for the session's lock while a render holds
// it for its whole run: the start is kept and the timer tries again once the render is over, because the render's progress window needs this
// thread to paint and to answer Cancel. A take cannot be running during a render (a render refuses while REC runs, and this ends a take that
// a device change caught), so waiting costs nothing but the moment the new rate is adopted.
void MainComponent::applyDeviceStart (const DeviceStart& start)
{
    struct ClearStarting
    {
        MainComponent* s;
        std::uint64_t seq;
        bool keep = false;   // the start waits for a render: the silent-output guard stays set across the deferrals
        ~ClearStarting() { if (! keep && seq == s->deviceStartSeq.load()) s->deviceStarting = false; }
    } clear { this, start.seq };
    if (start.seq != deviceStartSeq.load())
    {
        // A newer start (or a device that stopped) took over: this one is stale, and only what it deferred itself is thrown away. The kept
        // start can be newer than this call (its lambda was queued before it and ran after), and nothing else would apply it: the timer
        // would find nothing to retry and the silent-output guard would stay set until the next device event.
        if (pendingDeviceStart.has_value() && pendingDeviceStart->seq == start.seq)
            pendingDeviceStart.reset();
        return;
    }
    if (session.renderRunning())
    {
        pendingDeviceStart = start;
        deviceStarting = true;   // the device is already the new one and its scratch and recorder are not: hold its blocks silent, and keep
                                 // the flag set across the deferrals (the timer's apply is what lets go of it)
        clear.keep = true;
        return;
    }
    pendingDeviceStart.reset();
    if (recorder.isRecording())
        stopRecording ("The audio device changed while recording: the take was kept up to where it changed.");
    readDevice();
    if (! start.hasDevice)
        return;
    deviceRate = start.rate;
    io.prepareScratch (start.bufSize);
    recorder.prepare (start.rate, start.bufSize, start.activeInputs);
    adoptDeviceRate();
    if (start.refresh)
        refreshNow();
}

// Device thread. Only a flag: the timer ends a take that the device stopped under (on the message thread).
void MainComponent::DeviceCallback::audioDeviceStopped()
{
    ++owner.deviceStartSeq;
    owner.deviceStarting = false;
    owner.deviceStopped = true;
}

// ---- the window --------------------------------------------------------------------------------------------------------------
MainComponent::MainComponent() : MainComponent (Options {}) {}

MainComponent::MainComponent (Options options) : opts (options)
{
    setLookAndFeel (&look);
    setWantsKeyboardFocus (true);
    settings = Settings::load (opts.settingsFile);
    lastAutosaveMs = juce::Time::getMillisecondCounterHiRes();
    // a pedal the last start left: it died loading the monitor chain or the last project, which are skipped this time (Settings.h)
    monitorSkipped = Settings::takePedal (Settings::monitorPedalFile (opts.settingsFile));
    projectSkipped = Settings::takePedal (Settings::projectPedalFile (opts.settingsFile));

    // The questions. Save / Don't save / Cancel: showYesNoCancelBox returns 1 for the first button, 2 for the second, 0 for the third.
    askUnsaved = [] (const juce::String& message) -> Unsaved
    {
        const int answer = juce::AlertWindow::showYesNoCancelBox (juce::MessageBoxIconType::QuestionIcon, "Olympus", message, "Save", "Don't save", "Cancel");
        return answer == 1 ? Unsaved::save : answer == 2 ? Unsaved::discard : Unsaved::cancel;
    };
    chooseSaveFile = [this] (const juce::String& title, const juce::File& start, std::function<void (const juce::File&)> done)
    {
        if (dialogOpen) return;   // one dialog at a time: a second chooser would destroy the one that is open
        dialogOpen = true;
        chooser = std::make_unique<juce::FileChooser> (title, start, "*.olympus");
        chooser->launchAsync (juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles | juce::FileBrowserComponent::warnAboutOverwriting,
                              [safe = juce::Component::SafePointer<MainComponent> (this), done] (const juce::FileChooser& fc)
                              {
                                  if (safe == nullptr) return;
                                  safe->dialogOpen = false;
                                  done (fc.getResult());
                              });
    };
    askRecover = [] (const juce::String& message)
    {
        return juce::AlertWindow::showOkCancelBox (juce::MessageBoxIconType::QuestionIcon, "Olympus", message, "Open it", "Ignore");
    };
    askReload = [] (const juce::String& message)
    {
        return juce::AlertWindow::showOkCancelBox (juce::MessageBoxIconType::QuestionIcon, "Olympus", message, "Reload", "Keep mine");
    };
    notify = [] (const juce::String& message) { juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::InfoIcon, "Olympus", message); };
    reveal = [] (const juce::File& folder) { folder.revealToUser(); };
    reportError = [] (const juce::String& message) { juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, "Olympus", message); };
    makeEditorWindow = [this] (const juce::String& title, juce::AudioProcessorEditor* pluginEditor, std::function<void()> closed) -> std::unique_ptr<juce::Component>
    {
        return std::make_unique<PluginWindow> (title, pluginEditor, std::move (closed), true, this);
    };

    // the arrangement reports what the user did; the window owns the transport, the files, the menus and the plugin windows
    arrange.dialogOpen = &dialogOpen;
    arrange.onSeek = [this] (juce::int64 s) { seek (s); };
    arrange.onStructure = [this] { syncTransport(); updateTitle(); updateTransport(); repaint (noteStrip); };
    arrange.onTrackMenu = [this] (int track) { showTrackMenu (track); };
    arrange.onRemoveTrack = [this] (int track) { removeTrack (track); };
    arrange.onFilesDropped = [this] (const juce::StringArray& files, int track, juce::int64 at)
    {
        if (! refuseWhileBusy()) importFiles (files, track, at);
    };
    arrange.onProjectDropped = [this] (const juce::File& file) { openProjectRequested (file); };
    arrange.reportError = [this] (const juce::String& message) { showError (message); };
    arrange.onInputMenu = [this] (int track) { showInputMenu (track); };
    // an armed track's meter shows its input: the larger of its one or two device inputs, since the last read
    arrange.inputLevel = [this] (int track)
    {
        const juce::ScopedTryLock sl (session.lock);
        if (! sl.isLocked() || session.renderRunning()) return 0.0f;
        if (! juce::isPositiveAndBelow (track, (int) session.tracks.size())) return 0.0f;
        const auto& t = *session.tracks[(size_t) track];
        float peak = recorder.inputPeak (t.inputLeft);
        if (t.inputRight >= 0) peak = juce::jmax (peak, recorder.inputPeak (t.inputRight));
        return peak;
    };
    addAndMakeVisible (arrange);

    for (auto* b : { &newButton, &openButton, &saveButton, &saveAsButton, &addTrackButton, &importButton, &pluginsButton, &renderButton, &exportButton, &audioButton,
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
    // the master section: the fader (a drag is one undo step: its Gesture lives from the drag's start to its end), the peak read-out and CLIP
    addAndMakeVisible (masterFader);
    masterFader.setSliderStyle (juce::Slider::LinearHorizontal);
    masterFader.setTextBoxStyle (juce::Slider::NoTextBox, true, 0, 0);
    masterFader.setRange (-100.0, 12.0, 0.01);     // -100 is silence (-inf); the Editor clamps the same range
    masterFader.setDoubleClickReturnValue (true, 0.0);
    masterFader.setWantsKeyboardFocus (false);
    masterFader.setTooltip (masterTooltip (0.0));
    masterFader.onDragStart = [this] { masterGesture = std::make_unique<Editor::Gesture> (editor, "Master gain"); };
    masterFader.onDragEnd = [this] { masterGesture.reset(); };
    masterFader.onValueChange = [this]
    {
        editor.setMasterDb ((float) masterFader.getValue());
        masterFader.setTooltip (masterTooltip (masterFader.getValue()));
    };
    addAndMakeVisible (masterPeakLabel);
    masterPeakLabel.setFont (ui::font (10.5f));
    masterPeakLabel.setColour (juce::Label::textColourId, colours::dim);
    masterPeakLabel.setJustificationType (juce::Justification::centred);
    masterPeakLabel.setTooltip ("The master's peak over the last 2 seconds, dBFS, read before the output ceiling of the device.");
    addAndMakeVisible (masterClipLamp);
    masterClipLamp.setTooltip ("CLIP: lit once the master reaches 0 dBFS (the device's output is held at full scale). Click to clear it.");
    masterClipLamp.onClick = [this] { clearMasterClip(); };
    // a focused button would take Space and Return for itself: the keys stay with the window
    for (auto* c : std::initializer_list<juce::Component*> { &newButton, &openButton, &saveButton, &saveAsButton, &addTrackButton, &importButton, &pluginsButton,
                                                             &renderButton, &exportButton, &audioButton, &rewindButton, &undoButton, &redoButton, &zoomOutButton, &zoomInButton,
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
    exportButton.onClick = [this] { chooseAndExport(); };
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
    exportButton.setTooltip ("Export the final mix (mix.wav) without stems or the monitor chain");
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
        if (! cached)
        {
            scanQueued = true;   // the note of an empty scan waits for this scan (notes())
            juce::MessageManager::callAsync ([safe = juce::Component::SafePointer<MainComponent> (this)] { if (safe != nullptr) safe->scanPlugins (false, true); });
        }
    }
    // the checks fill the catalog here, before the monitor chain is read (the app's scan does it above, or after the first scan)
    if (opts.fillCatalog)
        opts.fillCatalog (session.catalog);
    if (! scanQueued)
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
    if (searchWindow != nullptr)
        searchWindow->exitModalState (0);
    audioDialog.reset();
    closeEditors();
    setLookAndFeel (nullptr);
}

void MainComponent::scanPlugins (bool rescan, bool suiteOnlyScan)
{
    if (scanning) return;
    scanQueued = false;
    scanning = true;
    session.catalog.setSuiteOnly (settings.scanSuiteOnly);
    ScanOptions options;
    options.includeSystem = opts.includeSystemPlugins;
    options.rescan = rescan;
    options.suiteOnly = settings.scanSuiteOnly || suiteOnlyScan;
    options.cacheFile = Settings::pluginCacheFile (opts.settingsFile);
    options.deadMansPedal = Settings::scanPedalFile (opts.settingsFile);
    session.catalog.scan (settings.searchPath(), options);
    session.catalog.saveCache (options.cacheFile);
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
    advice.scanning = scanning || scanQueued;
    monitorLatencyMs();   // refreshes the cache with a try-lock (never waits: notes() runs in paint)
    advice.monitorLatencySamples = monitorLatencyCacheSamples;
    const juce::ScopedTryLock sl (session.lock);
    if (sl.isLocked() && ! session.renderRunning())
    {
        notesCache = adviceFor (session, advice);
        return notesCache;
    }
    // The lock is busy or a render runs: what was read with the lock last time, even when that is nothing. adviceFor reads the tracks, the
    // clips and the latencies, so it is never called here without the lock (the same rule as monitorLatencyMs above).
    return notesCache;
}

void MainComponent::askToDiscard (const juce::String& action, std::function<void()> proceed)
{
    // NEW, OPEN and Quit wait while a render runs or another dialog is open (the message thread stays free, and no second chooser replaces one)
    if (refuseWhileBusy()) return;
    // a take that runs is placed first: its clips are what would be lost, and the question then sees them as changes
    if (recorder.isRecording())
        stopRecording();
    if (! session.dirty) { proceed(); return; }
    auto answer = Unsaved::discard;
    {
        const DialogBusy busy (dialogOpen);
        answer = askUnsaved ? askUnsaved ("The project has changes that are not saved. " + action) : Unsaved::discard;
    }
    if (answer == Unsaved::cancel) return;
    // the discarded work's autosave is deleted by the action itself, once that action has gone through (NEW, OPEN, Quit)
    if (answer == Unsaved::discard) { proceed(); return; }
    // Save: a project with a file is written here; one without gets the Save As chooser, and goes on only once its file is written
    if (session.projectFile != juce::File())
    {
        if (saveTo (session.projectFile))
            proceed();
        return;
    }
    // the chooser that asked is still inside its callback: the action (it may open another chooser) runs on the next message
    saveProjectAs ([safe = juce::Component::SafePointer<MainComponent> (this), proceed = std::move (proceed)] (bool saved)
    {
        if (safe == nullptr || ! saved) return;
        juce::MessageManager::callAsync ([safe, proceed]
        {
            if (safe != nullptr) proceed();
        });
    });
}

bool MainComponent::refuseWhileBusy()
{
    if (session.renderRunning())
    {
        showError ("A render is running: wait for it or cancel it first.");
        return true;
    }
    return dialogOpen;   // a dialog is open: its answer is the one that counts
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
    const auto posText = positionText (session, position());
    const bool recNow = recorder.isRecording();
    const bool recChanged = recNow != lastRecordingState;
    if (posText != lastPositionText || recChanged)
    {
        lastPositionText = posText;
        lastRecordingState = recNow;
        repaint (positionBox);
    }
    if (recChanged || recNow)
        repaint ({ positionBox.getX() - 148, kRowC, 140, 28 });
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
    // The recorder meters and records the first Recorder::kMaxInputs device inputs, so the menu stops there too (an input past them is
    // refused by the recorder with its own reason, and the AUDIO dialog offers no more than this).
    const int highest = juce::jmin (state.activeInputs.getHighestBit(), Recorder::kMaxInputs - 1);
    for (int i = 0; i <= highest; ++i)
    {
        if (! state.activeInputs[i])
            continue;
        // the device's own name for the input, when it has one ("In 3: Guitar"); the number is the device's, as the take records it
        const auto name = i < state.inputNames.size() ? state.inputNames[i].trim() : juce::String();
        out.push_back ({ "In " + juce::String (i + 1) + (name.isNotEmpty() ? ": " + name : juce::String()), i, -1 });
        if (i + 1 < Recorder::kMaxInputs && state.activeInputs[i + 1])
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
    juce::String error;
    if (! session.adoptRate (deviceRate, error))
    {
        showError (error);
        return;
    }
    if (session.sampleRate == before)
        return;
    editor.clearUndo();
    // The playhead goes to the same moment in seconds at the new rate, read again now rather than before the (slow) conversion, which may
    // have left the transport further on. It is put in with a compare-exchange and retried while a block still being built moves it: that
    // advance only takes effect from the value the block loaded, so this store is not swallowed by it, and the scale is never taken from a
    // position that is already past. Message thread only; the callback never waits here.
    if (before > 0.0)
    {
        const auto scaledFrom = [this, before] (juce::int64 from)
        { return (juce::int64) std::llround ((double) from * session.sampleRate / before); };
        bool landed = false;
        for (int attempt = 0; attempt < 16 && ! landed; ++attempt)
        {
            const juce::int64 current = source.position.load();
            juce::int64 expected = current;
            landed = source.position.compare_exchange_strong (expected, scaledFrom (current));
        }
        if (! landed)   // a block kept moving the playhead while the retries ran out: the store goes in regardless. The device's rate wins
                        // over a seek that lands in the same moment here — a start redefines where the playhead is (the same second at the
                        // new rate), so the position it overtakes is not one anyone meant to keep. Nothing else puts it right.
            source.position.store (scaledFrom (source.position.load ()));
    }
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
        // the inputs the dialog may switch on are exactly the ones the recorder meters and the IN menu offers (Recorder::kMaxInputs), so no
        // input can be chosen that a take then refuses
        audioDialog = std::make_unique<AudioDialog> (std::make_unique<juce::AudioDeviceSelectorComponent> (deviceManager, 0, Recorder::kMaxInputs, 0, 2, false, false, false, false));
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
        menu.addItem (c.label, true, c.left == current.inputLeft && c.right == current.inputRight,
                      [safe = juce::Component::SafePointer<MainComponent> (this), track, c]
        {
            if (safe == nullptr) return;
            juce::String error;
            if (! safe->session.setTrackInput (track, c.left, c.right, error)) safe->showError (error);
            safe->requestRefresh();
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
    // A render holds the session's lock for its whole run, and the set-up below waits for it three times (the take's latency, the session's
    // prepare, the transport's copy): refused like every other action that runs while a render does (refuseWhileBusy).
    if (session.renderRunning())
    {
        showError ("A render is running: wait for it or cancel it first.");
        return false;
    }
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
    takeFrom = position();
    setTakeLatency();
    syncTransport();
    session.prepare (false);
    // recordStarting: the blocks that arrive until the files are open are counted as skipped, not written. Recorder::start clears this
    // take's start and its counters and raises the flag itself, after the clear and before it opens the files, so a block counted in the
    // window is not erased by the count; it is let go of here, once the take began or was refused.
    if (! beginRecording (recorder, session, recordingFolder(), device.rate, error, &recordStarting))
    {
        recordStarting = false;
        showError (error);
        return false;
    }
    source.playing = true;
    recordStarting = false;
    session.dirty = true;   // a take's WAV is on disk from its first block: the project is dirty now, so a quit asks and the autosave runs
    {
        const juce::ScopedLock sl (session.lock);
        arrange.recordingTracks.clear();
        for (int i = 0; i < (int) session.tracks.size(); ++i)
            if (session.tracks[(size_t) i]->armed)
                arrange.recordingTracks.push_back (i);
    }
    arrange.recordingFrom = arrange.recordingTo = (double) takeFrom;
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
    // where the take really began (the playhead of its first block, which stop() waited for) and how much of its head never got written
    // because the files were still opening. The first is -1 when no block ran, the second 0 when the window cost nothing.
    const juce::int64 heard = recorder.takeStartSample();
    const juce::int64 headSkipped = recorder.samplesSkippedWhileOpening();
    finishRecordingTakes (takes, heard, headSkipped, problem);
}

void MainComponent::finishRecordingTakes (const std::vector<Recorder::Finished>& takes, juce::int64 heard, juce::int64 headSkipped,
                                          const juce::String& problem)
{
    const juce::int64 start = heard >= 0 ? heard : takeFrom;
    const juce::int64 begin = juce::jmax ((juce::int64) 0, start - takeLatency);

    juce::String error;
    std::vector<ClipRef> placed;
    if (! finishRecording (takes, start, (int) takeLatency, editor, error, &placed) && error.isEmpty())
        error = "A take could not be placed on its track.";
    for (const auto& take : takes)
        if (take.frames > 0) { session.dirty = true; break; }   // a take that placed nothing changes nothing

    // the clips the takes actually became, in the order of the takes: not any clip that happens to start at the same sample, which would
    // win the selection when a take was skipped and scroll the view to a clip the user never recorded
    ClipRef first;
    {
        const juce::ScopedLock sl (session.lock);
        for (const auto& ref : placed)
            if (ref.valid() && juce::isPositiveAndBelow (ref.track, (int) session.tracks.size())
                && juce::isPositiveAndBelow (ref.clip, (int) session.tracks[(size_t) ref.track]->clips.size()))
            {
                first = ref;
                break;
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
    // the disk problems of the takes: the samples lost where the disk could not keep up (the largest of the takes, which hear the same blocks), and a file
    // whose writes failed (it keeps what was written)
    // Dropped frames are not written, so a lossy take is shorter by that much and what follows the loss sits that much early: no silence is
    // padded in. Several takes that lost samples name their tracks; the largest loss is the one reported by number.
    juce::int64 lost = 0;
    juce::StringArray lossTracks;
    juce::String largestTrack;
    for (const auto& take : takes)
    {
        const bool validTrack = juce::isPositiveAndBelow (take.track, (int) session.tracks.size());
        const juce::String trackName = validTrack ? session.tracks[(size_t) take.track]->name : ("Track " + juce::String (take.track + 1));
        if (take.lost > lost) { lost = take.lost; largestTrack = trackName; }
        if (take.lost > 0 && validTrack) lossTracks.add (trackName);
        if (take.failed) problems.add ("The take could not be written: " + take.file.getFullPathName());
    }
    if (lost > 0)
    {
        const double ms = device.rate > 0.0 ? 1000.0 * (double) lost / device.rate : 0.0;
        const juce::String amount = juce::String (lost) + " samples (" + juce::String (ms, 0) + " ms) of the take";
        lossTracks.removeString (largestTrack);
        const juce::String where = lossTracks.size() > 0
            ? " on track '" + largestTrack + "' (the largest loss; the takes on " + lossTracks.joinIntoString (", ") + " lost some too)"
            : juce::String();
        problems.add (amount + where + " were lost because the disk could not keep up: the take is shorter by that much and what follows the loss sits that much early.");
    }
    // the head the start cost (the files opening, or the device's start guard): those frames of input were never pushed to the takes, so
    // the take begins where its first frame was written (named as a loss is, because it is one; nothing is padded in)
    if (headSkipped > 0)
    {
        const double ms = device.rate > 0.0 ? 1000.0 * (double) headSkipped / device.rate : 0.0;
        problems.add (juce::String (headSkipped) + " samples (" + juce::String (ms, 0) + " ms) at the head of the take were not written before the take could run: the take begins where its first frame was written.");
    }
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
    const juce::int64 heard = recorder.takeStartSample();
    arrange.recordingFrom = (double) (heard >= 0 ? heard : takeFrom);
    arrange.recordingTo = (double) position();
    repaint ({ positionBox.getX() - 148, kRowC, 140, 28 });
}

// The master section, on the timer. The session's peaks are read here and only here (exchange resets them). They are read before the device's
// output ceiling, so an over shows on CLIP although the device is held at full scale.
void MainComponent::updateMaster (double nowMs)
{
    const float left = session.masterPeakL.exchange (0.0f), right = session.masterPeakR.exchange (0.0f);
    const float peak = juce::jmax (left, right);
    const float prevL = masterLeft.level, prevLH = masterLeft.hold;
    const float prevR = masterRight.level, prevRH = masterRight.hold;
    masterLeft.add ((float) dbOf (left));
    masterRight.add ((float) dbOf (right));
    if (peak >= 1.0f)
        clipLatched = true;             // a sample at or over full scale (0 dBFS): latched until clicked or a new project
    if (masterClipLamp.lit != clipLatched)
    {
        masterClipLamp.lit = clipLatched;
        masterClipLamp.repaint();
    }
    // the peak read-out: the highest peak of the last kMasterHoldMs, then the current one
    const float db = (float) dbOf (peak);
    if (db >= masterHoldDb || nowMs - masterHoldMs >= kMasterHoldMs)
    {
        masterHoldDb = db;
        masterHoldMs = nowMs;
    }
    const auto peakText = masterHoldDb <= -100.0f ? juce::String ("-inf dB") : juce::String (masterHoldDb, 1) + " dB";
    if (masterPeakLabel.getText() != peakText)
        masterPeakLabel.setText (peakText, juce::dontSendNotification);
    updateMasterFader();
    if (std::abs (masterLeft.level - prevL) > 1e-4f || std::abs (masterLeft.hold - prevLH) > 1e-4f
        || std::abs (masterRight.level - prevR) > 1e-4f || std::abs (masterRight.hold - prevRH) > 1e-4f)
        repaint (masterBarLeft.getUnion (masterBarRight).expanded (2.0f, 2.0f).toNearestInt()); // the meter's bars
}

void MainComponent::clearMasterClip()
{
    clipLatched = false;
    masterClipLamp.lit = false;
    masterClipLamp.repaint();
}

// The fader shows the session's gain (undo, a project, the server): it is not moved while the user drags it, and not while a block or a render
// holds the lock (the next tick looks again).
void MainComponent::updateMasterFader()
{
    if (masterFader.isMouseButtonDown())
        return;
    const juce::ScopedTryLock sl (session.lock);
    if (! sl.isLocked())
        return;
    // within 0.05 dB is the same gain: the slider snaps to its interval (0.01), and a tighter test would set it again on every tick
    if (std::abs (masterFader.getValue() - (double) session.masterDb) > 0.05)
    {
        masterFader.setValue (session.masterDb, juce::dontSendNotification);
        masterFader.setTooltip (masterTooltip (masterFader.getValue()));
    }
}

// ---- actions -----------------------------------------------------------------------------------------------------------------
void MainComponent::newProject()
{
    stopRecording();
    deleteAutosaves();   // the session it replaces is gone: its autosave goes with it
    closeTrackEditors();
    source.playing = false;
    session.newProject ("Untitled");
    lastEditorHash = 0;
    loadedProjectMtime = {};
    editor.clearUndo();
    editor.clearClipboard();
    clearMasterClip();   // a new project starts with no CLIP latched
    adoptDeviceRate();
    seek (0);
    arrange.selected = {};
    arrange.zoom = ArrangeView::kDefaultZoom;
    arrange.scrollTo (0);
    refreshNow();
}

bool MainComponent::openProject (const juce::File& file)
{
    if (! file.existsAsFile())
    {
        showError (file.getFileName() + " could not be found.");
        return false;
    }
    projectSkipped = false;   // the user opened one: the note about the last start's project is no longer news
    stopRecording();
    closeTrackEditors();
    source.playing = false;
    juce::String error;
    if (! session.load (file, error))
    {
        showError (error);
        return false;
    }
    lastEditorHash = 0;
    loadedProjectMtime = file.getLastModificationTime();
    editor.clearUndo();
    editor.clearClipboard();
    clearMasterClip(); // a project opened starts with no CLIP latched, as NEW does
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
    return true;
}

void MainComponent::openProjectRequested (const juce::File& file)
{
    if (refuseWhileBusy()) return;
    askToDiscard ("Open another project and lose them?", [safe = juce::Component::SafePointer<MainComponent> (this), file]
    {
        if (safe == nullptr) return;
        const auto discarded = safe->autosaveFile();
        if (safe->openProject (file))
            discarded.deleteFile();
    });
}

bool MainComponent::saveProject()
{
    if (session.projectFile == juce::File()) { saveProjectAs(); return false; }
    return saveTo (session.projectFile);
}

void MainComponent::saveProjectAs (std::function<void (bool)> done)
{
    const auto start = session.projectFile == juce::File() ? juce::File::getSpecialLocation (juce::File::userDocumentsDirectory) : session.projectFile;
    chooseSaveFile ("Save the project", start, [safe = juce::Component::SafePointer<MainComponent> (this), done] (const juce::File& file)
    {
        if (safe == nullptr) return;
        const bool saved = file != juce::File() && safe->saveTo (file.withFileExtension ("olympus"));
        if (done) done (saved);
    });
}

bool MainComponent::saveTo (const juce::File& file)
{
    const juce::File before = session.projectFile;   // the file this session was saved to until now (none when untitled)
    juce::String error;
    if (! session.save (file, error)) { showError (error); return false; }
    lastEditorHash = 0;
    loadedProjectMtime = file.getLastModificationTime();
    settings.lastProject = file.getFullPathName();
    settings.save (opts.settingsFile);
    deleteAutosaves();   // the project is on disk: its autosave is no longer news
    // Save As: the old project's autosave holds this session's work too, so it goes; an untitled session's autosave goes with its first save
    if (before != juce::File() && before != file) autosaveFileFor (before).deleteFile();
    if (before == juce::File()) untitledAutosaveFile().deleteFile();
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
    syncTransport();   // the callback's copy of the click follows the switch, so a click-only playback stops when it goes off
    updateTransport();
}

void MainComponent::zoomBy (double factor)
{
    arrange.zoomBy (factor);
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
        editor.clearClipboard();   // it remembers the copy's track by number: the tracks after the removed one have moved up
        arrange.selected = {};
    }
    else showError (error);
    requestRefresh();
}

// editorsClosed: the caller (chooseAndRender) has already closed the tracks' windows on the message thread, so this may run on the progress
// window's worker thread; a direct call (the checks, on the message thread) closes them here.
juce::String MainComponent::renderTo (const juce::File& folder, const std::function<bool (float)>& progress, bool editorsClosed,
                                     RenderOptions options, RenderResult* result)
{
    source.playing = false;
    if (! editorsClosed)
        closeTrackEditors();
    options.folder = folder;
    RenderResult local;
    juce::String error;
    if (! session.renderStems (options, result != nullptr ? *result : local, error, progress))
        return error.isNotEmpty() ? error : juce::String ("The render failed.");
    return {};
}

void MainComponent::togglePlay()
{
    if (recorder.isRecording()) { stopRecording(); return; }
    if (source.playing.load()) { source.playing = false; return; }
    if (session.tracks.empty() || (session.length() <= 0 && ! session.loopOn && ! session.metronomeOn))
    {
        if (notify) notify ("No tracks or audio yet: drop audio files on the window or click Import audio.");
        return;
    }
    syncTransport();
    // at the end of the audio (and not looping), playing starts again from the beginning
    if (! session.loopOn && position() >= endOfPlay()) seek (0);
    session.prepare (false);
    source.playing = true;
}

// , and . : the marker before or after the playhead. With no marker that way nothing moves (and nothing is said).
void MainComponent::jumpToMarker (bool forward)
{
    const juce::int64 at = position();
    bool found = false;
    juce::int64 target = 0;
    {
        const juce::ScopedLock sl (session.lock);
        for (auto& m : session.markers)
        {
            const bool beyond = forward ? m.start > at : m.start < at;
            if (! beyond) continue;
            if (! found || (forward ? m.start < target : m.start > target))
            {
                target = m.start;
                found = true;
            }
        }
    }
    if (found) seek (target);
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
    session.clearDelayRings();
    source.position = juce::jmax ((juce::int64) 0, sample);
    syncPlayhead();
}

void MainComponent::syncTransport()
{
    const juce::ScopedLock sl (session.lock);
    source.loopOn = session.loopOn;
    source.loopStart = session.loopStart;
    source.loopEnd = session.loopEnd;
    source.metronomeOn = session.metronomeOn;
    source.endOfAudio = session.length() + session.maxLatency();
}

void MainComponent::filesDropped (const juce::StringArray& files, int, int)
{
    if (refuseWhileBusy()) return;
    // dropped outside the arrangement: a project opens, audio goes on new tracks at the playhead
    if (files.size() == 1 && juce::File (files[0]).hasFileExtension ("olympus"))
        openProjectRequested (juce::File (files[0]));
    else
        importFiles (files, -1, position());
}

// ---- file choosers -----------------------------------------------------------------------------------------------------------
void MainComponent::chooseAndOpen()
{
    if (dialogOpen) return;
    dialogOpen = true;
    chooser = std::make_unique<juce::FileChooser> ("Open a project", juce::File::getSpecialLocation (juce::File::userDocumentsDirectory), "*.olympus");
    chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                          [safe = juce::Component::SafePointer<MainComponent> (this)] (const juce::FileChooser& fc)
                          {
                              if (safe == nullptr) return;
                              safe->dialogOpen = false;
                              const auto file = fc.getResult();
                              if (file == juce::File()) return;
                              juce::MessageManager::callAsync ([safe, file]
                              {
                                  if (safe == nullptr) return;
                                  const auto discarded = safe->autosaveFile();   // the session the answer discarded (still open until now)
                                  // a failed open keeps it: only an open that happened discards the work
                                  if (safe->openProject (file))
                                      discarded.deleteFile();
                              });
                          });
}

void MainComponent::chooseAndImport()
{
    const auto at = position();
    if (dialogOpen) return;
    dialogOpen = true;
    const auto start = juce::File::getSpecialLocation (juce::File::userMusicDirectory);
    auto finish = [safe = juce::Component::SafePointer<MainComponent> (this), at] (const juce::StringArray& files)
    {
        if (safe == nullptr) return;
        safe->dialogOpen = false;
        if (! files.isEmpty()) safe->importFiles (files, -1, at);
    };
    if (chooseImportFiles)
    {
        chooseImportFiles (start, std::move (finish));
        return;
    }
    chooser = std::make_unique<juce::FileChooser> ("Import audio", start, kAudioWildcard);
    chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles | juce::FileBrowserComponent::canSelectMultipleItems,
                          [finishCb = std::move (finish)] (const juce::FileChooser& fc)
                          {
                              juce::StringArray files;
                              for (auto& f : fc.getResults()) files.add (f.getFullPathName());
                              finishCb (files);
                          });
}

juce::File MainComponent::defaultPluginFolder() const
{
    if (! settings.pluginFolders.isEmpty())
    {
        const juce::File last (settings.pluginFolders[settings.pluginFolders.size() - 1]);
        if (last.isDirectory() && ! Catalog::isForbiddenPluginRoot (last))
            return last;
    }
    const juce::File sysVst3 ("/Library/Audio/Plug-Ins/VST3");
    if (sysVst3.isDirectory())
        return sysVst3;
    const juce::File userVst3 = juce::File::getSpecialLocation (juce::File::userHomeDirectory).getChildFile ("Library/Audio/Plug-Ins/VST3");
    if (userVst3.isDirectory())
        return userVst3;
    return opts.settingsFile.getParentDirectory();
}

bool MainComponent::addPluginFolder (const juce::File& folder)
{
    if (folder == juce::File())
        return false;
    if (Catalog::isForbiddenPluginRoot (folder))
    {
        showError ("Choose a specific plugin folder (such as a VST3 or build folder), not your home or a protected folder.");
        return false;
    }
    settings.pluginFolders.addIfNotAlreadyThere (folder.getFullPathName());
    settings.save (opts.settingsFile);
    scanPlugins (false, false);
    return true;
}

void MainComponent::chooseFolder()
{
    if (dialogOpen) return;
    dialogOpen = true;
    chooser = std::make_unique<juce::FileChooser> ("The folder with the built plugins (.vst3), or the repository's plugins folder", defaultPluginFolder());
    chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories,
                          [safe = juce::Component::SafePointer<MainComponent> (this)] (const juce::FileChooser& fc)
                          {
                              if (safe == nullptr) return;
                              safe->dialogOpen = false;
                              safe->addPluginFolder (fc.getResult());
                          });
}

// The render dialog's default: bit depth and mix only, the choice remembered in settings.xml. Modal, so the caller gets the answer at once.
bool MainComponent::askRenderDialog (RenderOptions& options)
{
    juce::AlertWindow box ("Render the stems", "The mix is the sum of every audible track, with the master gain. Stems are one file per track.",
                           juce::AlertWindow::NoIcon);
    box.addComboBox ("bits", { "32-bit float", "24-bit", "16-bit" }, "Bit depth:");
    box.getComboBoxComponent ("bits")->setSelectedItemIndex (options.bits == 16 ? 2 : options.bits == 24 ? 1 : 0, juce::dontSendNotification);
    auto* mixOnly = new juce::ToggleButton ("Mix only (no stems)");   // the box takes ownership of it
    mixOnly->setToggleState (! options.writeStems, juce::dontSendNotification);
    box.addCustomComponent (mixOnly);
    box.addButton ("Render", 1, juce::KeyPress (juce::KeyPress::returnKey));
    box.addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));
    if (box.runModalLoop() != 1)
        return false;
    const int index = box.getComboBoxComponent ("bits")->getSelectedItemIndex();
    options.bits = index == 2 ? 16 : index == 1 ? 24 : 32;
    options.writeStems = ! mixOnly->getToggleState();
    options.writeMix = true;
    return true;
}

bool MainComponent::askExportDialog (RenderOptions& options)
{
    juce::AlertWindow box ("Export final audio",
                           "Writes the final mix (mix.wav) with the master gain. No stems are written, and the monitor/headphone chain is never rendered.",
                           juce::AlertWindow::NoIcon);
    box.addComboBox ("bits", { "32-bit float", "24-bit", "16-bit" }, "Bit depth:");
    box.getComboBoxComponent ("bits")->setSelectedItemIndex (options.bits == 16 ? 2 : options.bits == 24 ? 1 : 0, juce::dontSendNotification);
    box.addButton ("Export", 1, juce::KeyPress (juce::KeyPress::returnKey));
    box.addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));
    if (box.runModalLoop() != 1)
        return false;
    const int index = box.getComboBoxComponent ("bits")->getSelectedItemIndex();
    options.bits = index == 2 ? 16 : index == 1 ? 24 : 32;
    options.writeStems = false;
    options.writeMix = true;
    return true;
}

void MainComponent::runRenderJob (const juce::File& folder, RenderOptions choice, const juce::String& title)
{
    if (folder == juce::File())
        return;
    // the message thread stops the transport and closes the tracks' plugin windows before the worker starts:
    // a window is never destroyed on the worker (renderTo's editorsClosed)
    source.playing = false;
    closeTrackEditors();
    if (chooseRenderFolder)
    {
        RenderResult result;
        const auto error = renderTo (folder, {}, true, choice, &result);
        finishRender (folder, result, error);
        return;
    }
    struct Job : juce::ThreadWithProgressWindow
    {
        Job (MainComponent& m, juce::File f, RenderOptions o, const juce::String& t)
            : juce::ThreadWithProgressWindow (t, true, true), owner (m), folder (std::move (f)), options (o) {}
        void run() override
        {
            error = owner.renderTo (folder, [this] (float p) { setProgress ((double) p); return ! threadShouldExit(); }, true, options, &result);
        }
        MainComponent& owner;
        juce::File folder;
        RenderOptions options;
        RenderResult result;
        juce::String error;
    } job (*this, folder, choice, title);
    job.runThread();
    finishRender (folder, job.result, job.error);
}

void MainComponent::launchRenderChooser (const juce::String& chooserTitle, const juce::String& progressTitle, const juce::File& start, RenderOptions choice)
{
    if (dialogOpen) return;
    dialogOpen = true;
    auto finish = [safe = juce::Component::SafePointer<MainComponent> (this), choice, progressTitle] (const juce::File& folder)
    {
        if (safe == nullptr) return;
        struct BusyReset
        {
            juce::Component::SafePointer<MainComponent> s;
            ~BusyReset() { if (s != nullptr) s->dialogOpen = false; }
        } reset { safe };
        safe->runRenderJob (folder, choice, progressTitle);
    };
    if (chooseRenderFolder)
    {
        chooseRenderFolder (chooserTitle, start, std::move (finish));
        return;
    }
    chooser = std::make_unique<juce::FileChooser> (chooserTitle, start);
    chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories,
                          [finishCb = std::move (finish)] (const juce::FileChooser& fc)
                          {
                              finishCb (fc.getResult());
                          });
}

void MainComponent::chooseAndRender()
{
    if (refuseWhileBusy()) return;
    if (session.tracks.empty()) { showError ("There are no tracks to render."); return; }
    if (recorder.isRecording()) { showError ("Stop the recording before you render the stems."); return; }
    RenderOptions choice;
    choice.bits = settings.renderBits;
    choice.writeStems = settings.renderStems;
    bool chosen = true;
    {
        const DialogBusy busy (dialogOpen);
        chosen = chooseRenderOptions ? chooseRenderOptions (choice) : askRenderDialog (choice);
    }
    if (! chosen)
        return;
    settings.renderBits = choice.bits;
    settings.renderStems = choice.writeStems;
    settings.save (opts.settingsFile);
    const auto start = session.projectFile != juce::File() ? session.projectFile.getParentDirectory().getChildFile (session.name + " stems") : juce::File::getSpecialLocation (juce::File::userDocumentsDirectory);
    launchRenderChooser ("Render the stems into this folder", "Rendering the stems", start, choice);
}

void MainComponent::chooseAndExport()
{
    if (refuseWhileBusy()) return;
    if (session.tracks.empty()) { showError ("There are no tracks to export."); return; }
    if (recorder.isRecording()) { showError ("Stop the recording before you export the mix."); return; }
    RenderOptions choice;
    choice.bits = settings.renderBits;
    choice.writeStems = false;
    choice.writeMix = true;
    bool chosen = true;
    {
        const DialogBusy busy (dialogOpen);
        chosen = chooseRenderOptions ? chooseRenderOptions (choice) : askExportDialog (choice);
    }
    if (! chosen)
        return;
    choice.writeStems = false;
    choice.writeMix = true;
    settings.renderBits = choice.bits;
    settings.save (opts.settingsFile);
    const auto start = session.projectFile != juce::File() ? session.projectFile.getParentDirectory() : juce::File::getSpecialLocation (juce::File::userDocumentsDirectory);
    launchRenderChooser ("Export final audio into this folder", "Exporting final audio", start, choice);
}

// After a render: a cancel is quiet (its files are gone); a failure is shown; a success names the folder and the files and reveals it.
void MainComponent::finishRender (const juce::File& folder, const RenderResult& result, const juce::String& error)
{
    if (result.stopped)
        return;
    if (error.isNotEmpty())
    {
        showError (error);
        return;
    }
    const int stems = (int) result.stems.size();
    juce::String what;
    if (stems > 0)
        what = juce::String (stems) + (stems == 1 ? " stem" : " stems");
    if (! result.mixFile.isEmpty())
        what = what.isEmpty() ? juce::String ("mix.wav") : what + " and mix.wav";
    if (notify)
        notify ("Rendered " + what + " into " + folder.getFullPathName());
    if (reveal)
        reveal (folder);
}

// ---- plugins and tracks ------------------------------------------------------------------------------------------------------
void MainComponent::showPluginsMenu()
{
    const auto safe = juce::Component::SafePointer<MainComponent> (this);
    juce::PopupMenu menu;
    menu.addSectionHeader ("Found: " + juce::String (session.catalog.names().size()) + " plugins");
    for (auto& n : session.catalog.names()) menu.addItem (n, false, false, nullptr);
    menu.addSeparator();
    menu.addItem ("Suite plugins only", true, settings.scanSuiteOnly, [safe]
    {
        if (safe == nullptr) return;
        safe->settings.scanSuiteOnly = ! safe->settings.scanSuiteOnly;
        safe->session.catalog.setSuiteOnly (safe->settings.scanSuiteOnly);
        safe->settings.save (safe->opts.settingsFile);
        safe->requestRefresh();
    });
    menu.addItem ("Look in a folder...", [safe] { if (safe != nullptr) safe->chooseFolder(); });
    menu.addItem ("Rescan plugins", [safe] { if (safe != nullptr) { safe->scanPlugins (true); safe->requestRefresh(); } });
    if (! session.catalog.blacklisted().isEmpty())
        menu.addItem ("Forget blacklisted plugins", [safe] { if (safe != nullptr) { safe->session.catalog.clearBlacklist(); safe->scanPlugins (true); safe->requestRefresh(); } });
    if (monitorChainSkipped())
        menu.addItem ("Load saved monitor chain", [safe] { if (safe != nullptr) { safe->loadSkippedMonitorChain(); safe->requestRefresh(); } });
    if (! settings.pluginFolders.isEmpty())
    {
        menu.addSeparator();
        menu.addSectionHeader ("Folders");
        for (auto& f : settings.pluginFolders)
            menu.addItem ("Forget " + juce::File (f).getFileName(), [safe, f]
            {
                if (safe == nullptr) return;
                safe->settings.pluginFolders.removeString (f);
                safe->settings.save (safe->opts.settingsFile);
                safe->scanPlugins();
            });
    }
    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&pluginsButton));
}

void MainComponent::populatePluginMenu (juce::PopupMenu& menu, std::function<void (const juce::String&)> onPick)
{
    const auto safe = juce::Component::SafePointer<MainComponent> (this);
    const auto names = session.catalog.names();
    if (names.isEmpty())
    {
        menu.addItem ("No plugin found: use PLUGINS", false, false, nullptr);
        return;
    }
    menu.addItem ("Search plugins...", [safe, onPick]
    {
        if (safe != nullptr) safe->showPluginSearch (onPick);
    });
    menu.addSeparator();
    if (names.size() <= 25)
    {
        for (const auto& n : names)
            menu.addItem (n, [safe, onPick, n] { if (safe != nullptr && onPick) onPick (n); });
    }
    else
    {
        std::map<juce::String, juce::PopupMenu> makers;
        for (const auto& n : names)
            makers[makerOf (session.catalog, n)].addItem (n, [safe, onPick, n] { if (safe != nullptr && onPick) onPick (n); });
        for (auto& [maker, sub] : makers)
            menu.addSubMenu (maker, sub);
    }
}

void MainComponent::showPluginSearch (std::function<void (const juce::String&)> onPick)
{
    if (dialogOpen) return;
    dialogOpen = true;
    auto* box = new juce::AlertWindow ("Search plugins", "Filter by plugin name or maker:", juce::MessageBoxIconType::NoIcon);
    searchWindow = box;
    box->addTextEditor ("query", {}, {});
    box->addButton ("Find", 1, juce::KeyPress (juce::KeyPress::returnKey));
    box->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));
    const auto safe = juce::Component::SafePointer<MainComponent> (this);
    const auto safeBox = juce::Component::SafePointer<juce::AlertWindow> (box);
    box->enterModalState (true, juce::ModalCallbackFunction::create ([safe, safeBox, onPick] (int result)
    {
        if (safe == nullptr) return;
        safe->searchWindow = nullptr;
        if (result != 1 || safeBox == nullptr) { safe->dialogOpen = false; return; }
        const auto query = safeBox->getTextEditorContents ("query").trim();
        if (query.isEmpty()) { safe->dialogOpen = false; return; }

        juce::StringArray matches;
        for (const auto& n : safe->session.catalog.names())
        {
            const auto maker = makerOf (safe->session.catalog, n);
            if (n.containsIgnoreCase (query) || maker.containsIgnoreCase (query))
                matches.add (n);
        }
        if (matches.isEmpty())
        {
            safe->dialogOpen = false;
            safe->showError ("No plugin matching \"" + query + "\".");
            return;
        }
        for (const auto& n : matches)
        {
            if (n.equalsIgnoreCase (query))
            {
                safe->dialogOpen = false;
                if (onPick) onPick (n);
                return;
            }
        }
        if (matches.size() == 1)
        {
            safe->dialogOpen = false;
            if (onPick) onPick (matches[0]);
            return;
        }
        juce::PopupMenu sub;
        for (const auto& n : matches)
        {
            const auto maker = makerOf (safe->session.catalog, n);
            const auto label = maker != "Other" ? (n + "  [" + maker + "]") : n;
            sub.addItem (label, [safe, onPick, n] { if (safe != nullptr && onPick) onPick (n); });
        }
        sub.showMenuAsync (juce::PopupMenu::Options(), [safe] (int)
        {
            if (safe != nullptr) safe->dialogOpen = false;
        });
    }), true);
}

void MainComponent::showAddPluginMenu (int track)
{
    if (! juce::isPositiveAndBelow (track, (int) session.tracks.size())) return;
    const auto safe = juce::Component::SafePointer<MainComponent> (this);
    juce::PopupMenu menu;
    populatePluginMenu (menu, [safe, track] (const juce::String& n)
    {
        if (safe == nullptr) return;
        if (! juce::isPositiveAndBelow (track, (int) safe->session.tracks.size())) return;
        juce::String error;
        if (! safe->session.addPlugin (track, n, -1, error)) safe->showError (error);
        safe->editor.clearUndo();
        safe->requestRefresh();
    });
    menu.showMenuAsync (juce::PopupMenu::Options());
}

// The FX menu of a track: one submenu per plugin (its first item opens the plugin's window, then bypass, move and remove), and Add plugin.
void MainComponent::showTrackMenu (int track)
{
    if (! juce::isPositiveAndBelow (track, (int) session.tracks.size())) return;
    const auto safe = juce::Component::SafePointer<MainComponent> (this);
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
        sub.addItem ("Open", ! missing, false, [safe, track, s] { if (safe != nullptr) safe->openEditor (track, s); });
        sub.addItem (bypassed ? "Switch on" : "Bypass", [safe, track, s, bypassed]
        {
            if (safe == nullptr) return;
            juce::String error;
            if (! safe->session.setBypass (track, s, ! bypassed, error)) safe->showError (error);
            safe->requestRefresh();
        });
        sub.addItem ("Move earlier", s > 0, false, [safe, track, s]
        {
            if (safe == nullptr) return;
            safe->closeTrackEditors();
            juce::String e;
            safe->session.movePlugin (track, s, s - 1, e);
            safe->requestRefresh();
        });
        sub.addItem ("Move later", s < count - 1, false, [safe, track, s]
        {
            if (safe == nullptr) return;
            safe->closeTrackEditors();
            juce::String e;
            safe->session.movePlugin (track, s, s + 1, e);
            safe->requestRefresh();
        });
        sub.addSeparator();
        sub.addItem ("Remove", [safe, track, s]
        {
            if (safe == nullptr) return;
            safe->closeTrackEditors();
            juce::String error;
            if (safe->session.removePlugin (track, s, error)) safe->editor.clearUndo(); else safe->showError (error);
            safe->requestRefresh();
        });
        menu.addSubMenu (chain[(size_t) s]->plugin + (missing ? "  (not installed)" : bypassed ? "  (bypassed)" : ""), sub);
    }
    menu.addSeparator();
    menu.addItem ("Add plugin...", [safe, track] { if (safe != nullptr) safe->showAddPluginMenu (track); });
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
    // the window that opens now is the first of a track's, so the state the timer compares against has to be the state as it is now: the
    // periodic read only runs every 15 ticks, and an edit made and its window closed inside half a second would never be noticed
    const bool firstTrackEditor = track >= 0 && ! hasOpenTrackEditor();
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
    if (firstTrackEditor)
    {
        const juce::ScopedTryLock sl (session.lock);
        if (sl.isLocked() && ! session.renderRunning())
            lastEditorHash = trackPluginStateHash();   // the same read as the timer's: a busy lock leaves it to the next look
    }
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
    pedal.getParentDirectory().createDirectory();
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
    pedal.getParentDirectory().createDirectory();
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
    // Paint and the timer read this: they never wait for Session::lock (a render holds it for its whole run). The lock is tried, and the
    // last value is kept while it is busy or a render runs (Session::monitorLatency takes it blocking, so it is not called here).
    const juce::ScopedTryLock sl (session.lock);
    if (sl.isLocked() && ! session.renderRunning())
    {
        int sum = 0;
        if (! session.monitorBypass)
            for (auto& s : session.monitor) sum += s->latency();
        monitorLatencyCacheSamples = sum;
    }
    return 1000.0 * (double) monitorLatencyCacheSamples / rate;
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
    bool changed = false;
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
        changed = true;
    }
    for (int i = 0; i < count; ++i)
    {
        auto* b = monitorButtons[i];
        const auto& slot = *session.monitor[(size_t) i];
        if (b->getButtonText() != slot.plugin) { b->setButtonText (slot.plugin); changed = true; }
        const bool isMissing = slot.instance == nullptr;
        if (b->missing != isMissing) { b->missing = isMissing; b->repaint(); changed = true; }
        // lit when it is installed and on; a bypassed or missing plugin is dark (a missing one reads in red)
        const bool lit = slot.instance != nullptr && ! slot.bypass;
        if (b->getToggleState() != lit) { b->setToggleState (lit, juce::dontSendNotification); changed = true; }
        const auto tip = isMissing ? juce::String ("Not installed on this computer: its name is in red. It passes the audio through and keeps its settings.")
                                   : juce::String ("Click: open its window. Shift-click: bypass. Right-click: the menu.");
        if (b->getTooltip() != tip) b->setTooltip (tip);
    }
    const auto correction = juce::String (session.monitorBypass ? "Bypass" : "Correction on");
    if (monitorBypassButton.getButtonText() != correction) { monitorBypassButton.setButtonText (correction); changed = true; }
    if (monitorBypassButton.getToggleState() != ! session.monitorBypass) { monitorBypassButton.setToggleState (! session.monitorBypass, juce::dontSendNotification); changed = true; }
    const double latMs = monitorLatencyMs();
    if (std::abs (latMs - lastMonitorLatencyMs) > 1e-6)
    {
        lastMonitorLatencyMs = latMs;
        changed = true;
    }
    if (changed)
    {
        repaint (monitorStrip);
        repaint (noteStrip);
    }
}

// The menu of a monitor plugin: its window, its bypass, its place, and its removal (the chain is saved after each).
void MainComponent::showMonitorMenu (int index)
{
    juce::String error;
    auto* slot = session.monitorSlotAt (index, error);
    if (slot == nullptr) return;
    const auto safe = juce::Component::SafePointer<MainComponent> (this);
    const bool missing = slot->instance == nullptr, bypassed = slot->bypass;
    const int count = (int) session.monitor.size();
    juce::PopupMenu menu;
    menu.addItem ("Open", ! missing, false, [safe, slot]
    {
        if (safe == nullptr) return;
        for (int i = 0; i < (int) safe->session.monitor.size(); ++i)
            if (safe->session.monitor[(size_t) i].get() == slot) { safe->openEditor (-1, i); return; }
    });
    menu.addItem (bypassed ? "Switch on" : "Bypass", [safe, slot, bypassed]
    {
        if (safe == nullptr) return;
        for (int i = 0; i < (int) safe->session.monitor.size(); ++i)
            if (safe->session.monitor[(size_t) i].get() == slot) { safe->setMonitorPluginBypass (i, ! bypassed); return; }
    });
    menu.addSeparator();
    menu.addItem ("Move left", index > 0, false, [safe, index] { if (safe != nullptr) safe->moveMonitorPlugin (index, index - 1); });
    menu.addItem ("Move right", index < count - 1, false, [safe, index] { if (safe != nullptr) safe->moveMonitorPlugin (index, index + 1); });
    menu.addSeparator();
    menu.addItem ("Remove", [safe, slot]
    {
        if (safe == nullptr) return;
        for (int i = 0; i < (int) safe->session.monitor.size(); ++i)
            if (safe->session.monitor[(size_t) i].get() == slot) { safe->removeMonitorPlugin (i); return; }
    });
    menu.showMenuAsync (juce::PopupMenu::Options());
}

// The plugins to add to the monitor chain: searchable, and over 25 plugins grouped into one sub-menu per maker.
void MainComponent::showAddMonitorMenu()
{
    if (! monitorLoaded)
    {
        showError (monitorProblem.isNotEmpty() ? "The monitor chain could not be read, so it is not changed: " + monitorProblem
                                               : juce::String ("The monitor chain is read after the plugin scan has finished."));
        return;
    }
    const auto safe = juce::Component::SafePointer<MainComponent> (this);
    juce::PopupMenu menu;
    populatePluginMenu (menu, [safe] (const juce::String& n)
    {
        if (safe != nullptr) safe->addMonitorPlugin (n);
    });
    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&monitorAddButton));
}

// ---- autosave and recovery ----------------------------------------------------------------------------------------------------------
juce::File MainComponent::autosaveFileFor (const juce::File& project) const
{
    return project.getSiblingFile (project.getFileName() + ".autosave");   // "mix.olympus.autosave": never a project (saveTo adds .olympus, the chooser filters *.olympus)
}

juce::File MainComponent::untitledAutosaveFile() const
{
    return opts.settingsFile.getSiblingFile ("autosave-untitled.olympus");
}

juce::File MainComponent::autosaveFile() const
{
    return session.projectFile != juce::File() ? autosaveFileFor (session.projectFile) : untitledAutosaveFile();
}

// Writes the project to its autosave file (Session::saveCopy: the session keeps its name, file and dirty flag). Nothing when there are no
// changes, or while a render holds the session.
bool MainComponent::autosaveNow()
{
    if (! session.dirty || session.renderRunning())
        return false;
    juce::String error;
    if (! session.saveCopy (autosaveFile(), error))
    {
        // one message per failure streak, not one every interval
        if (! autosaveFailing) showError ("Autosave failed: " + error);
        autosaveFailing = true;
        return false;
    }
    autosaveFailing = false;
    return true;
}

// Deletes the autosave of the session as it is now (its project's, or the untitled one). Another project's autosave is never touched.
void MainComponent::deleteAutosaves()
{
    autosaveFile().deleteFile();
}

bool MainComponent::offerRecovery()
{
    // a named project's autosave counts when it is newer than the project; an untitled one counts when it exists
    juce::File autosave, project;
    if (session.projectFile != juce::File())
    {
        const auto named = autosaveFileFor (session.projectFile);
        if (named.existsAsFile() && named.getLastModificationTime() > session.projectFile.getLastModificationTime())
        {
            autosave = named;
            project = session.projectFile;
        }
    }
    if (autosave == juce::File() && untitledAutosaveFile().existsAsFile())
        autosave = untitledAutosaveFile();
    if (autosave == juce::File())
        return false;
    bool open = false;
    {
        const DialogBusy busy (dialogOpen);
        open = askRecover && askRecover ("Olympus found unsaved work from the last session. Open it?");
    }
    if (open)
        recoverFrom (autosave, project);
    else
        autosave.deleteFile();   // Ignore: the work is dropped
    return true;
}

// Loads an autosave as the session, with changes, and the project file it belongs to (none for an untitled one), so the next Save writes
// the real file. The settings keep the project as the last one opened.
void MainComponent::recoverFrom (const juce::File& autosave, const juce::File& project)
{
    stopRecording();
    closeTrackEditors();
    source.playing = false;
    const auto pedal = Settings::projectPedalFile (opts.settingsFile);
    pedal.getParentDirectory().createDirectory();
    pedal.replaceWithText ("recovering the last project\n");
    struct PedalGuard { juce::File f; ~PedalGuard() { f.deleteFile(); } } pedalGuard { pedal };
    juce::String error;
    if (! session.load (autosave, error))
    {
        showError (error);
        return;
    }
    lastEditorHash = 0;
    loadedProjectMtime = project != juce::File() ? project.getLastModificationTime() : juce::Time();
    session.projectFile = project;
    session.dirty = true;
    editor.clearUndo();
    clearMasterClip();
    adoptDeviceRate();
    arrange.selected = {};
    seek (0);
    refreshNow();
    arrange.zoomToFit();
    arrange.repaint();
    if (session.warnings.size() > 0)
        showError ("Recovered with notes:\n\n" + session.warnings.joinIntoString ("\n"));
}

bool MainComponent::hasOpenTrackEditor() const
{
    for (const auto& [slot, win] : windows)
    {
        bool isMonitor = false;
        for (const auto& m : session.monitor)
            if (m.get() == slot) { isMonitor = true; break; }
        if (! isMonitor)
            return true;
    }
    return false;
}

juce::int64 MainComponent::trackPluginStateHash() const
{
    juce::uint64 h = 14695981039346656037ULL;
    for (const auto& t : session.tracks)
        for (const auto& s : t->chain)
        {
            const auto mb = session.stateOf (*s);
            const auto* data = static_cast<const juce::uint8*> (mb.getData());
            const size_t n = mb.getSize();
            h ^= (juce::uint64) n;
            h *= 1099511628211ULL;
            for (size_t i = 0; i < n; ++i)
            {
                h ^= data[i];
                h *= 1099511628211ULL;
            }
        }
    return h == 0 ? 1 : (juce::int64) h;
}

bool MainComponent::checkExternalProjectChange()
{
    if (session.projectFile == juce::File() || session.renderRunning() || dialogOpen || recorder.isRecording() || isPlaying()
        || masterGesture != nullptr || masterFader.isMouseButtonDown() || arrange.isInteracting())
        return false;
    if (! session.projectFile.existsAsFile())
        return false;
    const auto mtime = session.projectFile.getLastModificationTime();
    if (loadedProjectMtime == juce::Time())
    {
        loadedProjectMtime = mtime;
        return false;
    }
    if (mtime == loadedProjectMtime)
        return false;
    if (hasOpenTrackEditor())
    {
        const juce::ScopedTryLock sl (session.lock);
        if (! sl.isLocked())
            return false;
        const auto hash = trackPluginStateHash();
        if (lastEditorHash != 0 && hash != lastEditorHash)
            session.dirty = true;
        lastEditorHash = hash;
    }
    if (session.dirty)
    {
        bool reload = false;
        {
            const DialogBusy busy (dialogOpen);
            reload = askReload && askReload ("The project file was changed on disk (for example by the MCP server). Reload it and lose the unsaved changes in this window?");
        }
        if (! reload)
        {
            loadedProjectMtime = mtime;
            return false;
        }
    }
    const auto file = session.projectFile;
    const auto discarded = autosaveFile();
    if (! openProject (file))
        return false;
    loadedProjectMtime = mtime;
    discarded.deleteFile();
    return true;
}

// ---- timer, keys, layout -----------------------------------------------------------------------------------------------------
void MainComponent::timerCallback()
{
    // A render holds the session's lock for its whole run and writes the plugins: while it runs the timer never waits for the lock (the
    // arrangement tries it) and reads no plugin. The latency flag stays set until the render is over.
    const bool rendering = session.renderRunning();
    // a device start that found a render holding the session's lock: tried again here, where waiting for that lock is allowed again
    if (! rendering && pendingDeviceStart.has_value())
        applyDeviceStart (*pendingDeviceStart);
    // The audio copy of the transport is not taken here: every change that moves it calls syncTransport (refreshNow, the loop, the arrangement's
    // edits). A timer that took the session's lock would make the audio callback's try-lock miss a block now and then.
    syncPlayhead();
    arrange.showInputLevels = ! isPlaying() || recorder.isRecording();
    updateRecording();
    updateMaster (juce::Time::getMillisecondCounterHiRes());   // the master's meter, peak read-out and CLIP: the only reader of the master's peaks
    // a plugin reported a new latency (its listener only sets a flag): the tracks' delays follow it, and the read-outs below show it
    if (! rendering && session.takeLatencyChange())
    {
        session.updateLatencies();
        syncTransport(); // the end of the audio includes the longest chain's latency: the callback's copy follows it
        repaint (noteStrip);
    }
    arrange.refresh();
    if (rendering)
        return;
    updateTransport();
    // the autosave: every kAutosaveInterval seconds, the project is written beside its file if it has changes (autosaveNow)
    if (juce::Time::getMillisecondCounterHiRes() - lastAutosaveMs >= kAutosaveInterval * 1000.0)
    {
        lastAutosaveMs = juce::Time::getMillisecondCounterHiRes();
        autosaveNow();
    }
    // A plugin edited in its own window changes the project: notice it by what the plugins would save. Read every 15 ticks while a track
    // plugin window is open, and once more when the last one closes (so an edit made just before the close is not lost). The plugins are read
    // under the lock, which is only tried: processBlock may run beside it, and a busy lock is looked at again on the next tick.
    const bool closing = ! hasOpenTrackEditor();
    if (! (closing ? lastEditorHash == 0 : (++tick % 15 != 0)))
    {
        const juce::ScopedTryLock sl (session.lock);
        if (sl.isLocked())
        {
            const auto hash = trackPluginStateHash();
            if (lastEditorHash != 0 && hash != lastEditorHash) session.dirty = true;
            lastEditorHash = closing ? 0 : hash;
        }
    }
    if (++externalCheckTick >= 15)
    {
        externalCheckTick = 0;
        checkExternalProjectChange();
    }
}

// The keys. The window owns the transport and the clip keys; the arrangement takes only its selection and nudges (arrows, Escape).
// Keys typed into a text editor never get here (or are ignored by the window).
bool MainComponent::keyPressed (const juce::KeyPress& key)
{
    const auto code = key.getKeyCode();
    const auto text = lowerCase (key.getTextCharacter());
    const auto mods = key.getModifiers();
    const bool cmd = mods.isCommandDown(), shift = mods.isShiftDown();
    const auto is = [text, code] (juce::juce_wchar c) { return text == c || lowerCase ((juce::juce_wchar) code) == c; };
    // a text editor (the tempo field, track rename, knob entry) keeps its own keys
    const bool typing = bpmField.hasKeyboardFocus (true)
                        || dynamic_cast<juce::TextEditor*> (juce::Component::getCurrentlyFocusedComponent()) != nullptr;

    if (cmd)
    {
        if (is ('z')) { if (shift) redoEdit(); else undoEdit(); return true; }
        if (is ('s')) { if (shift) saveProjectAs(); else saveProject(); return true; }
        if (is ('o')) { openButton.triggerClick(); return true; }
        if (is ('n')) { newButton.triggerClick(); return true; }
        if (typing) return false;
        if (is ('d')) { arrange.duplicateSelected(); return true; }
        if (is ('c')) { arrange.copySelected(); return true; }
        if (is ('x')) { arrange.cutSelected(); return true; }
        if (is ('v')) { syncPlayhead(); arrange.pasteAtPlayhead(); return true; }
        return false;
    }
    if (typing)
        return false;
    if (code == juce::KeyPress::spaceKey) { togglePlay(); updateTransport(); return true; }
    if (code == juce::KeyPress::returnKey) { rewind(); updateTransport(); return true; }
    if (code == juce::KeyPress::deleteKey || code == juce::KeyPress::backspaceKey) { arrange.deleteSelected(); return true; }
    if (code == juce::KeyPress::homeKey) { seek (0); syncPlayhead(); updateTransport(); return true; }
    if (code == juce::KeyPress::endKey) { seek (session.length()); syncPlayhead(); updateTransport(); return true; }
    if (is (',') || is ('.')) { jumpToMarker (is ('.')); syncPlayhead(); updateTransport(); return true; }
    if (is ('r')) { toggleRecord(); return true; }
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
    renderButton.setBounds (x, rowA, 104, h);
    x += 110;
    exportButton.setBounds (x, rowA, 76, h);
    x += 82;
    audioButton.setBounds (x, rowA, 76, h);
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
    positionBox = { w - kMargin - 184, kRowC, 184, hc };
    deviceLabel.setBounds (290, kRowC, juce::jmax (120, positionBox.getX() - 154 - 290), hc);
    // the master section, in row C left of the device line (x 16 to 282): MASTER (painted), the fader, the stereo meter's two bars, the peak
    // read-out and the CLIP lamp
    masterFader.setBounds (68, kRowC + 6, 80, 16);
    masterBarLeft = { 152.0f, (float) kRowC + 4.0f, 36.0f, 9.0f };
    masterBarRight = { 152.0f, (float) kRowC + 15.0f, 36.0f, 9.0f };
    masterPeakLabel.setBounds (192, kRowC, 46, hc);
    masterClipLamp.setBounds (240, kRowC, 42, hc);

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
    const auto clip = g.getClipBounds();
    if (clip.intersects (juce::Rectangle<int> (0, 0, getWidth(), kHeaderHeight)))
        ui::hw::drawHeaderPlate (g, juce::Rectangle<float> (0.0f, 0.0f, (float) getWidth(), (float) kHeaderHeight - 6.0f));
    if (clip.intersects (juce::Rectangle<int> (kMargin, 8, 250, 66)))
        ui::hw::drawNameplate (g, { kMargin, 8, 250, 66 }, "Olympus", "Basic DAW", "Pantheon plugins", ! session.tracks.empty(), 24.0f);

    // the position read-out, on glass
    if (clip.intersects (positionBox))
    {
        ui::hw::drawGlass (g, positionBox.toFloat(), 3.0f, colours::amber);
        g.setColour (colours::amber);
        g.setFont (ui::lcdFont (14.0f));
        g.drawText (positionText (session, position()), positionBox.reduced (8, 0), juce::Justification::centredRight);
        ui::hw::drawGlassOverlay (g, positionBox.toFloat(), 3.0f, false);
    }

    // beside the position: the time of the take in progress in red, else the track count and the sample rate
    const juce::Rectangle<int> readoutArea { positionBox.getX() - 148, kRowC, 140, 28 };
    if (clip.intersects (readoutArea))
    {
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
        ui::hw::drawLabel (g, readout, readoutArea, juce::Justification::centredRight, colour, 10.5f);
    }
    // the MONITOR strip: its label, a hint while the chain is empty, and the chain's latency beside the CORRECTION switch
    if (clip.intersects (monitorStrip))
    {
        ui::hw::drawLabel (g, "MONITOR", { monitorStrip.getX(), monitorStrip.getY(), 90, monitorStrip.getHeight() }, juce::Justification::centredLeft, colours::dim, 10.5f);
        if (monitorButtons.isEmpty())
            ui::hw::drawLabel (g, "no plugin: the headphones hear the mix as it is", { monitorStrip.getX() + 96, monitorStrip.getY(), 400, monitorStrip.getHeight() },
                               juce::Justification::centredLeft, colours::dim, 10.0f);
        ui::hw::drawLabel (g, "latency " + juce::String (monitorLatencyMs(), 1) + " ms", monitorLatencyArea (monitorStrip), juce::Justification::centredRight,
                           colours::dim, 10.0f);
    }
    // the master section: its label, and the stereo meter's bars (the track meter's ballistics and colours, on the kit's dark slot)
    const juce::Rectangle<int> masterLabelArea { kMargin, kRowC, 50, 28 };
    if (clip.intersects (masterLabelArea))
        ui::hw::drawLabel (g, "MASTER", masterLabelArea, juce::Justification::centredLeft, colours::dim, 10.5f);
    if (clip.intersects (masterBarLeft.getUnion (masterBarRight).expanded (2.0f, 2.0f).toNearestInt()))
    {
        for (const auto* bar : { &masterBarLeft, &masterBarRight })
        {
            g.setColour (juce::Colours::black.withAlpha (0.85f));
            g.fillRoundedRectangle (bar->expanded (0.0f, 1.0f), 2.0f);
        }
        paintLevelBar (g, masterBarLeft, masterLeft);
        paintLevelBar (g, masterBarRight, masterRight);
    }
    if (clip.intersects (noteStrip))
        ui::drawNoteStrip (g, noteStrip, notes());
}
}
