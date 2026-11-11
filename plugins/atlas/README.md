# Atlas Span

A **six-band compressor** (VST3 + AU + Standalone) for mixing and mastering. The bands are the six main bands of music
(Sub, Bass, Low-Mid, Midrange, Upper-Mid, High), the same ones the other plugins of this repository solo by, and their edges
can be moved. Each band has its own compressor, working on **Stereo**, **L/R**, **M/S**, or on Left, Right, Mid or Side alone;
its own **SOLO** and **DELTA**; and its own **gain MATCH**. The split is **Zero Latency** or **Linear** phase, and the
compressors can run **oversampled** (2x, 4x).

## Name origin

*Atlas* is the Titan who carries the sky on his shoulders: he holds the weight wherever it presses. A *Span* is what he holds
up, from one end to the other, here the spectrum from the sub-bass to the air. (CMake targets, namespaces and plugin codes use
the short name "Atlas".)

![Atlas Span](docs/screenshot.png)

*Headless render of the editor (the hardware-style interface shared with the other plugins) over a programme with a beat. Five
bands are soloed (the Sub is dimmed), the Midrange is on DELTA, the Bass works on M/S and the High band has MATCH on. Regenerate
with the command under "Development".*

## What it does

| | |
|---|---|
| **Six bands** | Sub 20–60 Hz, Bass 60–250 Hz, Low-Mid 250–500 Hz, Midrange 500 Hz–2 kHz, Upper-Mid 2–6 kHz, High 6–20 kHz by default. **XOVER** on a strip moves its upper edge (40 Hz to 16 kHz; the edges keep their order, at least a ratio of 1.2 apart). |
| **Per band** | **ON**, **THRESH**, **RATIO**, **ATTACK**, **RELEASE**, **MAKEUP**, and a meter of the dB taken off. The knee is 6 dB wide. A band that is off passes as it is. |
| **Channels** | **Stereo**: left and right with one gain (the louder decides), so the image does not move. **L/R**: left and right, each with its own gain. **M/S**: the centre and the sides, each with its own gain. **Left / Right / Mid / Side**: that one only; the rest of the band is untouched. |
| **SOLO** | Hear the band alone, as it comes out. Click more to hear them together; any combination adds up flat. Not saved with the session. |
| **DELTA** | Hear only what that band's compressor changes: its output minus its input. Not saved with the session. **ALL BANDS** ends every solo and delta. |
| **MATCH** | Brings the band back to the loudness it had before its compressor (a 0.4 s window, never more than 12 dB), so the band is not judged by being quieter. MAKEUP adds to it. |
| **Phase** | **Zero Latency**: Linkwitz-Riley crossovers (24 dB per octave), no delay; the bands add to the input with its phase turned around each edge, as any such crossover does. **Linear**: the same bands with no phase shift, so the bands add to the input itself, only later (64 ms at 48 kHz, reported to the host). |
| **Oversampling** | Off, 2x, 4x: the compressors' gain is made and applied at that rate, so a fast gain on high notes does not fold back as false tones. Adds 49 / 61 samples of delay at 48 kHz, reported to the host. |

## How it works

```
input -> split into six bands -> per band:  level -> gain computer -> attack / release -> band + band x (gain - 1)
      -> makeup, gain match -> solo / delta -> sum -> output gain
```

- **The level** of a band is its mean square, smoothed over about a cycle of the band's lowest frequency and read as the peak
  of a sine of the same power, so "-20 dB" means a sine whose peak is at -20 dBFS and the threshold means the same for any
  attack. That is also the limit on how fast a band can react: a short click is read lower than its peak, and the Sub band
  cannot move inside a cycle of 20 Hz, which is what keeps it from distorting the bass.
- **Attack and release act on the gain**, in dB, after the level: the times you set are the times it takes. With 100 ms the gain is
  3.0 dB of an 18 dB reduction 15 ms in (a single-pole curve gives 2.5); with a 400 ms release it is at -1.87 dB 900 ms after
  the signal stops (the curve gives -1.89).
- **A band adds its difference.** What the compressor takes off (`band x (gain - 1)`) is added to the band, so a band whose
  gain is 1 is its input bit for bit, and only the difference goes through the oversampling filters: with nothing compressing,
  oversampling is a delay and nothing else.
- **A tone is in more than one band.** A 1 kHz tone is 1.08 dB down in the Midrange band and the rest of it is in the
  neighbours, so taking the band down 14.2 dB takes the whole output down 10.8 dB. That is how any multiband compressor behaves;
  SOLO shows the band's own gain.

## Measured (`AtlasTests`, 48 kHz)

- **Split.** Zero latency, every band off: flat within 0.0000 dB from 30 Hz to 12 kHz. Linear, every band off: the input
  3072 samples (64 ms) later, within -135 dB. Both modes cut at the same places: the Midrange soloed gives -1.08 dB at 1 kHz,
  -6.06 dB at its 2 kHz edge, and less than -50 dB at 8 kHz and 125 Hz. A click through a linear band comes out symmetric.
- **Compressor.** At -30 dB and 4:1 a tone at -11.08 dB in the band is taken down 14.21 dB (the law gives 14.19); under the
  threshold the output is unchanged bit for bit. Attack and release as quoted above.
- **Channels.** Stereo: -14.21 / -14.21 dB with the tone loud on the left only. L/R: -14.21 / 0.000 dB. Left: the right
  untouched. Mid: the centre -14.21 dB, the side +0.0000 dB; M/S: each its own gain.
- **Solo and delta.** Three bands soloed plus the other three is the plain output; input + delta = output (both within 6e-8);
  after a solo is released the output is the plain output again, bit for bit.
- **Gain match.** A band taken down 10.5 dB gets +10.46 dB and the output is within 0.00 dB of the input; never more than 12 dB.
- **Oversampling.** With the bands off, 2x and 4x are the plain output 49 and 61 samples later, bit for bit. An 11 kHz tone
  compressed with the fastest times: the harmonics that fold back to 15 and 7 kHz are 89 / 92 dB under the tone without
  oversampling, 134 / 136 dB at 2x, 150 / 142 dB at 4x.
- **Robust.** The output is the same, bit for bit, for host blocks of 512, 377 and 1 samples in every mode; an inf or a NaN in the
  input does not get through; it runs at 44.1, 96 and 192 kHz; the modes can be switched while playing.

## Limits

- No look-ahead, no sidechain input, no presets, and the times are not automatic.
- The level is not a peak reading (see above): this is not a limiter.
- Linear mode resolves a crossover down to 40 Hz, which is why XOVER stops there. Its FFT is single precision: the floor is about -135 dB.
- Changing Phase or Oversampling while playing restarts the bands from silence (a short gap) and changes the latency.
- Not yet built on macOS, run through `auval` or loaded in a host by a check tool; there is no `HostCheck` yet. CI builds the VST3 on Linux.

## Building

Requires CMake 3.22 or newer and a C++20 compiler. JUCE is fetched by CMake (see [cmake/Juce.cmake](../../cmake/Juce.cmake)).

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --target Atlas_VST3 Atlas_AU Atlas_Standalone -j 10
```

Building does **not** install the plugins; add `-DATLAS_INSTALL_AFTER_BUILD=ON` for that (macOS).

## Development

```bash
cmake --build build --target AtlasTests && ./build/AtlasTests_artefacts/Release/AtlasTests            # DSP: no plugin, no GUI
cmake --build build --target AtlasUiCheck && ./build/AtlasUiCheck_artefacts/Release/AtlasUiCheck      # the editor on the real processor
cmake --build build --target AtlasSnapshot && ./build/AtlasSnapshot_artefacts/Release/AtlasSnapshot docs/screenshot.png solo=62 delta=8 os=1
```

`docs/screenshot.png` is the output of `AtlasSnapshot` (arguments in the file's header), never edited by hand. See
[CLAUDE.md](CLAUDE.md) for the rules that keep the plugin correct.

## Licence

AGPL-3.0-or-later, source only. See [LICENSE](../../LICENSE), [NOTICE.md](../../NOTICE.md) (third-party software, trademarks,
the source-only policy), [CONTRIBUTING.md](../../CONTRIBUTING.md) and [SECURITY.md](../../SECURITY.md).
