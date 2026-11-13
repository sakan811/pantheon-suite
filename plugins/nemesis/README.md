# Nemesis Hush

A **de-esser** (VST3 + AU + Standalone) for vocals and anything else with sibilance: it listens to a band around the s, and when that band is louder than a threshold it turns it down,
by up to a range. It turns down **only the band** (split-band) or the whole sound (wideband), can compare the band with the whole sound (relative detection) so one setting works for
quiet and loud passages, and can play you the band it listens to, so you can find the s.

## Name origin

*Nemesis* is the goddess of what is out of measure being brought back to it. (CMake targets, namespaces and plugin codes use the short name "Nemesis".)

## What it does

| | |
|---|---|
| **Freq** | 2 to 12 kHz, 6.5 kHz by default: the centre of the sibilance. Male voices sit at 4 to 7 kHz, female at 6 to 10. |
| **Width** | Q 0.5 to 5, 1.5 by default: how wide the band is (higher is narrower). |
| **Thresh** | -60 to 0 dB, -30 by default. *Absolute:* the level of the band (peak of a sine, dBFS) above which it is turned down. *Relative:* how far under the whole sound the band may be. |
| **Range** | 0 to 24 dB, 8 by default: the most it takes off. |
| **Release** | 5 to 300 ms, 40 by default. The attack is fixed at 0.3 ms. |
| **Mode** | *Split-band:* only the band is turned down (the rest of the voice is not touched). *Wideband:* the whole sound is turned down. |
| **Detection** | *Absolute* or *Relative* (see Thresh). |
| **Listen** | Hear the band the detector hears. Monitoring: switch it off before a bounce. |
| **Look-ahead** | Off (default), 0.5, 1, 2 or 5 ms: the detector hears the sound as it arrives and the sound is heard that long after, so the first of a loud s is already turned down. It is a delay (the host compensates). |
| **Presets** | Seven starting points (Init, Male Vocal, Female Vocal, Gentle Touch, Hard Sibilance, Broadcast Voice, Find the S). A preset sets every control; what it does not list goes back to its default. Find the s with LISTEN and move FREQ onto it first. |

The display shows the response of the band the detector hears and the reduction of the last eight seconds.

## How it works

1. **Detect.** Each channel goes through a constant-0-dB band-pass at FREQ and WIDTH (an RBJ biquad). Its peak envelope takes a peak at once and lets it fall over 2 ms (so the gain does not ripple
   with the waveform); the louder channel decides, so both channels get one gain and the image does not move.
2. **Level.** *Absolute:* the envelope in dB. *Relative:* the envelope in dB minus the level of the whole sound (a 30 ms mean square, read as the peak of a sine).
3. **Reduction.** Infinite ratio: it holds the band at the threshold, with a 6 dB soft knee, up to RANGE. A one-pole follower (0.3 ms attack, RELEASE) smooths it; at rest (under 0.001 dB with nothing to reduce) it is let go to exactly 0.
4. **Apply.** *Split-band:* `output = input - (1 - gain) x band`, so the band is turned down by the gain and the rest is not touched, and with a gain of exactly 1 the output is exactly the input. *Wideband:* `input x gain`.
   LISTEN outputs the band.

## Measured (`NemesisTests`, 48 kHz)

- A sibilant (a 6.5 kHz tone) at -20 dBFS over a -30 dB threshold is turned down by 9.95 dB (the law: 10, it holds the band at the threshold); RANGE 6 dB stops the reduction at 6.00 dB.
- Split-band: with a vowel (300 Hz, -12 dBFS) and a sibilant (6.5 kHz, -16 dBFS) the vowel changes by -0.00 dB and the sibilant by -12.0 dB (RANGE 12). Wideband: both by -12.0 dB.
- A tone at 1.5 kHz as loud as the sibilant is not touched (-0.000 dB): the detector listens to the band.
- LISTEN: the 6.5 kHz tone passes (-0.00 dB) and the 300 Hz tone is 31 dB down.
- Under the threshold the output is the input, bit for bit, in both modes; half a second after a loud sibilant the gain is back at exactly 1 and a quiet sound is the input, bit for bit.
- RELEASE 50 ms: 14.8 dB at the end of a one-second sibilant, 38% of it (5.6 dB) 50 ms later, and gone after a second.
- Relative detection: the same sound 30 dB quieter is turned down by the same amount (5.09 dB both). Absolute: 18.10 dB for the loud one and +0.000 dB for the quiet one.
- LOOK-AHEAD 1 ms: with nothing to turn down the output is the input 48 samples later, bit for bit; in the first 0.3 ms of a -10 dBFS burst (wideband) the output peaks at -11.0 dBFS without it and -21.7 dBFS with it; a change of the setting while running moves the latency (240 samples for 5 ms).
- A loud left and a quiet right get the same gain. The same output for blocks of 1, 7, 64, 333, 512 and 4096 samples, bit for bit. NaN and infinity in the input are silence.

## Limits

- **With LOOK-AHEAD off** the first 0.3 ms of a very loud s gets through before the attack closes; with it on the gain is already down (in split-band the band-pass itself takes about 70 microseconds to answer, so the very first cycle of a burst is still not turned down). LOOK-AHEAD delays the sound by its time (the host compensates); changing it while playing restarts the de-esser (a short gap).
- **Taking off 10 dB or more makes a lisp.** Raise THRESH or lower RANGE. In wideband mode the whole voice ducks with the s.
- **One band.** It cannot follow a voice whose s moves much in frequency; widen the band (a low Q) or use two instances. It is not a dynamic equaliser (Iris is).
- **The detector reads the band, not "the s":** a cymbal or a hi-hat in the band is turned down too. Use it on the voice alone.
- It builds on macOS (VST3 and AU, run by hand on 2026-11-13) and `auval -v` passes for the AU; it is not in a macOS CI job, and it has no `HostCheck` (see "Not yet in line" in the repository's [CLAUDE.md](../../CLAUDE.md)).

## Building

Requires CMake 3.22 or newer and a C++20 compiler. JUCE is fetched by CMake (see [cmake/Juce.cmake](../../cmake/Juce.cmake)).

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --target Nemesis_VST3 Nemesis_AU Nemesis_Standalone -j 10
```

Building does **not** install the plugins; add `-DNEMESIS_INSTALL_AFTER_BUILD=ON` for that (macOS).

## Development

```bash
cmake --build build --target NemesisTests && ./build/NemesisTests_artefacts/Release/NemesisTests            # DSP: no plugin, no GUI
cmake --build build --target NemesisUiCheck && ./build/NemesisUiCheck_artefacts/Release/NemesisUiCheck      # the editor on the real processor
cmake --build build --target NemesisSnapshot && ./build/NemesisSnapshot_artefacts/Release/NemesisSnapshot docs/screenshot.png
```

`docs/screenshot.png` is the output of `NemesisSnapshot` (arguments in the file's header), never edited by hand. See
[CLAUDE.md](CLAUDE.md) for the rules that keep the plugin correct.

## Licence

AGPL-3.0-or-later, source only. See [LICENSE](../../LICENSE), [NOTICE.md](../../NOTICE.md) (third-party software, trademarks,
the source-only policy), [CONTRIBUTING.md](../../CONTRIBUTING.md) and [SECURITY.md](../../SECURITY.md).
