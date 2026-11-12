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

- Building the plugin targets does **not** install them (`AEGIS_INSTALL_AFTER_BUILD`, off by default).
- `AegisTests`, `UiCheck` and `Snapshot` are `EXCLUDE_FROM_ALL`; build them by name. `Source/DSP/Engine.h` has no JUCE in it, so the DSP tests also
  compile with `g++ -std=c++20 -O2 -I../../shared/dsp/include -ISource -ITests Tests/Tests.cpp` (a quick loop).
- Definition of done for a DSP change: `AegisTests` passes **and** the numbers the README quotes still match its output.
- Definition of done for a UI or parameter change: `UiCheck` passes and `docs/screenshot.png` is regenerated and looked at.

## Layout

```
Source/DSP/Engine.h  no GUI, no JUCE: SlidingMin, Engine (the whole signal path); covered by Tests/Tests.cpp
../../shared/dsp/    TruePeak.h (`pdsp::TruePeakReader`: the 8x, 64-tap reader; shared with Hermes, so run HermesTests too when you change it)
Source/Advice.h      the notes of the strip under the controls and the GUIDE text; plain C++
Source/Parameters.h  parameter layout, readParams()                       <- the single source of truth for parameters
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

**Real time.** `Engine::process` allocates and locks nothing; everything is sized in `prepare()`. `configure()` (a change of look-ahead) clears the state: it clicks, so
it is a choice, not a knob, and the latency is reported through `AegisProcessor::handleAsyncUpdate` on the message thread.

**Bit exact when idle.** With the gain at 1 the means sum exactly (they are sums of 1.0), `std::fmin (1.0, ...)` removes the last rounding, and the output is the delayed
input bit for bit (test [3]). The smoothed input GAIN starts at its target (`gainStarted`), so a session does not open with a ramp.

**The final clamp** compares with `ceiling * (1 + 1e-9)`: the gain brings a sample *to* the ceiling, and a comparison without the margin counts rounding.

**Bad samples.** NaN and infinity are replaced by 0 on the way in (`finite()`).

**Parameters.** Ids (`aegis::id`) are stored in sessions and automation: never rename or remove one. All are `ParameterID` version 1; a new one needs a higher version, an entry in
`readParams()` and a place in the editor.

## DRY: where each thing lives

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
