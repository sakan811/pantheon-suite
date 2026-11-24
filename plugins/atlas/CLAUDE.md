# CLAUDE.md

Atlas Span: a JUCE multiband compressor (VST3 / AU / Standalone) with one to six bands made on a spectrogram (they start as
the six main bands of music, `shared/dsp` MusicBands.h); the display marks peaks and regions that jump; each band has a compressor on Stereo / L/R / M/S / Left / Right / Mid / Side, a solo, a delta and a gain match; the
split is Zero Latency or Linear; the compressors can be oversampled. See `README.md` for what it does and what has been
measured; this file is what you need to change it safely.

## Build and verify

Build, test and snapshot commands live in [README.md](README.md) (`## Build & develop`); shared defaults and `scripts/build.sh` live in [../../CLAUDE.md](../../CLAUDE.md) (`## Building and checking`, `## Module conventions & defaults`).

## Layout

```
Source/DSP/        no GUI dependencies; covered by Tests/Tests.cpp
  Compressor.h       Channels, BandParams, reductionDb() (the gain computer), Level (the detector), GainFollower (attack / release on the gain)
  LinearSplit.h      the linear-phase split: kernels from the crossovers, FFT overlap-save, the top band as the complement
  Learn.h            BandProfile: what LEARN makes of a band's level over time (THRESH, RATIO, ATTACK, RELEASE); LoudnessMatch: the OUTPUT that LEARN ALL sets; plain C++
  Engine.h           the whole signal path, the tick, oversampling, makeup and gain match, solo and delta, meters, latency
Source/Target.h      TARGET: the shifts of THRESH from a curve and the bands' typical levels; plain C++
Source/Parameters.h  readParams(), layoutOf(), addSplit() / removeSplit()
Source/UI/BandView.h the display (spectrogram, band edges, PEAK / JUMPS)
../../shared/dsp/    SpectrumIssues.h (`pdsp::SpectrumGrid`, `pdsp::IssueFinder`: plain C++, tested by AtlasTests [9]), SpectrumAnalyzer.h, AnalyzerTap.h
../../shared/ui/     Spectrogram.h (`ui::Spectrogram`: the ring of rows the display draws)
../../shared/dsp/    MusicBands.h (the six bands, `pdsp::BandSplitter`: the zero-latency split), Biquad.h
```

## Rules that keep it correct

**Setup & parameters.** Everything is sized in `prepare()`: both
oversamplers of every band, the delay lines, the linear split for the sample rate. `readParams` is called per block and looks
parameters up through the cached `bandIds()` table and the `juce::String` ids in `atlas::id`.

**The tick.** Everything that changes with time (parameters taken in, ramps of makeup / match / solo / delta / output gain, the
meters, a mode switch) steps every `Engine::tick` = 64 samples, counted from the start, and a segment never crosses a tick
boundary. That is what makes the output identical for any host block size (test [7], bit for bit), with the oversamplers and the
linear split included (both are sample-in, sample-out with their own state). A ramp is set to its target at the next tick
before a new one starts, so a value at rest is exact (a band's gain of 1 is exactly 1). The loop in `process` steps by the samples it did.

**A band adds its difference.** `compress()` turns the band's two signals into `x * (gain - 1)`; that difference (through the
oversampler when there is one) is added to the dry band, which waits in an `IntDelay` of the oversampler's latency. So a band whose
gain is 1 is bit-exact, oversampling with nothing compressing is a pure delay (test [6]), and delta is `output - dry` with no
second path. Do not oversample the band itself: the down-sampling filter would then colour bands that do nothing.
Mid and Side put their difference back as in Iris (Mid: to both; Side: to left and minus to right); never rebuild L/R from M and S.

**The detector.** `Level` is mean square over two one-pole stages of half a cycle of the band's *lowest* frequency (its lower
crossover, as ordered), read as the peak of a sine. Attack and release are in `GainFollower::step`, on the reduction in dB. A peak
detector feeding the gain computer directly was tried first: with a slow attack the reduction settled 2.6 dB short of the law on
a sine (the gain computer's output swings over the cycle), so the threshold meant something else for every attack.

**A band that is off is not run** once its reduction has come back to 0 (`BandState::idle`); its detector and oversampler are
reset when it starts again. Switching a band off is a release, not a cut: no click.

**The two splits have the same bands.** `LinearSplit::design` samples the *digital* Linkwitz-Riley magnitudes (`1 / (1 + r^4)`
with the frequencies warped by `tan`), the product form `low_k x high_0..k-1` that `pdsp::BandSplitter` realises, so a band is the
same part of the spectrum in both modes (test [1]). The top band is the delayed input minus the other five, which makes the sum
the delayed input whatever truncating the kernels did: keep it that way. The frame is `4 x half`, which is why the kernel's delay
is `(-j)^bin` and needs no table; changing the sizes independently breaks that. Sizes scale with the sample rate (`scaleFor`).
The crossovers stop at 40 Hz because the kernels (+-2048 samples at 48 kHz) cannot resolve a lower one.

**Upward compression** is the same follower with a negative reduction: `gr` (dB) is positive for a band turning down and negative for one lifting, `GainFollower::step` asks `boostDb` instead of `reductionDb` when `upward`, and the gain is `exp (gr * -0.115...)` for either sign. The attack is the time the gain takes to *give way* to a louder level (reduction growing, or lift shrinking: `wanted > gr` in both), the release the time it takes to come back: do not swap them for upward, the one condition covers both. The cap (`kUpwardMaxBoostDb`) and the gate (`kUpwardGateDb` + `kUpwardFadeDb` under the threshold) are what keep it from lifting noise: a lift without them boosts silence by the cap. Everything that read `gr > 0` reads `!= 0` or `fabs` (`run`, `most`, the rest snap in `step`), the meter (`reductionMeter`, `maxReduction`) is signed (the largest change of the tick), and the editor and the display read its sign. Downward bands are the old compressor bit for bit (tests [2], [6]). The level starts from zero, so an upward band lifts for the first milliseconds of any sound that starts from silence: that is why test [12] settles before comparing.

**The knee.** `BandParams::kneeDb` (`bN_knee`, version 6, 0 to 24 dB, default 6 = the knee the compressor had, bit for bit) is the width W of the soft knee in `reductionDb` and `boostDb` (Giannoulis, Massberg, Reiss, JAES 2012): the law's shape is the one tests [2] and [18] pin, only W changes, and `GainFollower::set` takes `floorSq` and `ceilSq` from it. At 0 the hard branch runs and nothing divides by W; keep that branch. TO ALL copies it with THRESH, RATIO, ATTACK and RELEASE; `bandIdList` carries it through `copyBand`, `addSplit` and `removeSplit` like every band setting.

**Look-ahead** is a delay of the audio *relative to the detector*, inside the compressor, not a delay of the detector's input: `compress()` reads the level from `detA` / `detB` and multiplies the signal that has waited in `BandState::look` (at the compressors' rate, so `lookSamples << osMode`); the dry band waits `osLatency + lookSamples` (`dry`). Everything that sums is therefore late by the same amount and `latencySamples` is the phase mode's + the oversampling's + the look-ahead's. It is a choice (`kLookaheadMs`, named by `lookaheadNames()`) because the latency must not move with a knob; a change goes through `configure()` like the other modes. With the look-ahead off `IntDelay::push` returns its input, so the compressor is the old one bit for bit: keep that (tests [2] and [6] pin it).

**The sidechain** is split by its own `scSplitter` / `scLinear` (never the main one: its filters must not hear it), only while a band listens to it (`scActive`, latched at a tick; it starts from clean filters), and each band reads *its own piece* of it: through the same owner summing as the main bands, the same Mid/Side transform as the band's channels, and held (each sample repeated) up to the oversampled rate, which leaves the level (a smoothed mean square) exactly as it was. A band with `sidechain` and no sidechain connected (`scPresent` false) listens to itself, so a session saved with it still works without one. The key's non-finite samples are zeroed in `splitInto`. The input is `Engine::process`'s last two arguments (null: none); the bus is the processor's second input, off by default, and `sidechainConnected` is what the editor's note reads.

**MIX** (`EngineParams::mix`, ramped per tick like the output gain, 1.0 exact) scales the difference a band adds in `runBand`, so 0 is the dry bands bit for bit; the dry bands are already aligned by `dry`. It is a session-wide control: presets keep it (`isGlobalParameter`).

**Bypass.** `pdsp::BypassDelay` is sized in `prepareToPlay` by `Engine::maxLatency()`, the largest any mode can report.

**Learn and TO ALL.** `BandView::learnBand` (the JUMPS marker's listening and a strip's LEARN) and `copyDynamicsToAll` (Parameters.h). The listening is the engine's, in audio time: `Engine::learnStart` / `learnState` / `learnTake` (`BandView::learnAll` starts every band at once; a TARGET curve is compared in `Target.h` when all have answered; `Learned::typicalDb` is what it compares) (an atomic state per slot: idle, requested, listening, done, silent; the editor only asks and takes), fed once per tick in `control()` with the level `levelMeter` shows, into a `BandProfile` (`DSP/Learn.h`: a 1 dB histogram of the level for THRESH and RATIO, a zigzag with 6 dB hysteresis for the rise and fall times of ATTACK and RELEASE; the rules are in its header comment). It writes nothing to the audio and the editor sets the four parameters when the answer comes (a timeout of three listenings ends it when no audio runs). Test [15] pins it, including that the output is the same with it running. LEARN ALL (and TARGET, which uses it) then sets OUTPUT: `Engine::loudnessStart` / `loudnessTake` measure the input and the bands' sum before OUTPUT (solo and delta left out: `sumBuffer`, the output `loudLatency` samples late) over `BandProfile::kListenSeconds` with the learned settings in effect; `LoudnessMatch` (Learn.h) says why it measures rather than computes; `BandView::matchOutputStep` writes OUTPUT when it is taken, and leaves it when the window is silent or dropped. AtlasTests `testLoudness` and UiCheck's LEARN ALL section pin it. `addSplit` / `removeSplit` copy through `copyBands`, which reads every strip before writing any (a copy must not read a strip an earlier one wrote).

**Latency** is `Engine::latencySamples` (linear split + oversampler + look-ahead), written on the audio thread in `configure()` when a tick sees a
new mode; `prepareToPlay` passes the saved
modes to `Engine::prepare` so the host hears the right latency before the first block. A mode switch clears the histories.

**The beginner's note** (`freshAndUntouched` in `Advice.h`): a fresh instance is told to press LEARN ALL while every band's THRESH, RATIO, ATTACK and RELEASE are at their init values (`BandParams()`), LEARN ALL is not pressed and `AtlasProcessor::restored` is false (set in `setStateInformation`, so a saved session never gets it). While it shows, the per-band "THRESH is at the top" warnings are left out, because the strip shows two lines and they would hide it. Changing any of those four settings on any band removes it.

**One to six bands.** The splitter always makes six pieces; `bandOwners (split)` says which band each piece belongs to (a split
that is off joins the piece above it to the band below), the pieces of a band are added together before its compressor, and only
owners run (`segment`). A band's settings, solo bit and meters are its owner piece's index (its *slot*). The engine accepts any
pattern of splits; the editor keeps the ones that are on in the first slots, in order (`addSplit` / `removeSplit` / `writeSplits` in
`Parameters.h`, which also move the bands' settings so they travel with the bands), so band i is strip i. Use `layoutOf()` for
anything that asks "which bands are there": never loop over `kBands` strips in the editor. A split that is off is parked at
`kMaxCrossoverHz`, so its (still running) crossover is out of the way.

**The display is not audio analysis.** `BandView::tick` runs on the editor's timer: the input tap (`AtlasProcessor::tapIn`) goes
through the shared `pdsp::SpectrumAnalyzer` onto `pdsp::SpectrumGrid` (1/24 octave), one row per tick into `ui::Spectrogram` and
into `pdsp::IssueFinder`. Nothing of it runs on the audio thread. The spectrogram's levels are per frequency and tilted, not a
band's level: that is why a band made from a JUMPS marker gets its threshold from `Engine::levelMeter` (what the band's own
detector read over `BandProfile::kListenSeconds`), not from the marker.

**Solo and delta are monitoring, not state.** They live in `Engine::monitor` (bit masks, atomics), not in parameters, and the
editor ends them in its destructor. Do not make them parameters. They fade over 10 ms (`heard`, `dryHeard`).

**Gain match** compares the band's power after its compressor with before it (0.4 s one-pole each, per sample), limited to
+-12 dB, recomputed per tick and ramped. It is measured before makeup, so MAKEUP adds to it.

**AUTO T and AUTO R.** `BandParams::autoThreshold` / `autoRelease` (`bN_autothr`, `bN_autorel`, version 5, off: the compressor as it was, bit for bit, which tests [2] and [6] pin). AUTO T: in `control()` the band's slow level (the power of the detector's loudest reading of each tick, averaged: `BandState::trackSq`, `kAutoThresholdSeconds`; a plain average until it is as long as that, gated by `kAutoSilenceDb`) plus THRESH is the threshold handed to `GainFollower::set` (`thresholdMeter` shows it); LEARN's answer is made relative to it (`learnStep`). AUTO R: `GainFollower::stepAuto` blends two release coefficients (`kAutoFast` / `kAutoSlow` x RELEASE) by a memory of how far down the gain has been (`BandState::memA` / `memB`, reset where the compressor is: idle, `configure`). Both live in the same per-tick / per-sample places as the rest: nothing is allocated. The histogram under a strip is `Engine::histogram` (counted per tick, taken out by the editor's timer).

**Parameters.** `atlas::id`: `xo0` ... `xo4` and `xo0_on` ... `xo4_on`; `bandIds (i)`: `b1_on` ... `b6_match`. All are `ParameterID` version 1 except `look` and each band's `listen` (version 2), each band's `dir` (version 3), `mix` (version 4), each band's `autothr` and `autorel` (version 5), and each band's `knee` (version 6), all declared after everything else so a host's list keeps its order. The order of `channelNames` is the order of `Channels`.

## DRY: where each thing lives

- the six bands, their names, default edges and the zero-latency splitter -> `shared/dsp` MusicBands.h (shared: run the other plugins' tests too)
- the defaults of a band -> `BandParams`
- which signals a channel mode runs -> `usesMidSide` / `runsFirst` / `runsSecond` / `isLinked` in `Compressor.h`
- the look-ahead choices -> `kLookaheadMs` / `lookaheadSamples()` in `Engine.h` (the parameter's names, the engine and the editor read them); the presets and what each is for -> `presets()` in `Parameters.h` (`Preset::purpose` is the tooltip; `isGlobalParameter()` says what a preset keeps)
- the order of the crossovers -> `orderedCrossovers()` in `Engine.h` (the engine and the editor's labels use it)

## Gotchas already paid for

- A single tone's reduction measured on the whole output is less than the band's (its neighbours carry part of it): tests that check the law solo the band (`runSolo`).
- A tone inside a band is lower than on the whole signal (100 Hz is 3 dB down in a 70 to 160 Hz band): anything that sets a threshold has to read the band, not the input.
- Every strip owns its `ui::Choice` / `ui::Toggle` controls (`AtlasEditor::Strip`).
