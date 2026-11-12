# Apollo Curve

A **parametric equaliser** (VST3 + AU + Standalone) for mixing and mastering: eight bands, each a bell, a low or high shelf, a notch, or a high or low-pass of 12 to
48 dB per octave, each working on the whole sound, only the middle, or only the sides; and a choice between the usual **minimum-phase** equaliser and a
**linear-phase** one that shifts no phase at all. You drag the bands on a display of the curve.

## Name origin

*Apollo* is the god of music and of harmony: of things in proportion. (CMake targets, namespaces and plugin codes use the short name "Apollo".)

## What it does

| | |
|---|---|
| **Bands** | Eight. A new instance opens with a high-pass and a low-pass at the ends (off), two shelves and four bells, all at 0 dB, so dragging a node does something at once. |
| **Type** | Bell (raises or cuts around a frequency), Low shelf, High shelf, High-pass, Low-pass, Notch. |
| **Freq / Gain / Q** | 20 Hz to 20 kHz; -18 to +18 dB (bells and shelves); Q 0.1 to 10 (the width of a bell or notch, the turn of a shelf). |
| **Slope** | For a high-pass or low-pass: 12, 24, 36 or 48 dB per octave (Butterworth; -3 dB at the corner). |
| **Channels** | Stereo (left and right alike), Mid (only what both channels share) or Side (only what differs between them). |
| **Phase** | Minimum (no delay) or Linear (no phase shift; about 48 ms of delay, and a softer low end). |
| **Output** | -24 to +24 dB after every band. |

On the display, drag a node sideways for the frequency and up and down for the gain; the wheel over a node changes the Q; a double click switches the band
on or off. The amber curve is the response of the settings; a teal one appears when a band works on the sides only (it is the sides' response). The curve is the
filter that runs, not a drawing of it.

## How it works

1. **Minimum phase.** Each band is a biquad (an RBJ design) or a Butterworth cascade, in double precision, run in order. A Mid or Side band transforms the sound to
   mid and side, filters one of them, and adds the *difference* back, so what it does not touch is not touched at all.
2. **Smoothing.** Everything that changes with time steps every 64 samples, counted from the start: the frequency (in the log domain), the gain and the Q of each band
   are moved toward the knob by a one-pole of 20 ms, and the coefficients are redesigned (the states stay). A band switched off or taken to 0 dB fades first, then
   is no longer run.
3. **A band that does nothing is not run.** Off, or a bell or shelf under 0.005 dB: it is not in the chain, so the sound is the input bit for bit even in a build that fuses
   multiplies and adds (a bell "at 0 dB" is the identity only on paper).
4. **Linear phase.** The magnitude of all the bands that reach the mid (and, separately, the side) is sampled on a grid of 4096 points (at 48 kHz; the frame scales with the
   sample rate so the resolution is the same 12 Hz), made into a real, symmetric impulse response with an inverse FFT, windowed (Hann, 3585 taps) and run by FFT overlap-save
   with a hop of 512. Left and right are run as mid and side, so a Stereo band, which is in both, treats the channels alike. The kernel is remade at most once a hop.
   With no band running the sound is only delayed, exactly (the same delay, an integer delay line), so the idle plug-in is bit-exact in this mode too.

## Measured (`ApolloTests`, 48 kHz)

- The bands: for each of a bell +9 dB, a bell -12 dB (Q 3), a low shelf +6, a high shelf -6, a 24 dB/oct high-pass, a 48 dB/oct low-pass and a notch, a tone's gain at seven frequencies is the
  response `responseDb` draws, to 0.000 dB. A +9 dB bell at 1 kHz is +9.00 dB there and under 0.5 dB at 100 Hz; a 24 dB/oct high-pass at 100 Hz is -3 dB at its corner and about -24 dB an octave below.
- Eight bells at 0 dB: the input, bit for bit (minimum phase), and the input delayed by 2304 samples, bit for bit (linear phase).
- Linear phase: the same magnitude as minimum phase within 0.007 dB from 100 Hz to 12 kHz for four bands in use; at 50 Hz, with a 24 dB/oct high-pass at 40 Hz and a shelf in it, 0.59 dB off (the kernel
  resolves about 12 Hz). An impulse comes out symmetric around its centre to 1e-5: no phase shift. Latency, as reported to the host: 2304 samples (48.0 ms at 48 kHz: 1792 for the kernel and 512 for the hop), and 48.0 to 52.2 ms at the other rates.
- Mid and Side: a Mid bell of +9 dB lifts a centred sound by 9.00 dB and leaves a side-only sound within 0.1 dB; a Side bell the reverse; a Stereo bell lifts a left-only sound by 9 dB too, in both phase modes.
- The same output for blocks of 1, 7, 64, 333, 512 and 4096 samples, bit for bit, in both phase modes.
- A +12 dB bell switched off while a 1 kHz tone plays: the largest sample-to-sample step is 0.1297 against 0.1299 for the steady tone, so there is no click; afterwards the tone is back at its level (+0.001 dB).
- NaN and infinity in the input are silence. OUTPUT +6 dB with every band off is +6.00 dB.

## Limits

- **Linear phase resolves about 12 Hz** (4096 points at 48 kHz). A band below about 60 Hz, or a steep high-pass there, is softer than the curve shows. Use minimum phase down there.
- **Linear phase delays the sound** by about 48 ms (the host compensates it); switching phase mode while playing restarts the equaliser (it clicks). It also has a little pre-ringing before a sharp boost: that is what linear phase is.
- **A narrow, strong bell rings in minimum phase.** That is the filter, not a fault; cut instead of boosting, or use linear phase.
- **It is static.** It does what the curve says, always; there is no analyser and no dynamics (Iris is the dynamic equaliser).
- It has not been built on macOS or run through `auval`, and it has no `HostCheck` (see "Not yet in line" in the repository's [CLAUDE.md](../../CLAUDE.md)).

## Building

Requires CMake 3.22 or newer and a C++20 compiler. JUCE is fetched by CMake (see [cmake/Juce.cmake](../../cmake/Juce.cmake)).

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --target Apollo_VST3 Apollo_AU Apollo_Standalone -j 10
```

Building does **not** install the plugins; add `-DAPOLLO_INSTALL_AFTER_BUILD=ON` for that (macOS).

## Development

```bash
cmake --build build --target ApolloTests && ./build/ApolloTests_artefacts/Release/ApolloTests            # DSP: no plugin, no GUI
cmake --build build --target ApolloUiCheck && ./build/ApolloUiCheck_artefacts/Release/ApolloUiCheck      # the editor on the real processor
cmake --build build --target ApolloSnapshot && ./build/ApolloSnapshot_artefacts/Release/ApolloSnapshot docs/screenshot.png
```

`docs/screenshot.png` is the output of `ApolloSnapshot` (arguments in the file's header), never edited by hand. See
[CLAUDE.md](CLAUDE.md) for the rules that keep the plugin correct.

## Licence

AGPL-3.0-or-later, source only. See [LICENSE](../../LICENSE), [NOTICE.md](../../NOTICE.md) (third-party software, trademarks,
the source-only policy), [CONTRIBUTING.md](../../CONTRIBUTING.md) and [SECURITY.md](../../SECURITY.md).
