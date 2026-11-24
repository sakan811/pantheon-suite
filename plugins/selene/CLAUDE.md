# CLAUDE.md

Selene Poly: a 16-voice polyphonic analog-style synthesizer (VST3 / AU / Standalone) voiced for City Pop: dual oscillators with PWM, -1 octave sub, 2-operator FM, ZDF SVF filter, dual ADSRs, global LFO, BBD stereo chorus, and stereo tape echo. See `README.md` for what it does and what has been measured; this file owns what keeps it safe to change.

## Build and verify

```bash
scripts/build.sh selene check snap
```

Or from `plugins/selene/`:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --target SeleneTests -j 10 && ./build/SeleneTests_artefacts/Release/SeleneTests
cmake --build build --target Selene_VST3 -j 10
cmake --build build --target SeleneUiCheck && ./build/SeleneUiCheck_artefacts/Release/SeleneUiCheck
cmake --build build --target SeleneSnapshot && ./build/SeleneSnapshot_artefacts/Release/SeleneSnapshot docs/screenshot.png
```

- `SELENE_INSTALL_AFTER_BUILD` defaults to `OFF`.
- `Source/DSP/Engine.h` has no JUCE in it; `SeleneTests` tests it directly.
- Definition of done for a DSP change: `SeleneTests` passes and the measured numbers in `README.md` match.
- Definition of done for a UI or parameter change: `SeleneUiCheck` passes and `docs/screenshot.png` is regenerated and inspected.

## Layout

```
Source/DSP/Engine.h     Plain C++20 16-voice poly/mono/unison synth engine, filterMagnitudeAt(), no JUCE, no allocations in process()
Source/Parameters.h     Single source of truth: 38 parameter IDs, ranges, defaults, choice lists, readParams()
Source/Presets.h        24 City Pop presets (ui::Preset)
Source/Advice.h         Plain C++ adviceFor() notes (<= ui::kMaxNoteChars) and guideText()
Source/UI/SeleneUi.h    Nameplate strings and display colours
Source/Plugin*.{h,cpp}  JUCE synth AudioProcessor wrapper (0 inputs, stereo output, MIDI in) and 1280x800 hardware editor
Tests/                  Tests.cpp (DSP), UiCheck.cpp (processor + editor), Snapshot.cpp (editor -> PNG), Check.h
```

## Rules that keep it correct

- **Real time.** `Engine::process` and `SeleneProcessor::processBlock` allocate and lock nothing. `SeleneProcessor::processBlock` converts JUCE MIDI messages into a fixed stack array (`std::array<selene::MidiEvent, 512>`) and passes `keyboardState.processNextMidiBuffer` first so the editor's audition keyboard plays alongside host MIDI.
- **Synth buses.** No input bus, stereo output only (`isBusesLayoutSupported` rejects active input buses and non-stereo outputs). `processBlockBypassed` clears the output buffer.
- **Parameters.** IDs in `selene::id` are stored in sessions and automation: never rename or remove one. The order of `voiceModeNames`, `oscWaveNames`, `filterModeNames`, `lfoWaveNames`, and `chorusModeNames` must match the engine enums in `Engine.h`.
- **UI kit.** Every knob, selector, preset box, undo/redo, A/B, module plate, glass display, and note strip comes from `shared/ui`. `SeleneEditor` has fixed design size `1280 x 800` (`ui::editorHeightFor(1280)`), wrapped in `ui::ScaledEditor<SeleneEditor>`. Call `setSize()` **last** in the editor constructor.

## Conventions

Allman braces, a space before call parentheses, `juce::` qualified in headers; comments explain *why*.
