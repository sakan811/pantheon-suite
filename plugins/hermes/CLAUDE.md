# CLAUDE.md

Hermes Level: a JUCE gain-staging and loudness tool (VST3 / AU / Standalone): trim, polarity, listening checks, BS.1770 / R128 loudness and true-peak meters, a level-matched reference A/B. See `README.md` for what it does and what has been measured; this file is what you need to change it safely.

## Build and verify

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release     # JUCE: version in cmake/Juce.cmake, or third_party/JUCE at the repository root
cmake --build build --target HermesTests -j 10 && ./build/HermesTests_artefacts/Release/HermesTests
cmake --build build --target Hermes_VST3 -j 10
cmake --build build --target HermesUiCheck && ./build/HermesUiCheck_artefacts/Release/HermesUiCheck
cmake --build build --target HermesSnapshot && ./build/HermesSnapshot_artefacts/Release/HermesSnapshot docs/screenshot.png
```

- Building the plugin targets does **not** install them (`HERMES_INSTALL_AFTER_BUILD`, off by default; a build folder configured with it ON does: check the cache). `scripts/build.sh hermes` is the way to build and check (see the repository's CLAUDE.md).
- `HermesTests`, `UiCheck` and `Snapshot` are `EXCLUDE_FROM_ALL`; build them by name. `Source/DSP` has no JUCE in it, so the DSP tests also compile with
  `g++ -std=c++20 -O2 -I../../shared/dsp/include -ISource -ITests Tests/Tests.cpp` (a quick loop); run them also with `-O3 -march=native` and with `-O3 -ffast-math`, each on its own.
- Definition of done for a DSP change: `HermesTests` passes **and** the numbers the README quotes still match its output.
- Definition of done for a UI or parameter change: `UiCheck` passes and `docs/screenshot.png` is regenerated and looked at.

## Layout

```
Source/DSP/Loudness.h  no GUI, no JUCE: K-weighting for any rate, LoudnessMeter (momentary, short-term, integrated, range from fixed histograms)
Source/DSP/Engine.h    the trim, the listening modes, the meters, the reference (Reference, the A/B crossfade, the level match); covered by Tests/Tests.cpp
../../shared/dsp/      TruePeak.h (`pdsp::TruePeakReader`: the 8x, 64-tap reader; shared with Aegis, so run AegisTests too when you change it)
Source/Presets.h       the presets (settings, names, one-line purposes); the mechanism is `shared/ui/.../Presets.h`
Source/Advice.h        the notes of the strip under the controls and the GUIDE text; plain C++
Source/Parameters.h    parameter layout, readParams()                       <- the single source of truth for parameters
Source/Plugin*.{h,cpp}, Source/UI/   JUCE wrapper (it also decodes the reference file) and editor (the look is `shared/ui`)
Tests/                 Tests.cpp (DSP), UiCheck.cpp (editor on the real processor; it writes WAV files and loads them), Snapshot.cpp (editor -> PNG), Check.h
```

## Rules that keep it correct

**The meters are the standard's, and a test says so.** K-weighting is made from the analog prototypes (`kWeighting`), not from the 48 kHz table, so that it is right at every rate; test [1] compares the loudness of sines with the
analog response at five frequencies (0.006 dB), and with EBU Tech 3341 / 3342 cases. Do not "improve" the filters or the offset (-0.691). The 400 ms blocks overlap by 75%: a sub-block is 100 ms and a gating block is the last four.

**Fixed memory.** The integrated loudness and the range read histograms of 801 bins (0.1 LU from -70 to +10 LUFS) holding the count and the *power* of each bin, so the mean is exact and the gates are in 0.1 LU steps. Nothing grows with
the length of the programme, so the meter runs on the audio thread, and the same class measures a whole file on the message thread (`measureIntegrated`).

**Meters read the sound after the trim and before what is listened to.** LISTEN and the reference change what is heard, never what is measured (test [3]); the reference's loudness is measured separately when it is loaded.

**The reference crosses to the audio thread by a pointer.** The editor's thread (`HermesProcessor::loadReference`) builds a `Reference`, keeps the last two alive in `loaded` and gives the engine a raw pointer (`setReference`, an atomic
store with release); the audio thread reads it with acquire once per block. Deleting the one before the previous is safe because a block never lasts that long. Do not free the previous one at once. The restart of the position
and a seek (`seekReference`, a fraction) are flags the audio thread takes (`restart`, `seekTo`), never a write to `position` from another thread; because the pointer and the flag are not one atomic, `process` also wraps `position` into the reference it just loaded (a shorter one swapped in must not be read at the old position).

**The match is read at ticks.** The reference's gain follows the sound's integrated loudness, read every `kTick` = 64 samples counted from the start, not every block: that is what makes the output the same for any block size
(test [5], with a reference playing). Anything that reads a meter inside `process` has to be read at a tick.

**A to B is a crossfade** (`listenMix`, 20 ms, snapped at its ends so A is exactly the sound). The reference advances whether it is heard or not and loops with `position`; a file that loops with a step in it clicks at the loop.

**Idle is the input.** Trim 1.0 exactly, no polarity, Stereo, A: `l = x x 1.0` and the output is the input, bit for bit (test [3]). The trim is smoothed per sample and starts at its target.

**State.** The reference's path is a property of the state (`refPath`), not a parameter; a state with no `refPath`, or whose file is gone, clears the reference (`clearReference`); a processor given a state with a file loads it again with `callAsync` through a `WeakReference` (the processor may be gone by then), and
`prepareToPlay` loads it again if the sample rate changed.

**Real time.** `Engine::process` allocates and locks nothing (the histograms are fixed arrays). **Bad samples:** NaN and infinity are replaced by 0 on the way in.

**Presets** are `ui::Preset`s in `Source/Presets.h` (the settings as real values, a name, a one-line purpose of at most 140 characters); the processor's `ui::Programs` applies them (every parameter that the plugin's `keepsParameter` does not claim goes back to its default first, so a preset does not inherit the last one) and keeps the chosen index in the session's state
(`program`); the editor's `ui::PresetBox` lists them and `sync()`s with the host. A new parameter needs thinking about in each preset: it goes back to its default unless the preset lists it. UiCheck pins that every preset applies, what one specific preset sets, what is kept, and that the box fits the header.

**Parameters.** Ids (`hermes::id`) are stored in sessions and automation: never rename or remove one. All are `ParameterID` version 1 except `reset_play` (AUTO RESET, version 2, off: the processor clears the meters on the rising edge of the host's `isPlaying`; `keepsParameter` keeps it through presets); a new one needs a higher version, an entry in `readParams()` and a place in the editor.
The order of `listenNames` is the order of `Listen`.

## DRY: where each thing lives

- loudness and K-weighting -> `Loudness.h`; the true peak -> `shared/dsp` TruePeak.h
- parameter ranges, defaults, choice lists -> `Parameters.h` (the editor reads the items from the parameters)
- what the plugin warns about and what GUIDE says -> `adviceFor()` and `guideText()` in `Source/Advice.h`. A note restates a limit the README's Limits documents: a new one needs it there, a test in UiCheck's notes
  section, and at most `ui::kMaxNoteChars` characters
- the plugin's name -> `HERMES_PLUGIN_NAME` in `CMakeLists.txt`; the nameplate's two words -> `UI/HermesUi.h` (`UiCheck` checks they agree)
- every pixel of the interface -> `shared/ui`; the editor calls it and does not draw its own controls (the readings and the graph are drawn on `ui::hw` glass)

## Gotchas already paid for

- A test helper that takes the signal by value and returns it processed has to have its result used (`auto out = run (...)`): two tests of the reference measured the input and passed for the wrong reason until they were made to fail.
- Loading a reference from a test needs a `juce::AudioFormatManager` with `registerBasicFormats()` (the processor has one); the audio formats module is in the plugin's and the checks' link lines.
- `ui::Choice` and `ui::Toggle` are bound to one parameter for good. In an editor constructor call `setSize()` **last**.
- `createEditor()` returns a `ui::ScaledEditor<HermesEditor>`, not the editor: tests and snapshots reach it with `ui::innerOf<HermesEditor> (*processor.createEditor())`.

## Conventions

As in the other plugins: Allman braces, a space before call parentheses, `juce::` qualified in headers; comments explain *why*; claims in the README are backed by a test; commit by topic and end commit messages with the
Co-Authored-By trailer.
