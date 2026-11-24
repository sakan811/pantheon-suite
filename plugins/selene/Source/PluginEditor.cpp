// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 The Pantheon authors

#include "PluginEditor.h"

#include <pantheon/ui/Aspect.h>
#include <pantheon/ui/FreqAxis.h>

using namespace selene;
namespace hw = ui::hw;

namespace
{
constexpr int headerHeight = 84;
constexpr int rowHeight = 114;
constexpr int kTimerHz = 30;
constexpr int kWidth = 1280;
constexpr int kHeight = ui::editorHeightFor (kWidth); // 800 (16:10)
constexpr int displayHeight = kHeight - (headerHeight + 10 + 8 + 3 * rowHeight + 2 * 8 + 8 + ui::kNoteStripHeight + 12);

constexpr int kMidiChannel = 1;
constexpr float kAuditionVelocity = 0.85f;
} // namespace

// ---- AuditionKeyboard ---------------------------------------------------------------------------

AuditionKeyboard::AuditionKeyboard (juce::MidiKeyboardState& state) : keyboardState (state)
{
    setTooltip ("Audition keyboard (C3-B4): click or drag to play notes.");
}

AuditionKeyboard::~AuditionKeyboard()
{
    if (pressedNote >= 0)
        keyboardState.noteOff (kMidiChannel, pressedNote, 0.0f);
}

bool AuditionKeyboard::isBlack (int midiNote) noexcept
{
    const int pc = midiNote % 12;
    return pc == 1 || pc == 3 || pc == 6 || pc == 8 || pc == 10;
}

juce::Rectangle<float> AuditionKeyboard::whiteKeyRect (int whiteIdx) const noexcept
{
    const auto area = getLocalBounds().toFloat().reduced (2.0f);
    const float w = area.getWidth() / (float) kNumWhiteKeys;
    return { area.getX() + (float) whiteIdx * w, area.getY(), w - 1.0f, area.getHeight() };
}

juce::Rectangle<float> AuditionKeyboard::blackKeyRect (int midiNote) const noexcept
{
    // Map pitch class within octave to the white key boundary immediately to its left.
    static constexpr int kWhiteBefore[12] { 0, 0, 1, 1, 2, 3, 3, 4, 4, 5, 5, 6 };
    const int rel = midiNote - kFirstNote;
    const int oct = rel / 12;
    const int pc = rel % 12;
    const int leftWhite = oct * 7 + kWhiteBefore[pc];
    const auto area = getLocalBounds().toFloat().reduced (2.0f);
    const float ww = area.getWidth() / (float) kNumWhiteKeys;
    const float bw = ww * 0.60f;
    const float bh = area.getHeight() * 0.60f;
    const float cx = area.getX() + (float) (leftWhite + 1) * ww - 0.5f;
    return { cx - bw * 0.5f, area.getY(), bw, bh };
}

int AuditionKeyboard::noteAtPoint (juce::Point<int> pt) const noexcept
{
    const auto p = pt.toFloat();
    if (! getLocalBounds().toFloat().reduced (2.0f).contains (p))
        return -1;

    for (int i = 0; i < kNumNotes; ++i)
    {
        const int note = kFirstNote + i;
        if (isBlack (note) && blackKeyRect (note).contains (p))
            return note;
    }

    int whiteIdx = 0;
    for (int i = 0; i < kNumNotes; ++i)
    {
        const int note = kFirstNote + i;
        if (! isBlack (note))
        {
            if (whiteKeyRect (whiteIdx).contains (p))
                return note;
            ++whiteIdx;
        }
    }
    return -1;
}

void AuditionKeyboard::paint (juce::Graphics& g)
{
    const auto well = getLocalBounds().toFloat();
    g.setColour (juce::Colours::black.withAlpha (0.65f));
    g.fillRoundedRectangle (well, 3.0f);

    int whiteIdx = 0;
    for (int i = 0; i < kNumNotes; ++i)
    {
        const int note = kFirstNote + i;
        if (isBlack (note))
            continue;
        const auto r = whiteKeyRect (whiteIdx);
        const bool on = keyboardState.isNoteOnForChannels (0xffff, note);
        const juce::Colour top = on ? colours::accent.brighter (0.15f) : juce::Colour (0xff49453e);
        const juce::Colour bot = on ? colours::accent.darker (0.25f) : juce::Colour (0xff312e29);
        g.setGradientFill (juce::ColourGradient (top, 0.0f, r.getY(), bot, 0.0f, r.getBottom(), false));
        g.fillRoundedRectangle (r, 2.0f);
        g.setColour (juce::Colours::white.withAlpha (on ? 0.35f : 0.10f));
        g.drawHorizontalLine (juce::roundToInt (r.getY() + 1.0f), r.getX() + 1.0f, r.getRight() - 1.0f);
        g.setColour (juce::Colours::black.withAlpha (0.75f));
        g.drawRoundedRectangle (r, 2.0f, 1.0f);

        if (note == 48 || note == 60)
        {
            g.setFont (ui::lcdFont (9.0f));
            g.setColour (on ? colours::lampInk : colours::dim);
            g.drawText (note == 48 ? "C3" : "C4", r.reduced (2.0f, 4.0f), juce::Justification::centredBottom);
        }
        ++whiteIdx;
    }

    for (int i = 0; i < kNumNotes; ++i)
    {
        const int note = kFirstNote + i;
        if (! isBlack (note))
            continue;
        const auto r = blackKeyRect (note);
        const bool on = keyboardState.isNoteOnForChannels (0xffff, note);
        const juce::Colour top = on ? colours::amber.brighter (0.25f) : juce::Colour (0xff23211e);
        const juce::Colour bot = on ? colours::amber.darker (0.35f) : juce::Colour (0xff0d0c0b);
        g.setGradientFill (juce::ColourGradient (top, 0.0f, r.getY(), bot, 0.0f, r.getBottom(), false));
        g.fillRoundedRectangle (r, 2.0f);
        g.setColour (juce::Colours::black.withAlpha (0.85f));
        g.drawRoundedRectangle (r, 2.0f, 1.0f);
    }
}

void AuditionKeyboard::mouseDown (const juce::MouseEvent& e)
{
    const int note = noteAtPoint (e.getPosition());
    if (note >= 0)
    {
        if (pressedNote >= 0 && pressedNote != note)
            keyboardState.noteOff (kMidiChannel, pressedNote, 0.0f);
        pressedNote = note;
        keyboardState.noteOn (kMidiChannel, pressedNote, kAuditionVelocity);
        repaint();
    }
}

void AuditionKeyboard::mouseDrag (const juce::MouseEvent& e)
{
    const int note = noteAtPoint (e.getPosition());
    if (note != pressedNote)
    {
        if (pressedNote >= 0)
            keyboardState.noteOff (kMidiChannel, pressedNote, 0.0f);
        pressedNote = note;
        if (pressedNote >= 0)
            keyboardState.noteOn (kMidiChannel, pressedNote, kAuditionVelocity);
        repaint();
    }
}

void AuditionKeyboard::mouseUp (const juce::MouseEvent&)
{
    if (pressedNote >= 0)
    {
        keyboardState.noteOff (kMidiChannel, pressedNote, 0.0f);
        pressedNote = -1;
        repaint();
    }
}

// ---- SeleneEditor -------------------------------------------------------------------------------

SeleneEditor::SeleneEditor (SeleneProcessor& p)
    : AudioProcessorEditor (&p),
      undoButtons (p.undo, *this),
      abButtons (p.ab),
      presetBox (p, presets(), "Every parameter is set by a preset; parameters not listed return to defaults."),
      voiceMode (p.apvts, id::voiceMode, "Poly: up to 16 voices.\nMono: single voice with last-note priority and legato glide.\nUnison: 4 detuned, panned voices per note (up to 4 notes)."),

      osc1Wave (p.apvts, id::osc1Wave, "Oscillator 1 waveform: Saw, Pulse (with PWM), Triangle, or Sine."),
      osc1Pwm (p.apvts, id::osc1Pwm, "PWM", "Pulse width of Oscillator 1 when set to Pulse.", ui::Knob::Layout::below),
      subLevel (p.apvts, id::subLevel, "SUB", "Level of the -1 octave sub oscillator.", ui::Knob::Layout::below),

      osc2Wave (p.apvts, id::osc2Wave, "Oscillator 2 waveform: Saw, Pulse, Triangle, or Sine."),
      osc2Semi (p.apvts, id::osc2Semi, "SEMI", "Oscillator 2 coarse tuning in semitones (-24 to +24).", ui::Knob::Layout::below),
      osc2Detune (p.apvts, id::osc2Detune, "DETUNE", "Oscillator 2 fine detune in cents (-50 to +50).", ui::Knob::Layout::below),
      osc2Fm (p.apvts, id::osc2Fm, "FM", "Frequency modulation depth from Oscillator 2 onto Oscillator 1.", ui::Knob::Layout::below),
      oscMix (p.apvts, id::oscMix, "MIX", "Crossfade between Oscillator 1 (0%) and Oscillator 2 (100%).", ui::Knob::Layout::below),
      noiseLevel (p.apvts, id::noiseLevel, "NOISE", "White noise level fed into the filter.", ui::Knob::Layout::below),

      filterMode (p.apvts, id::filterMode, "ZDF SVF filter topology: 24 dB low-pass, 12 dB low-pass, 12 dB band-pass, or 12 dB high-pass."),
      cutoff (p.apvts, id::cutoff, "CUTOFF", "Filter cutoff frequency (20 Hz to 20 kHz).", ui::Knob::Layout::below),
      resonance (p.apvts, id::resonance, "RESO", "Filter resonance up to near self-oscillation.", ui::Knob::Layout::below),
      filterEnv (p.apvts, id::filterEnvAmt, "ENV AMT", "Bipolar filter envelope modulation (-100% to +100%).", ui::Knob::Layout::below),
      keytrack (p.apvts, id::keytrack, "KEYTRK", "Keyboard tracking on filter cutoff around C4.", ui::Knob::Layout::below),
      drive (p.apvts, id::drive, "DRIVE", "Pre-filter and SVF stage analog saturation drive.", ui::Knob::Layout::below),

      ampA (p.apvts, id::ampAttack, "ATTACK", "Amplitude envelope attack time.", ui::Knob::Layout::below),
      ampD (p.apvts, id::ampDecay, "DECAY", "Amplitude envelope decay time.", ui::Knob::Layout::below),
      ampS (p.apvts, id::ampSustain, "SUSTAIN", "Amplitude envelope sustain level.", ui::Knob::Layout::below),
      ampR (p.apvts, id::ampRelease, "RELEASE", "Amplitude envelope release time.", ui::Knob::Layout::below),
      velAmp (p.apvts, id::velAmp, "VEL->AMP", "MIDI velocity sensitivity for voice amplitude.", ui::Knob::Layout::below),

      fltA (p.apvts, id::fltAttack, "ATTACK", "Filter envelope attack time.", ui::Knob::Layout::below),
      fltD (p.apvts, id::fltDecay, "DECAY", "Filter envelope decay time.", ui::Knob::Layout::below),
      fltS (p.apvts, id::fltSustain, "SUSTAIN", "Filter envelope sustain level.", ui::Knob::Layout::below),
      fltR (p.apvts, id::fltRelease, "RELEASE", "Filter envelope release time.", ui::Knob::Layout::below),
      velFlt (p.apvts, id::velFlt, "VEL->FLT", "MIDI velocity sensitivity for filter envelope depth.", ui::Knob::Layout::below),

      lfoWave (p.apvts, id::lfoWave, "Global LFO waveform: Sine, Triangle, Square, or Sample & Hold."),
      lfoRate (p.apvts, id::lfoRate, "RATE", "Global LFO frequency (0.05 Hz to 20 Hz).", ui::Knob::Layout::below),
      lfoPitch (p.apvts, id::lfoPitch, "->PITCH", "LFO vibrato depth in cents.", ui::Knob::Layout::below),
      lfoFilter (p.apvts, id::lfoFilter, "->FLT", "LFO modulation depth onto filter cutoff.", ui::Knob::Layout::below),
      lfoPwm (p.apvts, id::lfoPwm, "->PWM", "LFO modulation depth onto Oscillator 1 pulse width.", ui::Knob::Layout::below),

      glide (p.apvts, id::glide, "GLIDE", "Portamento / pitch glide time between notes.", ui::Knob::Layout::below),
      drift (p.apvts, id::drift, "DRIFT", "Per-voice slow analog pitch and filter drift.", ui::Knob::Layout::below),
      masterGain (p.apvts, id::masterGain, "MASTER", "Master output gain (-24 dB to +12 dB).", ui::Knob::Layout::below),

      chorusMode (p.apvts, id::chorusMode, "Classic 80s BBD stereo chorus: Mode I (gentle), Mode II (deep), or I+II (wide dual-rate shimmer)."),
      echoTime (p.apvts, id::echoTime, "TIME", "Stereo tape echo delay time (40 ms to 1 s).", ui::Knob::Layout::below),
      echoFeedback (p.apvts, id::echoFeedback, "FEEDBK", "Cross-fed stereo tape echo feedback.", ui::Knob::Layout::below),
      echoMix (p.apvts, id::echoMix, "MIX", "Wet mix of the stereo tape echo.", ui::Knob::Layout::below),

      auditionKeyboard (p.keyboardState),
      proc (p)
{
    setLookAndFeel (&look);

    prevPresetButton.setTooltip ("Previous preset");
    nextPresetButton.setTooltip ("Next preset");
    const auto stepPreset = [this] (int delta)
    {
        const int n = proc.getNumPrograms();
        if (n <= 0)
            return;
        const int nextIdx = (proc.getCurrentProgram() + delta + n) % n;
        proc.setCurrentProgram (nextIdx);
        presetBox.sync();
    };
    prevPresetButton.onClick = [stepPreset] { stepPreset (-1); };
    nextPresetButton.onClick = [stepPreset] { stepPreset (1); };

    for (auto* c : std::initializer_list<juce::Component*> {
             &undoButtons, &abButtons, &presetBox, &prevPresetButton, &nextPresetButton, &voiceMode, &guideButton,
             &osc1Wave, &osc1Pwm, &subLevel,
             &osc2Wave, &osc2Semi, &osc2Detune, &osc2Fm, &oscMix, &noiseLevel,
             &filterMode, &cutoff, &resonance, &filterEnv, &keytrack, &drive,
             &ampA, &ampD, &ampS, &ampR, &velAmp,
             &fltA, &fltD, &fltS, &fltR, &velFlt,
             &lfoWave, &lfoRate, &lfoPitch, &lfoFilter, &lfoPwm,
             &glide, &drift, &masterGain,
             &chorusMode, &echoTime, &echoFeedback, &echoMix,
             &auditionKeyboard })
        addAndMakeVisible (*c);

    startTimerHz (kTimerHz);
    setSize (kWidth, kHeight);
}

SeleneEditor::~SeleneEditor()
{
    setLookAndFeel (nullptr);
}

void SeleneEditor::resized()
{
    auto bounds = getLocalBounds().reduced (12, 0);

    // Header (84 px)
    auto header = bounds.removeFromTop (headerHeight).withTrimmedTop (8).withTrimmedBottom (8);
    nameplate = header.removeFromLeft (250);
    guideButton.setBounds (header.removeFromRight (64).withSizeKeepingCentre (64, 28));
    header.removeFromLeft (20);
    const auto strip = header.withHeight (16).translated (0, -6);
    undoButtons.setBounds (strip.getX(), strip.getY(), ui::UndoButtons::kWidth, strip.getHeight());
    abButtons.setBounds (strip.getX() + ui::UndoButtons::kWidth + 10, strip.getY(), ui::AbButtons::kWidth, strip.getHeight());
    auto headerRow = header.withTrimmedTop (28).withHeight (28);
    presetLabel = { headerRow.getX() + 2, headerRow.getY() - 15, 160, 12 };
    presetBox.setBounds (headerRow.removeFromLeft (190));
    headerRow.removeFromLeft (4);
    prevPresetButton.setBounds (headerRow.removeFromLeft (24));
    headerRow.removeFromLeft (2);
    nextPresetButton.setBounds (headerRow.removeFromLeft (24));
    headerRow.removeFromLeft (14);
    voiceModeLabel = { headerRow.getX() + 2, headerRow.getY() - 15, 120, 12 };
    voiceMode.setBounds (headerRow.removeFromLeft (120));

    // Top Glass Displays
    bounds.removeFromTop (10);
    auto display = bounds.removeFromTop (displayHeight);
    filterArea = display.removeFromLeft (display.getWidth() / 2 - 4);
    display.removeFromLeft (8);
    scopeArea = display;

    // Helper to lay out a choice + N knobs inside a module box
    const auto layoutChoiceAndKnobs = [] (juce::Rectangle<int> mod, juce::Rectangle<int>& choiceLabel, ui::Choice* choice, int choiceW,
                                          std::initializer_list<ui::Knob*> knobs, int knobW)
    {
        auto r = mod.withTrimmedTop (20).withTrimmedBottom (4).reduced (8, 0);
        if (choice != nullptr)
        {
            auto col = r.removeFromLeft (choiceW);
            choiceLabel = col.removeFromTop (16);
            choice->setBounds (col.withTrimmedTop (12).withHeight (26));
            r.removeFromLeft (6);
        }
        for (auto* k : knobs)
            k->setBounds (r.removeFromLeft (knobW));
    };

    // Row 1: OSC 1 (260), OSC 2 (490), FILTER (490) -> 260 + 8 + 490 + 8 + 490 = 1256
    bounds.removeFromTop (8);
    auto row1 = bounds.removeFromTop (rowHeight);
    osc1Mod = row1.removeFromLeft (260);
    row1.removeFromLeft (8);
    osc2Mod = row1.removeFromLeft (490);
    row1.removeFromLeft (8);
    filterMod = row1;

    layoutChoiceAndKnobs (osc1Mod, osc1WaveLabel, &osc1Wave, 84, { &osc1Pwm, &subLevel }, 77);
    layoutChoiceAndKnobs (osc2Mod, osc2WaveLabel, &osc2Wave, 84, { &osc2Semi, &osc2Detune, &osc2Fm, &oscMix, &noiseLevel }, 76);
    layoutChoiceAndKnobs (filterMod, filterModeLabel, &filterMode, 84, { &cutoff, &resonance, &filterEnv, &keytrack, &drive }, 76);

    // Row 2: AMP ENV (411), FILTER ENV (411), LFO (418) -> 411 + 8 + 411 + 8 + 418 = 1256
    bounds.removeFromTop (8);
    auto row2 = bounds.removeFromTop (rowHeight);
    ampEnvMod = row2.removeFromLeft (411);
    row2.removeFromLeft (8);
    fltEnvMod = row2.removeFromLeft (411);
    row2.removeFromLeft (8);
    lfoMod = row2;

    juce::Rectangle<int> dummy;
    layoutChoiceAndKnobs (ampEnvMod, dummy, nullptr, 0, { &ampA, &ampD, &ampS, &ampR, &velAmp }, 79);
    layoutChoiceAndKnobs (fltEnvMod, dummy, nullptr, 0, { &fltA, &fltD, &fltS, &fltR, &velFlt }, 79);
    layoutChoiceAndKnobs (lfoMod, lfoWaveLabel, &lfoWave, 84, { &lfoRate, &lfoPitch, &lfoFilter, &lfoPwm }, 78);

    // Row 3: ANALOG (256), CHORUS & ECHO (350), AUDITION (634) -> 256 + 8 + 350 + 8 + 634 = 1256
    bounds.removeFromTop (8);
    auto row3 = bounds.removeFromTop (rowHeight);
    analogMod = row3.removeFromLeft (256);
    row3.removeFromLeft (8);
    fxMod = row3.removeFromLeft (350);
    row3.removeFromLeft (8);
    auditionMod = row3;

    layoutChoiceAndKnobs (analogMod, dummy, nullptr, 0, { &glide, &drift, &masterGain }, 80);
    layoutChoiceAndKnobs (fxMod, chorusModeLabel, &chorusMode, 88, { &echoTime, &echoFeedback, &echoMix }, 80);
    auditionKeyboard.setBounds (auditionMod.withTrimmedTop (22).withTrimmedBottom (8).reduced (12, 0));

    // Bottom Note Strip
    noteStrip = bounds.withTrimmedTop (8).withHeight (ui::kNoteStripHeight);
}

selene::AdviceInput SeleneEditor::adviceInput() const
{
    selene::AdviceInput in;
    in.params = readParams (proc.apvts);
    in.activeVoices = shownVoices;
    in.outputPeak = shownPeak;
    return in;
}

void SeleneEditor::paint (juce::Graphics& g)
{
    g.fillAll (colours::background);

    // Header
    hw::drawHeaderPlate (g, getLocalBounds().removeFromTop (headerHeight).toFloat());
    hw::drawNameplate (g, nameplate, gui::kProjectWord, gui::kPluginWord, "Poly Synth",
                       shownVoices > 0 || shownPeak > 0.001f, 25.0f);
    hw::drawLabel (g, "Preset", presetLabel, juce::Justification::left, colours::dim, 9.0f);
    hw::drawLabel (g, "Voice Mode", voiceModeLabel, juce::Justification::left, colours::dim, 9.0f);

    const auto params = readParams (proc.apvts);

    // Left Glass: Filter Frequency Response
    {
        const auto glass = filterArea.toFloat();
        hw::drawGlass (g, glass, 4.0f, gui::kFilterCurve);
        const auto r = glass.reduced (34.0f, 22.0f).withTrimmedBottom (6.0f);
        const auto xOf = [&] (double hz) { return r.getX() + r.getWidth() * ui::FreqAxis::toNormalised ((float) hz); };
        const auto yOf = [&] (double dbv)
        {
            return r.getY() + r.getHeight() * (float) ((gui::kFilterCeilDb - dbv) / (gui::kFilterCeilDb - gui::kFilterFloorDb));
        };

        g.setFont (ui::lcdFont (9.5f));
        for (const double hz : { 50.0, 100.0, 200.0, 500.0, 1000.0, 2000.0, 5000.0, 10000.0 })
        {
            g.setColour (colours::glassGrid.withAlpha (hz == 100.0 || hz == 1000.0 || hz == 10000.0 ? 1.0f : 0.45f));
            g.drawVerticalLine ((int) xOf (hz), r.getY(), r.getBottom());
        }
        for (const double hz : { 100.0, 1000.0, 10000.0 })
        {
            g.setColour (colours::dim);
            g.drawText (hz < 1000.0 ? juce::String ((int) hz) : juce::String ((int) (hz / 1000.0)) + "k",
                        juce::Rectangle<float> (xOf (hz) - 16.0f, r.getBottom() + 2.0f, 32.0f, 11.0f),
                        juce::Justification::centred);
        }
        for (int d = -36; d <= 12; d += 12)
        {
            g.setColour (d == 0 ? colours::glassGrid.brighter (0.4f) : colours::glassGrid);
            g.drawHorizontalLine ((int) yOf (d), r.getX(), r.getRight());
            g.setColour (colours::dim);
            g.drawText (d > 0 ? "+" + juce::String (d) : juce::String (d),
                        juce::Rectangle<float> (glass.getX() + 4.0f, yOf (d) - 7.0f, 26.0f, 14.0f),
                        juce::Justification::centredRight);
        }

        const double fs = std::max (proc.getSampleRate(), 8000.0);
        const int steps = std::max (1, (int) r.getWidth());

        // Optional dashed amber curve showing peak filter envelope modulation
        if (std::fabs (params.filterEnvAmt) > 0.01f)
        {
            EngineParams envParams = params;
            envParams.cutoffHz = (float) std::clamp ((double) params.cutoffHz * std::pow (2.0, (double) params.filterEnvAmt * 5.0),
                                                     20.0, fs * 0.45);
            juce::Path envCurve;
            for (int i = 0; i <= steps; ++i)
            {
                const double hz = ui::FreqAxis::toHz ((float) i / (float) steps);
                const double dbv = 20.0 * std::log10 (std::fmax (filterMagnitudeAt (envParams, hz, fs), 1e-4));
                const float y = juce::jlimit (r.getY(), r.getBottom(), yOf (dbv));
                i == 0 ? envCurve.startNewSubPath (r.getX(), y) : envCurve.lineTo (r.getX() + (float) i, y);
            }
            juce::Path dashed;
            const float dashLengths[2] { 4.0f, 4.0f };
            juce::PathStrokeType (1.2f).createDashedStroke (dashed, envCurve, dashLengths, 2);
            g.setColour (colours::amber.withAlpha (0.55f));
            g.fillPath (dashed);
        }

        // Main static filter response curve
        juce::Path curve;
        for (int i = 0; i <= steps; ++i)
        {
            const double hz = ui::FreqAxis::toHz ((float) i / (float) steps);
            const double dbv = 20.0 * std::log10 (std::fmax (filterMagnitudeAt (params, hz, fs), 1e-4));
            const float y = juce::jlimit (r.getY(), r.getBottom(), yOf (dbv));
            i == 0 ? curve.startNewSubPath (r.getX(), y) : curve.lineTo (r.getX() + (float) i, y);
        }
        juce::Path filled (curve);
        filled.lineTo (r.getRight(), r.getBottom());
        filled.lineTo (r.getX(), r.getBottom());
        filled.closeSubPath();
        g.setColour (gui::kFilterCurve.withAlpha (0.18f));
        g.fillPath (filled);
        g.setColour (gui::kFilterCurve);
        g.strokePath (curve, juce::PathStrokeType (1.8f));

        hw::drawLabel (g, "FILTER RESPONSE", { (int) glass.getX() + 12, (int) glass.getY() + 5, 160, 12 }, juce::Justification::left, colours::dim, 8.5f);
        const juce::String cutoffText = params.cutoffHz < 1000.0f
                                            ? juce::String (juce::roundToInt (params.cutoffHz)) + " Hz"
                                            : juce::String (params.cutoffHz / 1000.0f, 2) + " kHz";
        const juce::String filterReadout = filterModeNames[(int) params.filterMode] + " | " + cutoffText + " | "
                                           + juce::String (juce::roundToInt (params.resonance * 100.0f)) + "%";
        g.setFont (ui::lcdFont (10.5f));
        g.setColour (colours::amber);
        g.drawText (filterReadout, juce::Rectangle<float> (glass.getX() + 170.0f, glass.getY() + 4.0f, glass.getWidth() - 182.0f, 14.0f),
                    juce::Justification::centredRight);

        hw::drawGlassOverlay (g, glass, 4.0f, false);
    }

    // Right Glass: Oscilloscope
    {
        const auto glass = scopeArea.toFloat();
        hw::drawGlass (g, glass, 4.0f, gui::kScopeTrace);
        const auto r = glass.reduced (18.0f, 22.0f).withTrimmedBottom (6.0f);

        for (int div = 1; div < 8; ++div)
        {
            g.setColour (colours::glassGrid.withAlpha (div == 4 ? 0.85f : 0.45f));
            g.drawVerticalLine ((int) (r.getX() + r.getWidth() * (float) div / 8.0f), r.getY(), r.getBottom());
        }
        for (const float level : { -0.5f, 0.0f, 0.5f })
        {
            const float y = r.getCentreY() - level * (r.getHeight() * 0.45f);
            g.setColour (level == 0.0f ? colours::glassGrid.brighter (0.4f) : colours::glassGrid);
            g.drawHorizontalLine ((int) y, r.getX(), r.getRight());
        }

        // Trigger on first rising zero-crossing in the first half so periodic waves stand steady on glass.
        const int window = kScopeSamples / 2;
        int trig = 0;
        for (int i = 1; i < window; ++i)
        {
            if (scopeData[(size_t) (i - 1)] <= 0.0f && scopeData[(size_t) i] > 0.0f)
            {
                trig = i;
                break;
            }
        }

        juce::Path trace;
        for (int i = 0; i < window; ++i)
        {
            const float x = r.getX() + r.getWidth() * (float) i / (float) (window - 1);
            const float s = juce::jlimit (-1.1f, 1.1f, scopeData[(size_t) (trig + i)]);
            const float y = r.getCentreY() - s * (r.getHeight() * 0.42f);
            i == 0 ? trace.startNewSubPath (x, y) : trace.lineTo (x, y);
        }
        g.setColour (gui::kScopeTrace.withAlpha (0.20f));
        g.strokePath (trace, juce::PathStrokeType (3.8f));
        g.setColour (gui::kScopeTrace);
        g.strokePath (trace, juce::PathStrokeType (1.7f));

        hw::drawLabel (g, "OSCILLOSCOPE", { (int) glass.getX() + 12, (int) glass.getY() + 5, 130, 12 }, juce::Justification::left, colours::dim, 8.5f);
        const juce::String presetName = proc.getProgramName (proc.getCurrentProgram());
        const juce::String scopeReadout = "VOICES: " + juce::String (shownVoices) + "/" + juce::String (kMaxVoices)
                                          + (presetName.isNotEmpty() ? " | " + presetName : juce::String());
        g.setFont (ui::lcdFont (10.5f));
        g.setColour (colours::amber);
        g.drawText (scopeReadout, juce::Rectangle<float> (glass.getX() + 140.0f, glass.getY() + 4.0f, glass.getWidth() - 152.0f, 14.0f),
                    juce::Justification::centredRight);

        hw::drawGlassOverlay (g, glass, 4.0f, false);
    }

    // Hardware Modules & Section Labels
    const auto drawSection = [&g] (juce::Rectangle<int> mod, const juce::String& title)
    {
        hw::drawModule (g, mod.toFloat());
        hw::drawLabel (g, title, { mod.getX() + 16, mod.getY() + 5, mod.getWidth() - 32, 13 }, juce::Justification::left, colours::dim, 9.5f);
    };

    drawSection (osc1Mod, "OSC 1");
    drawSection (osc2Mod, "OSC 2");
    drawSection (filterMod, "FILTER");
    drawSection (ampEnvMod, "AMP ENV");
    drawSection (fltEnvMod, "FILTER ENV");
    drawSection (lfoMod, "LFO");
    drawSection (analogMod, "ANALOG");
    drawSection (fxMod, "CHORUS & ECHO");
    drawSection (auditionMod, "AUDITION (C3 - B4)");

    // Choice sub-labels
    hw::drawLabel (g, "WAVE", osc1WaveLabel, juce::Justification::centred, colours::dim, 9.0f);
    hw::drawLabel (g, "WAVE", osc2WaveLabel, juce::Justification::centred, colours::dim, 9.0f);
    hw::drawLabel (g, "MODE", filterModeLabel, juce::Justification::centred, colours::dim, 9.0f);
    hw::drawLabel (g, "WAVE", lfoWaveLabel, juce::Justification::centred, colours::dim, 9.0f);
    hw::drawLabel (g, "CHORUS", chorusModeLabel, juce::Justification::centred, colours::dim, 9.0f);

    // Bottom Note Strip
    ui::drawNoteStrip (g, noteStrip, selene::adviceFor (adviceInput()));
}

void SeleneEditor::timerCallback()
{
    proc.engine.copyScope (scopeData.data(), kScopeSamples);
    shownVoices = proc.engine.activeVoices.load (std::memory_order_relaxed);
    shownPeak = proc.engine.outputPeak.load (std::memory_order_relaxed);
    presetBox.sync();
    auditionKeyboard.repaint();
    repaint();
}
