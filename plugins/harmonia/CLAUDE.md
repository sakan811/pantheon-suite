# CLAUDE.md

Harmonia Pitch: a JUCE pitch corrector for one solo vocal (VST3 / AU / Standalone): YIN detection, a corrector that decides the target note and smooths toward it, PSOLA re-synthesis that keeps the formants, key and scale
(or custom notes), key detection and Auto key. See `README.md` for what it does and what has been verified; this file is what you need to change it safely. It was imported as it was, so it differs from the newer plugins
in places that are listed below as such: do not "fix" them in passing.

## Build and verify

Build, test and snapshot commands live in [README.md](README.md) (`## Build & develop`); shared defaults and `scripts/build.sh` live in [../../CLAUDE.md](../../CLAUDE.md) (`## Building and checking`, `## Module conventions & defaults`).

- Harmonia has its own scripts (`scripts/build_tests.sh` for the JUCE-free offline DSP tools `dsp_test`, `tune_wav`, `pitch_track`, `stress_corrector` in `build-tools/`, and `scripts/build.sh` which builds, installs unconditionally via `COPY_PLUGIN_AFTER_BUILD TRUE` — see `## Not yet in line` in [../../CLAUDE.md](../../CLAUDE.md) — and ad-hoc signs); `scripts/build.sh` at the repo root also works on an existing build folder.
- `HarmoniaDevTool` runs headless checks (`selftest`, `snapshot docs/screenshot.png`, `process`). There is no CI workflow and no `UiCheck`; `dsp_test` (sections 1 to 9) and `HarmoniaDevTool selftest` are run by hand.

## Layout

```
Source/dsp/AutoTuneEngine.h   the engine: detect -> decide + smooth -> PSOLA; EngineParams, VoiceRange, HopInfo history for the display, key learning; fixed look-ahead
Source/dsp/PitchDetector.h    YIN on a low-passed copy of the voice, searched at about 24 kHz and refined at the full rate
Source/dsp/PitchCorrector.h   the target note, Speed / Strength / Natural / Note Hold (and its opt-in LEAP HOLD), in cents from C zero; called once per analysis hop
Source/dsp/KeyTracker.h       Auto key: a pitch-class histogram that forgets in seconds of singing, matched to major and minor profiles
Source/dsp/Scales.h           note masks (12 bits, bit 0 = C), the scale table, key names
Source/dsp/Kernels.h, Rings.h SIMD-friendly sums (kLanes independent partial sums), the mirror ring buffer
Source/PluginProcessor.*      parameters (`ids::`), host glue, the key-learn handshake with the editor
Source/ui/                    `PitchGraph` and `NoteKeyboard` are the plugin's own (on `ui::hw` glass)
Tests/                        `dsp_test`, `tune_wav`, `pitch_track`, `stress_corrector`, `DevTool` (selftest, snapshot, process a WAV), `TestSignals.h` (synthetic voice), `WavIO.h`
```

## Rules that keep it correct

**The engine has no JUCE in it** and is C++17 (the plug-in layer is JUCE; the tools in `Tests/` compile `Source/dsp` alone). Keep it that way: `dsp_test` and `tune_wav` are how every change is measured.

**The latency is fixed and reported.** `AutoTuneEngine::latencySamples()` is `ceil (0.046 x fs)`: the pitch track is known that long before the audio is re-synthesised. The processor tells the host in `prepareToPlay`; it does not change with a setting. BYPASS
(`mix` 0) is the input delayed by that latency, not the input as it is, so the original stays aligned (`selftest`: "bypass = input delayed by reported latency"). Do not make bypass a pass-through.

**One analysis hop is `fs / 250` samples (at least 32), 4 ms.** The corrector, the key tracker and the display's history all run per hop; `hopRateHz()` is what the editor draws against. The history is a power-of-two ring (`kHistory` = 1024) read by the UI thread with `historyAt (index & (kHistory - 1))`.

**The output depends on the input only.** Not on the block size, and `reset()` brings a clean state (dsp_test 7d). The key tracker is deliberately **not** part of `reset()`: the key survives the host jumping around. The host's `reset()` only sets a flag; the engine is reset on the audio thread
in `processBlock`.

**An inf or a NaN in the input is silence** (dsp_test 9): one of them stays in the detector's filters or the corrector's smoothing for good, and the voice would never come back in tune. Do not take that check out of the input path.

**Formants stay because the grains are re-spaced, not resampled.** PSOLA keeps each grain's spectral envelope; Formant 0 is exactly that, and any other value is the only thing that moves the colour. Unvoiced sound (consonants, breath) passes through untouched.

**The note mask is the single truth for "which notes".** `HarmoniaProcessor::currentNoteMask()`: the custom note switches when Scale is Custom, else the scale of the key; with Auto key on, the key being followed (the parameters stay as they were and come back when it is switched off).
The editor's keyboard, the engine and the notes all read it; do not rebuild a mask elsewhere. While a MIDI note is held (`heldMask`, kept by `followMidi` on the audio thread, cleared by a host reset and by `prepareToPlay`) `currentNoteMask()` is the held notes' pitch classes; `keyboardNoteMask()` is what the keys, key and scale say without MIDI, and is what the keyboard buttons and the notes' advice read. The plug-in accepts MIDI (`NEEDS_MIDI_INPUT TRUE`) and stays an effect (`AU_MAIN_TYPE kAudioUnitType_Effect`: do not change it, a different type is a different plug-in to a host).

**Undo and A/B.** `ab` (`ui::AbCompare`) leaves the host's bypass out; the editor's own parameter writes (`setNotes`, `toggleNote`, the style buttons, a detected key) call `ui::beginUndoStep` first so each is one step. The editor's size is kept as `uiScale` in the state (read at the top of the constructor: `setResizeLimits` resizes the window and `resized()` writes the property).

**Key detection is a handshake.** The editor (message thread, the only writer) says what it wants (`learnWanted`) and bumps `learnSequence`; `processBlock` acts once per change, on the last wish, so a start and a stop between two blocks cannot cancel or reorder, and a stop with nothing started does nothing; it publishes a result only if about a second of confident singing was heard (`learnedAmount() >= 150`); the editor takes it once with `takeKeyResult`. Listening starts in `processBlock`, so with a host that is not running audio nothing starts: the editor waits a second after Detect, says the host is not running audio and withdraws the request (`blocksRun()` counts the blocks). Nothing here blocks or allocates.

**LEAP HOLD is opt-in and Off by default.** It keeps the Note Hold in full for a jump of a semitone or less from the held note and shrinks it between a semitone and a whole tone (`kLeapStartCents`, `kLeapCents`, `kLeapMinFraction` in `PitchCorrector.h`; a design choice, see the README's `### Limits`). A one-semitone step must behave exactly as with it Off. With it Off the corrector must stay bit-identical to the one before it existed: check any change to the hold path against that (dsp_test 7d3 pins the on/off numbers; compare the per-hop output with `leapHold` left at its default).

**Voice range follows the tuning.** `VoiceRange` limits are for A4 = 440 Hz and move with the Tuning knob (`setVoiceRange (index, refA4)`); each top sits a whole tone above the usual top note so that note still tracks with a wide vibrato (dsp_test 7b4). A detection more than an octave away from where the voice just was is a glitch and
passes unless it persists.

**Real time.** `AutoTuneEngine::process` allocates and locks nothing; everything is sized in `prepare()`. The editor reads atomics and the history ring only.

**Parameters.** `ids::`, `note0` ... `note11`; never reorder the choice lists (key names, `kScales`). All are `ParameterID` version 1 except `leap` (LEAP HOLD), which is version 2. `bypass` is the host's bypass parameter (`getBypassParameter()`).

## DRY: where each thing lives

- the file map in [README.md](README.md) (`## Build & develop`): update it when a source file is added, renamed or removed

## Gotchas already paid for

- **Not a `ui::ScaledEditor`.** Harmonia's editor scales itself (`kDesignW` x `kDesignH` drawn in a `content` component and scaled by a transform, from 75% to 200%, 16:10 like every plugin and app, with the kit's four corner handles `ui::ResizeGrips` in `grips`), as `ScaledEditor` does for the other plugins; it is the one the kit's was modelled on. Do not wrap it in `ScaledEditor` and do not change its size without changing `kDesignW`/`kDesignH` and regenerating the screenshot (`kDesignH` is `ui::editorHeightFor (kDesignW)`; the layout is absolute, drawn for 880 px and widened by `kWide`).
- The default `/usr/bin/clang` can link against a broken Command Line Tools SDK: that is why the scripts source `scripts/toolchain.sh` and point everything at Xcode's SDK (`DEVELOPER_DIR`, `SDKROOT`). A plain `cmake` on a fresh folder may need the same.
- The build is a universal binary (arm64 and x86_64, deployment target 11.0) so the plug-in loads whether the host runs natively or under Rosetta.
- `Tests/` has the synthetic voice (`TestSignals.h`): vibrato, drift, formants. A test on a real voice is a listening test; `tune_wav` and `pitch_track` are the tools for it.
