// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 The Pantheon authors

#include "PluginEditor.h"

using namespace harmonia;

namespace
{
struct Preset
{
    const char* name;
    float speed, strength, natural;
};

// "Natural" is transparent; "Pop" is the audible-but-musical studio sound; "Hard" is the robotic effect.
const Preset kPresets[3] = {
    { "Natural",   55.0f, 100.0f, 85.0f },
    { "Pop",       18.0f, 100.0f, 50.0f },
    { "Hard-tune",  0.0f, 100.0f,  0.0f },
};

const juce::Identifier kScaleProperty { "uiScale" };
constexpr int kKnobX = 190, kKnobW = 111, kKnobTop = 412, kKnobH = 140, kNumKnobs = 6;
constexpr int kRowTop = 96, kRowH = 292;       // the graph and the key module
constexpr int kStyleTop = 400, kStyleH = 168;  // the style and knob module
const juce::Rectangle<int> kNoteStrip { 20, kStyleTop + kStyleH + 6, 840, ui::kNoteStripHeight }; // the notes, under it
} // namespace

HarmoniaEditor::HarmoniaEditor (HarmoniaProcessor& p)
    : AudioProcessorEditor (&p), proc (p), graph (p),
      rangeBox (p.apvts, ids::range,
                "Pick your voice type. It narrows the pitch search so tracking is faster and never jumps an octave."),
      keyBox (p.apvts, ids::key, "The key (root note) of your song."),
      scaleBox (p.apvts, ids::scale,
                "The notes your vocal is allowed to land on. Chromatic = every semitone (fixes pitch only)."),
      bypassButton (p.apvts, ids::bypass, "Bypass",
                    "Hear your vocal untouched (still delayed by the plug-in latency, so it stays in time)."),
      autoKeyButton (p.apvts, ids::autoKey, "Auto key",
                     "Follow the key of the vocal by itself, and change it when the song changes key. "
                     "Works out the key from the singing, so give it a few seconds of vocal. Major/minor only.")
{
    const float savedScale = juce::jlimit (0.75f, 2.0f, (float) (double) p.apvts.state.getProperty (kScaleProperty, 1.0));
    setLookAndFeel (&laf);

    addAndMakeVisible (content);
    content.onPaint = [this] (juce::Graphics& g) { paintContent (g); };

    // ---- header ----
    bypassButton.setColour (juce::TextButton::buttonOnColourId, colours::warn);

    // ---- key card ----
    detectButton.setTooltip ("Click, sing or play a phrase of the song, then click again - Harmonia picks the key for you.");
    detectButton.onClick = [this] { onDetectClicked(); };
    resetButton.setTooltip ("Start tracking afresh: drops the pitch history and the graph. Use it if the pitch or the graph looks stuck.");
    resetButton.onClick = [this]
    {
        proc.reset(); // done on the audio thread, in the next block
        if (proc.learnState() == HarmoniaProcessor::LearnState::idle)
            detectedText = "Not sure of the key? Sing a phrase and let Harmonia find it.";
        awaitingKeyResult = false;
    };
    for (auto* c : std::initializer_list<juce::Component*> { &rangeBox, &bypassButton, &keyBox, &scaleBox, &autoKeyButton, &detectButton, &resetButton, &guideButton, &undoButtons, &abButtons })
        content.addAndMakeVisible (c);

    detectedText = "Not sure of the key? Sing a phrase and let Harmonia find it.";
    statusLabel.setJustificationType (juce::Justification::topLeft);
    statusLabel.setColour (juce::Label::textColourId, colours::dim);
    statusLabel.setFont (ui::font (12.0f));
    statusLabel.setMinimumHorizontalScale (1.0f); // wrap onto the second line rather than squash the letters
    content.addAndMakeVisible (statusLabel);

    keyboard.onToggle = [this] (int pc) { toggleNote (pc); };
    keyboard.setTooltip ("Click a key to allow or block that note. Lit keys are the notes your vocal can be tuned to.");
    content.addAndMakeVisible (keyboard);

    const char* noteTips[4] = { "Allow every note (a chromatic correction, pitch only).", "Block every note. Click keys to allow the ones the song uses.",
                                "Flip every key: allowed notes become blocked and blocked ones allowed.",
                                "Back to the scale of the key shown (the last scale you chose, Major at first), after editing the keys by hand." };
    const char* noteNames[4] = { "ALL", "NONE", "INVERT", "SCALE" };
    for (int i = 0; i < 4; ++i)
    {
        noteButtons[i].setButtonText (noteNames[i]);
        noteButtons[i].setTooltip (noteTips[i]);
        noteButtons[i].onClick = [this, i] { noteAction ((NoteAction) i); };
        content.addAndMakeVisible (noteButtons[i]);
    }

    content.addAndMakeVisible (graph);

    // ---- style presets ----
    for (int i = 0; i < 3; ++i)
    {
        presetButtons[i].setButtonText (kPresets[i].name);
        presetButtons[i].onClick = [this, i] { applyPreset (i); };
        content.addAndMakeVisible (presetButtons[i]);
    }
    presetButtons[0].setTooltip ("Transparent correction. Keeps your vibrato and expression.");
    presetButtons[1].setTooltip ("Polished modern pop vocal.");
    presetButtons[2].setTooltip ("Instant snapping: the robotic T-Pain style effect.");

    // ---- knobs ----
    setupKnob (0, ids::speed, "SPEED",
               "How fast the voice is pulled onto the note. Instant = robotic effect. 20-60 ms is transparent, higher is looser.");
    setupKnob (1, ids::strength, "STRENGTH",
               "How much correction is applied. 100% = fully on the note. Lower keeps more of your own pitch.");
    setupKnob (2, ids::natural, "NATURAL",
               "Keeps your own vibrato and expression. 100% = all of it. 0% = flattened onto the note.");
    setupKnob (3, ids::hold, "NOTE HOLD",
               "How long the voice has to stay near a new note before Harmonia moves to it. Higher stops the correction flickering to a wrong neighbouring note. 0 = switch at once.");
    setupKnob (4, ids::formant, "FORMANT",
               "Changes the vocal character (throat size) without changing pitch. Leave at 0 for your natural voice.");
    setupKnob (5, ids::tuning, "TUNING (A4)",
               "Reference pitch for A4. Change it if the song is not tuned to 440 Hz.");

    setResizable (true, true);
    setResizeLimits (kDesignW * 3 / 4, kDesignH * 3 / 4, kDesignW * 2, kDesignH * 2);
    getConstrainer()->setFixedAspectRatio ((double) kDesignW / (double) kDesignH);
    // The size is kept with the session (a property of the state, as the other plug-ins keep `uiScale`), so the window opens as it was left.
    // (read at the top of the constructor: setResizeLimits resizes the window and resized() writes the property)
    setSize (juce::roundToInt ((float) kDesignW * savedScale), juce::roundToInt ((float) kDesignH * savedScale));

    latencyText = makeLatencyText();
    startTimerHz (20);
}

HarmoniaEditor::~HarmoniaEditor()
{
    setLookAndFeel (nullptr);
}

void HarmoniaEditor::setupKnob (int index, const char* id, const juce::String& title, const juce::String& tip)
{
    auto& k = knobs[(size_t) index];
    k = std::make_unique<ui::Knob> (proc.apvts, id, title, tip, ui::Knob::Layout::below);
    content.addAndMakeVisible (*k);
}

void HarmoniaEditor::resized()
{
    content.setBounds (0, 0, kDesignW, kDesignH);
    const float scale = (float) getWidth() / (float) kDesignW;
    content.setTransform (juce::AffineTransform::scale (scale));
    if (std::fabs (scale - (float) (double) proc.apvts.state.getProperty (kScaleProperty, -1.0)) > 0.002f)
        proc.apvts.state.setProperty (kScaleProperty, (double) scale, nullptr);

    rangeBox.setBounds (580, 36, 190, 28);
    bypassButton.setBounds (784, 36, 76, 28);
    resetButton.setBounds (kLatencyBounds.getRight() + 16, 36, 84, 28);
    guideButton.setBounds (resetButton.getRight() + 10, 36, 76, 28);
    undoButtons.setBounds (kLatencyBounds.getX(), 6, ui::UndoButtons::kWidth, 17); // in the strip above the controls, which the captions leave free
    abButtons.setBounds (kLatencyBounds.getX() + ui::UndoButtons::kWidth + 10, 6, ui::AbButtons::kWidth, 17);

    graph.setBounds (20, kRowTop, 560, kRowH);

    keyBox.setBounds (616, kRowTop + 38, 82, 28);
    scaleBox.setBounds (706, kRowTop + 38, 138, 28);
    detectButton.setBounds (616, kRowTop + 76, 146, 32);
    autoKeyButton.setBounds (770, kRowTop + 76, 74, 32);
    statusLabel.setBounds (616, kRowTop + 114, 228, 34);
    keyboard.setBounds (616, kRowTop + 154, 228, 100);

    for (int i = 0; i < 4; ++i)
        noteButtons[i].setBounds (616 + i * 58, kRowTop + 260, 54, 24);

    for (int i = 0; i < 3; ++i)
        presetButtons[i].setBounds (36, kStyleTop + 40 + i * 40, 130, 32);

    for (int i = 0; i < kNumKnobs; ++i)
        knobs[(size_t) i]->setBounds (kKnobX + i * kKnobW, kKnobTop, kKnobW, kKnobH);
}

void HarmoniaEditor::paintContent (juce::Graphics& g)
{
    namespace hw = ui::hw;

    hw::fillMetal (g, juce::Rectangle<float> (0.0f, 0.0f, (float) kDesignW, (float) kDesignH), colours::background);
    hw::drawHeaderPlate (g, juce::Rectangle<float> (0.0f, 0.0f, (float) kDesignW, (float) kHeaderH));
    hw::drawNameplate (g, kNameplateBounds, words::kProjectWord, words::kPluginWord, "Natural pitch correction", singing, 21.0f);

    hw::drawLabel (g, "Latency", { kLatencyBounds.getX() + 2, kLatencyBounds.getY() - 14, 90, 12 }, juce::Justification::left, colours::dim, 9.0f);
    hw::drawGlass (g, kLatencyBounds.toFloat(), 3.0f, colours::amber);
    g.setFont (ui::lcdFont (12.0f));
    g.setColour (colours::amber);
    g.drawText (latencyText, kLatencyBounds.reduced (6, 0), juce::Justification::centred);
    hw::drawGlassOverlay (g, kLatencyBounds.toFloat(), 3.0f, false);

    hw::drawLabel (g, "Voice", { rangeBox.getX() + 2, rangeBox.getY() - 14, 90, 12 }, juce::Justification::left, colours::dim, 9.0f);

    // key and scale
    hw::drawModule (g, juce::Rectangle<float> (600.0f, (float) kRowTop, 260.0f, (float) kRowH));
    hw::drawLabel (g, "Key & scale", { 620, kRowTop + 12, 220, 16 }, juce::Justification::left, colours::text, 10.5f);
    if (keysLocked)
    {
        g.setColour (colours::dim);
        g.setFont (ui::font (11.0f));
        g.drawText ("Follows your vocal", 716, kRowTop + 10, 128, 20, juce::Justification::centredRight);
    }

    ui::drawNoteStrip (g, kNoteStrip, harmonia::adviceFor (adviceInput()));

    // style and the knobs
    hw::drawModule (g, juce::Rectangle<float> (20.0f, (float) kStyleTop, 840.0f, (float) kStyleH));
    hw::drawLabel (g, "Style", { 40, kStyleTop + 14, 120, 16 }, juce::Justification::left, colours::text, 10.5f);
    g.setColour (juce::Colours::black.withAlpha (0.6f)); // a groove between the presets and the knobs
    g.fillRect (180.0f, (float) kStyleTop + 16.0f, 1.0f, 136.0f);
    g.setColour (juce::Colours::white.withAlpha (0.07f));
    g.fillRect (181.0f, (float) kStyleTop + 16.0f, 1.0f, 136.0f);
}

harmonia::AdviceInput HarmoniaEditor::adviceInput() const
{
    harmonia::AdviceInput in;
    in.bypass = proc.paramValue (ids::bypass) >= 0.5f;
    in.autoKey = proc.paramValue (ids::autoKey) >= 0.5f;
    in.speedMs = proc.paramValue (ids::speed);
    in.holdMs = proc.paramValue (ids::hold);
    in.formantSemitones = proc.paramValue (ids::formant);
    in.tuningHz = proc.paramValue (ids::tuning);
    in.voiceRangeFixed = (int) proc.paramValue (ids::range) != 0;
    const uint16_t mask = proc.keyboardNoteMask(); // not the MIDI notes held: a chord on a keyboard is not a setting to warn about
    in.allowedNotes = 0;
    for (int i = 0; i < 12; ++i)
        in.allowedNotes += (mask >> i) & 1u;
    return in;
}

// ---- parameter helpers ---------------------------------------------------------------

void HarmoniaEditor::setParam (const juce::String& id, float plainValue)
{
    if (auto* p = proc.apvts.getParameter (id))
    {
        p->beginChangeGesture();
        p->setValueNotifyingHost (p->convertTo0to1 (plainValue));
        p->endChangeGesture();
    }
}

void HarmoniaEditor::toggleNote (int pc)
{
    ui::beginUndoStep (proc.apvts, "Note");
    int root = 0;
    bool minor = false;
    if (proc.followedKey (root, minor)) // the key is being followed: the keys on screen are not editable
        return;
    if (proc.scaleIndex() != kCustomScale)
        setNotes (proc.keyboardNoteMask()); // start from what is currently lit, then switch to Custom so edits stick
    const bool now = proc.paramValue (ids::note (pc)) >= 0.5f;
    setParam (ids::note (pc), now ? 0.0f : 1.0f);
}

void HarmoniaEditor::setNotes (uint16_t mask)
{
    ui::beginUndoStep (proc.apvts, "Notes");
    for (int i = 0; i < 12; ++i)
        setParam (ids::note (i), (mask & (1u << i)) != 0 ? 1.0f : 0.0f);
    setParam (ids::scale, (float) kCustomScale);
}

void HarmoniaEditor::noteAction (NoteAction action)
{
    int root = 0;
    bool minor = false;
    if (proc.followedKey (root, minor)) // the key is being followed: the keys are not editable
        return;
    switch (action)
    {
        case NoteAction::all: setNotes (kAllNotes); break;
        case NoteAction::none: setNotes (0); break;
        case NoteAction::invert: setNotes ((uint16_t) (~proc.keyboardNoteMask() & kAllNotes)); break;
        case NoteAction::scale:
            ui::beginUndoStep (proc.apvts, "Scale");
            if (proc.scaleIndex() == kCustomScale)
                setParam (ids::scale, (float) lastRealScale);
            break;
    }
}

void HarmoniaEditor::applyPreset (int index)
{
    ui::beginUndoStep (proc.apvts, "Style");
    setParam (ids::speed, kPresets[index].speed);
    setParam (ids::strength, kPresets[index].strength);
    setParam (ids::natural, kPresets[index].natural);
}

void HarmoniaEditor::onDetectClicked()
{
    if (proc.learnState() == HarmoniaProcessor::LearnState::idle)
    {
        detectedText = "Listening starts now.";
        awaitingKeyResult = false;
        awaitingListen = true;
        detectClickedAt = juce::Time::getMillisecondCounter();
        proc.startKeyLearn();
    }
    else
    {
        proc.finishKeyLearn();
        awaitingKeyResult = true;
        finishClickedAt = juce::Time::getMillisecondCounter();
    }
}

void HarmoniaEditor::timerCallback()
{
    int followRoot = 0;
    bool followMinor = false;
    const bool autoKey = proc.paramValue (ids::autoKey) >= 0.5f;
    const bool following = proc.followedKey (followRoot, followMinor);
    const bool usesRoot = scaleUsesRoot (proc.scaleIndex());

    if (const int sc = proc.scaleIndex(); sc != kCustomScale)
        lastRealScale = sc;
    keyboard.setState (proc.currentNoteMask(), following ? followRoot : (usesRoot ? (int) proc.paramValue (ids::key) : -1), graph.singingPitchClass());
    if (following != keysLocked)
    {
        keysLocked = following;
        content.repaint (716, kRowTop + 10, 128, 20);
        for (auto& b : noteButtons)
            b.setEnabled (! keysLocked);
    }
    keyBox.setEnabled (! autoKey && usesRoot);
    scaleBox.setEnabled (! autoKey);

    // key detection feedback
    HarmoniaProcessor::KeyResult result;
    if (proc.takeKeyResult (result))
    {
        ui::beginUndoStep (proc.apvts, "Detected key");
        setParam (ids::key, (float) result.root);
        setParam (ids::scale, (float) (result.minor ? kMinorScale : kMajorScale));
        detectedText = "Detected " + juce::String (kNoteNames[result.root]) + (result.minor ? " minor" : " major")
                       + "  (" + juce::String (juce::roundToInt (juce::jlimit (0.0f, 1.0f, result.score) * 100.0f)) + "% match)";
        awaitingKeyResult = false;
    }
    else if (awaitingKeyResult && juce::Time::getMillisecondCounter() - finishClickedAt > 700)
    {
        detectedText = "Not enough singing to tell. Sing a few notes, then try again.";
        awaitingKeyResult = false;
    }

    const bool listening = proc.learnState() == HarmoniaProcessor::LearnState::listening;
    if (awaitingListen)
    {
        // Listening starts in the audio callback: a host that is stopped (or not calling this plug-in) never starts it, and the click would seem to do nothing.
        if (listening)
            awaitingListen = false;
        else if (juce::Time::getMillisecondCounter() - detectClickedAt > 1000)
        {
            awaitingListen = false;
            proc.finishKeyLearn(); // withdraw the request, or it would start by itself when the host does run
            detectedText = "The host is not running audio. Press play, then click Detect key again.";
        }
    }
    detectButton.setToggleState (listening, juce::dontSendNotification);
    detectButton.setButtonText (listening ? "Stop & detect" : "Detect key");
    detectButton.setEnabled (! autoKey);
    if (listening)
    {
        const float seconds = proc.learnedAmount() / (float) (proc.hopRateHz() * 0.85);
        statusLabel.setText ("Listening... " + juce::String (seconds, 1) + " s of singing heard. Click again when done.", juce::dontSendNotification);
    }
    else if (autoKey)
        statusLabel.setText (following ? "Following the key: " + juce::String (kNoteNames[followRoot]) + (followMinor ? " minor" : " major")
                                       : juce::String ("Auto key: listening for the key. Play some vocal."),
                             juce::dontSendNotification);
    else
        statusLabel.setText (detectedText, juce::dontSendNotification);

    // highlight the matching style preset
    const float speed = proc.paramValue (ids::speed);
    const float strength = proc.paramValue (ids::strength);
    const float natural = proc.paramValue (ids::natural);
    for (int i = 0; i < 3; ++i)
    {
        const bool match = std::abs (speed - kPresets[i].speed) < 0.5f && std::abs (strength - kPresets[i].strength) < 0.5f
                           && std::abs (natural - kPresets[i].natural) < 0.5f;
        presetButtons[i].setToggleState (match, juce::dontSendNotification);
    }

    // The notes follow the parameters, which change without the editor's help.
    {
        juce::String all;
        for (auto& n : notes())
            all << n.text << '\n';
        if (all != shownNotes)
        {
            shownNotes = all;
            content.repaint (kNoteStrip);
        }
    }

    // The latency readout only changes when the host (re)prepares the plug-in.
    const auto text = makeLatencyText();
    if (text != latencyText)
    {
        latencyText = text;
        content.repaint (kLatencyBounds);
    }

    if (const bool nowSinging = graph.singingPitchClass() >= 0; nowSinging != singing)
    {
        singing = nowSinging;
        content.repaint (kNameplateBounds);
    }
}

juce::String HarmoniaEditor::makeLatencyText() const
{
    if (! proc.engineReady())
        return "--";

    const int latencyMs = juce::roundToInt (1000.0 * proc.getLatencySamples() / juce::jmax (1.0, proc.getSampleRate()));
    return juce::String (latencyMs) + " ms";
}
