# CLAUDE.md

Dionysus Warmth: a JUCE saturator (VST3 / AU / Standalone): six curves, bias, tone, parallel mix, auto gain, half-band oversampling up to 8x. See `README.md` for what it does and what has been measured; this file is what you need to change it safely.

## Build and verify

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release     # JUCE: version in cmake/Juce.cmake, or third_party/JUCE at the repository root
cmake --build build --target DionysusTests -j 10 && ./build/DionysusTests_artefacts/Release/DionysusTests
cmake --build build --target Dionysus_VST3 -j 10
cmake --build build --target DionysusUiCheck && ./build/DionysusUiCheck_artefacts/Release/DionysusUiCheck
cmake --build build --target DionysusSnapshot && ./build/DionysusSnapshot_artefacts/Release/DionysusSnapshot docs/screenshot.png
```

- Building the plugin targets does **not** install them (`DIONYSUS_INSTALL_AFTER_BUILD`, off by default).
- `DionysusTests`, `UiCheck` and `Snapshot` are `EXCLUDE_FROM_ALL`; build them by name. `Source/DSP` has no JUCE in it, so the DSP tests also compile with
  `g++ -std=c++20 -O2 -I../../shared/dsp/include -ISource -ITests Tests/Tests.cpp` (a quick loop); run them also with `-O3 -march=native` and with `-O3 -ffast-math`, each on its own.
- Definition of done for a DSP change: `DionysusTests` passes **and** the numbers the README quotes still match its output.
- Definition of done for a UI or parameter change: `UiCheck` passes and `docs/screenshot.png` is regenerated and looked at.

## Layout

```
Source/DSP/Shapes.h        the curves, saturate() (curve + bias), harmonicsDb() (what the editor shows), compensation() (AUTO GAIN)   <- one source for the engine, the display and the tests
Source/DSP/Oversampler.h   HalfbandStage (one doubling), Oversampler (the chain of one channel), oversamplingLatency()
Source/DSP/Engine.h        the signal path: drive, the oversampler around the curve, DC block, tone, the delayed dry sound, mix, output
Source/Advice.h            the notes of the strip under the controls and the GUIDE text; plain C++
Source/Parameters.h        parameter layout, readParams()                       <- the single source of truth for parameters
Source/Plugin*.{h,cpp}, Source/UI/   JUCE wrapper and editor (the look is `shared/ui`)
Tests/                     Tests.cpp (DSP), UiCheck.cpp (editor on the real processor), Snapshot.cpp (editor -> PNG), Check.h
```

## Rules that keep it correct

**Latency is a whole number of samples.** Stage 0 has 65 taps (half-length M = 32), the others 33 (M = 16); a stage's round trip delays by M samples of its lower rate, so the total is 32, 32 + 8 = 40 and 32 + 8 + 4 = 44
samples (`oversamplingLatency`). The decimator returns the filter's value at the *even* sample of each pair (it computes it when the first arrives and hands it out with the second): taking the odd one added half a sample per stage
and made the wet path one sample later than the dry delay (test [4], the impulse). The dry sound waits in a ring of `latency + 1` samples (a fixed array of `kMaxLatency + 1`). Change a tap count and the latency formula, the test and the README move together.

**The stages are half-bands, and the code uses it.** Every tap an even distance from the centre is exactly 0, the centre is 1/2, the filter is symmetric: `up` gives its first output as the delayed input and its second from the odd taps in pairs, `down` is the centre plus the odd taps in pairs, and each history is written twice (`taps` apart) so a window is contiguous, with no modulo. That is about a fifth of the CPU of the plain convolution it replaced (8x went from 25-50% to about 4% of a core, stereo at 48 kHz) with the same output to 1e-15. Taps must stay odd with (taps - 1) / 2 even and at most `HalfbandStage::kMaxTaps`. `Engine::process` measures AUTO GAIN only when the shape, drive or bias moves (`compensationFor`: it is a loop of 256 curve evaluations, which at small host blocks cost more than the oversampler).

**Gain 2 on the way up.** Zero-stuffing halves the level; the up filter has a gain of 2 (`first = 2 x a`). The fundamental is the same at every oversampling (test [3]).

**BIAS and the DC.** The curve gets `x + b` and gives back `curve (x + b) - curve (b)`, so zero in is zero out; an asymmetric curve (BIAS, or the diode) still has DC while a signal plays, so a 10 Hz high-pass runs after the curve
whenever either is in use (`dcOn`) and is reset while it is not. `compensation()` takes the mean off the output before it reads the RMS for the same reason (test [5]).

**The display is the curve, not a measurement.** `harmonicsDb()` and the editor's curve call `saturate()`, the function the engine runs; test [2] checks the engine against it at 8x. Do not write a second formula for either.

**MIX.** 100% takes the saturated sound itself (not `dry + mix x (wet - dry)`), 0% the delayed dry sound itself: exact at both ends. The wet path runs all the time so its states are warm.

**Real time.** `Engine::process` allocates and locks nothing; everything is a fixed array or sized in `prepare()` (`configure()` allocates nothing, so a change of oversampling on the audio thread is safe; a change of oversampling restarts the saturator and is reported through `DionysusProcessor::handleAsyncUpdate`).
**Bad samples:** NaN and infinity are replaced by 0 on the way in.

**Parameters.** Ids (`dionysus::id`) are stored in sessions and automation: never rename or remove one. All are `ParameterID` version 1; a new one needs a higher version, an entry in `readParams()` and a place in the editor.
The order of `shapeNames` is the order of `Shape`; of `oversamplingNames`, the index of `kOversamplingFactor`.

## DRY: where each thing lives

- the curves and their names for the user -> `Shapes.h` (`shape()`), `Parameters.h` (`shapeNames`); the harmonics and the AUTO GAIN factor -> `Shapes.h`
- the oversampling factors and latencies -> `Oversampler.h`
- parameter ranges, defaults, choice lists -> `Parameters.h` (the editor reads the items from the parameters)
- what the plugin warns about and what GUIDE says -> `adviceFor()` and `guideText()` in `Source/Advice.h`. A note restates a limit the README's Limits documents: a new one needs it there, a test in UiCheck's notes
  section, and at most `ui::kMaxNoteChars` characters
- the plugin's name -> `DIONYSUS_PLUGIN_NAME` in `CMakeLists.txt`; the nameplate's two words -> `UI/DionysusUi.h` (`UiCheck` checks they agree)
- every pixel of the interface -> `shared/ui`; the editor calls it and does not draw its own controls

## Gotchas already paid for

- Test aliasing with a harmonic far above Nyquist (a 7 kHz sine's 5th, 35 kHz), not one just above (a 9 kHz sine's 3rd, 27 kHz, sits in the half-band's transition band and is only 38 dB down at 8x).
- `ui::Choice` and `ui::Toggle` are bound to one parameter for good. In an editor constructor call `setSize()` **last**.
- `createEditor()` returns a `ui::ScaledEditor<DionysusEditor>`, not the editor: tests and snapshots reach it with `ui::innerOf<DionysusEditor> (*processor.createEditor())`.

## Conventions

As in the other plugins: Allman braces, a space before call parentheses, `juce::` qualified in headers; comments explain *why*; claims in the README are backed by a test; commit by topic and end commit messages with the
Co-Authored-By trailer.
