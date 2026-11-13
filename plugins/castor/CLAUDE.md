# CLAUDE.md

Castor Pair: a JUCE stereo imager (VST3 / AU / Standalone): the width of the sides in the six bands of music, mid and side level, mono bass, balance, a goniometer and a correlation meter. See `README.md` for what it does and what has been measured; this file is what you need to change it safely.

## Build and verify

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release     # JUCE: version in cmake/Juce.cmake, or third_party/JUCE at the repository root
cmake --build build --target CastorTests -j 10 && ./build/CastorTests_artefacts/Release/CastorTests
cmake --build build --target Castor_VST3 -j 10
cmake --build build --target CastorUiCheck && ./build/CastorUiCheck_artefacts/Release/CastorUiCheck
cmake --build build --target CastorSnapshot && ./build/CastorSnapshot_artefacts/Release/CastorSnapshot docs/screenshot.png
```

- Building the plugin targets does **not** install them (`CASTOR_INSTALL_AFTER_BUILD`, off by default; a build folder configured with it ON does: check the cache). `scripts/build.sh castor` is the way to build and check (see the repository's CLAUDE.md).
- `CastorTests`, `UiCheck` and `Snapshot` are `EXCLUDE_FROM_ALL`; build them by name. `Source/DSP` has no JUCE in it, so the DSP tests also compile with
  `g++ -std=c++20 -O2 -I../../shared/dsp/include -ISource -ITests Tests/Tests.cpp` (a quick loop); run them also with `-O3 -march=native` and with `-O3 -ffast-math`, each on its own.
- Definition of done for a DSP change: `CastorTests` passes **and** the numbers the README quotes still match its output.
- Definition of done for a UI or parameter change: `UiCheck` passes and `docs/screenshot.png` is regenerated and looked at.

## Layout

```
Source/DSP/Engine.h  no GUI, no JUCE: the width of each band (pdsp::BandSplitter), the crossfade, mid / side / mono below / balance / output, the correlation; covered by Tests/Tests.cpp
Source/Presets.h     the presets (settings, names, one-line purposes); the mechanism is `shared/ui/.../Presets.h`
Source/Advice.h      the notes of the strip under the controls and the GUIDE text; plain C++
Source/Parameters.h  parameter layout, readParams()                       <- the single source of truth for parameters
Source/Plugin*.{h,cpp}, Source/UI/   JUCE wrapper and editor (the look is `shared/ui`); the processor feeds `tapOut` (a `pdsp::StereoTap`) for the goniometer
../../shared/dsp/    MusicBands.h (the six bands and `pdsp::BandSplitter`)
Tests/               Tests.cpp (DSP), UiCheck.cpp (editor on the real processor), Snapshot.cpp (editor -> PNG), Check.h
```

## Rules that keep it correct

**The splitter's bands are in phase, and so they sum to an all-pass of the sound, not to the sound.** An earlier version split only the side by differences of low-passes (`LP_k - LP_k-1`, which adds
up exactly to the side) and added each band's `(width - 1) x band` to the input. It was bit-exact when neutral but wrong when not: a difference of two low-passes is not a band-pass in phase with the
signal, so a band at 0% *raised* the side by 2 to 4 dB near its edges (measured). Do not go back to subtractive bands. With `pdsp::BandSplitter` the sum of the bands has the magnitude of the input (test [2]:
all at 200% is +6.02 dB everywhere), at the price of the all-pass phase, which is why the split is not used while every width is exactly 100%.

**`engaged` is the crossfade between the sound as it is and the split.** It goes to 1 when a width target or the current width is off 100% and back to 0 when both are at rest at 100%; the widths are smoothed
per sample (10 ms) and snapped to their target within 1e-9, so "100%" is exactly 1.0 and `engaged` reaches exactly 0. The splitter runs all the time (its states must be warm); only the choice of what is heard changes.
With `engaged == 0` nothing touches the sample: `l = l0`.

**The rest is differences and gains that are exactly neutral.** `dM = (midGain - 1) x m`, `dS = (sideGain - 1) x s`, and MONO BELOW `dS = HP (s + dS) - s` (only while it is on; the filter is reset while it is off): zero when
neutral, so `l = l0 + 0 + 0`. BALANCE and OUTPUT multiply by `leftGain x outGain`, exactly 1.0 at rest. Never rebuild left and right from the mid and the side (`m + s` is not `l` to the last bit).

**Real time.** `Engine::process` allocates and locks nothing. The goniometer is fed by the processor through the shared lock-free `pdsp::StereoTap`; nothing of it runs on the audio thread except the push.
**Bad samples:** NaN and infinity are replaced by 0 on the way in.

**Presets** are `ui::Preset`s in `Source/Presets.h` (the settings as real values, a name, a one-line purpose of at most 140 characters); the processor's `ui::Programs` applies them (every parameter that the plugin's `keepsParameter` does not claim goes back to its default first, so a preset does not inherit the last one) and keeps the chosen index in the session's state
(`program`); the editor's `ui::PresetBox` lists them and `sync()`s with the host. A new parameter needs thinking about in each preset: it goes back to its default unless the preset lists it. UiCheck pins that every preset applies, what one specific preset sets, what is kept, and that the box fits the header.

**Parameters.** Ids (`castor::id`: `width1` ... `width6`, `mid`, `side`, `mono`, `balance`, `out`) are stored in sessions and automation: never rename or remove one. All are `ParameterID` version 1; a new one
needs a higher version, an entry in `readParams()` and a place in the editor. The width ids follow the order of `pdsp::bands::kBands`.

## DRY: where each thing lives

- the six bands, their names, edges and the splitter -> `shared/dsp` MusicBands.h (shared: run the other plugins' tests too); the width knobs take their names and tooltips from it
- parameter ranges, defaults -> `Parameters.h`
- what the plugin warns about and what GUIDE says -> `adviceFor()` and `guideText()` in `Source/Advice.h`. A note restates a limit the README's Limits documents: a new one needs it there, a test in UiCheck's notes
  section, and at most `ui::kMaxNoteChars` characters
- the plugin's name -> `CASTOR_PLUGIN_NAME` in `CMakeLists.txt`; the nameplate's two words -> `UI/CastorUi.h` (`UiCheck` checks they agree)
- every pixel of the interface -> `shared/ui`; the editor calls it and does not draw its own controls (the goniometer is drawn on `ui::hw` glass)

## Gotchas already paid for

- In an editor constructor call `setSize()` **last**.
- `createEditor()` returns a `ui::ScaledEditor<CastorEditor>`, not the editor: tests and snapshots reach it with `ui::innerOf<CastorEditor> (*processor.createEditor())`. A snapshot has to call `tickDisplay()` for the goniometer to get samples.

## Conventions

As in the other plugins: Allman braces, a space before call parentheses, `juce::` qualified in headers; comments explain *why*; claims in the README are backed by a test; commit by topic and end commit messages with the
Co-Authored-By trailer.
