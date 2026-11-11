// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 The Pantheon authors

#include "PluginEditor.h"

#include <themis/EqFit.h>

ThemisEditor::ThemisEditor (ThemisProcessor& p)
    : AudioProcessorEditor (&p),
      proc (p),
      view (p),
      scopeView (p),
      levelView (p),
      fundamentalView (p),
      advicePanel (p),
      auditionPanel (p),
      targetBox (p.apvts, params::target),
      deviceBox (p.apvts, params::device),
      loudnessBox (p.apvts, params::loudnessTarget),
      avgKnob (p.apvts, params::avgTime, "Averaging", "Averaging window for the long-term curve. Infinite measures the whole track since Reset."),
      toleranceKnob (p.apvts, params::tolerance, "Tolerance", "How wide the target band is, as a multiple of the curve's own spread. Narrower is stricter; double-click for 1.00x.")
{
    setLookAndFeel (&laf);

    addAndMakeVisible (view);
    addAndMakeVisible (scopeView);
    addAndMakeVisible (levelView);
    addAndMakeVisible (fundamentalView);
    addAndMakeVisible (advicePanel);
    addAndMakeVisible (auditionPanel);
    for (auto* c : std::initializer_list<juce::Component*> { &targetBox, &avgKnob, &deviceBox, &deviceLabel, &loudnessBox, &loudnessLabel, &learnButton, &saveButton, &loadButton,
                                                              &copyEqButton, &resetButton, &statusLabel, &toleranceKnob, &guideButton })
        addAndMakeVisible (c);

    deviceBox.setTooltip ("Preview how the balance would translate to a typical playback device (approximate response; "
                          "analysis only, the audio is never changed). \"Off\" hides the preview.");
    loudnessBox.setTooltip ("The loudness this master is for. The master plan judges the integrated loudness against it: "
                            "streaming services turn anything louder than " + juce::String (static_cast<int> (-themis::master::kStreamingLufs))
                            + " LUFS down, so extra loudness there only costs dynamics.");
    for (auto* label : { &deviceLabel, &loudnessLabel })
    {
        label->setColour (juce::Label::textColourId, colours::dim);
        label->setFont (ui::labelFont (9.5f));
        label->setJustificationType (juce::Justification::centredRight);
    }
    deviceLabel.setText ("TRANSLATE TO", juce::dontSendNotification);
    loudnessLabel.setText ("DELIVER FOR", juce::dontSendNotification);
    resetButton.setTooltip ("Clear every measurement (curve, loudness, peaks, graphs) and start again. Press it, then play the whole track.");

    statusLabel.setJustificationType (juce::Justification::centredLeft);
    statusLabel.setFont (ui::lcdFont (11.5f));
    statusLabel.setMinimumHorizontalScale (0.8f);

    learnButton.onClick = [this] { chooseReferenceTracks(); };
    saveButton.onClick = [this] { saveCurve(); };
    copyEqButton.setTooltip ("Copy the equaliser moves that bring the measured curve towards the target (a low shelf, up to three bells and a high "
                             "shelf, from the long-term average), and the target curve itself, to paste in Iris (PASTE on its band panel; the curve "
                             "becomes its reference, drawn behind the spectrum). A difference from the target is a "
                             "matter of taste, so treat it as a starting point and judge it by ear.");
    copyEqButton.onClick = [this] { copyTonalEq(); };
    advicePanel.onCopied = [this] (const juce::String& text) { setStatus (text, false); };
    loadButton.onClick = [this] { loadCurve(); };
    resetButton.onClick = [this] {
        proc.resetAnalyzer();
        advicePanel.refresh();
    };

    showIdleStatus();

    setResizable (true, true);
    setResizeLimits (1120, 780, 2200, 1300);
    setSize (1440, 900);
    advicePanel.refresh();
    startTimerHz (30);
}

ThemisEditor::~ThemisEditor()
{
    stopTimer();
    // The preview and reference gains are kept up to date by this editor's timer; without it they would go stale, so they end with it.
    proc.setStreamingPreview (false);
    proc.setReferenceOn (false);
    setLookAndFeel (nullptr);
}

void ThemisEditor::timerCallback()
{
    view.update();
    scopeView.update();
    levelView.update();
    fundamentalView.update();
    auditionPanel.update();
    advicePanel.auditionChanged();
    if (const bool signal = proc.stereoScope().hasData(); signal != signalPresent)
    {
        signalPresent = signal;
        repaint (nameplate);
    }
    if (++planTicks >= kPlanRefreshTicks)
    {
        planTicks = 0;
        proc.updateListen(); // the preview and reference gains follow the loudness measured so far
        advicePanel.refresh();
    }
    if (statusTicks > 0)
        --statusTicks;
    if (statusTicks == 0)
        showIdleStatus(); // nothing is being said: say how the plugin is being used, or the tip
}

void ThemisEditor::copyTonalEq()
{
    themis::BandArray measured {};
    if (!proc.readSpectrum (measured))
    {
        setStatus ("Nothing measured yet: play the track first.", true);
        return;
    }
    const auto moves = themis::tonalMoves (measured, proc.activeTarget(), proc.getSampleRate() > 0.0 ? proc.getSampleRate() : 48000.0);
    // The target curve goes along with the bands: Iris draws it behind its spectrum as the reference to match.
    const auto target = proc.activeTarget();
    juce::SystemClipboard::copyTextToClipboard (ui::linkText (moves, &target));
    if (moves.empty())
    {
        setStatus ("The curve is within 0.5 dB of the target: no bands to copy, but the target curve is on the clipboard for Iris' reference.", false);
        return;
    }
    setStatus ("Copied " + juce::String (static_cast<int> (moves.size())) + " band(s) and the target curve: paste them in " + ui::linkDestination (moves) + " (PASTE on its band panel).", false);
}

usage::Input ThemisEditor::usageInput() const
{
    usage::Input in;
    in.signal = proc.stereoScope().hasData();
    in.auditionActive = proc.auditionSwitches().active();
    in.comparing = proc.streamingPreview() || proc.referenceOn();
    in.analysedSeconds = proc.loudnessMonitor().seconds();
    return in;
}

// The status window between messages: the first note about how the plugin is being used (a warning in amber), else the tip.
void ThemisEditor::showIdleStatus()
{
    const auto notes = usage::notesFor (usageInput());
    const juce::String text = notes.empty() ? juce::String (usage::idleTip()) : juce::String (notes.front().text);
    const auto colour = ! notes.empty() && notes.front().severity == ui::Severity::warning ? colours::warn : colours::amber.withAlpha (0.8f);
    if (statusLabel.getText() != text || statusLabel.findColour (juce::Label::textColourId) != colour)
    {
        statusLabel.setColour (juce::Label::textColourId, colour);
        statusLabel.setText (text, juce::dontSendNotification);
    }
}

void ThemisEditor::setStatus (const juce::String& text, bool isError)
{
    statusLabel.setColour (juce::Label::textColourId, isError ? colours::warn : colours::amber.withAlpha (0.8f));
    statusLabel.setText (text, juce::dontSendNotification);
    statusTicks = 30 * (isError ? 12 : 20);
}

// ---- actions ------------------------------------------------------------------------------------
// Shows a file chooser and calls `onChosen` with it if the editor still exists when the user is done.
void ThemisEditor::launchChooser (const juce::String& title, const juce::File& start, const juce::String& patterns, int chooserFlags,
                                  std::function<void (ThemisEditor&, const juce::FileChooser&)> onChosen)
{
    chooser = std::make_unique<juce::FileChooser> (title, start, patterns);
    chooser->launchAsync (chooserFlags, [safe = juce::Component::SafePointer<ThemisEditor> (this), handler = std::move (onChosen)] (const juce::FileChooser& fc) {
        if (safe != nullptr)
            handler (*safe, fc);
    });
}

void ThemisEditor::chooseReferenceTracks()
{
    launchChooser ("Choose one or more reference tracks (final masters work best)",
                   juce::File::getSpecialLocation (juce::File::userMusicDirectory), "*.wav;*.aif;*.aiff;*.flac;*.mp3;*.m4a;*.ogg",
                   juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles | juce::FileBrowserComponent::canSelectMultipleItems,
                   [] (ThemisEditor& self, const juce::FileChooser& fc) {
                       const auto results = fc.getResults();
                       if (results.isEmpty())
                           return;
                       self.setStatus ("Analysing " + juce::String (results.size()) + " track(s)...", false);
                       self.proc.learnFromFiles (results, [safe = juce::Component::SafePointer<ThemisEditor> (&self)] (const juce::String& error) {
                           if (safe == nullptr)
                               return;
                           if (error.isEmpty())
                               safe->setStatus ("Learned a new target curve and selected it (\"Custom\").", false);
                           else
                               safe->setStatus (error, true);
                       });
                   });
}

void ThemisEditor::saveCurve()
{
    launchChooser ("Save the active target curve",
                   juce::File::getSpecialLocation (juce::File::userDocumentsDirectory)
                       .getChildFile (juce::File::createLegalFileName (juce::String (proc.activeTarget().name)) + kCurveExtension),
                   "*" + juce::String (kCurveExtension),
                   juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles | juce::FileBrowserComponent::warnAboutOverwriting,
                   [] (ThemisEditor& self, const juce::FileChooser& fc) {
                       auto file = fc.getResult();
                       if (file == juce::File())
                           return;
                       if (!file.hasFileExtension (kCurveExtension)) // append: withFileExtension would cut "Mix 1.5" down to "Mix 1"
                           file = file.getSiblingFile (file.getFileName() + kCurveExtension);
                       if (file.replaceWithText (juce::String (self.proc.activeTarget().toJson())))
                           self.setStatus ("Saved " + file.getFileName(), false);
                       else
                           self.setStatus ("Could not write " + file.getFullPathName(), true);
                   });
}

void ThemisEditor::loadCurve()
{
    launchChooser ("Load a target curve", juce::File::getSpecialLocation (juce::File::userDocumentsDirectory), "*" + juce::String (kCurveExtension),
                   juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                   [] (ThemisEditor& self, const juce::FileChooser& fc) {
                       const auto file = fc.getResult();
                       if (file == juce::File())
                           return;
                       themis::TargetCurve curve;
                       if (themis::TargetCurve::fromJson (file.loadFileAsString().toStdString(), curve))
                       {
                           self.proc.setCustomCurve (curve, true);
                           self.setStatus ("Loaded \"" + juce::String (curve.name) + "\".", false);
                       }
                       else
                           self.setStatus (file.getFileName() + " is not a valid Themis curve.", true);
                   });
}

// ---- layout -------------------------------------------------------------------------------------
void ThemisEditor::paint (juce::Graphics& g)
{
    ui::hw::fillMetal (g, getLocalBounds().toFloat(), colours::background);
    ui::hw::drawHeaderPlate (g, juce::Rectangle<float> (0.0f, 0.0f, static_cast<float> (getWidth()), static_cast<float> (kHeaderHeight + 8)));
    ui::hw::drawNameplate (g, nameplate, ui::kProjectWord, ui::kPluginWord, "Mastering check suite", signalPresent);

    ui::hw::drawGlass (g, statusWindow.toFloat(), 3.0f, colours::amber);
    ui::hw::drawGlassOverlay (g, statusWindow.toFloat(), 3.0f, false);
}

void ThemisEditor::resized()
{
    auto r = getLocalBounds().reduced (10, 8);

    auto head = r.removeFromTop (kHeaderHeight - 8);
    nameplate = head.removeFromLeft (kTitleWidth - 14).reduced (0, 2);
    head.removeFromLeft (14);
    auto row1 = head.removeFromTop (30);
    targetBox.setBounds (row1.removeFromLeft (200).reduced (0, 2));
    row1.removeFromLeft (8);
    learnButton.setBounds (row1.removeFromLeft (150).reduced (0, 3));
    row1.removeFromLeft (4);
    saveButton.setBounds (row1.removeFromLeft (84).reduced (0, 3));
    row1.removeFromLeft (4);
    loadButton.setBounds (row1.removeFromLeft (84).reduced (0, 3));
    row1.removeFromLeft (4);
    copyEqButton.setBounds (row1.removeFromLeft (76).reduced (0, 3));
    row1.removeFromLeft (14);
    deviceLabel.setBounds (row1.removeFromLeft (84));
    row1.removeFromLeft (6);
    deviceBox.setBounds (row1.removeFromLeft (160).reduced (0, 2));

    head.removeFromTop (4);
    auto row2 = head;
    avgKnob.setBounds (row2.removeFromLeft (158));
    row2.removeFromLeft (8);
    toleranceKnob.setBounds (row2.removeFromLeft (158));
    row2.removeFromLeft (16);
    resetButton.setBounds (row2.removeFromLeft (70).withSizeKeepingCentre (70, 26));
    row2.removeFromLeft (18);
    loudnessLabel.setBounds (row2.removeFromLeft (84).withSizeKeepingCentre (84, 20));
    row2.removeFromLeft (6);
    loudnessBox.setBounds (row2.removeFromLeft (170).withSizeKeepingCentre (170, 26));
    row2.removeFromLeft (14);
    statusWindow = row2.withSizeKeepingCentre (row2.getWidth(), 26);
    guideButton.setBounds (statusWindow.withTrimmedLeft (statusWindow.getWidth() - 62).reduced (3));
    statusLabel.setBounds (statusWindow.withTrimmedRight (62).reduced (8, 0));

    auto history = r.removeFromBottom (kHistoryHeight);
    r.removeFromBottom (8);
    levelView.setBounds (history.removeFromLeft (history.getWidth() * 3 / 5));
    history.removeFromLeft (8);
    fundamentalView.setBounds (history);

    r.removeFromTop (6);
    auto side = r.removeFromRight (kSideWidth);
    r.removeFromRight (8);
    view.setBounds (r);

    auto lower = side.removeFromBottom (kScopeHeight);
    side.removeFromBottom (8);
    advicePanel.setBounds (side);
    auditionPanel.setBounds (lower.removeFromRight (kAuditionWidth));
    lower.removeFromRight (8);
    scopeView.setBounds (lower);
}
