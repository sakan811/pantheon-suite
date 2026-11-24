# CLAUDE.md

Iris Prism: a JUCE dynamic equaliser (VST3 / AU / Standalone, macOS). Eight bands, each on Stereo / Left / Right / Mid / Side,
optionally dynamic; a Clean and a Sat mode; a Phase mode (Zero Latency, Linear, Linear HQ); Delta, Gain Match; solo for a band, for what a band adds, and for six frequency
regions. See `README.md` for what it does and what has been measured; this file is what you need to change it safely.

## Build and verify

Build, test and snapshot commands live in [README.md](README.md) (`## Build & develop`); shared defaults and `scripts/build.sh` live in [../../CLAUDE.md](../../CLAUDE.md) (`## Building and checking`, `## Module conventions & defaults`).

- After rebuilding the plugin, also run `IrisHostCheck` on `build/Iris_artefacts/Release/VST3/Iris Prism.vst3` and `auval -v aufx Irsp Irsa` (`pluginval` is not available).

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
Source/Parameters.h  presets, band helpers
Source/UI/           `UI/IrisUi.h` (Iris' own words/colours), `UI/EqView.h` (display)
../../shared/dsp/    Biquad.h (with the EQ designs), AnalyzerTap.h, SpectrumAnalyzer.h, BandLink.h (PASTE), MusicBands.h (the six bands and their splitter): shared with Hephaestus and Themis, namespace `pdsp`
Tests/               HostCheck.cpp (loads the VST3), TestSignals.h
```

## Rules that keep it correct

**Parameters & buffers.** `readParams` is called per block: look
parameters up through the cached `bandIds()` table and the `juce::String` ids in `iris::id` (a `const char*` would build a
`String` on the audio thread). Zero Latency needs no buffers of its own; the linear modes' are sized once in `prepare()` (below).

**Phase modes and latency.** In Zero Latency (`PhaseMode::zeroLatency`, the default) Clean and Sat add no latency and the bands run as
minimum-phase biquads in series. In the two linear modes (`linear`, `linearHq`; `LinearSpec` in `LinearPhase.h`) the bands are not run: `Engine::runLinear`
feeds the input to `LinearFilter`, which hands back the sound `latencySamples` (= half + hop) late, together with the input delayed by the same amount (it replaces
`dryL`/`dryR`, so Delta and Gain Match stay aligned) and the soloed band's curve. The latency is `Engine::latencySamples` (an atomic the audio thread writes when
`control()` sees a new mode); `prepareToPlay`
passes the saved mode to `Engine::prepare` so the host hears the right latency before the first block. The sizes scale with the sample rate (`sizeScaleFor`: x2 over 50 kHz, x4 over 100 kHz; `linearSpec (mode, rate)`, `latencyFor (mode, rate)`, defaults 48 kHz), so 44.1 / 48 kHz are unchanged. `pdsp::BypassDelay` is sized by `maxLatencyFor (rate)` in `prepareToPlay`. Rules:
- **No allocation after `prepare()`.** `LinearFilter::prepare` sizes everything for the larger mode; a switch only clears and re-points (`start`).
- **Hops are counted in samples from the start**, like the tick, and a hop is a multiple of the tick (256, 1024), so the output is the same for any host block (test [15]).
  The kernels are rebuilt at a hop only if something moved (`kernelsDirty`, set wherever a filter is reconfigured, a band fades, starts or stops, or the mode changes);
  anything new that changes a band's curve has to set it too, or the linear modes will not follow it. `runLinearHop` rebuilds the main kernels when a band is working **or a curve solo is on** (`processHop` takes the filtering path for either; with no band working the kernels are the plain delay): test [15] pins the fade of a CURVE solo of a flat band.
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
`Drive` moves the knee, so with a boost far above the knee the boost itself is limited: that is the behaviour, not a bug (README, `### Signal flow & phase modes`). The tube curve is asymmetric, so its residual is DC-blocked. Everything in the shaper is `double`.

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
placement order there is the same as `Placement`'s. A harmonic-fill band is skipped (it is Hephaestus'). Each gain is multiplied by PASTE AMOUNT (`pasteAmount` / 100, read inside `pasteBands`; 100 % is exact, so it is bit for bit the old paste); frequency and width are not. A `curve` line in the same text is the reference (`pasteReference`).

**Parameters.** Global ids (`iris::id`, `phase` included: version 3; `pasteAmount`, PASTE AMOUNT: version 2, session-wide, read only by `pasteBands`, so it has no `readParams` entry) and band ids (`bandIds (i)`: `b1_on` ... `b8_release`, `b1_listen` ... `b8_listen`, version 4, and `b1_auto` ... `b8_auto`, version 5, default off so old sessions keep their times while `addBand` switches it on, declared after all the others so the first layout keeps its order in a host's list). Session-wide controls go in `isGlobalParameter()` so presets leave them alone (Iris owns its own preset code rather than `shared/ui/Presets.h`). JUCE snaps a 0.1-step float parameter to a float grid, so "0.0" can arrive as 0.000002: anything that has to be exactly zero (the output gain does) needs a tolerance, never `==`.

## DRY: where each thing lives (change it there only)

- crossover defaults -> `defaultCrossoverHz` in `DSP/Band.h` (`Parameters.h` and the engine use it)
- per-band parameter ids -> `bandIds()`; adding, removing and clearing bands -> `addBand`, `removeBand`, `clearBands` (the display's double-click, its menu, Delete and Backspace, and the ADD BAND / REMOVE / CLEAR ALL buttons all call them; a solo on a band that goes off ends with it: `EqView::keyPressed` ends it at once, every other way of removing a band — REMOVE, the menu, CLEAR ALL, UNDO — is caught by `EqView::timerCallback`)
- which channels a placement touches -> `touchesLeft` / `touchesRight` / `touchesMid` / `touchesSide` in `Band.h`
- the curve of a band -> `BandFilter` (the engine runs it, the display draws it through `magnitudeAt`, the linear modes sample it with `powerAt`, the tests measure it: all agree)
- the phase mode's latency and sizes -> `LinearSpec` / `latencyFor()` in `DSP/LinearPhase.h` (`phaseNames` in `Parameters.h`)
- placement colours -> `iris::gui::forPlacement()` in `UI/IrisUi.h`
- log-frequency axis -> `ui::FreqAxis` in `shared/ui`; biquad designs & analyzer -> `shared/dsp` (shared with Hephaestus: run `HephaestusTests` too; `Biquad::powerAt` was added for the linear modes)
- test signals and `check()` -> `Tests/TestSignals.h`, `Tests/Check.h`

## Gotchas already paid for

- `ui::Choice` and `ui::Toggle` are bound to one parameter for good, while `ui::Knob` can be re-bound: each band has its own type / placement / slope / dynamic-mode box and dynamic switch (only the selected band's is shown) while the knobs are shared and re-bound in `refreshBandPanel()`. The panel is driven by `bandSignature()` polled from the timer (selection, which bands exist, their types, whether they are dynamic): add state that changes what it shows to the signature.
- A hosted VST3's parameters are wrapper objects: find them by name, not `dynamic_cast<RangedAudioParameter*>`. Its editor is a native view, so editor screenshots instantiate `IrisProcessor` directly (`Snapshot.cpp`).
- In `HostCheck`, change the bus layout with `setBusesLayout` (and put it back), never with `setPlayConfigDetails` alone: the plugin's buses stay as they were, the host then sends the wrong number of channels, and JUCE's own wrapper overruns its scratch buffer. Symptom: a crash with a different backtrace every time, in whatever allocates next (the FFT setup, `Desktop::Desktop`, a `ValueTree` destructor). Apple's Guard Malloc (`DYLD_INSERT_LIBRARIES=/usr/lib/libgmalloc.dylib`) names the real site.
- `HostCheck` randomises every wrapper parameter, the host's own **Bypass** included; clear it before anything that measures audio. Its `test::failures` is global, so one early failure also fails the later "all of them" checks: read the first `[FAIL]`.
- `Component::hitTest (int, int)` is virtual: don't name a one-argument helper `hitTest`. The warning flags reject hiding it.
- The digital filters are the analogue ones with the frequency axis warped (bilinear transform): a test that expects the analogue attenuation near Nyquist is wrong; use the prewarped formula (test [3]).
- A bell's contribution `H - 1` is not small far from its centre: it has phase, so a +6 dB Q = 1 bell still adds about -12 dB of the signal two octaves away. Test the *curve solo* of a band with a narrow bell, or far away.
