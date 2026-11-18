# CLAUDE.md

Aegis Ceiling: a JUCE true-peak look-ahead limiter (VST3 / AU / Standalone). See `README.md` for what it does and what has been measured; this file is
what you need to change it safely.

## Build and verify

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release     # JUCE: version in cmake/Juce.cmake, or third_party/JUCE at the repository root
cmake --build build --target AegisTests -j 10 && ./build/AegisTests_artefacts/Release/AegisTests
cmake --build build --target Aegis_VST3 -j 10
cmake --build build --target AegisUiCheck && ./build/AegisUiCheck_artefacts/Release/AegisUiCheck
cmake --build build --target AegisSnapshot && ./build/AegisSnapshot_artefacts/Release/AegisSnapshot docs/screenshot.png
```

- Building the plugin targets does **not** install them (`AEGIS_INSTALL_AFTER_BUILD`, off by default; a build folder configured with it ON does: check the cache). `scripts/build.sh aegis` is the way to build and check (see the repository's CLAUDE.md).
- `AegisTests`, `UiCheck` and `Snapshot` are `EXCLUDE_FROM_ALL`; build them by name. `Source/DSP/Engine.h` has no JUCE in it, so the DSP tests also
  compile with `g++ -std=c++20 -O2 -I../../shared/dsp/include -ISource -ITests Tests/Tests.cpp` (a quick loop).
- Definition of done for a DSP change: `AegisTests` passes **and** the numbers the README quotes still match its output.
- Definition of done for a UI or parameter change: `UiCheck` passes and `docs/screenshot.png` is regenerated and looked at.

## Layout

```
Source/DSP/Engine.h  no GUI, no JUCE: SlidingMin, Engine (the whole signal path); covered by Tests/Tests.cpp
../../shared/dsp/    TruePeak.h (`pdsp::TruePeakReader`: the 8x, 64-tap reader; shared with Hermes, so run HermesTests too when you change it)
../../shared/dsp/    BandLink.h too: `decodeLimiter` reads what Themis' COPY wrote (PASTE); run Themis' `make test` and `ThemisSmoke` when you change the `limiter` line
Source/Presets.h     the presets (settings, names, one-line purposes); the mechanism is `shared/ui/.../Presets.h`
Source/Advice.h      the notes of the strip under the controls and the GUIDE text; plain C++
Source/Parameters.h  parameter layout, readParams()                       <- the single source of truth for parameters
Source/TipArea.h     the transparent strips that carry the tooltips of the painted display; a click on one goes to the editor
Source/Plugin*.{h,cpp}, Source/UI/   JUCE wrapper and editor (the look is `shared/ui`)
Tests/               Tests.cpp (DSP), UiCheck.cpp (editor on the real processor), Snapshot.cpp (editor -> PNG), Check.h
```

## Rules that keep it correct

**The ceiling is a chain of inequalities; each link is tested.** The gain at the output sample that is heard now must be no more than what the detector needed for the
two interpolation points on either side of that sample. The reader reports the peak around `history[kCentre]`, `kDetectorDelay` samples behind the newest input, so
`required[k]` belongs to the pair `(x[k-D], x[k-D+1])`; the sample output at `n` is `x[n-look-D]`, which sits in the pairs `required[n-look-1]` and `required[n-look]`. So
the hold window is `look + 1` (that is `look + 2` values), and the two running means together reach back at most `look` samples (`widthOne + widthTwo = look + 2`).
A mean is never above the smallest value it covers, so `G[n] <= required` of both. Change any of look, D or the window and test [1] is the check; do not tune
until it passes.

**The reader needs its taps.** A 12-tap reader missed a peak by 1.5 dB on a programme with energy up to Nyquist, 32 taps by 0.7 dB, and a 4x reader reads up to 0.7 dB low
by construction (the peak between two points). It is 64 taps at 8x on purpose (`shared/dsp` TruePeak.h, which Hermes reads its meter with too); do not shorten it for speed without redoing test [8]. The cost is 7 x 64 multiplies per
sample per channel and a delay of 32 samples (`kDetectorDelay`, `pdsp::kTruePeakDelay`), which is constant (TRUE PEAK off does not change it, so the latency does not move with a switch).

**Gain smoothing is what keeps the peaks from coming back.** With one mean (a plain moving average) the gain's own modulation made peaks between samples that the
reader had not seen (0.3 to 0.7 dB over on noise). Two means of half the width fix that: do not collapse them into one.

**AUTO RELEASE only changes the release step.** `release = fmin (held, ...)` whatever the step, so the ceiling's chain of inequalities does not depend on it (test [1] runs with it on and off); do not move the `fmin`. With it off the step is RELEASE's exactly (the old behaviour, bit for bit). `sustained` is a mean of the envelope that decides how far from the fast step to the slow one it is (`kAutoMeanMs`, `kAutoDepth`, `kAutoFastShare`: tuned on test [11], where a short burst must let go much sooner and a long one at RELEASE).

**OUT PEAK reads the output with its own `TruePeakReader`s** (`outPeak`), after the final clamp, so it is what leaves. It is the same reader as the detector's: it proves the clamp and the gain, not the reader (test [8] and [12] do, with an independent one). It costs a second pair of readers; do not drop them for speed.

**DELTA and MATCH LEVEL are listening aids after the limiter.** OUT PEAK reads the limited output before them (it proves the clamp, not the aid). DELTA is `delayL[read] - limited` (the sound after GAIN, aligned by the same read position, minus the output); MATCH multiplies the output by `min (1, sqrt (inPower / outPower))` (a mean of the RMS over `kMatchMeanMs`, smoothed by `kMatchStepMs`), so it only turns down and the ceiling holds. With both off nothing changes, bit for bit (test [13]). Presets switch both off (a preset resets every parameter it does not list); both are `ParameterID` version 2.

**Bypass.** The plugin reports latency, so `processBlockBypassed` delays the main bus by `getLatencySamples()` with `pdsp::BypassDelay` (prepared for the longest look-ahead + `kDetectorDelay`); UiCheck pins that an impulse comes out that late.

**MAX REDUCTION** is a hold in the editor (`reductionHold`, from the engine's `reductionMeter` each tick): cleared by CLEAR, by a click on its number (`holdArea`, set in `resized()`) and when CEILING or TRUE PEAK changes.

**PASTE** (`pasteLimiter` in `Parameters.h`) reads the `limiter` line of `pdsp::link` (`shared/dsp` BandLink.h, written by Themis' COPY): the ceiling is absolute, the gain change is relative to GAIN, and both are clamped to the parameters' ranges. A ceiling switches TRUE PEAK on. It says what it did in the strip (`AegisEditor::shownNotes`), for eight seconds.

**Real time.** `Engine::process` allocates and locks nothing; everything is sized in `prepare()`. `configure()` (a change of look-ahead) clears the state: it clicks, so
it is a choice, not a knob, and the latency is reported through `AegisProcessor::handleAsyncUpdate` on the message thread.

**Bit exact when idle.** With the gain at 1 the means sum exactly (they are sums of 1.0), `std::fmin (1.0, ...)` removes the last rounding, and the output is the delayed
input bit for bit (test [3]). The smoothed input GAIN starts at its target (`gainStarted`), so a session does not open with a ramp.

**The final clamp** compares with `ceiling * (1 + 1e-9)`: the gain brings a sample *to* the ceiling, and a comparison without the margin counts rounding.

**Bad samples.** NaN and infinity are replaced by 0 on the way in (`finite()`).

**Presets** are `ui::Preset`s in `Source/Presets.h` (the settings as real values, a name, a one-line purpose of at most 140 characters); the processor's `ui::Programs` applies them (every parameter goes back to its default first, so a preset does not inherit the last one: `Programs` is given no `keepsParameter`, so nothing is kept, and a preset switches DELTA and MATCH off) and keeps the chosen index in the session's state
(`program`); `setCurrentProgram` begins an undo step (`ui::beginUndoStep`) first, so a preset is one UNDO step. The editor's `ui::PresetBox` lists them and `sync()`s with the host. A new parameter needs thinking about in each preset: it goes back to its default unless the preset lists it. UiCheck pins that every preset applies, what one specific preset sets, and that the box fits the header.

**Undo and A/B.** The processor declares `juce::UndoManager undo` before `apvts` and hands it over (`&undo`), so every control change is one undo step; a preset (`setCurrentProgram`) and a PASTE (`pasteFromClipboard`) each begin a step first, so each is one. `ab` (`ui::AbCompare`) is declared after `apvts` and is given `aegis::keepsParameter`: DELTA and MATCH are left out of both slots and are not changed by a switch (they are listening aids, so a switch does not change how the sound is heard). Presets do not use that function and still switch them off. The active slot is saved in the state as `abSlot`, and a session without it opens on slot 0. The two slots are not saved (a session opens with both empty). Both are the kit's (`Undo.h`, `AbCompare.h`).

**Tooltips over the display.** The painted parts (the trace, MAX, OUT PEAK, the IN and OUT bars, the delay) have `TipArea`s laid over them. A component that does not take clicks is never the one under the mouse, so no tooltip would show on it: a `TipArea` takes the click and hands it to the editor's `mouseDown`, which is where the MAX number clears its hold.

**AUTO GAIN only sets GAIN, once.** The detector's peak before GAIN is kept in `Engine::detectorPeak` (the peak divided by the gain in force at each moment, so a GAIN move while it listens skews the measure: the README's Limits says so). The editor clears it on the click, reads it after four seconds (`finishAutoGain`) and passes it to `autoGain()` in `Engine.h`, which is the whole rule: GAIN is `CEILING + 3 dB - peak`, held to GAIN's range, and nothing is set under -80 dB. It is one undo step, and GAIN stays an ordinary parameter. The readers' last taps of earlier audio count in the first milliseconds of the window, so a test lets them run out before it clicks.

**Parameters.** Ids (`aegis::id`) are stored in sessions and automation: never rename or remove one. All are `ParameterID` version 1 except `autorel`, `delta` and `match` (version 2, off: a session saved before them has the old behaviour; UiCheck pins `autorel`); a new one needs a higher version, an entry in
`readParams()` and a place in the editor.

## DRY: where each thing lives

- the undo and A/B wiring -> the kit (`Undo.h`, `AbCompare.h`); the plugin only names what A/B keeps (`keepsParameter` in `Presets.h`)
- parameter ranges, defaults, choice lists -> `Parameters.h`; the look-ahead times -> `kLookaheadMs` in `Engine.h` (the parameter's names, the engine and the editor read it)
- what the plugin warns about and what GUIDE says -> `adviceFor()` and `guideText()` in `Source/Advice.h`. A note restates a limit the README's Limits documents: a new one needs it
  there, a test in UiCheck's Notes section, and at most `ui::kMaxNoteChars` characters
- the plugin's name -> `AEGIS_PLUGIN_NAME` in `CMakeLists.txt`; the nameplate's two words -> `UI/AegisUi.h` (`UiCheck` checks they agree)
- every pixel of the interface -> `shared/ui`; the editor calls it and does not draw its own controls

## Gotchas already paid for

- `ui::Choice` and `ui::Toggle` are bound to one parameter for good.
- In an editor constructor call `setSize()` **last**.
- `createEditor()` returns a `ui::ScaledEditor<AegisEditor>`, not the editor: tests and snapshots reach it with `ui::innerOf<AegisEditor> (*processor.createEditor())`.
- A test of the reader's accuracy must use a different interpolator than the reader (test: 8x, 64-point Hann sinc, over the whole signal).

## Conventions

As in the other plugins: Allman braces, a space before call parentheses, `juce::` qualified in headers; comments explain *why*; claims in the README are backed by
a test; commit by topic and end commit messages with the Co-Authored-By trailer.
