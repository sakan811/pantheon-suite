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

**AUTO release** is two followers and the gain is the larger of the two. The fast one has ATTACK and a fixed 50 ms release (`kFastAutoReleaseMs`); the slow one has an attack 20 times longer (`kSlowAttackFactor`) and a fixed 600 ms release
(`kSlowAutoReleaseMs`). A short burst only moves the fast one far, a long passage moves both. RELEASE is not used.

**HOLD** (`EngineParams::holdMs`, `Channel::hold`, plain release only) is a counter of samples: while `wanted < fast` and the counter is above 0 the follower does not move and the counter counts down; whenever `wanted >= fast` it is set back to the full hold. At 0 ms the counter is never above 0, so the follower is the one without a hold, bit for bit (tests [2], [8]). Not used with AUTO release.

**LINK** is `own + link x (louder - own)` on the linear detector levels, not on the gains: at 100% both channels see the same level and get the same gain exactly.

**The sidechain** is read only when KEY is on and the host connected one (`keyL != nullptr`); a mono key is given with `keyR == nullptr`. The detector's high-pass is on the key, whichever it is. The key is never heard.
The bus is the processor's second input, off by default, and `sidechainConnected` is what the editor's note reads.

**Look-ahead delays the sound, not the detector.** `Engine::process` writes each input sample into a ring (`Channel::ring`) and reads the sound that is heard `delaySamples` back; the detector reads what is arriving (or the sidechain). `dry` (what MIX blends, and what the gain is applied to) is the delayed one, so a blend does not comb. With
`delaySamples == 0` the ring returns the input itself, so the engine is bit-identical to the one without look-ahead. A change of the setting clears the ring (a short gap) and moves `latencySamples`, which the processor hands to the host on the message thread (`handleAsyncUpdate`, as Apollo does). The list is `kLookaheadMs`;
its order is the order of `lookaheadNames`, the parameter's choice.

**TOPOLOGY** (`EngineParams::topology`, a choice: Feed-forward is the default). Feedback's detector hears the previous output sample of its own channel, after the gain and before MAKEUP and MIX (`Channel::fb`, `dry x gain`), and only when no sidechain is connected: with KEY and a key connected the detector reads the key, as in feed-forward. Three rules follow, and the README's Limits says the same:
Feedback **ignores LOOK-AHEAD**: its `delaySamples` is 0, the latency reported is 0, and the setting is kept for feed-forward (`prepare` and `process` both take the topology). **Its law differs and is not compensated**: the steady state is `gr = reductionDb (level - gr)`, solved by `steadyFeedbackReductionDb`, which is `slope / (1 + slope)` of the overshoot (never half of it), and RATIO is not changed for it. Anything that asks what the law does takes the topology: the transfer curve, AUTO makeup (`autoMakeupDb`) and the AUTO THRESH answer (`topologyReductionDb`, `reductionAverageDb`, `autoThresholdFor`). A change of the topology restarts the delay ring like a look-ahead change.

**LEVEL MATCH, DELTA and SC LISTEN.** `match` is a parameter (`EngineParams::match`): the energies of the delayed input and of the output are followed per sample (0.6 s), the gain that equals them (within 12 dB) at 0.15 s, applied after MIX, exactly 1.0 when off (so the idle plug-in stays bit-exact). `Engine::delta` and `Engine::scListen` are atomics the editor sets, not parameters: never saved, ended by the editor's destructor, shown in the notes. SC LISTEN replaces the output with the key after the detector's high-pass (it wins over DELTA); DELTA is output minus the delayed input.

**Bypass keeps the latency.** `processBlockBypassed` delays the main bus by `getLatencySamples()` through `pdsp::BypassDelay` (prepared for `Engine::maxLatencySamples()`); `HeraUiCheck` pins it.

**Presets** are `ui::Preset`s in `Presets.h`; the processor's `ui::Programs` applies them (every parameter back to its default first, except what `keepsParameter` claims: LEVEL MATCH) and saves the chosen index with the session. A new parameter needs to be thought about in each preset (it goes back to its default unless the preset lists it).

**Undo and A/B.** The processor declares `juce::UndoManager undo` before `apvts` and hands it over (`&undo`), so every control change is one undo step; `setCurrentProgram` begins a step first, so a preset is one. `ab` (`ui::AbCompare`) is declared after `apvts`; its active slot is saved in the state as `abSlot`, and a session without it opens on slot 0. The two slots are not saved (a session opens with both empty). Both are the kit's (`Undo.h`, `AbCompare.h`).

**AUTO SETUP** (the button; the engine's listening is still called AUTO THRESH) is a one-shot measure, like LEARN in Atlas: the editor requests it (`autoThreshStart`), the audio thread listens to `kAutoListenSeconds` of frames and says `done`, the editor takes the answer and cancels. The histogram (`LevelHistogram`) is written only while listening, and read only once `done` is seen. The answer is the highest THRESH at which the law's reduction, averaged over the audible histogram, reaches the target, corrected by the ballistics the listening measured (the follower's mean reduction minus the law's mean at the same settings: a release tail is not the law). The crest factor (peak minus RMS of the same audible frames, `autoCrestDb`) gives RATIO, ATTACK and RELEASE through `autoSetupFor`, and THRESH is answered at that RATIO. Only the answer `set` moves anything: RATIO, ATTACK, RELEASE and THRESH, through one gesture each inside one undo step. The ballistics correction is measured at the listening's settings, so it is off when the press changes RATIO, ATTACK or RELEASE (README, Limits). It is monitoring state: not a parameter, never saved.

**Smoothing.** Makeup and mix are smoothed per sample (5 ms); everything else is per sample by nature, so the output does not depend on the host's block size (test [8], bit for bit). The first block starts them at their targets.

**Real time.** `Engine::process` allocates and locks nothing. **Bad samples:** NaN and infinity are replaced by 0 on the way in.

**Parameters.** Ids (`hera::id`) are stored in sessions and automation: never rename or remove one. All are `ParameterID` version 1 except `lookahead`, which is version 2 (a session saved before it opens with it Off), `match`, version 3 (off), `hold`, version 4 (0 ms), and `topology`, version 5 (Feed-forward); a new one needs a higher version, an entry in `readParams()` and a place in the editor.
The order of `detectorNames` is the order of `Detector`.

## DRY: where each thing lives

- the law -> `reductionDb()` in `Engine.h` (the engine, the editor's transfer curve, the tests); feedback's steady state -> `steadyFeedbackReductionDb()`, and the choice between them -> `topologyReductionDb()`; what AUTO makeup adds -> `autoMakeupDb()`; what AUTO THRESH answers -> `autoThresholdFor()` and `reductionAverageDb()` in `Engine.h`; the crest factor -> `crestFactorDb()`; what AUTO SETUP sets -> `autoSetupFor()` (all in `Engine.h`)
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
