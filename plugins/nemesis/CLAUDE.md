# CLAUDE.md

Nemesis Hush: a JUCE de-esser (VST3 / AU / Standalone): a band detector, split-band or wideband, absolute or relative detection, listen. See `README.md` for what it does and what has been measured; this file is what you need to change it safely.

## Build and verify

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release     # JUCE: version in cmake/Juce.cmake, or third_party/JUCE at the repository root
cmake --build build --target NemesisTests -j 10 && ./build/NemesisTests_artefacts/Release/NemesisTests
cmake --build build --target Nemesis_VST3 -j 10
cmake --build build --target NemesisUiCheck && ./build/NemesisUiCheck_artefacts/Release/NemesisUiCheck
cmake --build build --target NemesisSnapshot && ./build/NemesisSnapshot_artefacts/Release/NemesisSnapshot docs/screenshot.png
```

- Building the plugin targets does **not** install them (`NEMESIS_INSTALL_AFTER_BUILD`, off by default; a build folder configured with it ON does: check the cache). `scripts/build.sh nemesis` is the way to build and check (see the repository's CLAUDE.md).
- `NemesisTests`, `UiCheck` and `Snapshot` are `EXCLUDE_FROM_ALL`; build them by name. `Source/DSP` has no JUCE in it, so the DSP tests also compile with
  `g++ -std=c++20 -O2 -I../../shared/dsp/include -ISource -ITests Tests/Tests.cpp` (a quick loop); run them also with `-O3 -march=native` and with `-O3 -ffast-math`, each on its own.
- Definition of done for a DSP change: `NemesisTests` passes **and** the numbers the README quotes still match its output.
- Definition of done for a UI or parameter change: `UiCheck` passes and `docs/screenshot.png` is regenerated and looked at.

## Layout

```
Source/DSP/Engine.h  no GUI, no JUCE: reductionDb() (the law), Engine (the band, the envelope, the follower, split-band / wideband / listen); covered by Tests/Tests.cpp
Source/Presets.h     the presets (settings, names, one-line purposes); the mechanism is `shared/ui/.../Presets.h`
Source/Advice.h      the notes of the strip under the controls and the GUIDE text; plain C++
Source/Parameters.h  parameter layout, readParams()                       <- the single source of truth for parameters
Source/Plugin*.{h,cpp}, Source/UI/   JUCE wrapper and editor (the look is `shared/ui`)
Tests/               Tests.cpp (DSP), UiCheck.cpp (editor on the real processor), Snapshot.cpp (editor -> PNG), Check.h
```

## Rules that keep it correct

**Split-band is a subtraction, not a filter in the signal path.** `y = x - (1 - gain) x band(x)`: the band-pass (constant 0 dB at the centre) is the same one the detector uses, so the band is turned down
by exactly the gain at its centre and the rest is not touched, and at a gain of exactly 1 the product `0 x band` is exactly 0 and `y == x` bit for bit even when the compiler fuses multiplies and adds.
A notch or a shelf in the path at 0 dB would not be exact (that is the lesson of Hestia's carver). Do not replace it by a dynamic filter.

**Look-ahead delays the sound and its band, not the detector.** The detector's band-pass reads what is arriving; the sound and that band are written into a ring (`Channel::ringSound`, `ringBand`) and what is heard is `delaySamples` back, so `y = heard - (1 - gain) x heardBand` is still the subtraction above (the delayed band is the band of the delayed sound).
With `delaySamples == 0` the ring returns the input, so the engine is bit-identical to the one without. A change of the setting clears the rings (a short gap) and moves `latencySamples`, which the processor hands to the host on the message thread (`handleAsyncUpdate`). The list is `kLookaheadMs`, in the order of `lookaheadNames`.

**DELTA is the third output branch** (`y = (1 - gain) x heardBand` in split-band, `heard x (1 - gain)` in wideband, ahead of LISTEN): it is exactly what the other branches take off, so output + DELTA is the delayed input. **FIND** (`startFind`, `findCount`, `foundHz`) is a bank of `kFindBins` band-passes that only runs for `kFindMs` after a request, on the mono sum, fixed arrays, no allocation; the editor's FIND button (enabled only with LISTEN on) sets FREQ from `foundHz` when `findCount` moves.

**Bypass.** The plugin reports latency (the look-ahead), so `processBlockBypassed` delays the main bus by `getLatencySamples()` with `pdsp::BypassDelay` (prepared for the longest look-ahead); UiCheck pins it.

**Presets** are `ui::Preset`s in `Presets.h`; the processor's `ui::Programs` applies them (every parameter back to its default first) and saves the chosen index with the session.

**One gain for both channels.** The louder band envelope decides; the gain is applied to both. Do not detect per channel: a sibilant panned to one side would move.

**The envelope has a 2 ms fall** (`kEnvelopeFallMs`): the band's output swings through zero every half cycle, and a detector fed with `|band|` per sample made the follower ripple. Do not remove it.

**At rest the gain is exactly 1** (`kRestDb`): a reduction under it, with nothing to reduce, is set to exactly 0, and `gr == 0` gives a gain of exactly 1.

**RELATIVE** subtracts the broadband level (30 ms mean square of the input, `x 2` for the peak of a sine) from the band level in dB, so a sound 30 dB quieter has the same relative level (test [6]).

**The coefficients move, the states stay.** `setBandpass` sets the coefficients only; FREQ and WIDTH are read per block and applied at the start of it (the band-pass has no audible zipper at the rates a knob turns, and a
block-size dependence only exists while a knob moves).

**Real time.** `Engine::process` allocates and locks nothing. **Bad samples:** NaN and infinity are replaced by 0 on the way in.

**Parameters.** Ids (`nemesis::id`) are stored in sessions and automation: never rename or remove one. All are `ParameterID` version 1 except `lookahead`, which is version 2 (a session saved before it opens with it Off), and `delta`, version 3 (off); a new one needs a higher version, an entry in `readParams()` and a place in the editor.
The order of `modeNames` and `detectionNames` is the order of `Mode` and `Detection`.

## DRY: where each thing lives

- the law -> `reductionDb()` in `Engine.h` (the engine, the tests); the fixed times -> the constants at the top of `Engine.h`
- parameter ranges, defaults, choice lists -> `Parameters.h` (the editor reads the items from the parameters)
- what the plugin warns about and what GUIDE says -> `adviceFor()` and `guideText()` in `Source/Advice.h`. A note restates a limit the README's Limits documents: a new one needs it there, a test in UiCheck's notes
  section, and at most `ui::kMaxNoteChars` characters
- the plugin's name -> `NEMESIS_PLUGIN_NAME` in `CMakeLists.txt`; the nameplate's two words -> `UI/NemesisUi.h` (`UiCheck` checks they agree)
- every pixel of the interface -> `shared/ui`; the editor calls it and does not draw its own controls

## Gotchas already paid for

- `ui::Choice` and `ui::Toggle` are bound to one parameter for good.
- In an editor constructor call `setSize()` **last**.
- `createEditor()` returns a `ui::ScaledEditor<NemesisEditor>`, not the editor: tests and snapshots reach it with `ui::innerOf<NemesisEditor> (*processor.createEditor())`.
- A test of the absolute threshold needs a level well under it (-12 dB), not one just under: the 6 dB knee already takes 0.1 dB off at 2 dB under.

## Conventions

As in the other plugins: Allman braces, a space before call parentheses, `juce::` qualified in headers; comments explain *why*; claims in the README are backed by a test; commit by topic and end commit messages with the
Co-Authored-By trailer.
