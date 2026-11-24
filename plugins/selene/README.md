# Selene Poly

A 16-voice **polyphonic analog-style synthesizer** (VST3 + AU + Standalone) voiced for 80s City Pop: warm classic 80s BBD chorus pads, brass stabs, glassy 2-operator FM electric pianos, slap synth bass, and tape-echo leads.

## Name origin

*Selene* is the Greek goddess of the moon — fitting for late-night City Pop chords and shimmering twilight pads. (CMake targets, namespaces, and plugin codes use the short name "Selene".)

![Selene Poly](docs/screenshot.png)

*Headless render of the editor (`SeleneSnapshot`) playing an Fmaj9 chord on the "Tokyo Night Pad" preset: filter frequency response with envelope peak modulation on the left, triggered oscilloscope and active voice readout on the right, three hardware module rows (Oscillators, Filter, Dual ADSR, LFO, Analog, Chorus & Echo), and a 2-octave clickable audition keyboard.*

## What it does

- **Voice Architecture**: 16-voice Poly, Mono (last-note priority with legato glide), and 4-voice Unison (stereo spread, up to 4 notes).
- **Dual Oscillators + Sub + Noise**: PolyBLEP Saw and Pulse (with PWM), PolyBLAMP Triangle, and Sine; -1 octave Sub oscillator; Osc 2 semitone (`-24..+24`), fine detune (`-50..+50 ct`), and linear FM onto Osc 1; white noise generator.
- **ZDF State-Variable Filter**: 24 dB/oct LP, 12 dB/oct LP, 12 dB/oct BP, and 12 dB/oct HP with pre-filter & stage saturation drive, bipolar filter envelope, keyboard tracking, and resonance up to near self-oscillation.
- **Modulation**: Dual ADSR envelopes (Amp and Filter) with per-envelope velocity sensitivity, plus a global LFO (Sine, Triangle, Square, Sample & Hold) routable to Pitch, Filter Cutoff, and PWM (plus MIDI Mod Wheel and Pitch Bend).
- **Effects**: Classic BBD stereo chorus (Off, Mode I, Mode II, I+II) and cross-fed stereo tape echo with feedback filtering and soft saturation.
- **Workflow**: 24 City Pop presets, A/B compare, parameter Undo/Redo, contextual Advice strip, Guide dialog, and a built-in 2-octave (C3–B4) audition keyboard.

## Building

Requires CMake 3.22 or newer and a C++20 compiler. JUCE is fetched by CMake (see [cmake/Juce.cmake](../../cmake/Juce.cmake)).

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --target Selene_VST3 Selene_AU Selene_Standalone -j 10
```

Building does **not** install the plugins; add `-DSELENE_INSTALL_AFTER_BUILD=ON` for that (macOS).

## Development

```bash
cmake --build build --target SeleneTests && ./build/SeleneTests_artefacts/Release/SeleneTests
cmake --build build --target SeleneUiCheck && ./build/SeleneUiCheck_artefacts/Release/SeleneUiCheck
cmake --build build --target SeleneSnapshot && ./build/SeleneSnapshot_artefacts/Release/SeleneSnapshot docs/screenshot.png
```

See [CLAUDE.md](CLAUDE.md) for the rules that keep the plugin safe to change.

## Licence

AGPL-3.0-or-later, source only. See [LICENSE](../../LICENSE), [NOTICE.md](../../NOTICE.md), [CONTRIBUTING.md](../../CONTRIBUTING.md), and [SECURITY.md](../../SECURITY.md).
