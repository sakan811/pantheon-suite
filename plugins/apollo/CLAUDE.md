# CLAUDE.md

Apollo Curve: a JUCE eight-band parametric equaliser (VST3 / AU / Standalone), minimum or linear phase, Stereo / Mid / Side per band. See `README.md` for what it
does and what has been measured; this file is what you need to change it safely.

## Build and verify

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release     # JUCE: version in cmake/Juce.cmake, or third_party/JUCE at the repository root
cmake --build build --target ApolloTests -j 10 && ./build/ApolloTests_artefacts/Release/ApolloTests
cmake --build build --target Apollo_VST3 -j 10
cmake --build build --target ApolloUiCheck && ./build/ApolloUiCheck_artefacts/Release/ApolloUiCheck
cmake --build build --target ApolloSnapshot && ./build/ApolloSnapshot_artefacts/Release/ApolloSnapshot docs/screenshot.png
```

- Building the plugin targets does **not** install them (`APOLLO_INSTALL_AFTER_BUILD`, off by default).
- `ApolloTests`, `UiCheck` and `Snapshot` are `EXCLUDE_FROM_ALL`; build them by name. `Source/DSP` has no JUCE in it (the FFT is its own), so the DSP tests also compile
  with `g++ -std=c++20 -O2 -I../../shared/dsp/include -ISource -ITests Tests/Tests.cpp` (a quick loop); run them also with `-O3 -march=native` and with `-O3 -ffast-math`.
- Definition of done for a DSP change: `ApolloTests` passes **and** the numbers the README quotes still match its output.
- Definition of done for a UI or parameter change: `UiCheck` passes and `docs/screenshot.png` is regenerated and looked at.

## Layout

```
Source/DSP/Bands.h         BandParams, Section (the filter of a band), isIdentity(), responseDb()  <- the curve the editor draws and the filter the engine runs
Source/DSP/LinearPhase.h   Fft, LinearSizes, Convolver (the kernel from the magnitude, FFT overlap-save, the exact delay for the idle case)
Source/DSP/Engine.h        the signal path, the tick, the smoothing, Mid/Side, the phase mode, latency
Source/Advice.h            the notes of the strip under the bands and the GUIDE text; plain C++
Source/Parameters.h        parameter layout, bandIds(), defaultBand(), readParams()                <- the single source of truth for parameters
Source/Plugin*.{h,cpp}, Source/UI/   JUCE wrapper and editor (the look is `shared/ui`); `UI/CurveView.h` is the display and its nodes
Tests/                     Tests.cpp (DSP), UiCheck.cpp (editor on the real processor), Snapshot.cpp (editor -> PNG), Check.h
```

## Rules that keep it correct

**A band that does nothing is not run.** `isIdentity()` (off, or a bell or shelf under `kIdentityDb`) takes it out of the chain; this is what makes the idle sound bit-exact even
when the compiler fuses multiplies and adds (a biquad at 0 dB is the identity only on paper). The engine keeps running a band whose gain is on its way to 0 (`fading`) so a gain
taken to 0 or a band switched off fades instead of cutting; once the smoothed gain reaches 0 exactly (the last bit is snapped) it stops and is reset. A band that starts, or whose type,
target or slope changes, starts from clean filters and from 0 dB, and then moves to its gain like any change.

**The tick.** Smoothing, the output gain and the linear kernels step every `Engine::kTick` = 64 samples, counted from the start; a segment never crosses one. That is what makes the output the
same for any block size (test [5], bit for bit, in both phase modes). Parameters are read per block and used at the next tick.

**The coefficients move, the states stay.** `Section::design()` sets the coefficients only (the `Biquad::set` family never touches `z1` / `z2`). A Stereo band has two sections, one for each channel,
with the same coefficients; a Mid or Side band uses the first only. Both are redesigned at every step.

**Mid and Side add the difference.** A Mid band computes `d = filter(m) - m` and adds `d` to both channels; a Side band adds `d` to the left and subtracts it from the right. Never rebuild left and
right from the filtered mid and the side: with the filter at 0 dB the sum would not be the input.

**Linear phase: sizes in samples, resolution in hertz.** `linearSizesFor (fs)` doubles the frame (4096 at 48 kHz, up to 16384) until a bin is about 12 Hz, with the hop an eighth of it; the kernel is
`frame - hop + 1` taps. The latency is `(frame + hop) / 2`: the kernel's delay `(frame - hop) / 2` plus the hop the output waits for. It is reported through `ApolloProcessor::handleAsyncUpdate`.
The kernel is remade at most once a hop (`sinceDesign`): four FFTs of the frame. The magnitude is read from the same `Section::powerAt` as the curve, on a table of cosines made in `prepare()`.

**Linear phase runs on mid and side.** Left and right go to `(L + R) / 2` and `(L - R) / 2`, each through its own kernel (the bands that reach it: Stereo and Mid for the mid; Stereo and Side for the side),
and come back as `m + s` and `m - s`. With no band in a path the `Convolver` returns the exact delayed input (`kernelIsIdentity`) instead of the FFT's, which is only equal to 1e-16.

**The display is the settings, not the engine.** `responseDb()` designs each band from the parameters as they are (no smoothing); the engine's curve is the same once it has caught up. Draw it, test with it, do not
duplicate a formula for the curve.

**Real time.** `Engine::process` allocates and locks nothing; everything is sized in `prepare()` (the FFT tables, the rings, the kernels). A change of phase mode restarts the equaliser (`configure()`).

**Bad samples.** NaN and infinity are replaced by 0 on the way in.

**Parameters.** Ids (`apollo::id`, `bandIds (i)`: `b1_on` ... `b8_target`) are stored in sessions and automation: never rename or remove one, nor reorder the choice lists (`typeNames`, `targetNames`, `slopeNames`,
`phaseNames` follow the enums). All are `ParameterID` version 1; a new one needs a higher version, an entry in `readParams()` and a place in the editor.

## DRY: where each thing lives

- band types, targets, slopes and what a type has (`hasGain`, `hasSlope`) -> `Bands.h`; the names the user reads -> `Parameters.h`
- the response of the EQ -> `responseDb()` in `Bands.h` (the editor's curve, the tests)
- what the plugin warns about and what GUIDE says -> `adviceFor()` and `guideText()` in `Source/Advice.h`. A note restates a limit the README's Limits documents: a new one needs it there, a test in UiCheck's notes
  section, and at most `ui::kMaxNoteChars` characters
- the plugin's name -> `APOLLO_PLUGIN_NAME` in `CMakeLists.txt`; the nameplate's two words -> `UI/ApolloUi.h` (`UiCheck` checks they agree)
- every pixel of the interface -> `shared/ui`; the editor calls it and does not draw its own controls (the curve and its nodes are drawn on `ui::hw` glass)

## Gotchas already paid for

- `ui::Choice` and `ui::Toggle` are bound to one parameter for good, so every strip owns its controls (`ApolloEditor::Strip`); the strip shows the ones its type uses (`showTypes()`).
- In an editor constructor call `setSize()` **last**.
- `createEditor()` returns a `ui::ScaledEditor<ApolloEditor>`, not the editor: tests and snapshots reach it with `ui::innerOf<ApolloEditor> (*processor.createEditor())`.
- Building the test with `-O3 -march=native -ffast-math` *together* breaks the linear-phase comparison (the NaN tests and the FFT's rounding do not survive fast-math); each flag on its own passes. Not a supported build.

## Conventions

As in the other plugins: Allman braces, a space before call parentheses, `juce::` qualified in headers; comments explain *why*; claims in the README are backed by a test; commit by topic and end commit messages with the
Co-Authored-By trailer.
