# CLAUDE.md

Atlas Span: a JUCE six-band compressor (VST3 / AU / Standalone). The bands are the six main bands of music (`shared/dsp`
MusicBands.h); each has a compressor on Stereo / L/R / M/S / Left / Right / Mid / Side, a solo, a delta and a gain match; the
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
Source/Parameters.h  parameter layout, readParams(), bandRangeHz()                    <- the single source of truth for parameters
Source/Plugin*.{h,cpp}, Source/UI/AtlasUi.h   JUCE wrapper and editor (the look is `shared/ui`)
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

**Latency** is `Engine::latencySamples` (linear split + oversampler), written on the audio thread in `configure()` when a tick sees a
new mode, and reaches the host through `AtlasProcessor::handleAsyncUpdate` on the message thread; `prepareToPlay` passes the saved
modes to `Engine::prepare` so the host hears the right latency before the first block. A mode switch clears the histories.

**Solo and delta are monitoring, not state.** They live in `Engine::monitor` (bit masks, atomics), not in parameters, and the
editor ends them in its destructor. Do not make them parameters. They fade over 10 ms (`heard`, `dryHeard`).

**Gain match** compares the band's power after its compressor with before it (0.4 s one-pole each, per sample), limited to
+-12 dB, recomputed per tick and ramped. It is measured before makeup, so MAKEUP adds to it.

**Bad samples.** An inf or NaN in the input is replaced by silence on the way in (`segment`).

**Parameters.** Ids (`atlas::id`, `bandIds (i)`: `b1_on` ... `b6_match`) are stored in sessions and automation: never rename or
remove one. All are `ParameterID` version 1; a new one needs a higher version, an entry in `readParams()` and a place in the editor.
The order of `channelNames` is the order of `Channels`.

## DRY: where each thing lives

- the six bands, their names, default edges and the zero-latency splitter -> `shared/dsp` MusicBands.h (shared: run the other plugins' tests too)
- parameter ranges, defaults, choice lists -> `Parameters.h` (the editor reads the items from the parameters); the defaults of a band -> `BandParams`
- which signals a channel mode runs -> `usesMidSide` / `runsFirst` / `runsSecond` / `isLinked` in `Compressor.h`
- the order of the crossovers -> `orderedCrossovers()` in `Engine.h` (the engine and the editor's labels use it)
- the plugin's name -> `ATLAS_PLUGIN_NAME` in `CMakeLists.txt`; the nameplate's two words -> `UI/AtlasUi.h` (`UiCheck` checks they agree)
- every pixel of the interface -> `shared/ui`; the editor calls it and does not draw its own controls

## Gotchas already paid for

- A single tone's reduction measured on the whole output is less than the band's (its neighbours carry part of it): tests
  that check the law solo the band (`runSolo`).
- `ui::Choice` and `ui::Toggle` are bound to one parameter for good, so every strip owns its controls (`AtlasEditor::Strip`).
- In an editor constructor call `setSize()` **last**.

## Conventions

As in the other plugins: Allman braces, a space before call parentheses, `juce::` qualified in headers; comments explain *why*;
claims in the README are backed by a test; commit by topic and end commit messages with the Co-Authored-By trailer.
