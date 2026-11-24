# CLAUDE.md

Hera Bond: a JUCE compressor (VST3 / AU / Standalone): peak or RMS detector, soft knee, auto release and makeup, parallel mix, stereo link, sidechain with a detector high-pass. See `README.md` for what it does and what has been measured; this file is what you need to change it safely.

## Build and verify

Build, test and snapshot commands live in [README.md](README.md) (`## Build & develop`); shared defaults and `scripts/build.sh` live in [../../CLAUDE.md](../../CLAUDE.md) (`## Building and checking`, `## Module conventions & defaults`).

- `Source/DSP` has no JUCE in it (supports standalone `g++` DSP tests).

## Layout

```
Source/DSP/Engine.h  no GUI, no JUCE: reductionDb() (the gain computer), autoMakeupDb(), Engine (the detector, the followers, makeup, mix); covered by Tests/Tests.cpp
```

## Rules that keep it correct

**The gain follows the reduction in dB**, not the level: `wanted = reductionDb (level)`, one-pole attack when `wanted > gr`, release otherwise, and the gain is `exp (-gr x ln10/20)`. A peak detector that
fed the gain computer directly (per-sample `|x|`) read 0.5 to 1.5 dB less reduction than the law on a 1 kHz sine, because the gain rippled with the waveform: the peak detector is an envelope with a 15 ms
fall (`kPeakFallMs`) and an instant rise. Do not make it faster without redoing test [1].

**At rest the gain is exactly 1.** A reduction under `kRestDb` with nothing to reduce is set to exactly 0, and `gr == 0` gives a gain of exactly 1 (not `exp (0)` times a rounding). MIX at 100% takes the compressed
sound itself (`pure`), not `dry + mix x (wet - dry)`. That is what makes the idle compressor bit-exact (tests [2]); keep both.

**AUTO release** is two followers and the gain is the larger of the two. The fast one has ATTACK and a fixed 50 ms release (`kFastAutoReleaseMs`); the slow one has an attack 20 times longer (`kSlowAttackFactor`) and a fixed 600 ms release
(`kSlowAutoReleaseMs`). A short burst only moves the fast one far, a long passage moves both. RELEASE is not used.

**HOLD** (`EngineParams::holdMs`, `Channel::hold`, plain release only) is a counter of samples: while `wanted < fast` and the counter is above 0 the follower does not move and the counter counts down; whenever `wanted >= fast` it is set back to the full hold. At 0 ms the counter is never above 0, so the follower is the one without a hold, bit for bit (tests [2], [8]). Not used with AUTO release.

**LINK** is `own + link x (louder - own)` on the linear detector levels, not on the gains: at 100% both channels see the same level and get the same gain exactly.

**The sidechain** is read only when KEY is on and the host connected one (`keyL != nullptr`); a mono key is given with `keyR == nullptr`. The detector's high-pass is on the key, whichever it is. The key is never heard.
The bus is the processor's second input, off by default, and `sidechainConnected` is what the editor's note reads.

**Look-ahead delays the sound, not the detector.** `Engine::process` writes each input sample into a ring (`Channel::ring`) and reads the sound that is heard `delaySamples` back; the detector reads what is arriving (or the sidechain). `dry` (what MIX blends, and what the gain is applied to) is the delayed one, so a blend does not comb. With
`delaySamples == 0` the ring returns the input itself, so the engine is bit-identical to the one without look-ahead. A change of the setting clears the ring (a short gap) and moves `latencySamples`. The list is `kLookaheadMs`;
its order is the order of `lookaheadNames`, the parameter's choice.

**TOPOLOGY** (`EngineParams::topology`, a choice: Feed-forward is the default). Feedback's detector hears the previous output sample of its own channel, after the gain and before MAKEUP and MIX (`Channel::fb`, `dry x gain`), and only when no sidechain is connected: with KEY and a key connected the detector reads the key, as in feed-forward. Three rules follow, and the README's Limits says the same:
Feedback **ignores LOOK-AHEAD**: its `delaySamples` is 0, the latency reported is 0, and the setting is kept for feed-forward (`prepare` and `process` both take the topology). **Its law differs and is not compensated**: the steady state is `gr = reductionDb (level - gr)`, solved by `steadyFeedbackReductionDb`, which is `slope / (1 + slope)` of the overshoot (never half of it), and RATIO is not changed for it. Anything that asks what the law does takes the topology: the transfer curve, AUTO makeup (`autoMakeupDb`) and the AUTO THRESH answer (`topologyReductionDb`, `reductionAverageDb`, `autoThresholdFor`). A change of the topology restarts the delay ring like a look-ahead change.

**LEVEL MATCH, DELTA and SC LISTEN.** `match` is a parameter (`EngineParams::match`): the energies of the delayed input and of the output are followed per sample (0.6 s), the gain that equals them (within 12 dB) at 0.15 s, applied after MIX, exactly 1.0 when off (so the idle plug-in stays bit-exact). `Engine::delta` and `Engine::scListen` are atomics the editor sets, not parameters: never saved, ended by the editor's destructor, shown in the notes. SC LISTEN replaces the output with the key after the detector's high-pass (it wins over DELTA); DELTA is output minus the delayed input.

**Bypass.** `pdsp::BypassDelay` is prepared for `Engine::maxLatencySamples()`.

**Presets & Undo.** `Programs` keeps LEVEL MATCH across presets (`keepsParameter`); `setCurrentProgram` begins an undo step.

**AUTO SETUP** (the button; the engine's listening is still called AUTO THRESH) is a one-shot measure, like LEARN in Atlas: the editor requests it (`autoThreshStart`), the audio thread listens to `kAutoListenSeconds` of frames and says `done`, the editor takes the answer and cancels. The histogram (`LevelHistogram`) is written only while listening, and read only once `done` is seen. The answer is the highest THRESH at which the law's reduction, averaged over the audible histogram, reaches the target, corrected by the ballistics the listening measured (the follower's mean reduction minus the law's mean at the same settings: a release tail is not the law). The crest factor (peak minus RMS of the same audible frames, `autoCrestDb`) gives RATIO, ATTACK and RELEASE through `autoSetupFor`, and THRESH is answered at that RATIO. Only the answer `set` moves anything: RATIO, ATTACK, RELEASE and THRESH, through one gesture each inside one undo step. The ballistics correction is measured at the listening's settings, so it is off when the press changes RATIO, ATTACK or RELEASE (README, Limits). It is monitoring state: not a parameter, never saved.

**Smoothing.** Makeup and mix are smoothed per sample (5 ms); everything else is per sample by nature, so the output does not depend on the host's block size (test [8], bit for bit). The first block starts them at their targets.

**Parameters.** `hera::id`: all are `ParameterID` version 1 except `lookahead` (version 2, Off), `match` (version 3, off), `hold` (version 4, 0 ms), and `topology` (version 5, Feed-forward). The order of `detectorNames` is the order of `Detector`.

## DRY: where each thing lives

- the law -> `reductionDb()` in `Engine.h` (the engine, the editor's transfer curve, the tests); feedback's steady state -> `steadyFeedbackReductionDb()`, and the choice between them -> `topologyReductionDb()`; what AUTO makeup adds -> `autoMakeupDb()`; what AUTO THRESH answers -> `autoThresholdFor()` and `reductionAverageDb()` in `Engine.h`; the crest factor -> `crestFactorDb()`; what AUTO SETUP sets -> `autoSetupFor()` (all in `Engine.h`)

## Gotchas already paid for

- Measuring a reduction on a tone: read the peak over a whole period (1 ms for 1 kHz), not one sample; and measure the release on a quiet tone that follows (under the threshold), because the output of silence says nothing about the gain.
