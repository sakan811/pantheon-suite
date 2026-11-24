# CLAUDE.md

Castor Pair: a JUCE stereo imager (VST3 / AU / Standalone): the width of the sides in the six bands of music, mid and side level, mono bass, balance, a goniometer and a correlation meter. See `README.md` for what it does and what has been measured; this file is what you need to change it safely.

## Build and verify

Build, test and snapshot commands live in [README.md](README.md) (`## Build & develop`); shared defaults and `scripts/build.sh` live in [../../CLAUDE.md](../../CLAUDE.md) (`## Building and checking`, `## Module conventions & defaults`).

- `Source/DSP` has no JUCE in it (supports standalone `g++` DSP tests).

## Layout

```
Source/DSP/Engine.h  no GUI, no JUCE: the width of each band (pdsp::BandSplitter), the crossfade, mid / side / mono below / balance / output, the correlation; covered by Tests/Tests.cpp
Source/Parameters.h  readParams(), pasteFromThemis()
Source/Plugin*.{h,cpp}  feeds `tapOut` (`pdsp::StereoTap`) for the goniometer
../../shared/dsp/    MusicBands.h (the six bands and `pdsp::BandSplitter`)
```

## Rules that keep it correct

**The splitter's bands are in phase, and so they sum to an all-pass of the sound, not to the sound.** An earlier version split only the side by differences of low-passes (`LP_k - LP_k-1`, which adds
up exactly to the side) and added each band's `(width - 1) x band` to the input. It was bit-exact when neutral but wrong when not: a difference of two low-passes is not a band-pass in phase with the
signal, so a band at 0% *raised* the side by 2 to 4 dB near its edges (measured). Do not go back to subtractive bands. With `pdsp::BandSplitter` the sum of the bands has the magnitude of the input (test [2]:
all at 200% is +6.02 dB everywhere), at the price of the all-pass phase, which is why the split is not used while every width is exactly 100%.

**`engaged` is the crossfade between the sound as it is and the split.** It goes to 1 when a width target or the current width is off 100% and back to 0 when both are at rest at 100%; the widths are smoothed
per sample (10 ms) and snapped to their target within 1e-9, so "100%" is exactly 1.0 and `engaged` reaches exactly 0. The splitter runs all the time (its states must be warm); only the choice of what is heard changes.
With `engaged == 0` nothing touches the sample: `l = l0`.

**The rest is differences and gains that are exactly neutral.** `dM = (midGain - 1) x m`, `dS = (sideGain - 1) x s`, and MONO BELOW `dS = HP (s + dS) - s` (only while it is on; the filter is reset while it is off): zero when
neutral, so `l = l0 + 0 + 0`. MIX (`mixGain`, smoothed over 10 ms, snapped to its target) blends `l0 + mix x (processed - l0)` only while it is below 1 (at 100% the processed sound is taken as it is, bit for bit), before BALANCE and OUTPUT. BALANCE and OUTPUT multiply by `leftGain x outGain`, exactly 1.0 at rest. Never rebuild left and right from the mid and the side (`m + s` is not `l` to the last bit).

**LISTEN is applied by the processor, after the goniometer's tap** (`applyListen`: Mono and Side put one signal in both channels), so the cloud and the correlation read the sound and not the check; on a mono bus it is not applied and BALANCE is set to 0 before the engine (it would only turn the one channel down). `correlationMin` is the lowest per-block reading since the editor last took it; the editor keeps the minimum (`minHold`) and clears it on the MIN button.

**Goniometer.** Fed by the processor through the shared lock-free `pdsp::StereoTap`; nothing of it runs on the audio thread except the push.

**PASTE.** `pasteFromThemis` in `Parameters.h` reads the `width` line of `pdsp::link` (`shared/dsp/BandLink.h`) that Themis writes for a width item, and the side `lowcut` band (`place=side`) of the low-end mono item, which sets MONO BELOW to its frequency (clamped to the range; its slope is not read). Every band the width line names and MONO BELOW are set in one undo step (`ui::beginUndoStep`), the others are left alone, and text with neither changes nothing and says so in the strip. The width line has no `amount` key (`encodeWidth` / `decodeWidth`), so each band's percent is taken as it is. A lowcut on the stereo is not MONO BELOW.

**Parameters.** `castor::id`: `width1` ... `width6` (following `pdsp::bands::kBands` order), `mid`, `side`, `mono`, `balance`, `out`, `listen`, `mix`. All are `ParameterID` version 1 except `listen` (version 2, Stereo: a session saved before it opens as it did; presets reset it, it is not kept) and `mix` (version 3, 100%: presets set it back to 100%).

## DRY: where each thing lives

- the six bands, their names, edges and the splitter -> `shared/dsp` MusicBands.h (shared: run the other plugins' tests too); the width knobs take their names and tooltips from it
- `drawWidthGlass` -> reads the six widths through `readParams` (a picture of the knobs and nothing measured)

## Gotchas already paid for

- A snapshot has to call `tickDisplay()` for the goniometer to get samples.
