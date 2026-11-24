# CLAUDE.md

Hermes Level: a JUCE gain-staging and loudness tool (VST3 / AU / Standalone): trim, polarity, listening checks, BS.1770 / R128 loudness and true-peak meters, a level-matched reference A/B. See `README.md` for what it does and what has been measured; this file is what you need to change it safely.

## Build and verify

Build, test and snapshot commands live in [README.md](README.md) (`## Build & develop`); shared defaults and `scripts/build.sh` live in [../../CLAUDE.md](../../CLAUDE.md) (`## Building and checking`, `## Module conventions & defaults`).

- `Source/DSP` has no JUCE in it (supports standalone `g++` DSP tests).

## Layout

```
Source/DSP/Loudness.h  no GUI, no JUCE: K-weighting for any rate, LoudnessMeter (momentary, short-term, integrated, range from fixed histograms)
Source/DSP/Engine.h    the trim, the listening modes, the meters, the reference (Reference, the A/B crossfade, the level match); covered by Tests/Tests.cpp
../../shared/dsp/      TruePeak.h (`pdsp::TruePeakReader`: the 8x, 64-tap reader; shared with Aegis, so run AegisTests too when you change it)
Source/Plugin*.{h,cpp} decodes the reference file
Tests/UiCheck.cpp      writes WAV files and loads them
```

## Rules that keep it correct

**The meters are the standard's, and a test says so.** K-weighting is made from the analog prototypes (`kWeighting`), not from the 48 kHz table, so that it is right at every rate; test [1] compares the loudness of sines with the
analog response at five frequencies (0.006 dB), and with EBU Tech 3341 / 3342 cases. Do not "improve" the filters or the offset (-0.691). The 400 ms blocks overlap by 75%: a sub-block is 100 ms and a gating block is the last four.

**Fixed memory.** The integrated loudness and the range read histograms of 801 bins (0.1 LU from -70 to +10 LUFS) holding the count and the *power* of each bin, so the mean is exact and the gates are in 0.1 LU steps. Nothing grows with
the length of the programme, so the meter runs on the audio thread, and the same class measures a whole file on the message thread (`measureIntegrated`).

**Meters read the sound after the trim and before what is listened to.** LISTEN and the reference change what is heard, never what is measured (test [3]); the reference's loudness is measured separately when it is loaded.

**The reference crosses to the audio thread by a pointer.** The editor's thread (`HermesProcessor::loadReference`) builds a `Reference`, keeps the last two alive in `loaded` and gives the engine a raw pointer (`setReference`, an atomic
store with release); the audio thread reads it with acquire once per block. Deleting the one before the previous is safe because a block never lasts that long. Do not free the previous one at once. The restart of the position
and a seek (`seekReference`, a fraction) are flags the audio thread takes (`restart`, `seekTo`), never a write to `position` from another thread; because the pointer and the flag are not one atomic, `process` also wraps `position` into the reference it just loaded (a shorter one swapped in must not be read at the old position).

**The match is read at ticks.** The reference's gain follows the sound's integrated loudness, read every `kTick` = 64 samples counted from the start, not every block: that is what makes the output the same for any block size
(test [5], with a reference playing). Anything that reads a meter inside `process` has to be read at a tick.

**A to B is a crossfade** (`listenMix`, 20 ms, snapped at its ends so A is exactly the sound). The reference advances whether it is heard or not and loops with `position`; a file that loops with a step in it clicks at the loop.

**Idle is the input.** Trim 1.0 exactly, no polarity, Stereo, A: `l = x x 1.0` and the output is the input, bit for bit (test [3]). The trim is smoothed per sample and starts at its target.

**State.** The reference's path is a property of the state (`refPath`), not a parameter; a state with no `refPath`, or whose file is gone, clears the reference (`clearReference`); a processor given a state with a file loads it again with `callAsync` through a `WeakReference` (the processor may be gone by then), and
`prepareToPlay` loads it again if the sample rate changed.

**Parameters.** `hermes::id`: all are `ParameterID` version 1 except `reset_play` (AUTO RESET, version 2, off: the processor clears the meters on the rising edge of the host's `isPlaying`; `keepsParameter` keeps it through presets) and `auto_target` and `auto_peak` (the AUTO TRIM target and peak ceiling; AUTO TRIM adds to the current TRIM, so TRIM 0 is where a fresh run starts, version 2, at the old behaviour: -14 and off, so an older session opens as it was). The order of `listenNames` is the order of `Listen`.

## DRY: where each thing lives

- loudness and K-weighting -> `Loudness.h`; the true peak -> `shared/dsp` TruePeak.h

## Gotchas already paid for

- A test helper that takes the signal by value and returns it processed has to have its result used (`auto out = run (...)`): two tests of the reference measured the input and passed for the wrong reason until they were made to fail.
- Loading a reference from a test needs a `juce::AudioFormatManager` with `registerBasicFormats()` (the processor has one); the audio formats module is in the plugin's and the checks' link lines.
