# CLAUDE.md

Nemesis Hush: a JUCE de-esser (VST3 / AU / Standalone): a band detector, split-band or wideband, absolute or relative detection, listen. See `README.md` for what it does and what has been measured; this file is what you need to change it safely.

## Build and verify

Build, test and snapshot commands live in [README.md](README.md) (`## Build & develop`); shared defaults and `scripts/build.sh` live in [../../CLAUDE.md](../../CLAUDE.md) (`## Building and checking`, `## Module conventions & defaults`).

- `Source/DSP` has no JUCE in it (supports standalone `g++` DSP tests).

## Layout

```
Source/DSP/Engine.h  no GUI, no JUCE: reductionDb() (the law), Engine (the band, the envelope, the follower, split-band / wideband / listen); covered by Tests/Tests.cpp
```

## Rules that keep it correct

**Split-band is a subtraction, not a filter in the signal path.** `y = x - (1 - gain) x band(x)`: the band-pass (constant 0 dB at the centre) is the same one the detector uses, so the band is turned down
by exactly the gain at its centre and the rest is not touched, and at a gain of exactly 1 the product `0 x band` is exactly 0 and `y == x` bit for bit even when the compiler fuses multiplies and adds.
A notch or a shelf in the path at 0 dB would not be exact (that is the lesson of Hestia's carver). Do not replace it by a dynamic filter.

**Look-ahead delays the sound and its band, not the detector.** The detector's band-pass reads what is arriving; the sound and that band are written into a ring (`Channel::ringSound`, `ringBand`) and what is heard is `delaySamples` back, so `y = heard - (1 - gain) x heardBand` is still the subtraction above (the delayed band is the band of the delayed sound).
With `delaySamples == 0` the ring returns the input, so the engine is bit-identical to the one without. A change of the setting clears the rings (a short gap) and moves `latencySamples`. The list is `kLookaheadMs`, in the order of `lookaheadNames`.

**DELTA is the third output branch** (`y = (1 - gain) x heardBand` in split-band, `heard x (1 - gain)` in wideband, ahead of LISTEN): it is exactly what the other branches take off, so output + DELTA is the delayed input. **FIND** (`startFind`, `findCount`, `foundHz`) is a bank of `kFindBins` band-passes that only runs for `kFindMs` after a request, on the mono sum, fixed arrays, no allocation; the editor's FIND button (enabled only with LISTEN on) sets FREQ from `foundHz` when `findCount` moves. **SET THRESH** (`startSetThreshold`, `setCount`, `setStatus`, `setDb`) is the same kind of pass on the band at the current FREQ: a 1 dB histogram of the active sound (above -100 dB in the band and the whole sound), THRESH at the top 6% of it, and no THRESH at all when the top is not 10 dB over the middle (no s). It moves THRESH only; the editor clips the value to THRESH's range and makes it one undo step (`beginUndoStep`). The processor owns the `UndoManager` for it, and the editor's UNDO / REDO (in the header strip) use the same manager.

**Bypass.** `pdsp::BypassDelay` is prepared for the longest look-ahead.

**One gain for both channels.** The louder band envelope decides; the gain is applied to both. Do not detect per channel: a sibilant panned to one side would move.

**The envelope has a 2 ms fall** (`kEnvelopeFallMs`): the band's output swings through zero every half cycle, and a detector fed with `|band|` per sample made the follower ripple. Do not remove it.

**The ratio scales the law, it does not replace it.** `slopeOf (ratio)` is `1 - 1/ratio` and `reductionDb` multiplies the overshoot and the knee by it (the knee stays continuous with the line above). At `kInfiniteRatio` (21, "Inf") the slope is exactly 1.0, so the law is the one of the first release bit for bit (test [11]); the default is Inf.

**At rest the gain is exactly 1** (`kRestDb`): a reduction under it, with nothing to reduce, is set to exactly 0, and `gr == 0` gives a gain of exactly 1.

**RELATIVE** subtracts the broadband level (30 ms mean square of the input, `x 2` for the peak of a sine) from the band level in dB, so a sound 30 dB quieter has the same relative level (test [6]).

**The coefficients move, the states stay.** `setBandpass` sets the coefficients only; FREQ and WIDTH are read per block and applied at the start of it (the band-pass has no audible zipper at the rates a knob turns, and a
block-size dependence only exists while a knob moves).

**Parameters.** `nemesis::id`: all are `ParameterID` version 1 except `lookahead`, which is version 2 (a session saved before it opens with it Off), `delta`, version 3 (off), and `ratio`, version 4 (Inf: the law as it was). The order of `modeNames` and `detectionNames` is the order of `Mode` and `Detection`.

## DRY: where each thing lives

- the law -> `reductionDb()` in `Engine.h` (the engine, the tests); the fixed times -> the constants at the top of `Engine.h`

## Gotchas already paid for

- A test of the absolute threshold needs a level well under it (-12 dB), not one just under: the 6 dB knee already takes 0.1 dB off at 2 dB under.
