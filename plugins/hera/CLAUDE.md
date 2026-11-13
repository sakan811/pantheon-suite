# CLAUDE.md

Hera Bond: a JUCE compressor (VST3 / AU / Standalone): peak or RMS detector, soft knee, auto release and makeup, parallel mix, stereo link, sidechain with a detector high-pass. See `README.md` for what it does and what has been measured; this file is what you need to change it safely.

## Build and verify

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release     # JUCE: version in cmake/Juce.cmake, or third_party/JUCE at the repository root
cmake --build build --target HeraTests -j 10 && ./build/HeraTests_artefacts/Release/HeraTests
cmake --build build --target Hera_VST3 -j 10
cmake --build build --target HeraUiCheck && ./build/HeraUiCheck_artefacts/Release/HeraUiCheck
cmake --build build --target HeraSnapshot && ./build/HeraSnapshot_artefacts/Release/HeraSnapshot docs/screenshot.png
```

- Building the plugin targets does **not** install them (`HERA_INSTALL_AFTER_BUILD`, off by default; a build folder configured with it ON does: check the cache). `scripts/build.sh hera` is the way to build and check (see the repository's CLAUDE.md).
- `HeraTests`, `UiCheck` and `Snapshot` are `EXCLUDE_FROM_ALL`; build them by name. `Source/DSP` has no JUCE in it, so the DSP tests also compile with
  `g++ -std=c++20 -O2 -I../../shared/dsp/include -ISource -ITests Tests/Tests.cpp` (a quick loop); run them also with `-O3 -march=native` and with `-O3 -ffast-math`, each on its own.
- Definition of done for a DSP change: `HeraTests` passes **and** the numbers the README quotes still match its output.
- Definition of done for a UI or parameter change: `UiCheck` passes and `docs/screenshot.png` is regenerated and looked at.

## Layout

```
Source/DSP/Engine.h  no GUI, no JUCE: reductionDb() (the gain computer), autoMakeupDb(), Engine (the detector, the followers, makeup, mix); covered by Tests/Tests.cpp
Source/Presets.h     the presets (settings, names, one-line purposes); the mechanism is `shared/ui/.../Presets.h`
Source/Advice.h      the notes of the strip under the controls and the GUIDE text; plain C++
Source/Parameters.h  parameter layout, readParams()                       <- the single source of truth for parameters
Source/Plugin*.{h,cpp}, Source/UI/   JUCE wrapper and editor (the look is `shared/ui`)
Tests/               Tests.cpp (DSP), UiCheck.cpp (editor on the real processor), Snapshot.cpp (editor -> PNG), Check.h
```

## Rules that keep it correct

**The gain follows the reduction in dB**, not the level: `wanted = reductionDb (level)`, one-pole attack when `wanted > gr`, release otherwise, and the gain is `exp (-gr x ln10/20)`. A peak detector that
fed the gain computer directly (per-sample `|x|`) read 0.5 to 1.5 dB less reduction than the law on a 1 kHz sine, because the gain rippled with the waveform: the peak detector is an envelope with a 15 ms
fall (`kPeakFallMs`) and an instant rise. Do not make it faster without redoing test [1].

**At rest the gain is exactly 1.** A reduction under `kRestDb` with nothing to reduce is set to exactly 0, and `gr == 0` gives a gain of exactly 1 (not `exp (0)` times a rounding). MIX at 100% takes the compressed
sound itself (`pure`), not `dry + mix x (wet - dry)`. That is what makes the idle compressor bit-exact (tests [2]); keep both.

**AUTO release** is two followers with the same attack but the slow one 20 times slower to go down (`kSlowAttackFactor`), and the gain is the larger of the two. A short burst only moves the fast one far, a long passage
moves both. The release times are fixed (`kFastAutoReleaseMs`, `kSlowAutoReleaseMs`); RELEASE is not used.

**LINK** is `own + link x (louder - own)` on the linear detector levels, not on the gains: at 100% both channels see the same level and get the same gain exactly.

**The sidechain** is read only when KEY is on and the host connected one (`keyL != nullptr`); a mono key is given with `keyR == nullptr`. The detector's high-pass is on the key, whichever it is. The key is never heard.
The bus is the processor's second input, off by default, and `sidechainConnected` is what the editor's note reads.

**Look-ahead delays the sound, not the detector.** `Engine::process` writes each input sample into a ring (`Channel::ring`) and reads the sound that is heard `delaySamples` back; the detector reads what is arriving (or the sidechain). `dry` (what MIX blends, and what the gain is applied to) is the delayed one, so a blend does not comb. With
`delaySamples == 0` the ring returns the input itself, so the engine is bit-identical to the one without look-ahead. A change of the setting clears the ring (a short gap) and moves `latencySamples`, which the processor hands to the host on the message thread (`handleAsyncUpdate`, as Apollo does). The list is `kLookaheadMs`;
its order is the order of `lookaheadNames`, the parameter's choice.

**Presets** are `ui::Preset`s in `Presets.h`; the processor's `ui::Programs` applies them (every parameter back to its default first) and saves the chosen index with the session. A new parameter needs to be thought about in each preset (it goes back to its default unless the preset lists it).

**Smoothing.** Makeup and mix are smoothed per sample (5 ms); everything else is per sample by nature, so the output does not depend on the host's block size (test [8], bit for bit). The first block starts them at their targets.

**Real time.** `Engine::process` allocates and locks nothing. **Bad samples:** NaN and infinity are replaced by 0 on the way in.

**Parameters.** Ids (`hera::id`) are stored in sessions and automation: never rename or remove one. All are `ParameterID` version 1 except `lookahead`, which is version 2 (a session saved before it opens with it Off); a new one needs a higher version, an entry in `readParams()` and a place in the editor.
The order of `detectorNames` is the order of `Detector`.

## DRY: where each thing lives

- the law -> `reductionDb()` in `Engine.h` (the engine, the editor's transfer curve, the tests); what AUTO makeup adds -> `autoMakeupDb()`
- parameter ranges, defaults, choice lists -> `Parameters.h` (the editor reads the items from the parameters)
- what the plugin warns about and what GUIDE says -> `adviceFor()` and `guideText()` in `Source/Advice.h`. A note restates a limit the README's Limits documents: a new one needs it there, a test in UiCheck's notes
  section, and at most `ui::kMaxNoteChars` characters
- the plugin's name -> `HERA_PLUGIN_NAME` in `CMakeLists.txt`; the nameplate's two words -> `UI/HeraUi.h` (`UiCheck` checks they agree)
- every pixel of the interface -> `shared/ui`; the editor calls it and does not draw its own controls

## Gotchas already paid for

- `ui::Choice` and `ui::Toggle` are bound to one parameter for good.
- In an editor constructor call `setSize()` **last**.
- `createEditor()` returns a `ui::ScaledEditor<HeraEditor>`, not the editor: tests and snapshots reach it with `ui::innerOf<HeraEditor> (*processor.createEditor())`.
- Measuring a reduction on a tone: read the peak over a whole period (1 ms for 1 kHz), not one sample; and measure the release on a quiet tone that follows (under the threshold), because the output of silence says nothing about the gain.

## Conventions

As in the other plugins: Allman braces, a space before call parentheses, `juce::` qualified in headers; comments explain *why*; claims in the README are backed by a test; commit by topic and end commit messages with the
Co-Authored-By trailer.
