# Hermes Level

A **gain-staging and loudness tool** (VST3 + AU + Standalone) for mixing and mastering: a **trim** and a polarity, **listening checks** (mono, one channel, the sides, swapped), **loudness meters** as ITU-R BS.1770-4 and
EBU R128 define them (momentary, short-term, integrated, loudness range) with a **true-peak** meter, and a **reference track** to compare with, **level-matched** to your sound so you judge the tone and not the level.
It changes nothing but the level and what you listen to.

## Name origin

*Hermes* is the messenger and the god of measures and exchanges: he carries a thing from one place to another and says what it weighs. (CMake targets, namespaces and plugin codes use the short name "Hermes".)

## What it does

| | |
|---|---|
| **Trim** | -24 to +24 dB, 0 by default; **Polarity** inverts both channels. The meters read the sound after them. |
| **Listen** | *Stereo* (as it is), *Mono* (`(L + R) / 2` in both: what a mono speaker plays), *Left* and *Right* (one channel in both), *Side* (`(L - R) / 2` in both: what differs between the channels), *Swap*. A check: the meters still read the whole sound. |
| **Meters** | **Momentary** (400 ms) and **short-term** (3 s) loudness in LUFS; **integrated** loudness (gated, since RESET); **range** in LU; the **true peak** in dBTP and the sample peak in dBFS; and a graph of the last minute. |
| **Reference** | **LOAD** a track (WAV, AIFF, FLAC, OGG, MP3 where the system can read it; at most 10 minutes, kept in memory), **REF (B)** to hear it instead of your sound (A) with a 20 ms crossfade, **LEVEL MATCH** to bring it to the integrated loudness of your sound, **REF TRIM** (+-12 dB) to add to that, **RESTART**. It loops, and plays on while you listen to A. The file's path is kept in the session. |

## How it works

1. **K-weighting**, as BS.1770: a high shelf of +4 dB around 1.68 kHz and a 2nd-order high-pass at 38 Hz, made for the session's sample rate from the standard's analog prototypes by the bilinear transform.
2. **Blocks.** The weighted energy of both channels (weight 1.0 each) is summed over 100 ms sub-blocks. **Momentary** is the mean of the last four (400 ms, 75% overlap), **short-term** of the last thirty (3 s).
3. **Integrated.** The 400 ms blocks over -70 LUFS (the absolute gate) are kept in a histogram of 0.1 LU bins (count and power); the mean power of those sets the relative gate, 10 LU under it; the integrated loudness is the mean
   power of the blocks over that gate. **Range:** the same on the 3 s values, a relative gate of 20 LU, and the span from the 10th to the 95th percentile.
4. **True peak** is read by `shared/dsp` TruePeak.h (an 8x, 64-tap windowed-sinc reader, the one Aegis detects with), on the sound after the trim.
5. **The reference** is decoded whole, brought to the session's sample rate (Lagrange interpolation), made stereo if it is mono, and its integrated loudness measured by the same meter. LEVEL MATCH reads the sound's integrated
   loudness every 64 samples and sets the reference's gain to `sound - reference` (at most 24 dB), smoothed over 5 ms.

## Measured (`HermesTests`, 48 kHz)

- A stereo 1 kHz sine at -23 dBFS reads -23.04 LUFS integrated, momentary and short-term (EBU Tech 3341: -23.0 +-0.1); the same within 0.038 LU at 44.1, 88.2, 96 and 192 kHz.
- The K-weighting follows the analog prototypes of the standard within 0.006 dB at 30, 100, 500 Hz, 1 and 4 kHz (-1.18 dB at 100 Hz, +3.92 dB at 4 kHz).
- Gating: 10 s at -23, 5 s at -36 and 10 s at -23 read -23.10 LUFS; 10 s of sound and 10 s of silence read -23.10 (the blocks half over the edge are counted, as the standard has them).
- A step from silence to -20 dBFS: after 0.5 s the momentary reads -20.04 and the short-term -24.8; after 3 s the short-term reads -20.04.
- Loudness range: 20 s at -20 and 20 s at -30 read 10.00 LU (EBU Tech 3342: 10 +-1); a steady tone reads under 0.3 LU.
- True peak: a tone whose samples read -9.03 dBFS and whose true peak is -6.02 dBTP is read -5.93; band-limited noise 0.23 against 0.33 dBTP from a separate 128-tap reader: within 0.1 dB.
- TRIM +6 dB is +6.00 dB; POLARITY, MONO, LEFT, RIGHT, SIDE and SWAP are exact (the formulas above, bit for bit); with nothing set the output is the input, bit for bit, with no delay.
- The reference: a -14 dBFS sine reads -14.04 LUFS; with LEVEL MATCH the reference and a -26.7 LUFS sound are heard at the same loudness (the reference brought -12.7 dB); without it the reference is heard as it is (-14.04 LUFS);
  REF TRIM +3 dB adds exactly +3.00 dB. Switching A and B four times in a tone gives no step over what the tone has (0.0131 against 0.0131): a crossfade, no click.
- The same output for blocks of 1, 7, 64, 333, 512 and 4096 samples, bit for bit, with a reference playing. NaN and infinity in the input are silence.

## Limits

- **The meters are not a limiter.** They say what to do (the true peak over -1 dBTP, the loudness over -9 LUFS); they do not do it. Use Aegis for the peak.
- **The integrated loudness needs time**, and LEVEL MATCH needs the integrated loudness: until the meter has read a few seconds of sound there is nothing to match to (the reference is then heard as it is).
  Press RESET at the start of a new passage.
- **The true peak is read at 8x** within 0.1 dB of a separate reader; BS.1770 asks for 4x with 0.2 dB. It is the peak of the sound after the trim, and not of what is listened to.
- **LISTEN and REF are parameters and are saved with the session**: set LISTEN back to Stereo and REF off before you bounce (a note reminds you).
- **A reference is kept whole in memory** (at most 10 minutes; about 230 MB at 48 kHz). It is brought to the sample rate the session had when it was loaded; if the session's rate changes it is made again from the file.
- **It reads the file again from its path** when a session is opened: if the file has moved, the session opens without a reference.
- It has not been built on macOS or run through `auval`, and it has no `HostCheck` (see "Not yet in line" in the repository's [CLAUDE.md](../../CLAUDE.md)).

## Building

Requires CMake 3.22 or newer and a C++20 compiler. JUCE is fetched by CMake (see [cmake/Juce.cmake](../../cmake/Juce.cmake)).

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --target Hermes_VST3 Hermes_AU Hermes_Standalone -j 10
```

Building does **not** install the plugins; add `-DHERMES_INSTALL_AFTER_BUILD=ON` for that (macOS).

## Development

```bash
cmake --build build --target HermesTests && ./build/HermesTests_artefacts/Release/HermesTests            # DSP: no plugin, no GUI
cmake --build build --target HermesUiCheck && ./build/HermesUiCheck_artefacts/Release/HermesUiCheck      # the editor on the real processor
cmake --build build --target HermesSnapshot && ./build/HermesSnapshot_artefacts/Release/HermesSnapshot docs/screenshot.png
```

`docs/screenshot.png` is the output of `HermesSnapshot` (arguments in the file's header), never edited by hand. See
[CLAUDE.md](CLAUDE.md) for the rules that keep the plugin correct.

## Licence

AGPL-3.0-or-later, source only. See [LICENSE](../../LICENSE), [NOTICE.md](../../NOTICE.md) (third-party software, trademarks,
the source-only policy), [CONTRIBUTING.md](../../CONTRIBUTING.md) and [SECURITY.md](../../SECURITY.md).
