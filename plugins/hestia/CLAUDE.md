# CLAUDE.md

Hestia Space: a JUCE stem spacer (VST3 / AU / Standalone). One instance sits on each stem; the instances of a session see each other's spectra
through a table in the plugin binary, and each cuts only its own stem, in thirty third-octave bands, where a stem with a bigger claim is playing, and
puts it back from the front with distance cues (no reverb). A sidechain input brings in a track that has no Hestia on it as one more stem.
See `README.md` for what it does and what has been measured; this file is what you need to change it safely.

## Build and verify

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release     # JUCE: version in cmake/Juce.cmake, or third_party/JUCE at the repository root
cmake --build build --target HestiaTests -j 10 && ./build/HestiaTests_artefacts/Release/HestiaTests
cmake --build build --target Hestia_VST3 -j 10
cmake --build build --target HestiaUiCheck && ./build/HestiaUiCheck_artefacts/Release/HestiaUiCheck
cmake --build build --target HestiaSnapshot && ./build/HestiaSnapshot_artefacts/Release/HestiaSnapshot docs/screenshot.png
```

- Building the plugin targets does **not** install them (`HESTIA_INSTALL_AFTER_BUILD`, off by default).
- `HestiaTests`, `UiCheck` and `Snapshot` are `EXCLUDE_FROM_ALL`; build them by name. `Tests.cpp` has no JUCE in it: it includes only `Source/DSP`
  and `shared/dsp`'s Biquad.h, so it also compiles with `g++ -std=c++20 -I Source -I Tests -I ../../shared/dsp/include Tests/Tests.cpp`.
- There is no `HostCheck`; the plugin has not been built on macOS or run through `auval` (the repository's CLAUDE.md lists it under "Not yet in line").
- Definition of done for a DSP change: `HestiaTests` passes **and** the numbers the README quotes still match its output.
- Definition of done for a UI or parameter change: `UiCheck` passes and `docs/screenshot.png` is regenerated and looked at.

## Layout

```
Source/DSP/        no GUI dependencies, no JUCE; covered by Tests/Tests.cpp
  Roles.h            kBands (30), bandCentreHz(), the roles, their claims (anchors in Hz, claimAt(), claims())
  Bus.h              the table of 32 slots the instances publish to and read from; sharedBus() is the plugin binary's one
  Analysis.h         LevelBank: thirty fourth-order band-passes and their mean squares -> dB per band
  Carver.h           the cut: thirty peaking bells in series, and the inverse of their overlap
  Depth.h            the distance cues (cuesFor(): air, level, width, room) and EarlyReflections (eight taps up to 37 ms)
  Engine.h           the signal path and the rule (pressureOn()), the tick, the sidechain stem, the monitor, the output gain
Source/Advice.h      the notes of the strip under the knobs and the GUIDE text; plain C++, no JUCE
Source/Parameters.h  parameter layout, readParams()                                                     <- the single source of truth for parameters
Source/Plugin*.{h,cpp}, Source/UI/   JUCE wrapper and editor (the look is `shared/ui`); `UI/SpaceView.h` is the display
../../shared/dsp/    Biquad.h (the bells and the band-passes are `pdsp::Biquad`)
../../shared/ui/     the interface kit
Tests/               Tests.cpp (DSP), UiCheck.cpp (editor on the real processor), Snapshot.cpp (editor -> PNG), Check.h
```

## Rules that keep it correct

**Real time.** `Engine::process` and everything under it must not allocate, lock or log. Everything lives in fixed arrays; `prepare()` only designs
the filters and the overlap's inverse. A slot on the bus is claimed in `Engine`'s constructor (message thread, one compare-and-swap) and given back in
its destructor, never on the audio thread.

**The bus is relaxed atomics, on purpose.** A reader can see half a frame (some bands of this tick, some of the last): the levels are smoothed over
50 ms, so that is not a different frame in any way that matters. Do not add a lock or a seqlock to "fix" it; do not make a reader wait. A slot is
live while `now - beat < kFreshMs` (500 ms, unsigned subtraction: it survives the wrap of the 32-bit millisecond counter); a stem the host has
stopped calling (the transport, a muted track) is thereby forgotten. The time is an *argument* (`Engine::process`'s last, `Bus::publish`/`gather`):
the processor passes `juce::Time::getMillisecondCounter()`, a test passes a clock that runs with the audio.

**What is published is the input, never the output.** Each instance measures and publishes its stem *before* its own cut, so no instance's cut can
change what another sees of it: there is no feedback loop and no order the instances have to run in. Keep it so.

**Decentralised.** Every instance decides only about itself, from what the others publish and the shared rule (`pressureOn`, `claims`). There is no
master and no message from one to another asking for a cut. A new behaviour that needs one stem to tell another what to do does not belong here.

**The rule.** `pressureOn(mine, other, myLevel, otherLevel, reach)` = dominance (the other's claim over mine, by `kDominanceSpan` 0.5 it is all) x presence
(the other's level from -80 to -60 dB) x audibility (the other within `reach` dB under me, or louder). The pressures of the stems combine as shares of
the band (`free *= 1 - pressure`), and the cut wanted is `-depth x (1 - free)`. A stem with an equal or bigger claim is never cut by another: so a
stem that owns a band is not touched there, and two stems with equal claims leave each other alone (test [6]). Changing a constant changes what the
README quotes: run the tests and fix the numbers.

**The tick.** Everything that changes with time (the level reading, the publishing, the gathering, the cut's smoothing, the bells' coefficients,
the output-gain and monitor ramps) steps every `Engine::kTick` = 64 samples, counted from the start, and a segment never crosses a tick boundary.
That is what makes one stem's output the same for any host block size (test [8], bit for bit, with a neighbour on the bus that does not change).
A ramp is set to its target at the next tick, so a value at rest is exact (a gain of 1 is exactly 1; a monitor of 0 is exactly 0).

**The cut is bit-exact when it is none.** A bell of 0 dB is the identity, bit for bit, as long as the coefficients are computed the same way for the
numerator and the denominator (`setPeak`: `A = 1` makes `b == a`). `Carver::setTarget` sets a bell to exactly 0 dB when its gain is under 0.005 dB, and
`Engine::tick` sets the cut to exactly 0 when it has come back and its target is 0. Tests [4] and [5] pin both. Do not skip the bells that are at 0: they
keep their own state, and skipping them in and out would leave a stale state behind.

**Depth is cues, not a room.** `cuesFor(distance)` gives a high shelf at `kAirHz` (to -9 dB), a level (to -4 dB), a loss of the side signal (to 30%) and the gain of the
first reflections (0.45); `EarlyReflections` is four taps on each channel, summed through a one-pole at 3.5 kHz, each heard once. Nothing feeds back, so there is no tail and
nothing to decay; keep it so (test [13] checks 45 ms after an impulse is silence), and keep the direct sound undelayed. All of it is exactly nothing at distance 0: a shelf
of 0 dB is the identity (`b == a`, as for the bells), the width loss and the room gain are ramped to exactly 0 (the delay line runs all the time and its output is multiplied by that 0, so it adds exactly nothing). The room is fed the stem *after* the carve and *before* the shelf. The distance moves to its target over a few ticks (`distanceNow`) and the
shelf is redesigned only when it moved. A stem's distance is published with its role and importance, and `claims()` takes it: a stem at the back has up to `kDistanceYield` (35%)
less claim. The depth map in the display reads the published distances.

**The sidechain is one more stem.** `Engine::process`'s last two arguments (null: none) feed `scBank`, a second `LevelBank` that hears only the sidechain; at the tick it is
appended to the gathered stems as a `Stem` with `slot -1`, the role and importance of `scRole` / `scImportance`, and distance 0; it is only used while the mix is not Off. It is
latched per tick (`scFed > 0`), reset when it goes away (so it returns from clean filters), and published to the editor in `Engine::sidechainLevel`. It is never heard, it is not
on the bus (another Hestia cannot see it), and `linkedStems()` counts it. The bus is the processor's second input, off by default, and `sidechainConnected` is what the editor reads.

**The overlap is inverted.** The bells overlap (a third of an octave apart, Q 3.6), so setting each to its band's wanted gain over-cuts wherever
neighbours are cut too. `Carver::prepare` measures, for the sample rate, the dB each bell makes at each centre and inverts that matrix once; `setTarget`
multiplies the target by it. The result is the wanted curve *at the centres*; between them it is whatever the bells draw (`responseDb` reads it back). A
band the sample rate cannot reach (centre over 0.49 fs) is left alone and its row and column are the identity.

**The analysis is in the time domain on purpose.** `LevelBank` has no FFT: no block, no latency, no dependence on the host's buffer size, and no
spikes on the audio thread. The cost is selectivity (two sections of Q 4.3: three bands away is 20 dB under; the test pins it). A sine of amplitude A
on a band's centre reads `20 log10 A`: every threshold and the -80/-60 dB presence are in that unit.

**Bad samples.** An inf or NaN in the input is replaced by silence on the way in (`Engine::finite`).

**Monitoring is not state.** DELTA lives in `Engine::deltaOn` (an atomic), not in a parameter, and the editor ends it in its destructor. It fades
over a tick through `deltaMix`; the output is `wet - deltaMix x dry`, `wet` being the stem after the cut and the depth.

**Parameters.** Ids (`hestia::id`) are stored in sessions and automation: never rename or remove one once a release has them (`carve` was `depth` before the Depth knob existed and the plugin had not been released; the user-facing names are Carve for the cut and Depth for the distance, the ids `carve` and `distance`). All are `ParameterID` version 1; a new one needs
a higher version, an entry in `readParams()` and a place in the editor. The order of the roles (`Role`, `roleNames()`) and of the mixes (`mixNames`: the
index is the group, 0 off) is stored in sessions: add at the end, never reorder.

## DRY: where each thing lives

- the bands, the roles and their claims -> `Roles.h` (the engine, the display and the tests read them); the anchors are the only place a role is defined
- the rule and its constants -> `Engine.h` (`pressureOn`, `kDominanceSpan`, `kPresenceLoDb` / `kPresenceHiDb`); the depth's cues and constants -> `Depth.h` (`cuesFor`, `kAirHz`, `kAirMaxCutDb`, `kLevelMaxCutDb`, `kWidthMaxLoss`, `kReflectMax`, `kDistanceYield`)
- parameter ranges, defaults, choice lists -> `Parameters.h`; the defaults -> `EngineParams`
- what the plugin warns about and what GUIDE says -> `adviceFor()` and `guideText()` in `Source/Advice.h`; the editor fills `AdviceInput` (`adviceInput()`) and
  draws the notes with `ui::drawNoteStrip`. A note restates a limit the README's Limits documents: a new one needs it there, a test in UiCheck's Advice
  section, and at most `ui::kMaxNoteChars` characters
- the plugin's name -> `HESTIA_PLUGIN_NAME` in `CMakeLists.txt`; the nameplate's two words -> `UI/HestiaUi.h` (`UiCheck` checks they agree)
- every pixel of the interface -> `shared/ui`; the editor calls it and does not draw its own controls. What Hestia adds (the colour of a stem, the scales) is `UI/HestiaUi.h`

## Gotchas already paid for

- `kFloorDb` is -120 in `hestia::` (the bus's silence) and the display's -90 is `gui::kSpectraFloorDb`: do not shadow one with the other.
- All instances of one test process share `sharedBus()`; a UiCheck that makes processors puts the pair it measures in a mix of its own (H) and sets the
  rest to Off, or they would see each other. `HestiaTests` builds its own `Bus` for each scenario.
- A reader that gathers *every* live stem of a group also gets the ones other tests published: use a `Bus` of your own in DSP tests.
- In an editor constructor call `setSize()` **last**.
- `ui::Choice` and `ui::Toggle` are bound to one parameter for good.

## Conventions

As in the other plugins: Allman braces, a space before call parentheses, `juce::` qualified in headers; comments explain *why*;
claims in the README are backed by a test; commit by topic and end commit messages with the Co-Authored-By trailer.
