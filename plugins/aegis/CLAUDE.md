# CLAUDE.md

Aegis Ceiling: a JUCE true-peak look-ahead limiter (VST3 / AU / Standalone). See `README.md` for what it does and what has been measured; this file is
what you need to change it safely.

## Build and verify

Build, test and snapshot commands live in [README.md](README.md) (`## Build & develop`); shared defaults and `scripts/build.sh` live in [../../CLAUDE.md](../../CLAUDE.md) (`## Building and checking`, `## Module conventions & defaults`).

- `Source/DSP/Engine.h` has no JUCE in it (supports standalone `g++` DSP tests).

## Layout

```
Source/DSP/Engine.h  no GUI, no JUCE: SlidingMin, Engine (the whole signal path); covered by Tests/Tests.cpp
../../shared/dsp/    TruePeak.h (`pdsp::TruePeakReader`: the 8x, 64-tap reader; shared with Hermes, so run HermesTests too when you change it)
../../shared/dsp/    BandLink.h too: `decodeLimiter` reads what Themis' COPY wrote (PASTE); run Themis' `make test` and `ThemisSmoke` when you change the `limiter` line
Source/Parameters.h  readParams(), pasteLimiter()
Source/TipArea.h     the transparent strips that carry the tooltips of the painted display; a click on one goes to the editor
```

## Rules that keep it correct

**The ceiling is a chain of inequalities; each link is tested.** The gain at the output sample that is heard now must be no more than what the detector needed for the
two interpolation points on either side of that sample. The reader reports the peak around `history[kCentre]`, `kDetectorDelay` samples behind the newest input, so
`required[k]` belongs to the pair `(x[k-D], x[k-D+1])`; the sample output at `n` is `x[n-look-D]`, which sits in the pairs `required[n-look-1]` and `required[n-look]`. So
the hold window is `look + 1` (that is `look + 2` values), and the two running means together reach back at most `look` samples (`widthOne + widthTwo = look + 2`).
A mean is never above the smallest value it covers, so `G[n] <= required` of both. Change any of look, D or the window and test [1] is the check; do not tune
until it passes.

**The reader needs its taps.** A 12-tap reader missed a peak by 1.5 dB on a programme with energy up to Nyquist, 32 taps by 0.7 dB, and a 4x reader reads up to 0.7 dB low
by construction (the peak between two points). It is 64 taps at 8x on purpose (`shared/dsp` TruePeak.h, which Hermes reads its meter with too); do not shorten it for speed without redoing test [8]. The cost is 7 x 64 multiplies per
sample per channel and a delay of 32 samples (`kDetectorDelay`, `pdsp::kTruePeakDelay`), which is constant (TRUE PEAK off does not change it, so the latency does not move with a switch).

**Gain smoothing is what keeps the peaks from coming back.** With one mean (a plain moving average) the gain's own modulation made peaks between samples that the
reader had not seen (0.3 to 0.7 dB over on noise). Two means of half the width fix that: do not collapse them into one.

**AUTO RELEASE only changes the release step.** `release = fmin (held, ...)` whatever the step, so the ceiling's chain of inequalities does not depend on it (test [1] runs with it on and off); do not move the `fmin`. With it off the step is RELEASE's exactly (the old behaviour, bit for bit). `sustained` is a mean of the envelope that decides how far from the fast step to the slow one it is (`kAutoMeanMs`, `kAutoDepth`, `kAutoFastShare`: tuned on test [11], where a short burst must let go much sooner and a long one at RELEASE).

**OUT PEAK reads the output with its own `TruePeakReader`s** (`outPeak`), after the final clamp, so it is what leaves. It is the same reader as the detector's: it proves the clamp and the gain, not the reader (test [8] and [12] do, with an independent one). It costs a second pair of readers; do not drop them for speed.

**DELTA and MATCH LEVEL are listening aids after the limiter.** OUT PEAK reads the limited output before them (it proves the clamp, not the aid). DELTA is `delayL[read] - limited` (the sound after GAIN, aligned by the same read position, minus the output); MATCH multiplies the output by `min (1, sqrt (inPower / outPower))` (a mean of the RMS over `kMatchMeanMs`, smoothed by `kMatchStepMs`), so it only turns down and the ceiling holds. With both off nothing changes, bit for bit (test [13]). Presets switch both off (a preset resets every parameter it does not list); both are `ParameterID` version 2.

**Bypass.** `pdsp::BypassDelay` is prepared for the longest look-ahead + `kDetectorDelay`.

**MAX REDUCTION** is a hold in the editor (`reductionHold`, from the engine's `reductionMeter` each tick): cleared by CLEAR, by a click on its number (`holdArea`, set in `resized()`) and when CEILING or TRUE PEAK changes.

**PASTE** (`pasteLimiter` in `Parameters.h`) reads the `limiter` line of `pdsp::link` (`shared/dsp` BandLink.h, written by Themis' COPY): the ceiling is absolute, the gain change is relative to GAIN, and both are clamped to the parameters' ranges. A ceiling switches TRUE PEAK on. It says what it did in the strip (`AegisEditor::shownNotes`), for eight seconds.

**Configuration.** Everything is sized in `prepare()`. `configure()` (a change of look-ahead) clears the state: it clicks, so it is a choice, not a knob.

**Bit exact when idle.** With the gain at 1 the means sum exactly (they are sums of 1.0), `std::fmin (1.0, ...)` removes the last rounding, and the output is the delayed
input bit for bit (test [3]). The smoothed input GAIN starts at its target (`gainStarted`), so a session does not open with a ramp.

**The final clamp** compares with `ceiling * (1 + 1e-9)`: the gain brings a sample *to* the ceiling, and a comparison without the margin counts rounding.

**Presets, Undo and A/B.** `Programs` is given no `keepsParameter`, so presets reset DELTA and MATCH off; `setCurrentProgram` and `pasteFromClipboard` begin an undo step. `ab` (`ui::AbCompare`) uses `aegis::keepsParameter`, so DELTA and MATCH are excluded from A/B switches.

**Tooltips over the display.** The painted parts (the trace, MAX, OUT PEAK, the IN and OUT bars, the delay) have `TipArea`s laid over them. A component that does not take clicks is never the one under the mouse, so no tooltip would show on it: a `TipArea` takes the click and hands it to the editor's `mouseDown`, which is where the MAX number clears its hold.

**AUTO GAIN only sets GAIN, once.** The detector's peak before GAIN is kept in `Engine::detectorPeak` (the peak divided by the gain in force at each moment, so a GAIN move while it listens skews the measure: the README's Limits says so). The editor clears it on the click, reads it after four seconds (`finishAutoGain`) and passes it to `autoGain()` in `Engine.h`, which is the whole rule: GAIN is `CEILING + 3 dB - peak`, held to GAIN's range, and nothing is set under -80 dB. It is one undo step, and GAIN stays an ordinary parameter. The readers' last taps of earlier audio count in the first milliseconds of the window, so a test lets them run out before it clicks.

**Parameters.** `aegis::id`: all `ParameterID` version 1 except `autorel`, `delta` and `match` (version 2, off: a session saved before them has the old behaviour; `UiCheck` pins `autorel`).

## DRY: where each thing lives

- what A/B keeps -> `keepsParameter` in `Presets.h`
- the look-ahead times -> `kLookaheadMs` in `Engine.h` (the parameter's names, the engine and the editor read it)

## Gotchas already paid for

- A test of the reader's accuracy must use a different interpolator than the reader (test: 8x, 64-point Hann sinc, over the whole signal).
