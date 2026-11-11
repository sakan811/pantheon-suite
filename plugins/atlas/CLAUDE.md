# CLAUDE.md

Atlas Span: a JUCE multiband compressor (VST3 / AU / Standalone) with one to six bands made on a spectrogram (they start as
the six main bands of music, `shared/dsp` MusicBands.h); the display marks peaks and regions that jump; each band has a compressor on Stereo / L/R / M/S / Left / Right / Mid / Side, a solo, a delta and a gain match; the
split is Zero Latency or Linear; the compressors can be oversampled. See `README.md` for what it does and what has been
measured; this file is what you need to change it safely.

## Build and verify

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release     # JUCE: version in cmake/Juce.cmake, or third_party/JUCE at the repository root
cmake --build build --target AtlasTests -j 10 && ./build/AtlasTests_artefacts/Release/AtlasTests
cmake --build build --target Atlas_VST3 -j 10
cmake --build build --target AtlasUiCheck && ./build/AtlasUiCheck_artefacts/Release/AtlasUiCheck
cmake --build build --target AtlasSnapshot && ./build/AtlasSnapshot_artefacts/Release/AtlasSnapshot docs/screenshot.png solo=62 delta=8 os=1
```

- Building the plugin targets does **not** install them (`ATLAS_INSTALL_AFTER_BUILD`, off by default).
- `AtlasTests`, `UiCheck` and `Snapshot` are `EXCLUDE_FROM_ALL`; build them by name. There is no `HostCheck` yet, and the plugin has not
  been built on macOS or run through `auval` (the repository's CLAUDE.md lists it under "Not yet in line").
- Definition of done for a DSP change: `AtlasTests` passes **and** the numbers the README quotes still match its output.
- Definition of done for a UI or parameter change: `UiCheck` passes and `docs/screenshot.png` is regenerated and looked at.

## Layout

```
Source/DSP/        no GUI dependencies; covered by Tests/Tests.cpp
  Compressor.h       Channels, BandParams, reductionDb() (the gain computer), Level (the detector), GainFollower (attack / release on the gain)
  LinearSplit.h      the linear-phase split: kernels from the crossovers, FFT overlap-save, the top band as the complement
  Engine.h           the whole signal path, the tick, oversampling, makeup and gain match, solo and delta, meters, latency
Source/Advice.h      the notes of the strip under the bands and the GUIDE text; plain C++, no JUCE
Source/Parameters.h  parameter layout, readParams(), layoutOf(), addSplit() / removeSplit()                 <- the single source of truth for parameters
Source/Plugin*.{h,cpp}, Source/UI/   JUCE wrapper and editor (the look is `shared/ui`); `UI/BandView.h` is the display (spectrogram, band edges, PEAK / JUMPS)
../../shared/dsp/    SpectrumIssues.h (`pdsp::SpectrumGrid`, `pdsp::IssueFinder`: plain C++, tested by AtlasTests [9]), SpectrumAnalyzer.h, AnalyzerTap.h
../../shared/ui/     Spectrogram.h (`ui::Spectrogram`: the ring of rows the display draws)
../../shared/dsp/    MusicBands.h (the six bands, `pdsp::BandSplitter`: the zero-latency split), Biquad.h
Tests/               Tests.cpp (DSP), UiCheck.cpp (editor on the real processor), Snapshot.cpp (editor -> PNG), Check.h
```

## Rules that keep it correct

**Real time.** `Engine::process` and everything under it must not allocate, lock or log. Everything is sized in `prepare()`: both
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

**Look-ahead** is a delay of the audio *relative to the detector*, inside the compressor, not a delay of the detector's input: `compress()` reads the level from `detA` / `detB` and multiplies the signal that has waited in `BandState::look` (at the compressors' rate, so `lookSamples << osMode`); the dry band waits `osLatency + lookSamples` (`dry`). Everything that sums is therefore late by the same amount and `latencySamples` is the phase mode's + the oversampling's + the look-ahead's. It is a choice (`kLookaheadMs`, named by `lookaheadNames()`) because the latency must not move with a knob; a change goes through `configure()` like the other modes. With the look-ahead off `IntDelay::push` returns its input, so the compressor is the old one bit for bit: keep that (tests [2] and [6] pin it).

**The sidechain** is split by its own `scSplitter` / `scLinear` (never the main one: its filters must not hear it), only while a band listens to it (`scActive`, latched at a tick; it starts from clean filters), and each band reads *its own piece* of it: through the same owner summing as the main bands, the same Mid/Side transform as the band's channels, and held (each sample repeated) up to the oversampled rate, which leaves the level (a smoothed mean square) exactly as it was. A band with `sidechain` and no sidechain connected (`scPresent` false) listens to itself, so a session saved with it still works without one. The key's non-finite samples are zeroed in `splitInto`. The input is `Engine::process`'s last two arguments (null: none); the bus is the processor's second input, off by default, and `sidechainConnected` is what the editor's note reads.

**Latency** is `Engine::latencySamples` (linear split + oversampler + look-ahead), written on the audio thread in `configure()` when a tick sees a
new mode, and reaches the host through `AtlasProcessor::handleAsyncUpdate` on the message thread; `prepareToPlay` passes the saved
modes to `Engine::prepare` so the host hears the right latency before the first block. A mode switch clears the histories.

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
detector read over `kLearnSeconds`), not from the marker.

**Solo and delta are monitoring, not state.** They live in `Engine::monitor` (bit masks, atomics), not in parameters, and the
editor ends them in its destructor. Do not make them parameters. They fade over 10 ms (`heard`, `dryHeard`).

**Gain match** compares the band's power after its compressor with before it (0.4 s one-pole each, per sample), limited to
+-12 dB, recomputed per tick and ramped. It is measured before makeup, so MAKEUP adds to it.

**Bad samples.** An inf or NaN in the input is replaced by silence on the way in (`segment`).

**Parameters.** Ids (`atlas::id`: `xo0` ... `xo4` and `xo0_on` ... `xo4_on` among them; `bandIds (i)`: `b1_on` ... `b6_match`) are stored in sessions and automation: never rename or
remove one. All are `ParameterID` version 1 except `look` and each band's `listen` (version 2, declared after everything else, so a host's list keeps its order); a new one needs a higher version, an entry in `readParams()` and a place in the editor.
The order of `channelNames` is the order of `Channels`.

## DRY: where each thing lives

- the six bands, their names, default edges and the zero-latency splitter -> `shared/dsp` MusicBands.h (shared: run the other plugins' tests too)
- parameter ranges, defaults, choice lists -> `Parameters.h` (the editor reads the items from the parameters); the defaults of a band -> `BandParams`
- which signals a channel mode runs -> `usesMidSide` / `runsFirst` / `runsSecond` / `isLinked` in `Compressor.h`
- what the plugin warns about and what GUIDE says -> `adviceFor()` and `guideText()` in `Source/Advice.h`; the editor fills `AdviceInput` (`adviceInput()`) and draws the notes with `ui::drawNoteStrip`. A note restates a limit the README's Limits documents: a new one needs it there, a test in UiCheck's Advice section, and at most `ui::kMaxNoteChars` characters
- the look-ahead choices -> `kLookaheadMs` / `lookaheadSamples()` in `Engine.h` (the parameter's names, the engine and the editor read them); the presets and what each is for -> `presets()` in `Parameters.h` (`Preset::purpose` is the tooltip; `isGlobalParameter()` says what a preset keeps)
- the order of the crossovers -> `orderedCrossovers()` in `Engine.h` (the engine and the editor's labels use it)
- the plugin's name -> `ATLAS_PLUGIN_NAME` in `CMakeLists.txt`; the nameplate's two words -> `UI/AtlasUi.h` (`UiCheck` checks they agree)
- every pixel of the interface -> `shared/ui`; the editor calls it and does not draw its own controls

## Gotchas already paid for

- A single tone's reduction measured on the whole output is less than the band's (its neighbours carry part of it): tests
  that check the law solo the band (`runSolo`).
- A tone inside a band is lower than on the whole signal (100 Hz is 3 dB down in a 70 to 160 Hz band): anything that sets a
  threshold has to read the band, not the input.
- `ui::Choice` and `ui::Toggle` are bound to one parameter for good, so every strip owns its controls (`AtlasEditor::Strip`).
- In an editor constructor call `setSize()` **last**.

## Conventions

As in the other plugins: Allman braces, a space before call parentheses, `juce::` qualified in headers; comments explain *why*;
claims in the README are backed by a test; commit by topic and end commit messages with the Co-Authored-By trailer.
