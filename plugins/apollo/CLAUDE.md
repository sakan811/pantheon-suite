# CLAUDE.md

Apollo Curve: a JUCE eight-band parametric equaliser (VST3 / AU / Standalone), minimum or linear phase, Stereo / Mid / Side per band, GAIN MATCH, DELTA, and PASTE from Themis. See `README.md` for what it
does and what has been measured; this file is what you need to change it safely.

## Build and verify

Build, test and snapshot commands live in [README.md](README.md) (`## Build & develop`); shared defaults and `scripts/build.sh` live in [../../CLAUDE.md](../../CLAUDE.md) (`## Building and checking`, `## Module conventions & defaults`).

- `Source/DSP` has no JUCE in it (the FFT is its own) and supports standalone `g++` DSP tests (see Gotchas for `-ffast-math`).

## Layout

```
Source/DSP/Bands.h         BandParams, Section (the filter of a band), isIdentity(), responseDb()  <- the curve the editor draws and the filter the engine runs
Source/DSP/LinearPhase.h   Fft, LinearSizes, Convolver (the kernel from the magnitude, FFT overlap-save, the exact delay for the idle case)
Source/DSP/Engine.h        the signal path, the tick, the smoothing, Mid/Side, the phase mode, latency, GAIN MATCH, DELTA
Source/Parameters.h        bandIds(), defaultBand(), readParams(), pasteBands() (PASTE)
Source/UI/CurveView.h      the display and its nodes
```

## Rules that keep it correct

**A band that does nothing is not run.** `isIdentity()` (off, or a bell or shelf under `kIdentityDb`) takes it out of the chain; this is what makes the idle sound bit-exact even
when the compiler fuses multiplies and adds (a biquad at 0 dB is the identity only on paper). The engine keeps running a band that is on its way out (`fading`) so a gain
taken to 0 or a band switched off fades instead of cutting; once the smoothed gain reaches 0 exactly (the last bit is snapped) it stops and is reset. A high-pass, low-pass or notch has no gain to take to 0, so it fades `BandState::mix` (1 = the filter, 0 = the plain input; the sample loop adds `mix * (filtered - input)`, and `Convolver::design` raises the magnitude to `mix`); the same snap applies. A band that starts, or whose type,
target or slope changes, starts from clean filters and from 0 dB, and then moves to its gain like any change.

**The tick.** Smoothing, the output gain and the linear kernels step every `Engine::kTick` = 64 samples, counted from the start; a segment never crosses one. That is what makes the output the
same for any block size (test [5], bit for bit, in both phase modes). Parameters are read per block and used at the next tick.

**The coefficients move, the states stay.** `Section::design()` sets the coefficients only (the `Biquad::set` family never touches `z1` / `z2`). A Stereo band has two sections, one for each channel,
with the same coefficients; a Mid or Side band uses the first only. Both are redesigned at every step.

**Mid and Side add the difference.** A Mid band computes `d = filter(m) - m` and adds `d` to both channels; a Side band adds `d` to the left and subtracts it from the right. Never rebuild left and
right from the filtered mid and the side: with the filter at 0 dB the sum would not be the input.

**Linear phase: sizes in samples, resolution in hertz.** `linearSizesFor (fs)` doubles the frame (4096 at 48 kHz, up to 16384) until a bin is about 12 Hz, with the hop an eighth of it; the kernel is
`frame - hop + 1` taps. The latency is `(frame + hop) / 2`: the kernel's delay `(frame - hop) / 2` plus the hop the output waits for.
The kernel is remade at most once a hop (`sinceDesign`): four FFTs of the frame. The magnitude is read from the same `Section::powerAt` as the curve, on a table of cosines made in `prepare()`.

**Linear phase runs on mid and side.** Left and right go to `(L + R) / 2` and `(L - R) / 2`, each through its own kernel (the bands that reach it: Stereo and Mid for the mid; Stereo and Side for the side),
and come back as `m + s` and `m - s`. With no band in a path the `Convolver` returns the exact delayed input (`kernelIsIdentity`) instead of the FFT's, which is only equal to 1e-16.

**The display is the settings, not the engine.** `responseDb()` designs each band from the parameters as they are (no smoothing); the engine's curve is the same once it has caught up. Draw it, test with it, do not
duplicate a formula for the curve.

**The analyser is the display's, not the engine's.** The processor pushes the sound before and after the bands into `tapIn` / `tapOut` (`pdsp::StereoTap`, lock-free, from `processBlock`; the engine does not know them); `CurveView::tick()`, called by the editor's timer,
reads them into two `pdsp::SpectrumAnalyzer`s and does nothing when ANALYZER is off (the switch is the `analyzer` property of the session's state, not a parameter, so it is not automatable and no id is added). Its scale is its own (`kAnalyzerTopDb`, `kAnalyzerRangeDb`), not the
curve's +-18 dB. A change to `SpectrumAnalyzer.h` is checked here by `ApolloUiCheck`.

**GAIN MATCH and DELTA compare with the input as the output hears it.** In linear phase the dry input is delayed by the latency in a ring (`dryL` / `dryR`, sized in `prepare()`; the Convolver's idle delay is the same integer, so DELTA with no band on is exactly 0); in minimum phase it is the input. The energies in and out are summed per sample and read at the tick (never per block: test [9] pins bit-for-bit block independence with the match on); the match gain is exactly 1.0 with the parameter off, so the idle output stays bit-exact (test [2]). It is applied before OUTPUT. DELTA is `Engine::delta`, an atomic the editor sets, not a parameter: never saved, ended by the editor's destructor, and it shows in the notes.

**PASTE takes free bands, never bands in use.** `pasteBands()` (Parameters.h) reads `pdsp::link::decode`; a band is free when `isIdentity()` (so the default high-pass, low-pass, shelves and bells all are), kinds go to their own end first (`slotOrder`), a link `lowCut` is Apollo's high-pass, left and right bands and fill bands are skipped and counted. The `curve` line becomes the display's reference (`CurveView::setReference`, kept in the state as `referenceCurve`, drawn dashed at the track's level: the analyser's mean over 200 Hz to 5 kHz without the tilt, which is added back). PASTE AMOUNT (`id::pasteAmount`, read in `pasteBands`) multiplies the gain of a bell or shelf by its percentage and nothing else; the reference curve is not scaled. Themis' wording names Apollo (`ThemisUi.h`'s `linkDestination`): run Themis' `ThemisSmoke` when you change what it writes.

**LISTEN is `Engine::listen`, an atomic like `delta`.** The band (0 to 7, or -1) whose region is heard alone, through a band-pass at its frequency and Q placed after the bands and GAIN MATCH, before OUTPUT. The display's LISTEN button (`CurveView::setListen`, which follows `select()`) sets it through `onListen`; the editor's destructor ends it; it shows in the notes. Double-clicking empty glass calls `CurveView::addBandAt` (first free bell slot, `slotOrder`). Every change of a parameter from the display that is not a drag goes through `setWithGesture`.

**Bypass.** `pdsp::BypassDelay` is prepared for the longest latency (`linearSizesFor (fs).latency()`).

**Linear-phase magnitudes are clamped at 0.** `Convolver::design` clamps each band's `powerAt` at 0 before the fade's fractional power (`pow (p, mix)`, NaN for a negative `p`; `std::max (NaN, 0)` is NaN too, so the clamp has to come first) and takes `sqrt (max (power, 0))`: a notch on a bin's exact frequency sums to a hair below 0 and its root was NaN (tests [6] for the fade, [10] for `mix` 1).

**Setup.** Everything is sized in `prepare()` (the FFT tables, the rings, the kernels). A change of phase mode restarts the equaliser (`configure()`).

**Undo.** A gesture that moves several parameters opens its step first with `ui::beginUndoStep`: a preset (`setCurrentProgram`), PASTE, `clearBands`, a node drag (`CurveView::mouseDown`), a double-click on empty glass (`addBandAt`), a wheel step, Delete. A new gesture that changes more than one parameter needs one too, or it merges into the step before it.

**A / B and presets.** `ab` (`ui::AbCompare`) and `Programs` use `apollo::keepsParameter`, so PHASE, OUTPUT, GAIN MATCH and PASTE AMOUNT stay as they are across a switch and across a preset. `ui::AbButtons` sit in the row above the display, after CLEAR ALL.

**Parameters.** `apollo::id`, `bandIds (i)` (`b1_on` ... `b8_target`); never reorder the choice lists (`typeNames`, `targetNames`, `slopeNames`, `phaseNames` follow the enums). All are `ParameterID` version 1 except `match` (version 2, off: a session saved before it sounds as it did; a preset keeps it) and `pasteAmount` (version 2, 100 %, not automatable, read only by `pasteBands`; a preset keeps it).

## DRY: where each thing lives

- band types, targets, slopes and what a type has (`hasGain`, `hasSlope`) -> `Bands.h`; the names the user reads -> `Parameters.h`
- the response of the EQ -> `responseDb()` in `Bands.h` (the editor's curve, the tests)
- band helpers that change several bands (`clearBands`, `pasteBands`) -> `Parameters.h`; the CLEAR ALL button in `PluginEditor.cpp` only calls `clearBands`; Delete / Backspace is `CurveView::keyPressed` (it switches the selected band off through `setWithGesture`, moves the selection and ends LISTEN itself)

## Gotchas already paid for

- Every strip owns its `ui::Choice` / `ui::Toggle` controls (`ApolloEditor::Strip`); the strip shows the ones its type uses (`showTypes()`).
- Building the test with `-O3 -march=native -ffast-math` *together* breaks the linear-phase comparison (the NaN tests and the FFT's rounding do not survive fast-math); each flag on its own passes. Not a supported build.
