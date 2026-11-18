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
      avgKnob (p.apvts, params::avgTime, "Averaging", "How much of the track the curve averages. Infinite (the default) is the whole track since Reset; "
                                                      "10 s, 30 s or 2 min follow only the last part you played."),
      toleranceKnob (p.apvts, params::tolerance, "Tolerance", "How wide the green band around the target curve is. 1.00x is the normal width; lower is stricter "
                                                              "(more of the curve turns amber or red), higher is looser. Double-click for 1.00x."),
      ceilingKnob (p.apvts, params::truePeakCeiling, "True-peak ceiling",
                   "The highest peak the master may reach, measured between the samples as an encode would see it. Above it Themis warns (CLIPPING, "
                   "the PEAKS graph and the master plan, whose COPY for Aegis uses this value). -1 dBTP is the usual streaming limit; double-click for it. "
                   "Over 0 dBTP is always bad."),
      resetOnPlayButton (p.apvts, params::resetOnPlay, "Auto reset",
                         "On by default: each time the host starts playing from the beginning of its timeline, the measurement restarts (as Reset does), "
                         "so a fresh pass of the track is measured on its own. Turn it off to let the measurement run on across stops and starts until you "
                         "press Reset. Playing on from a stop part-way never resets.")
{
    setLookAndFeel (&laf);

    addAndMakeVisible (view);
    addAndMakeVisible (scopeView);
    addAndMakeVisible (levelView);
    addAndMakeVisible (fundamentalView);
    addAndMakeVisible (advicePanel);
    addAndMakeVisible (auditionPanel);
    for (auto* c : std::initializer_list<juce::Component*> { &targetBox, &avgKnob, &deviceBox, &deviceLabel, &loudnessBox, &loudnessLabel, &learnButton, &closestButton, &saveButton, &loadButton,
                                                              &copyEqButton, &holdButton, &resetButton, &statusLabel, &toleranceKnob, &ceilingKnob, &resetOnPlayButton, &guideButton })
        addAndMakeVisible (c);

    deviceBox.setTooltip ("Preview how the balance would translate to a typical playback device (approximate response; "
                          "analysis only, the audio is never changed). \"Off\" hides the preview.");
    loudnessBox.setTooltip ("The loudness this master is for. The master plan judges the integrated loudness against it: "
                            "streaming services turn anything louder than " + juce::String (static_cast<int> (-themis::delivery::kStreamingLufs))
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
    closestButton.setTooltip ("Pick the built-in target curve whose shape is nearest the measured curve (the RMS difference over the bands your sample rate "
                              "carries, after removing the level offset). Needs " + juce::String (static_cast<int> (themis::master::kMinSeconds)) + " s of audio. "
                              "A shape match, not a genre detection: it only tells you which starting point is closest.");
    closestButton.onClick = [this] { chooseClosestTarget(); };
    saveButton.onClick = [this] { saveCurve(); };
    copyEqButton.setTooltip ("Copy EQ is for Iris (or Apollo): it copies the equaliser bands that bring the measured curve towards the target (a low shelf, up to "
                             "three bells and a high shelf, from the long-term average) and the target curve itself. In Iris, PASTE on the band panel or in its "
                             "header; the curve becomes its reference, drawn behind the spectrum. A difference from the target is taste, so treat it as a "
                             "starting point and judge it by ear.");
    copyEqButton.onClick = [this] { copyTonalEq(); };
    holdButton.setClickingTogglesState (true);
    holdButton.setToggleState (proc.hasHeldCurve(), juce::dontSendNotification);
    holdButton.setTooltip ("Keep the measured tonal curve as it is now and draw it behind the live one as a pale ghost, to judge a change against what it was: "
                           "change the master, press Reset, play the same part again and compare the two. Click again to let it go. Not saved with the session.");
    holdButton.onClick = [this] { toggleHeldCurve(); };
    shownCeilingDb = proc.paramReader.truePeakCeilingDb();
    advicePanel.onCopied = [this] (const juce::String& text) { setStatus (text, false); };
    loadButton.onClick = [this] { loadCurve(); };
    resetButton.onClick = [this] {
        proc.resetAnalyzer();
        advicePanel.refresh();
        repaint (verdictPlate);
    };

    showIdleStatus();

    grips = std::make_unique<ui::ResizeGrips> (*this, kMinWidth, kMinWidth, kMaxWidth); // drag any corner: the window keeps 16:10 between its smallest and largest size
    addAndMakeVisible (*grips);
    setSize (kMinWidth, kMinHeight); // opens at the smallest size it supports; the corners scale it up (the limits above)
    advicePanel.refresh();
    startTimerHz (30);
}

ThemisEditor::~ThemisEditor()
{
    stopTimer();
    // The preview and reference gains are kept up to date by this editor's timer; without it they would go stale, so they end with it.
    // The audition ends too: its panel is the only place that shows it, and an engaged solo with no window would be heard with no way to see why.
    proc.setStreamingPreview (false);
    proc.setReferenceOn (false);
    proc.setAudition (themis::Switches {});
    setLookAndFeel (nullptr);
}

void ThemisEditor::timerCallback()
{
    if (const float ceiling = proc.paramReader.truePeakCeilingDb(); ceiling != shownCeilingDb) // the verdicts of the graphs and the plan depend on it
    {
        shownCeilingDb = ceiling;
        levelView.refresh();
        advicePanel.refresh();
    }
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
        repaint (verdictPlate); // the plate reads the same refresh as the plan
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
    // The target curve goes along with the bands: Iris and Apollo draw it behind their spectrum as the reference to follow.
    const auto target = proc.activeTarget();
    juce::SystemClipboard::copyTextToClipboard (gui::linkText (moves, &target));
    if (moves.empty())
    {
        setStatus ("The curve is within 0.5 dB of the target: no bands to copy, but the target curve is on the clipboard for the reference of Iris or Apollo.", false);
        return;
    }
    setStatus ("Copied " + juce::String (static_cast<int> (moves.size())) + " band(s) and the target curve: paste them in " + gui::linkDestination (moves) + " (PASTE on its band panel).", false);
}

// The built-in target whose shape is nearest the measured curve. It selects it the way the Target selector does (the host
// is notified), and says how far off it is. Before enough audio, or with no signal, it changes nothing.
void ThemisEditor::chooseClosestTarget()
{
    themis::BandArray measured {};
    if (proc.loudnessMonitor().seconds() < themis::master::kMinSeconds || !proc.readSpectrum (measured))
    {
        setStatus ("Play at least " + juce::String (static_cast<int> (themis::master::kMinSeconds)) + " s first", true);
        return;
    }
    const double sampleRate = proc.getSampleRate() > 0.0 ? proc.getSampleRate() : 48000.0;
    float distance = 0.0f;
    const size_t index = themis::TargetCurve::nearestBuiltIn (measured, sampleRate, &distance);
    proc.selectTarget (static_cast<int> (index));
    setStatus ("Closest built-in: " + juce::String (themis::TargetCurve::builtIns()[index].name) + " (" + juce::String (distance, 1)
                   + " dB RMS). A shape match, not a genre detection.",
               false);
}

void ThemisEditor::toggleHeldCurve()
{
    if (!holdButton.getToggleState())
    {
        proc.releaseHeldCurve();
        view.update();
        setStatus ("Released the held curve.", false);
        return;
    }
    if (!proc.holdCurve())
    {
        holdButton.setToggleState (false, juce::dontSendNotification);
        setStatus ("Nothing measured yet: play the track first, then hold the curve.", true);
        return;
    }
    view.update();
    setStatus ("Holding the curve as it is now (the pale ghost). Change the master, press Reset, play the same part again and compare.", false);
}

// The status window between messages: the first note about how the plugin is being used (a warning in amber), else the tip.
void ThemisEditor::showIdleStatus()
{
    const auto notes = usage::notesFor (proc.usageState());
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
    ui::hw::drawNameplate (g, nameplate, gui::kProjectWord, gui::kPluginWord, "Mastering check suite", signalPresent);
    paintVerdictPlate (g);
    paintHeaderGroups (g);

    ui::hw::drawGlass (g, statusWindow.toFloat(), 3.0f, colours::amber);
    ui::hw::drawGlassOverlay (g, statusWindow.toFloat(), 3.0f, false);
}

// The three groups' captions and the engraved rules between them.
void ThemisEditor::paintHeaderGroups (juce::Graphics& g) const
{
    ui::hw::drawLabel (g, gui::kTargetCaption, captionTarget, juce::Justification::left, colours::text, 9.5f);
    ui::hw::drawLabel (g, gui::kJudgeCaption, captionJudge, juce::Justification::left, colours::text, 9.5f);
    ui::hw::drawLabel (g, gui::kSessionCaption, captionSession, juce::Justification::left, colours::text, 9.5f);
    gui::drawEngravedRule (g, ruleTarget);
    gui::drawEngravedRule (g, ruleJudge);
    gui::drawEngravedRule (g, ruleRows);
}

// The verdict in one word, read from the plan's own refresh: the word and the lamp say it, the colour only repeats it.
void ThemisEditor::paintVerdictPlate (juce::Graphics& g) const
{
    const auto state = advicePanel.verdictState();
    const auto colour = colours::forVerdict (state.verdict);
    const auto plate = verdictPlate.toFloat();
    ui::hw::drawGlass (g, plate, 3.0f, colours::amber);
    ui::hw::drawGlassOverlay (g, plate, 3.0f, false);

    const auto text = plate.reduced (8.0f, 0.0f);
    ui::hw::drawLamp (g, { text.getX() + 4.0f, text.getCentreY() }, 4.5f, colour, colour != colours::dim);
    auto area = text.withTrimmedLeft (16.0f);
    g.setColour (colour);
    g.setFont (ui::lcdFont (15.0f, juce::Font::bold));
    g.drawText (juce::String (themis::plan::verdictWord (state.verdict)), area.removeFromTop (17.0f).toNearestInt(), juce::Justification::centredLeft);
    g.setColour (colours::amber);
    g.setFont (ui::lcdFont (10.0f));
    g.drawText (advicePanel.plateDetail(), area.toNearestInt(), juce::Justification::centredLeft);
}

void ThemisEditor::resized()
{
    if (grips != nullptr)
        grips->setBounds (getLocalBounds());
    auto r = getLocalBounds().reduced (10, 8);

    auto head = r.removeFromTop (kHeaderHeight - 8);
    auto column = head.removeFromLeft (kTitleWidth - 14);
    verdictPlate = column.removeFromBottom (kVerdictHeight);
    column.removeFromBottom (2);
    nameplate = column;
    head.removeFromLeft (14);

    // Row A: TARGET, then the status window (the GUIDE line) with the rest of the row. Row B: JUDGE, then SESSION.
    auto rowA = head.removeFromTop (kCaptionHeight + 2 + kRowHeight);
    auto captionRowA = rowA.removeFromTop (kCaptionHeight + 2);
    const auto ruleY = static_cast<float> (head.getY() + kRowGap / 2); // the engraved rule between the two rows
    head.removeFromTop (kRowGap);
    auto rowB = head.removeFromTop (kCaptionHeight + 2 + kKnobRowHeight);
    auto captionRowB = rowB.removeFromTop (kCaptionHeight + 2);
    ruleRows = { static_cast<float> (head.getX()), ruleY, static_cast<float> (head.getRight()), ruleY };

    auto target = rowA.removeFromLeft (kTargetWidth);
    captionTarget = captionRowA.removeFromLeft (kTargetWidth);
    targetBox.setBounds (target.removeFromLeft (180).reduced (0, 2));
    target.removeFromLeft (8);
    learnButton.setBounds (target.removeFromLeft (140).reduced (0, 2));
    target.removeFromLeft (4);
    closestButton.setBounds (target.removeFromLeft (74).reduced (0, 2));
    target.removeFromLeft (4);
    saveButton.setBounds (target.removeFromLeft (84).reduced (0, 2));
    target.removeFromLeft (4);
    loadButton.setBounds (target.removeFromLeft (84).reduced (0, 2));
    target.removeFromLeft (4);
    copyEqButton.setBounds (target.removeFromLeft (70).reduced (0, 2));
    target.removeFromLeft (4);
    holdButton.setBounds (target.removeFromLeft (88).reduced (0, 2));
    rowA.removeFromLeft (kGroupGap);
    ruleTarget = { static_cast<float> (rowA.getX() - kGroupGap / 2), static_cast<float> (captionRowA.getY()), static_cast<float> (rowA.getX() - kGroupGap / 2), static_cast<float> (rowA.getBottom()) };
    statusWindow = rowA;
    guideButton.setBounds (statusWindow.withTrimmedLeft (statusWindow.getWidth() - 62).reduced (3));
    statusLabel.setBounds (statusWindow.withTrimmedRight (62).reduced (8, 0));

    auto judge = rowB.removeFromLeft (kJudgeWidth);
    captionJudge = captionRowB.removeFromLeft (kJudgeWidth);
    avgKnob.setBounds (judge.removeFromLeft (kKnobWidth));
    judge.removeFromLeft (8);
    toleranceKnob.setBounds (judge.removeFromLeft (kKnobWidth));
    judge.removeFromLeft (8);
    ceilingKnob.setBounds (judge.removeFromLeft (kKnobWidth));
    judge.removeFromLeft (14);
    deviceLabel.setBounds (judge.removeFromLeft (84).withSizeKeepingCentre (84, kRowHeight));
    judge.removeFromLeft (6);
    deviceBox.setBounds (judge.removeFromLeft (150).withSizeKeepingCentre (150, kRowHeight).reduced (0, 0));
    judge.removeFromLeft (14);
    loudnessLabel.setBounds (judge.removeFromLeft (84).withSizeKeepingCentre (84, kRowHeight));
    judge.removeFromLeft (6);
    loudnessBox.setBounds (judge.removeFromLeft (160).withSizeKeepingCentre (160, kRowHeight));
    rowB.removeFromLeft (kGroupGap);
    ruleJudge = { static_cast<float> (rowB.getX() - kGroupGap / 2), static_cast<float> (captionRowB.getY()), static_cast<float> (rowB.getX() - kGroupGap / 2), static_cast<float> (rowB.getBottom()) };
    captionSession = captionRowB.removeFromLeft (kSessionWidth);
    auto session = rowB.removeFromLeft (kSessionWidth);
    resetButton.setBounds (session.removeFromLeft (70).withSizeKeepingCentre (70, kRowHeight - 2));
    session.removeFromLeft (6);
    resetOnPlayButton.setBounds (session.removeFromLeft (96).withSizeKeepingCentre (96, kRowHeight));

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
