# CLAUDE.md

Iris Prism: a JUCE dynamic equaliser (VST3 / AU / Standalone, macOS). Eight bands, each on Stereo / Left / Right / Mid / Side,
optionally dynamic; a Clean and a Sat mode; a Phase mode (Zero Latency, Linear, Linear HQ); Delta, Gain Match; solo for a band, for what a band adds, and for six frequency
regions. See `README.md` for what it does and what has been measured; this file is what you need to change it safely.

## Build and verify

```bash
# Required on this machine: the Command Line Tools ship an SDK that Xcode's linker cannot read.
export SDKROOT=$(xcrun --sdk macosx --show-sdk-path)

cmake -S . -B build -DCMAKE_BUILD_TYPE=Release     # JUCE: version in cmake/Juce.cmake, or third_party/JUCE at the repository root
cmake --build build --target IrisTests -j 10 && ./build/IrisTests_artefacts/Release/IrisTests
cmake --build build --target Iris_VST3 Iris_AU Iris_Standalone -j 10
cmake --build build --target IrisUiCheck && ./build/IrisUiCheck_artefacts/Release/IrisUiCheck
cmake --build build --target IrisHostCheck && ./build/IrisHostCheck_artefacts/Release/IrisHostCheck "build/Iris_artefacts/Release/VST3/Iris Prism.vst3"
cmake --build build --target IrisSnapshot && ./build/IrisSnapshot_artefacts/Release/IrisSnapshot docs/screenshot.png "Mono Bass, Wide Air" band=2
auval -v aufx Irsp Irsa                             # Apple's AU validator; the AU has to be installed (copy it, run, remove it)
```

- Building the plugin targets does **not** install them (`IRIS_INSTALL_AFTER_BUILD`, off by default); the check tools read the
  bundles from `build/`.
- `IrisTests`, `UiCheck`, `HostCheck` and `Snapshot` are `EXCLUDE_FROM_ALL`; build them by name. `IrisTests` needs no GUI.
- Definition of done for a DSP change: `IrisTests` passes **and** the numbers the README quotes still match its output.
- Definition of done for a UI or parameter change: `UiCheck` passes, the screenshots in `docs/` are regenerated and looked at
  (commands in the README), and `HostCheck` + `auval` after rebuilding the plugin.
- pluginval is not available; `HostCheck` + `auval` are the substitutes.

## Layout

```
Source/DSP/        no GUI dependencies; everything here is covered by Tests/Tests.cpp
  Band.h             BandType, Placement, BandParams, BandFilter (biquad or Butterworth cascade), bandResponseDb(): the curve the display draws
  Dynamics.h         Detector (band-matched filter + smoothed mean square) and dynamicOffsetDb(): the gain computer; DynamicsUsage (the editor's "never moves / always at full range" check)
  ResonanceFinder.h  narrow, lasting peaks in a spectrum (the display's markers); GUI-free, fed by the display's timer
  Masking.h          MaskingMeter: where this track and the sidechain (or another instance) mask each other (the display's clash zones); GUI-free, fed by the display's timer
  Peers.h            PeerRing / Peer / PeerRegistry / PeerListener: other Iris instances in the host process, found by track name; GUI-free
  ReferenceMatch.h   SpectrumProfile (the display's spectrum averaged on a 1/6-octave grid), Shape (a profile with the level taken out), the reference as text, match::matchBands; GUI-free, fed by the display's timer
  LinearPhase.h      LinearFilter: the linear-phase modes (kernels from the bands' |H|^2, FFT overlap-save, the delay line, the solo curve); PhaseMode, LinearSpec, latencyFor()
  Saturator.h        Shaper: three curves, residual ADAA, DC block
  Crossover.h        RegionSplitter (the shared pdsp::BandSplitter: six regions, flat sum), regionName()
  Engine.h           the whole signal path, the tick, solo, delta, gain match, meters, analyzer taps
Source/Advice.h      the notes of the strip under the panels (warnings that a setting does not suit its job or that what is heard is not the track) and the GUIDE text; plain C++, no JUCE
Source/Parameters.h  parameter layout, presets, readParams(), band helpers                       <- the single source of truth for parameters
Source/Plugin*.{h,cpp}, Source/UI/   JUCE wrapper and editor (the look is `shared/ui`; `UI/IrisUi.h` holds what is Iris' own; `UI/EqView.h` is the display)
../../shared/dsp/    Biquad.h (with the EQ designs), AnalyzerTap.h, SpectrumAnalyzer.h, BandLink.h (PASTE), MusicBands.h (the six bands and their splitter): shared with Hephaestus and Themis, namespace `pdsp`
Tests/               Tests.cpp (DSP), UiCheck.cpp (mouse events, editor panels), HostCheck.cpp (loads the VST3), Snapshot.cpp (editor -> PNG),
                     Check.h, TestSignals.h
```

## Rules that keep it correct

**Real time.** `Engine::process` and everything under it must not allocate, lock or log. `readParams` is called per block: look
parameters up through the cached `bandIds()` table and the `juce::String` ids in `iris::id` (a `const char*` would build a
`String` on the audio thread). Zero Latency needs no buffers of its own; the linear modes' are sized once in `prepare()` (below).

**Phase modes and latency.** In Zero Latency (`PhaseMode::zeroLatency`, the default) Clean and Sat add no latency and the bands run as
minimum-phase biquads in series. In the two linear modes (`linear`, `linearHq`; `LinearSpec` in `LinearPhase.h`) the bands are not run: `Engine::runLinear`
feeds the input to `LinearFilter`, which hands back the sound `latencySamples` (= half + hop) late, together with the input delayed by the same amount (it replaces
`dryL`/`dryR`, so Delta and Gain Match stay aligned) and the soloed band's curve. The latency is `Engine::latencySamples` (an atomic the audio thread writes when
`control()` sees a new mode) and reaches the host through `IrisProcessor::handleAsyncUpdate` from the message thread, as Hephaestus does with its oversampling; `prepareToPlay`
passes the saved mode to `Engine::prepare` so the host hears the right latency before the first block. Rules:
- **No allocation after `prepare()`.** `LinearFilter::prepare` sizes everything for the larger mode; a switch only clears and re-points (`start`).
- **Hops are counted in samples from the start**, like the tick, and a hop is a multiple of the tick (256, 1024), so the output is the same for any host block (test [15]).
  The kernels are rebuilt at a hop only if something moved (`kernelsDirty`, set wherever a filter is reconfigured, a band fades, starts or stops, or the mode changes);
  anything new that changes a band's curve has to set it too, or the linear modes will not follow it.
- **The curve comes from `BandFilter::powerAt`** (the same filter the display draws, from cos w and cos 2w, no trigonometry per bin), never from a second design. Clamp it at 0:
  rounding leaves -1e-17 at a high-pass's DC and `sqrt` of that is NaN.
- **Placements are a 2 x 2 matrix of kernels** (`LinearFilter::finishKernels`), not a kernel and then M/S on its output: a second kernel applied to the delayed signal would need
  its own delay (a zero-phase kernel wraps in the overlap-save frame). Do not "simplify" it back.
- **Without bands the filter is skipped**, and the block is copied out of the input history, so the output is the input delayed exactly (tests [15]); do not run the FFT for an
  identity. Detectors are fed the input in `runLinear`, because the bands are not run.
- **SAT is ignored in the linear modes** (the shaper is a nonlinearity on a band's difference, which a kernel does not have); the editor says SAT OFF.
- The FFT is JUCE's single-precision one: the floor is about -130 dB, and the README says so. The kernels are packed two to a transform (real and imaginary parts), as are left and right.

**Bands add their difference.** A band filters the signal it works on and adds `filtered - input` back to L and R
(Mid: to both, Side: to L and minus to R). That is what makes everything a band does not touch bit-exact (tests [1] and [4]).
Do not rebuild the stereo pair from filtered M and S: the round trip is exact in maths, not in floats.

**The tick.** Everything that changes with time (glides to new settings, the dynamics, coefficients, the fade of a band, gain match,
the smoothed output gain, `satAmount`) steps every `Engine::tick` = 32 samples, counted from the start of playback, and a segment never
crosses a tick boundary (`process` cuts the host's block there). That is what makes the output identical for any block size (test [9]).
Anything new that varies over time belongs in `control()`, interpolated per sample from `...Prev` to the new value, and nowhere
else. The loop in `process` steps by the samples it just did (`start += n`), not by `tick`: stepping by `tick` skips samples when a
block begins in the middle of one, and one-sample blocks hide it.
Within a tick, read what a band is doing from its `BandState` (`s.place`, `satStyle`), never from `params`: the host's new value may
arrive 16 samples into a tick, and the filters were set up for the old one until the next `control()` (test [10]). Anything cached
from a parameter (`appliedGain`, `appliedOrder`, the detector's filter) must be refreshed when that parameter changes, not only
when the gain or frequency does: a cut's slope and a band's DYNAMIC switch are such parameters. `reset()` forgets every cache,
including `appliedCrossover`, because `prepare()` may change the sample rate.

**A bell or shelf at 0 dB is not run.** It is the identity (its biquad's memory is zero), so it costs nothing until its gain, or its
dynamics, moves it (`BandState::active`). A cut is always run. A band that is off and has faded out is not processed either, and starts
from clean filter, detector and shaper history when it comes back (`startBand`).

**Dynamics.** The detector (`Detector`) measures *mean square* over about two cycles of the band's own frequency and reads it as the
peak of a sine, so the threshold means the same at any speed. Attack and release are **not** in the detector: they act on the gain in
dB, in `controlBand` (`dynDb`). Putting them in the power domain was tried: the gain engaged almost at once whatever the attack,
and a fast attack made the reading depend on the waveform. A dynamic band exists only for bells and shelves (`BandParams::isDynamic`).
AUTO (`bN_auto`, `AutoTimes` in `Dynamics.h`) replaces the two knobs with times counted in cycles of the band's frequency and two gains, fast and slow, the larger applied;
`dynFast` is kept apart from the applied `dynDb` so the slow gain cannot hold the fast one up. The detector listens to the band's *input*; never move it to the output, which would make a feedback loop.

**Sat.** The shaper works on the residual of the curve (`f(u) - u`) of the band's difference, with ADAA on that residual, and the
residual is added to the untouched difference (`Shaper`). Do not "simplify" this to plain ADAA on the signal: it low-passes it.
Only boosting bands saturate (`BandState::saturating`: effective gain > 0.05 dB); a cut that got shallower with level would be wrong.
`Drive` moves the knee, so with a boost far above the knee the boost itself is limited: that is the behaviour, not a bug (README, "How
Sat works"). The tube curve is asymmetric, so its residual is DC-blocked. Everything in the shaper is `double`.

**Low-frequency filters are `double`** (`pdsp::Biquad`): poles near z = 1 do not survive single precision at tens of Hz.

**Solo and Delta are monitoring, not state.** They live in `Engine::monitor` (atomics), not in parameters, so a host cannot record,
save or restore them, and the editor ends them in its destructor. Do not make them parameters. Order of the output stage:
band solo replaces delta; output gain; then the region solo (the region splitter listens to whatever is left). Each solo fades in and
out over 10 ms and its filters are reset when it starts; releasing one returns the output to the plain output bit for bit (tested).

**Region solo sums flat.** Every region goes through the all-pass of each crossover above it (`RegionSplitter`), so any combination adds
to the input's magnitude (test [8]). Do not "save" the compensation filters: soloing two regions at once would then bump at the crossover.
The crossovers are kept in order, at least a ratio of 1.2 apart, in `control()`; the display and the engine agree on that. Their defaults are `defaultCrossoverHz` in `Band.h`, taken from the six main bands of music in `shared/dsp` MusicBands.h (`pdsp::bands`), which Themis' AUDITION and Hephaestus' band solo strip use too; the splitter is the shared `pdsp::BandSplitter` (`RegionSplitter` is an alias). `xo0`, the sub / bass split, was added after `xo1` ... `xo4` (version 2): the crossover array is `xo0` ... `xo4` in that order, so a session from before it keeps the other four where they were.

**The sidechain is never heard.** The second input bus (`Sidechain`, off unless the host connects it) is pushed into
`IrisProcessor::sidechainTap` for the masking view and handed to `Engine::process` as its third argument, where it reaches one thing only: the
detectors of the dynamic bands whose `bN_listen` is Sidechain (`BandParams::external`, latched into `BandState::external` at a tick like the
placement; `feedSidechain`). It is never added to the output, and a band that is not dynamic never reads it (test [16], bit for bit). Both of its
channels go to the band's two detectors whatever the placement (`controlBand` takes the louder), so do not route it through `feedDetectors`:
a mono-in-one-channel key would vanish from a Side band. With no sidechain (`scIn[0] == nullptr`) `external` stays false and the band listens to
itself; a change of what it listens to resets the detectors. Non-finite samples are zeroed on the way in, as on the main input. `HostCheck` builds
its layouts with the sidechain bus in them; a layout with one input bus is not this plugin's.

**Other instances (`Peers.h`).** Every `IrisProcessor` owns a `Peer`, which registers itself in the process-wide `PeerRegistry`; the masking view (`EqView`, through a `PeerListener`) can read another instance's *output* instead of the sidechain. The rules:
- **The audio thread never takes the registry's mutex.** `Peer::publish` (called at the end of `processBlock`) stores the level and, only while `listening > 0`, copies the block into the `PeerRing` (one writer, many readers, relaxed atomics for the samples and release / acquire for the position; a reader more than half a ring behind skips ahead). Everything that takes the lock (constructor and destructor, which hosts may run on any thread; `setTrackName`; the editor's timer) is off the audio thread. `~Peer` tells the listeners under the lock, so a listener never holds a dangling pointer.
- **The ring is on the heap** (`std::make_unique`, 256 KB), and so is the view's `peerTap`: UiCheck's `main` keeps many processors on one stack frame, and a 256 KB member overflowed it (exit 139, no output, because stdout is buffered: run it under `lldb -b -o run`). A check that makes more processors goes in a function of its own, with the processors on the heap.
- **The choice is the project's, not the view's**: the label is the state property `maskSource` (`""` = the sidechain input); `EqView::syncMaskSource` follows it from the timer, so a host restoring a session changes the source too. A label is the host's track name (`updateTrackProperties`) or "Iris", with `(2)`, `(3)` in load order for equal ones, so it can change when a twin is unloaded. It is not a stable id, on purpose: a duplicated track carries the same state.
- **A peer that is not playing is fed as silence** (`-200` dB) into the meter: a host that stops calling `processBlock` would otherwise leave the last spectrum frozen and "masking" for ever.
- What is published is the output after the equaliser (and after a band solo or DELTA, if one is on: they are monitoring, and a peer soloing hears itself soloed).

**Display analysis is not audio analysis.** The resonance markers, the masking zones and the dynamics-usage check read the display's
analyzers on the editor's timer: nothing runs on the audio thread, they need the editor open, and their limits are rules of thumb
(named constants in their headers, quoted in the README; tests [11] to [14], and [17] for finding other instances). A band they suggest is made with `addBand` like any other.

**Reference and match.** Everything runs on the display's timer (`EqView`), never the audio thread: the input spectrum without the display's tilt is averaged into `liveProfile` (always) and `learnProfile` (while LEARN REF is on). The reference is a `Shape` kept in `apvts.state` as the properties `referenceCurve` / `referenceName` (so it is saved with the project like the display settings, with no processor code); a band with no level is `kNoLevelDb`, not 0, so a silent top of a lowpassed track is not matched. `match::matchBands` is pure and tested without a UI; `addExistingCurve` (Parameters.h) adds what the stereo bands already on do before matching, so matching twice does not stack bands. Do not make a match more eager than the smoothing and the Q limits: matching single notes was tried first and cut 11 dB notches into a bass line.

**Paste.** `pasteBands` in `Parameters.h` turns the text other plugins copy (`pdsp::link`, `shared/dsp/BandLink.h`) into bands; the
placement order there is the same as `Placement`'s. A harmonic-fill band is skipped (it is Hephaestus'). A `curve` line in the same text is the reference (`pasteReference`).

**Bad samples.** An inf or NaN in the input is replaced by silence on the way in (`segment`): an IIR filter keeps a NaN for good.

**Parameters.** Global ids (`iris::id`, `phase` included: version 3) and band ids (`bandIds (i)`: `b1_on` ... `b8_release`, and `b1_listen` ... `b8_listen`, version 4, then `b1_auto` ... `b8_auto`, version 5 (default off so old sessions keep their times; `addBand` switches it on), declared after all the others so the first layout keeps its order in a host's list) are stored in sessions and automation: never
rename or remove one. A new parameter needs a `ParameterID` version above 1 (every parameter in the first layout is 1), an entry in
`readParams()` and, if it has a control, a place in the editor. Session-wide controls go in `isGlobalParameter()` so presets leave them
alone. JUCE snaps a 0.1-step float parameter to a float grid, so "0.0" can arrive as 0.000002: anything that has to be exactly zero
(the output gain does) needs a tolerance, never `==`.

## DRY: where each thing lives (change it there only)

- parameter ranges, defaults, choice lists (`typeNames`, `placeNames`, ...) -> `Parameters.h` (the editor reads the
  items from the parameters); the crossover defaults -> `defaultCrossoverHz` in `DSP/Band.h` (`Parameters.h` and the engine use it)
- per-band parameter ids -> `bandIds()`; adding and removing a band -> `addBand`, `removeBand` (the display's double-click, its menu and the
  ADD BAND / REMOVE buttons all call them)
- which channels a placement touches -> `touchesLeft` / `touchesRight` / `touchesMid` / `touchesSide` in `Band.h`
- the curve of a band -> `BandFilter` (the engine runs it, the display draws it through `magnitudeAt`, the linear modes sample it with `powerAt`, the tests measure it: all agree)
- the phase mode's latency and sizes -> `LinearSpec` / `latencyFor()` in `DSP/LinearPhase.h` (the README, the editor and the tests read them; the choice list is `phaseNames` in `Parameters.h`)
- the plugin's name ("Iris Prism") -> `IRIS_PLUGIN_NAME` in `CMakeLists.txt`, which reaches the code as `JucePlugin_Name`; the nameplate's
  two words are `iris::gui::kProjectWord` / `kPluginWord` in `UI/IrisUi.h` (`UiCheck` checks they agree)
- what the plugin warns about (DELTA or a solo on, SAT in a linear mode, a Side bass boost, a big boost without GAIN MATCH...) and what GUIDE says -> `adviceFor()` and `guideText()` in `Source/Advice.h`; the editor only fills `AdviceInput` (`adviceInput()`) and draws what comes back with `ui::drawNoteStrip`. A note restates a limit the README documents: a new note needs it there, a test in UiCheck's Advice section, and at most `ui::kMaxNoteChars` characters
- placement colours -> `iris::gui::forPlacement()` in `UI/IrisUi.h`
- every pixel of the interface and the parameter-bound controls -> `shared/ui`, shared with the other plugins (see the repository's
  [CLAUDE.md](../../CLAUDE.md)); the editor calls it and does not draw its own. The log-frequency axis is `ui::FreqAxis`.
- biquad designs, the analyzer -> `shared/dsp`, shared with Hephaestus: a change there is a change for both (run `HephaestusTests` too); `Biquad::powerAt` was added for the linear modes
- test signals and `check()` -> `Tests/TestSignals.h`, `Tests/Check.h`

## Gotchas already paid for

- `createEditor()` returns a `ui::ScaledEditor<IrisEditor>` (shared/ui Scaled.h), not the editor: the window opens at 100% and scales. Tests and snapshots reach the editor with `ui::innerOf<IrisEditor> (*processor.createEditor())`; the editor's own size is its design size, the one it sets in its constructor.
- In an editor constructor call `setSize()` **last**; `resized()` runs inside it and only positions controls that exist.
- A choice (`ui::Choice`) or a switch (`ui::Toggle`) is bound to one parameter for good; a knob (`ui::Knob`) can be re-bound. That is why
  each band has its own type / placement / slope / dynamic-mode box and dynamic switch (only the selected band's is shown) while the
  knobs are shared and re-bound in `refreshBandPanel()`. The panel is driven by `bandSignature()` polled from the timer (selection, which
  bands exist, their types, whether they are dynamic): add state that changes what it shows to the signature.
- A hosted VST3's parameters are wrapper objects: find them by name, not `dynamic_cast<RangedAudioParameter*>`. Its editor is a native
  view, so editor screenshots instantiate `IrisProcessor` directly (`Snapshot.cpp`).
- In `HostCheck`, change the bus layout with `setBusesLayout` (and put it back), never with `setPlayConfigDetails` alone: the plugin's buses
  stay as they were, the host then sends the wrong number of channels, and JUCE's own wrapper overruns its scratch buffer. Symptom: a
  crash with a different backtrace every time, in whatever allocates next (the FFT setup, `Desktop::Desktop`, a `ValueTree` destructor).
  Apple's Guard Malloc (`DYLD_INSERT_LIBRARIES=/usr/lib/libgmalloc.dylib`) names the real site.
- `HostCheck` randomises every wrapper parameter, the host's own **Bypass** included; clear it before anything that measures audio.
  Its `test::failures` is global, so one early failure also fails the later "all of them" checks: read the first `[FAIL]`.
- `runDispatchLoopUntil` needs `JUCE_MODAL_LOOPS_PERMITTED=1` in the test targets.
- `Component::hitTest (int, int)` is virtual: don't name a one-argument helper `hitTest`. The warning flags reject hiding it.
- The digital filters are the analogue ones with the frequency axis warped (bilinear transform): a test that expects the analogue
  attenuation near Nyquist is wrong; use the prewarped formula (test [3]).
- A bell's contribution `H - 1` is not small far from its centre: it has phase, so a +6 dB Q = 1 bell still adds about -12 dB of the
  signal two octaves away. Test the *curve solo* of a band with a narrow bell, or far away.

## Conventions

- Match the surrounding JUCE style: Allman braces, a space before call parentheses, `juce::` qualified in headers.
- Comments explain *why* (a measured failure, a constraint), not what the code does.
- Claims in the README must be backed by a test or a measurement; re-run the suite and update the numbers.
- Commit by topic (DSP / plugin / UI / tests / docs / build); end commit messages with the Co-Authored-By trailer.
