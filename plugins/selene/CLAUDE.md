# CLAUDE.md

Selene Poly: a 16-voice polyphonic analog-style synthesizer (VST3 / AU / Standalone) voiced for City Pop: dual oscillators with PWM, -1 octave sub, 2-operator FM, ZDF SVF filter, dual ADSRs, global LFO, BBD stereo chorus, and stereo tape echo. See `README.md` for what it does and what has been measured; this file owns what keeps it safe to change.

## Build and verify

Build, test and snapshot commands live in [README.md](README.md) (`## Build & develop`); shared defaults and `scripts/build.sh` live in [../../CLAUDE.md](../../CLAUDE.md) (`## Building and checking`, `## Module conventions & defaults`).

- `Source/DSP/Engine.h` has no JUCE in it; `SeleneTests` tests it directly.

## Layout

```
Source/DSP/Engine.h     Plain C++20 16-voice poly/mono/unison synth engine, filterMagnitudeAt(), no JUCE, no allocations in process()
Source/Parameters.h     38 parameter IDs, readParams()
Source/Presets.h        24 City Pop presets
Source/Plugin*.{h,cpp}  JUCE synth AudioProcessor wrapper (0 inputs, stereo output, MIDI in) and 1280x800 editor
```

## Rules that keep it correct

- **MIDI handling.** `SeleneProcessor::processBlock` converts JUCE MIDI messages into a fixed stack array (`std::array<selene::MidiEvent, 512>`) without allocating, and passes `keyboardState.processNextMidiBuffer` first so the editor's audition keyboard plays alongside host MIDI.
- **Synth buses.** No input bus, stereo output only (`isBusesLayoutSupported` rejects active input buses and non-stereo outputs). `processBlockBypassed` clears the output buffer.
- **Parameters.** The order of `voiceModeNames`, `oscWaveNames`, `filterModeNames`, `lfoWaveNames`, and `chorusModeNames` must match the engine enums in `Engine.h`.
- **Editor size.** `SeleneEditor` has fixed design size `1280 x 800` (`ui::editorHeightFor(1280)`).
