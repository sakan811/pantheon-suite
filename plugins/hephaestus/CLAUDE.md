# CLAUDE.md

Hephaestus Forge: a JUCE mastering saturator (VST3 / AU / Standalone, macOS). The user draws up to six
bands over the spectrum; each is either **Saturate** or **Harmonic Fill** (an adaptive, missing-fundamental
style harmonic generator). See `README.md` for what it does and the research behind it; this file is what
you need to change it safely.

## Build and verify

Build, test and snapshot commands live in [README.md](README.md) (`## Build & develop`); shared defaults and `scripts/build.sh` live in [../../CLAUDE.md](../../CLAUDE.md) (`## Building and checking`, `## Module conventions & defaults`).

- After rebuilding the plugin, also run `HephaestusHostCheck` on the installed VST3 bundle and `auval -v aufx Hphs Hpha` (`pluginval` is not available in the sandbox).
- Note: `COPY_PLUGIN_AFTER_BUILD TRUE` is unconditional (see `## Not yet in line` in [../../CLAUDE.md](../../CLAUDE.md)).

## Layout

```
Source/DSP/        no GUI dependencies; everything here is covered by Tests/Tests.cpp
  Band.h             BandMode, maxBands, effectiveRange(), harmonicReach(), harmonicSpan(): where a band really sits and where its harmonics land
  Waveshaper.h       curves + residual-ADAA shaper
  Saturator.h        user-placed saturate bands: band-pass, per-band oversample + shape, outputs only the distortion
  FundamentalEngine.h  Chebyshev "extend" (+ adaptive gain) + difference-tone "rebuild", GapDetector, harmonicsForTone()
  PartialTracker.h   the Clean fill's note follower: pitch bank + one integrator per channel + confidence gate (see "Clean fill" below)
  OctaveGenerator.h  the Octave fill: STFT peak synthesis, N - 1 samples of latency (see "Octave fill" below)
  Engine.h           BandParams, latency alignment, band mixing, M/S, gain match, delta, meters, analyzer taps
../../shared/dsp/  Biquad.h, AnalyzerTap.h, SpectrumAnalyzer.h (calibration pinned by test [10]), MusicBands.h (band solo strip; test [12b]), namespace `pdsp`, shared with Iris (and Themis)
Source/Parameters.h  presets, band helpers, legacy migration
Source/UserPresets.h the user's own presets: one XML file each (0..1 values by id, all but the session-wide ones) in `UserPresets::defaultFolder()`; the processor owns one, tests point it at a temp folder
Source/UI/HephaestusUi.h  words and mode colours
Tests/               HostCheck.cpp (loads the VST3), TestSignals.h
```

## Rules that keep it correct

**Setup.** Buffers are sized in `prepare()`.

**Latency.** Every path that is summed with another must pass through an `IntDelay` set to the oversampling
latency (`dryDelay`, `fundDelay`, `bypassDelay`). If you add a path, align it and extend test [2]/[8]. The latency is `std::atomic` in `Engine` (the audio thread sets it, the message thread reads it), the processor asks for `setLatencySamples` once per change (`requestedLatency`) and reports it as the tail (`getTailLengthSeconds`); `pdsp::BypassDelay` is sized with `Engine::getMaxLatency()`.

**Saturate bands add their distortion; they never replace the signal.** `Saturator::process` writes only what the
shapers add (`shapeBlock<Curve, false>`), and `Engine` sums that onto the clean, `bypassDelay`-aligned input. That
is what makes small-signal gain exactly 1 for any arrangement of overlapping bands (test [1]). Do not route the
clean signal through the band filters or the oversamplers, and do not go back to a complementary split.

**ADAA maths.** ADAA is applied to the *residual* (`f(u)` minus its tangent) and added to the untouched input, so
small-signal gain is exactly 1. Do not "simplify" this back to plain ADAA: it low-passes the wet path (about -5 dB at
15 kHz without oversampling). ADAA runs in `double`; keep the first-sample priming in `AdaaState` (without it a
non-zero bias clicks at start).

**Low-frequency filters are `double`.** Poles near z = 1 do not survive single precision at tens of Hz.

**Bands start and stop cleanly.** A band that is off and has faded out is not processed; when it returns its filters,
ADAA state, oversampler and fill engine are reset (`startBand`, `runFillBand`). The fade is one block. Band edges
glide towards their target so dragging doesn't zipper.

**Band solo is monitoring, not state.** `Engine::monitor` (atomics: band, and BAND or ADDS) is not a parameter, so a host cannot record or restore it, and the editor clears it in its destructor. It replaces the output (Mix and Delta) just before the output gain and fades over 10 ms; released, the output is the plain output bit for bit (test [12]). ADDS takes each band's own contribution from where it is made (`Saturator::process`'s `soloOut`, `runFillBand`'s capture) and lines the fill's up with `soloDelay`; the bands keep running while one is soloed, so nothing jumps on release. BAND is a band-pass over `effectiveRange` of the delayed dry signal. The band solo strip (`Monitor::regions`, a mask over `pdsp::bands`, the six main bands of music shared with Themis and Iris) runs after it, on whatever is heard and before the output gain, through the shared `pdsp::BandSplitter` (all six sum flat; it fades like the band solo and starts from clean filters; released, the output is bit for bit the plain output, test [12b]); the editor's strip lines each switch up with its band on the display's axis.

**Bad samples.** Non-finite inputs are zeroed at the top of `Engine::process` (test [9b]); a new path with memory needs no guard of its own as long as it reads the buffer after that loop.

**Fundamental engine.** Harmonic weights are exact only while the normalised amplitude `a` is ~1 (the k-th Chebyshev
harmonic scales as `a^k`), so the envelope release is deliberately slow (`6 / lo`). Shortening it costs accuracy at
the top harmonics (test [5]); lengthening it risks hanging tails (test [5b]). Rebuild's gap detector must stay
frequency-selective (`GapDetector`): comparing against the whole band, or correlating with the restored signal,
were both tried and failed.

**Clean fill.** `FundamentalEngine::Params::character == Character::clean` (the per-band `bN_character` parameter, 0 = Warm, which is how every session saved before it sounds) swaps only the EXTEND generator: `extendClean` builds each harmonic as `amplitude x Re (u^k)` from `PartialTracker`'s phase vector `u`, with the same `harmonic[]` weights, `usable[]`, band-limiting filter, `adaptGain` and `extGain` as Warm. Do not put a waveshaper (or anything that mixes components of the band) in that path: its whole point is that nothing but the tracked note makes harmonics. The tracker's pitch comes from a resonator bank, *not* from a frequency-locked loop: an FLL on its own was tried and does not lock to either of two notes. The integrator damping (`kSogiDamping`), the bank's Q and smoothing and the confidence thresholds trade onset speed against the non-harmonic residue; quicker settings were measured and were dirtier at the bottom of the band, so test [5g] (residue under -45 dB, 100 ms to 90 %, a quieter second note ignored) pins them. Rebuild is a squarer and is the same in both modes. A new Clean behaviour belongs in [5g] with its measured number in the README.

**Octave fill.** `Character::octave` swaps the EXTEND generator for `OctaveGenerator` (`extendOctave`): one FFT per quarter frame per channel, each resolved peak in the band becomes a sine at k times its frequency and phase written as the *window's own transform at that exact frequency*, inverse FFT, overlap-add. It has **latency**, `OctaveGenerator::latencyFor (fs)` = N - 1, and everything that sums with it must be brought to it: `Engine::applyLatencyFor (os, octave)` sets the total (`osLatency + octaveLatency`, only while `usesOctave (params)`, which depends on the parameters alone so it does not move while a knob does) and the delays: dry, bypass, plain fill bands and the solo fill by the total; octave fill bands (`octDelay`, `soloOctDelay`) by the oversampling's share only; the saturator's output (`satDelay`, `soloSatDelay`) by the octave share only; Rebuild inside an octave band by the generator's latency (`delayRestored`). A new path that sums with these belongs in that table, and in test [5h] (the engine part checks the paths bit for bit). The phase convention is the Chebyshev one (the harmonics' peaks line up with the fundamental's: harmonic k = cos (k phi) for a fundamental cos phi); keep it, or Warm / Clean / Octave stop agreeing. `extendOctave`'s guard (the band's own signal, held back by the latency, bounds the harmonics) is what removes the echo before and after a note: do not take it out without a replacement, test [5h] measures it. Nothing in it allocates after `prepare()`; the kernel is a table, not `sin()` per bin. Test [5h] pins the weights, the sample-exact phase and latency, the intermodulation, the dynamics, the note edges, the range, blocks, rates, Rebuild and the engine alignment; a change to the generator re-runs it and updates the README numbers.

**Adaptive fill.** `Amount` is the harmonics' total *energy* relative to the band (`harmonicsForTone` weights carry unit
energy, so `extGain` = amount and `target` = amount squared). Adapt measures the energy above the band
(`harmonicReach`) on the *input* and scales the harmonics by `(1 - adapt) + adapt * sqrt (shortfall)`. Keep the
measurement on the input: measuring the output would make it a feedback loop. Test [5c] pins the law; if you change
the reach, the time constants or the weights, re-run it and update the README numbers.

**Clean fill and a moving edge.** `FundamentalEngine::updateCoefficients` calls `PartialTracker::setup` every block while a band edge glides; `setup` keeps the pitch the tracker has locked to (it only clamps it into the new range) and centres it only the first time or at a new sample rate. Re-centring it each block lost the note while an edge was dragged (test [5g], the moving-edge case).

**Undo, A/B and presets.** A gesture of the plugin's own that moves several parameters (a preset, adding, duplicating or removing a band, a drag on the display) calls `ui::beginUndoStep` first so it is one step; `applyPreset` and `UserPresets::apply` write each parameter once with its final value, never "all defaults, then the preset" (Hephaestus owns its own preset code rather than `shared/ui/Presets.h`). User presets are files in the user's folder; the processor keeps only the name of the one chosen (`userPreset` in the state).

**Parameters.** `hep::id` and `bandIds (i)` (`b1_lo` ... `b6_rebuild`). Band parameters use `ParameterID` version 2; `bN_character` uses version 3. Session-wide controls go in `isGlobalParameter()` so presets leave them alone. `migrateLegacyState()` turns the very first layout (three fixed bands + one bass engine: `driveLow`, `fundLo`, ...) into bands, and `UiCheck` covers it. Never build a `juce::String` on the audio thread: look band parameters up through the cached `bandIds()` table.

## DRY: where each thing lives (change it there only)

- "off" thresholds -> `Parameters.h`
- how a fill band makes its harmonics (Warm / Clean / Octave) -> its `bN_character` parameter (`BandIds::character`, a choice, version 3), read into `Params::character` by `readParams()`; whether it asks for the octave latency -> `usesOctave()` in `Engine.h` (the processor passes it to `Engine::prepare`); the editor's selector is re-bound to the selected fill band in `refreshBandPanel()` and is not a knob slot (the six slots are full)
- per-band parameter ids -> `bandIds()`; adding / removing a band and switching its mode -> `addBand`, `removeBand`, `setBandMode`
  (the spectrum menu, the double-click menu and the band panel buttons all call these; Delete / Backspace in the spectrum calls
  `SpectrumView::keyPressed`, and removing a soloed band ends the solo there, `removeBandAndSolo`); CLEAR ALL -> `clearBands` (one undo step, none when no band is on; the editor ends the solo); bands pasted from another plugin
  (`pdsp::link`, `shared/dsp/BandLink.h`) -> `pasteBands` (fill bands only)
- what a fill band's ABOVE read-out shows -> `FundamentalEngine::currentReachDb()` (the same energies the adaptive fill compares), published per band as `Engine::bandReachDb`
- where a band really sits (minimum width, the 2 kHz fill limit), where its harmonics land (the zone the display shades) and which harmonics
  fit below Nyquist -> `effectiveRange()` / `harmonicSpan()` / `highestHarmonic()` / `harmonicReach()` (the adaptive measurement region) in `Band.h`
  (engine, spectrum view and tests all call them; test [5f] pins the span against measured energy)
- what a preset is for (its tooltip) -> `Preset::purpose` in `Parameters.h`, one line each; `UiCheck` fails a preset without one
- band colours -> `hep::gui::forMode()` in `UI/HephaestusUi.h`
- knob id / caption / tooltip / panel membership -> `knobSpecs` and `panelSpecs` (character, master) and `bandKnobSpecs` (the six slots that follow the selected band) in `PluginEditor.cpp`
- log-frequency axis -> `ui::FreqAxis` in `shared/ui` (`SpectrumView.h` only aliases it)
- test signals and `check()` -> `Tests/TestSignals.h`, `Tests/Check.h`

## Gotchas already paid for

- A hosted VST3's parameters are wrapper objects: find them by name, not `dynamic_cast<RangedAudioParameter*>`.
  Its editor is a native view, so editor screenshots must instantiate `HephaestusProcessor` directly (`Snapshot.cpp`).
- The analyzer uses Hann, so an off-bin sine can read up to 1.42 dB low (scalloping). That is expected, not a bug.
- `Component::hitTest (int, int)` is virtual: don't name a one-argument helper `hitTest` (it hides it and the plugin's
  warning flags reject that). Don't name a local `range` inside `SpectrumView` either; it shadows the analyzer setting.
- The editor's band panel is driven by `bandSignature()` polled from the timer (selection, which bands exist, their
  modes); knobs are re-attached when it changes. If you add state that changes what the panel shows, put it in the signature.
- `HostCheck` randomises every wrapper parameter, the host's own **Bypass** included; it must be cleared before anything
  that measures audio (it ends up engaged or not depending on how many parameters precede it, so adding parameters can
  silently flip it). Symptom: audio undelayed and latency "stuck" at whatever it was before. The same randomising leaves the plugin's own **Delta** and **Mix** wherever the last draw put them, so the latency section sets Delta, Gain Match and Mix by name, and every band's Character to Warm (an Octave band adds its generator's latency), before measuring, and the state round trip gives a stepped parameter (the wrapper's Program included) one of its own steps (`randomValue`): both failed after the Clean switch added six parameters, with a plugin that was fine, until they were made independent of the draw.
- A hosted VST3 wraps state in its own chunk, so `HostCheck` cannot feed it a hand-built session; legacy-session
  tests go through `HephaestusProcessor` directly (`UiCheck`).
