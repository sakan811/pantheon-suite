# CLAUDE.md

Harmonia Pitch: a JUCE pitch corrector for one solo vocal (VST3 / AU / Standalone): YIN detection, a corrector that decides the target note and smooths toward it, PSOLA re-synthesis that keeps the formants, key and scale
(or custom notes), key detection and Auto key. See `README.md` for what it does and what has been verified; this file is what you need to change it safely. It was imported as it was, so it differs from the newer plugins
in places that are listed below as such: do not "fix" them in passing.

## Build and verify

Harmonia has its own scripts (macOS, zsh, Xcode's own clang and SDK: they work when `xcodebuild` itself cannot start); `scripts/build.sh` at the repository root also works on an existing build folder.

```bash
scripts/build_tests.sh                    # the offline DSP tools with no JUCE (tune_wav, dsp_test, pitch_track, stress_corrector) in build-tools/, then runs dsp_test
scripts/build.sh                          # configure + build VST3, AU and Standalone, install them for the current user, ad-hoc sign them
cmake --build build --target HarmoniaDevTool
build/HarmoniaDevTool_artefacts/Release/HarmoniaDevTool selftest                        # the real processor and editor, headless
build/HarmoniaDevTool_artefacts/Release/HarmoniaDevTool snapshot docs/screenshot.png    # the editor to a PNG
```

- **Building the plugin targets installs them** (`COPY_PLUGIN_AFTER_BUILD TRUE`, unconditional: listed under "Not yet in line" in the repository's CLAUDE.md), and `scripts/build.sh` ad-hoc signs the copies (Apple Silicon will not load an unsigned plug-in).
- `dsp_test` is the DSP suite (sections 1 to 9, printed with their titles); `HarmoniaDevTool selftest` is the processor, the state, the key detection, Auto key, the notes and the editor. The tools in `build-tools/` are for listening and for comparing, not for CI.
- Definition of done for a DSP change: `dsp_test` passes **and** the numbers the README quotes (accuracy in cents, vibrato kept, formant ratio, CPU) still hold. For a UI or parameter change: `selftest` passes and `docs/screenshot.png` is regenerated and looked at.
- There is no workflow for Harmonia and no `UiCheck`; the checks above are run by hand.

## Layout

```
Source/dsp/AutoTuneEngine.h   the engine: detect -> decide + smooth -> PSOLA; EngineParams, VoiceRange, HopInfo history for the display, key learning; fixed look-ahead
Source/dsp/PitchDetector.h    YIN on a low-passed copy of the voice, searched at about 24 kHz and refined at the full rate
Source/dsp/PitchCorrector.h   the target note, Speed / Strength / Natural / Note Hold, in cents from C zero; called once per analysis hop
Source/dsp/KeyTracker.h       Auto key: a pitch-class histogram that forgets in seconds of singing, matched to major and minor profiles
Source/dsp/Scales.h           note masks (12 bits, bit 0 = C), the scale table, key names
Source/dsp/Kernels.h, Rings.h SIMD-friendly sums (kLanes independent partial sums), the mirror ring buffer
Source/PluginProcessor.*      parameters (ids::), host glue, the key-learn handshake with the editor
Source/PluginEditor.*, Source/ui/   the editor; the look is `shared/ui`; PitchGraph and NoteKeyboard are the plugin's own
Source/Advice.h               the notes of the strip under the controls and the GUIDE text; plain C++
Tests/                        dsp_test, tune_wav, pitch_track, stress_corrector, DevTool (selftest, snapshot, process a WAV), TestSignals.h (a synthetic voice), WavIO.h
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
The editor's keyboard, the engine and the notes all read it; do not rebuild a mask elsewhere.

**Key detection is a handshake.** The editor sets `learnCommand` (1 start, 2 finish); `processBlock` acts on it and publishes a result only if about a second of confident singing was heard (`learnedAmount() >= 150`); the editor takes it once with `takeKeyResult`. Nothing here blocks or allocates.

**Voice range follows the tuning.** `VoiceRange` limits are for A4 = 440 Hz and move with the Tuning knob (`setVoiceRange (index, refA4)`); each top sits a whole tone above the usual top note so that note still tracks with a wide vibrato (dsp_test 7b4). A detection more than an octave away from where the voice just was is a glitch and
passes unless it persists.

**Real time.** `AutoTuneEngine::process` allocates and locks nothing; everything is sized in `prepare()`. The editor reads atomics and the history ring only.

**Parameters.** Ids (`ids::`, `note0` ... `note11`) are stored in sessions and automation: never rename or remove one, nor reorder the choice lists (key names, `kScales`). All are `ParameterID` version 1; a new one needs a higher version, a place in `readParams()` and in the editor.
`bypass` is the host's bypass parameter (`getBypassParameter()`).

## Where each thing lives

- what the plugin warns about and what GUIDE says -> `adviceFor()` and `guideText()` in `Source/Advice.h`. A note restates a limit the README's Limitations (or Features) documents: a new one needs it there, a check in `DevTool selftest`, and at most `ui::kMaxNoteChars` characters
- the plugin's name -> `HARMONIA_PLUGIN_NAME` in `CMakeLists.txt` (the nameplate's two words are in `Source/ui/HarmoniaUi.h`; `selftest` checks they agree)
- the file map in the README's "Development" section: update it when a source file is added, renamed or removed
- every pixel of the interface -> `shared/ui`; the editor calls it and does not draw its own controls (the pitch graph and the keyboard are drawn on `ui::hw` glass with the kit's colours)

## Gotchas already paid for

- **Not a `ui::ScaledEditor`.** Harmonia's editor scales itself (`kDesignW` x `kDesignH` drawn in a `content` component and scaled by a transform, from 75% to 200%, fixed aspect), as `ScaledEditor` does for the other plugins; it is the one the kit's was modelled on. Do not wrap it in `ScaledEditor` and do not change its size without changing `kDesignW`/`kDesignH` and regenerating the screenshot.
- In an editor constructor call `setSize()` **last**.
- The default `/usr/bin/clang` can link against a broken Command Line Tools SDK: that is why the scripts source `scripts/toolchain.sh` and point everything at Xcode's SDK (`DEVELOPER_DIR`, `SDKROOT`). A plain `cmake` on a fresh folder may need the same.
- The build is a universal binary (arm64 and x86_64, deployment target 11.0) so the plug-in loads whether the host runs natively or under Rosetta.
- `Tests/` has the synthetic voice (`TestSignals.h`): vibrato, drift, formants. A test on a real voice is a listening test; `tune_wav` and `pitch_track` are the tools for it.

## Conventions

As in the other plugins: Allman braces, a space before call parentheses, `juce::` qualified in headers; comments explain *why*; claims in the README are backed by a test; commit by topic and end commit messages with the
Co-Authored-By trailer.
