# CLAUDE.md

Dionysus Warmth: a JUCE saturator (VST3 / AU / Standalone): six curves, bias, tone, parallel mix, auto gain, half-band oversampling up to 8x. See `README.md` for what it does and what has been measured; this file is what you need to change it safely.

## Build and verify

Build, test and snapshot commands live in [README.md](README.md) (`## Build & develop`); shared defaults and `scripts/build.sh` live in [../../CLAUDE.md](../../CLAUDE.md) (`## Building and checking`, `## Module conventions & defaults`).

- `Source/DSP` has no JUCE in it (supports standalone `g++` DSP tests).

## Layout

```
Source/DSP/Shapes.h        the curves, saturate() (curve + bias), harmonicsDb() (what the editor shows), compensation() (AUTO GAIN), the antiderivatives and saturateAdaa() (ADAA, 1x)   <- one source for the engine, the display and the tests
Source/DSP/Oversampler.h   HalfbandStage (one doubling), Oversampler (the chain of one channel), oversamplingLatency()
Source/DSP/Engine.h        the signal path: drive, the oversampler around the curve, DC block, tone, the delayed dry sound, mix, output
```

## Rules that keep it correct

**Latency is a whole number of samples.** Stage 0 has 65 taps (half-length M = 32), the others 33 (M = 16); a stage's round trip delays by M samples of its lower rate, so the total is 32, 32 + 8 = 40 and 32 + 8 + 4 = 44
samples (`oversamplingLatency`). The decimator returns the filter's value at the *even* sample of each pair (it computes it when the first arrives and hands it out with the second): taking the odd one added half a sample per stage
and made the wet path one sample later than the dry delay (test [4], the impulse). The dry sound waits in a ring of `latency + 1` samples (a fixed array of `kMaxLatency + 1`). Change a tap count and the latency formula, the test and the README move together.

**The stages are half-bands, and the code uses it.** Every tap an even distance from the centre is exactly 0, the centre is 1/2, the filter is symmetric: `up` gives its first output as the delayed input and its second from the odd taps in pairs, `down` is the centre plus the odd taps in pairs, and each history is written twice (`taps` apart) so a window is contiguous, with no modulo. That is about a fifth of the CPU of the plain convolution it replaced (8x went from 25-50% to about 4% of a core, stereo at 48 kHz) with the same output to 1e-15. Taps must stay odd with (taps - 1) / 2 even and at most `HalfbandStage::kMaxTaps`. `Engine::process` measures AUTO GAIN only when the shape, drive or bias moves (`compensationFor`: it is a loop of 256 curve evaluations, which at small host blocks cost more than the oversampler).

**ADAA is the 1x path's, and it is off.** `saturateAdaa()` (first-order antiderivative anti-aliasing, Parker et al. 2016) runs only when `adaa` is on and the oversampling is Off; at 2x, 4x and 8x the parameter is ignored and the oversampled path is untouched. It is not the default: on a linear piece the curve's average over a step is `(x[n] + x[n-1]) / 2`, a lowpass on the whole sound (test [10] pins -2 dB at 10 kHz). With MIX under 100% the dry sound stays the plain input (the ring at 1x is a delay of 0), so the blend is `(1 - m/2) x[n] + (m/2) x[n-1]` on a linear piece: the same lowpass in proportion, never a comb (test [10] pins it at 1, 10 and 20 kHz). Do not add a half-sample delay to the dry sound to "align" it: the latency could not be reported, and MIX 0% would stop being the input. The state (`AdaaState`) is per channel: `reset()` clears it, the first sample continues from silence at the working point, and a block with the switch off clears `primed` so that the next time it starts from silence again. Each antiderivative is checked against its curve by test [10]; the diode's is 0.49 ln cosh (x / 0.7), not the 0.343 the first draft had (the chain rule).

**Gain 2 on the way up.** Zero-stuffing halves the level; the up filter has a gain of 2 (`first = 2 x a`). The fundamental is the same at every oversampling (test [3]).

**BIAS and the DC.** The curve gets `x + b` and gives back `curve (x + b) - curve (b)`, so zero in is zero out; an asymmetric curve (BIAS, or the diode) still has DC while a signal plays, so a 10 Hz high-pass runs after the curve
whenever either is in use (`dcOn`) and is reset while it is not. `compensation()` takes the mean off the output before it reads the RMS for the same reason (test [5]).

**The display is the curve, not a measurement.** `harmonicsDb()` and the editor's curve call `saturate()`, the function the engine runs; test [2] checks the engine against it at 8x. Do not write a second formula for either. The one exception is ADAA at 1x (below): the engine runs `saturateAdaa()` there, the display stays the static curve, and AUTO GAIN measures the static curve too (test [10] bounds the difference).

**MIX.** 100% takes the saturated sound itself (not `dry + mix x (wet - dry)`), 0% the delayed dry sound itself: exact at both ends. The wet path runs all the time so its states are warm.

**LOW CUT is on the saturated path only.** A 2nd-order high-pass (`Channel::lowCut`) before DRIVE and the curve, not run (and reset) at 20 Hz or below; the dry sound in the ring is the unfiltered input, so MIX under 100% brings the lows back and at 0% it is exact. It is what `drivePeak` (the level that reaches the curve, an atomic the editor's meter reads and resets) measures.

**DELTA is `Engine::delta`, an atomic the editor sets, not a parameter**: never saved, ended by the editor's destructor, shown in the notes. The output is `result - dry` (the dry sound being the ring's, delayed like the output).

**Bypass.** `pdsp::BypassDelay` is prepared for `kMaxLatency`.

**Setup.** Everything is a fixed array or sized in `prepare()` (`configure()` allocates nothing, so changing oversampling on the audio thread is safe and restarts the saturator).

**Parameters.** `dionysus::id`: all are `ParameterID` version 1 except `lowcut`, version 2, and `adaa`, version 3 (Off: a session saved before either sounds as it did; a preset that does not list one leaves it Off). The order of `shapeNames` is the order of `Shape`; of `oversamplingNames`, the index of `kOversamplingFactor`.

## DRY: where each thing lives

- the curves and their names for the user -> `Shapes.h` (`shape()`), `Parameters.h` (`shapeNames`); the harmonics and the AUTO GAIN factor -> `Shapes.h`
- the oversampling factors and latencies -> `Oversampler.h`

## Gotchas already paid for

- Test aliasing with a harmonic far above Nyquist (a 7 kHz sine's 5th, 35 kHz), not one just above (a 9 kHz sine's 3rd, 27 kHz, sits in the half-band's transition band and is only 38 dB down at 8x).
