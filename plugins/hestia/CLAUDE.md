# CLAUDE.md

Hestia Space: a JUCE stem spacer (VST3 / AU / Standalone). One instance sits on each stem; the instances of a session see each other's spectra
through a table in the plugin binary, and each cuts only its own stem, in thirty third-octave bands, where a stem with a bigger claim is playing, and
puts it back from the front with distance cues (no reverb). A sidechain input brings in a track that has no Hestia on it as one more stem.
See `README.md` for what it does and what has been measured; this file is what you need to change it safely.

## Build and verify

Build, test and snapshot commands live in [README.md](README.md) (`## Build & develop`); shared defaults and `scripts/build.sh` live in [../../CLAUDE.md](../../CLAUDE.md) (`## Building and checking`, `## Module conventions & defaults`).

- `Tests.cpp` has no JUCE in it (`Source/DSP` + `shared/dsp/Biquad.h`) and supports standalone `g++` DSP tests (including `-O3 -march=native` and `-ffast-math`, where tests [4], [5], [10], [13] pin FMA-sensitive zero cuts).

## Layout

```
Source/DSP/        no GUI dependencies, no JUCE; covered by Tests/Tests.cpp
  Roles.h            kBands (30), bandCentreHz(), the roles, their claims (anchors in Hz, claimAt(), claims())
  Bus.h              the table of 32 slots the instances publish to and read from; sharedBus() is the plugin binary's one
  Analysis.h         LevelBank: thirty fourth-order band-passes and their mean squares -> dB per band
  Carver.h           the cut: thirty peaking bells in series, and the inverse of their overlap
  Depth.h            the distance cues (cuesFor(): air, level, width, room) and EarlyReflections (eight taps up to 37 ms)
  Engine.h           the signal path and the rule (pressureOn()), SPREAD (spreadWeight, the banded table, spreadLevels), the tick, the sidechain stem, the monitor, the output gain
Source/RoleGuess.h   AUTO ROLE: the role a track name suggests (its keyword list and priorities), and `autoRoleOnName` (the rule the processor runs by itself); plain C++, the roles are `DSP/Roles.h`'s
Source/UI/SpaceView.h  the display (and `UI/HestiaUi.h` for stem colours and display scales)
../../shared/dsp/    Biquad.h (the bells and the band-passes are `pdsp::Biquad`)
```

## Rules that keep it correct

**Setup & bus lifetime.** Everything lives in fixed arrays; `prepare()` only designs
the filters and the overlap's inverse, and builds the claim table (`anchorsFor`'s function-local static, which allocates: it must not be first touched on the audio thread). A slot on the bus is claimed in `Engine`'s constructor (message thread, one compare-and-swap) and given back in
its destructor, never on the audio thread.

**The bus is relaxed atomics, on purpose.** A reader can see half a frame (some bands of this tick, some of the last): the levels are smoothed over
50 ms, so that is not a different frame in any way that matters. Do not add a lock or a seqlock to "fix" it; do not make a reader wait. A slot is
live while `now - beat < kFreshMs` (500 ms, unsigned subtraction: it survives the wrap of the 32-bit millisecond counter); a stem the host has
stopped calling (the transport, a muted track) is thereby forgotten. The time is an *argument* (`Engine::process`'s last, `Bus::publish`/`gather`):
the processor passes `juce::Time::getMillisecondCounter()`, a test passes a clock that runs with the audio.

**What is published is the input, never the output.** Each instance measures and publishes its stem *before* its own cut, so no instance's cut can
change what another sees of it: there is no feedback loop and no order the instances have to run in. Keep it so.

**Decentralised.** Every instance decides only about itself, from what the others publish and the shared rule (`pressureOn`, `claims`). There is no
master and no message from one to another asking for a cut. (COPY TO MIX is not that: it hands over *settings* once, which the receiver sets on its own parameters, never a cut or a decision; see below.) A new behaviour that needs one stem to tell another what to do does not belong here.

**The rule.** `pressureOn(mine, other, myLevel, otherLevel, reach)` = dominance (the other's claim over mine, by `kDominanceSpan` 0.5 it is all) x presence
(the other's level from -80 to -60 dB) x audibility (the other within `reach` dB under me, or louder). The pressures of the stems combine as shares of
the band (`free *= 1 - pressure`), and the cut wanted is `-depth x (1 - free)`. A stem with an equal or bigger claim is never cut by another: so a
stem that owns a band is not touched there, and two stems with equal claims leave each other alone (test [6]). Changing a constant changes what the
README quotes: run the tests and fix the numbers.

**The tick.** Everything that changes with time (the level reading, the publishing, the gathering, the cut's smoothing, the bells' coefficients,
the output-gain and monitor ramps) steps every `Engine::kTick` = 64 samples, counted from the start, and a segment never crosses a tick boundary.
That is what makes one stem's output the same for any host block size (test [8], bit for bit, with a neighbour on the bus that does not change).
A ramp is set to its target at the next tick, so a value at rest is exact (a gain of 1 is exactly 1; a monitor of 0 is exactly 0).

**The cut is bit-exact when it is none, because the bells that are none are not run.** A bell of 0 dB is the identity on paper (`setPeak` with `A = 1` makes `b == a`), but
`b1*x - a1*y` only cancels to exactly 0 if the compiler does not fuse it into a multiply-add, and any build for a machine with FMA does (CI's does; the plain local build did not,
which is how this went unseen until CI). So `Carver::setTarget` takes a bell whose gain is under 0.005 dB out (`active[j] = false`, its coefficients and state reset to the identity
and to rest) and `Carver::process` runs only the active ones; `Engine::tick` sets the cut to exactly 0 when it has come back and its target is 0. The same for the air shelf
(`airOn`, only while the depth is above 0). A bell that is the identity has no state, so starting from rest when it is wanted again is right. Tests [4], [5], [10] and [13] pin it:
run `HestiaTests` also with `-march=native` (the standalone `g++` build with `-O3 -march=native`, and `-ffast-math`): they must pass in all three.

**SPREAD changes the levels the rule reads, not the claims or the bus.** `Engine::tick` spreads each band's power to the six bands either side (`spreadLevels`, a banded sum over the table `spreadW` that `prepare()` fills once from `spreadWeight`) for its own level and for every other stem's, inside `if (! p.hold)`, after the gather and after `bus.publish`: the bus keeps what was measured, so no instance's SPREAD changes what another sees. It runs only when `p.spread > 0`; at 0 the code is the code that was, bit for bit (test [18]). The two slopes (`kSpreadDownDbPerBark` 25, `kSpreadUpDbPerBark` 10) are fixed approximations, say so wherever they are described; the reach is six bands per *source*, and the bank's leakage is itself a source (test [17] pins that a loud tone moves bands seven to ten away). Do not extend the reach or iterate the spread (one step, from the levels as they were): the cost is 13 weights a band and that is what keeps it bounded. The spread is a stem setting, so no preset lists it (a preset sets it back to 0%); it is not copied by COPY TO MIX.

**Depth is cues, not a room.** `cuesFor(distance)` gives a high shelf at `kAirHz` (to -9 dB), a level (to -4 dB), a loss of the side signal (to 30%) and the gain of the
first reflections (0.45); `EarlyReflections` is four taps on each channel, summed through a one-pole at 3.5 kHz, each heard once. Nothing feeds back, so there is no tail and
nothing to decay; keep it so (test [13] checks 45 ms after an impulse is silence), and keep the direct sound undelayed. All of it is exactly nothing at distance 0: the shelf is not run (`airOn`), and the width loss and the room gain are ramped to exactly 0 (the delay line runs all the time and its output is multiplied by that 0, so it adds exactly nothing). The room is fed the stem *after* the carve and *before* the shelf. The distance moves to its target over a few ticks (`distanceNow`) and the
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

**Monitoring is not state.** DELTA lives in `Engine::deltaOn` (an atomic), not in a parameter, and the editor ends it in its destructor. It fades
over a tick through `deltaMix`; the output is `wet - deltaMix x dry`, `wet` being the stem after the cut and the depth.

**HOLD** (`EngineParams::hold`, the parameter `hold`, version 2) skips the whole decision in `tick` (the rule, the follow, `carver.setTarget`), so `cut`, `cutShown` and the bells stay as they are; the level, the publishing, the gathering and the depth go on. With it off the code is the code that was, so every earlier test stands.

**AUTO ROLE by itself** runs once, in `HestiaProcessor::updateTrackProperties`, on the first non-empty name the host gives. It sets ROLE only when `autoRoleOnName` (RoleGuess.h, pure, tested in HestiaTests [16]) says so: ROLE is still `EngineParams().role` (Full Range) and no saved session was loaded (`sessionRestored`, set in `setStateInformation`), and the word is at least `kConfidentPriority`. It is one UNDO step ("AUTO ROLE") and it sets the parameter with a gesture, as the button does. What it set (`autoRoleSet`, `autoRoleTo`, `autoRoleFrom`) is read by the editor's `adviceInput`, and `adviceFor` says so only while ROLE is still that role. A saved session clears it. Do not let it run on the audio thread or on a second name: the first name only. Known limit: a user who sets ROLE to Full Range by hand, before the name arrives, is indistinguishable from a fresh instance.

**Track names and COPY TO MIX live on the bus.** A slot keeps `kNameChars` (24) characters of the host's track name in six relaxed atomic words (`Bus::setName` / `nameOf`, message thread; cleared on claim and release); `HestiaProcessor::updateTrackProperties` sets it, `SpaceView::tick` reads it (`stemLabel`, `thisLabel`: the name, else the role). `Bus::broadcast` writes carve, reach, attack and release with the sender's slot and group and moves a sequence number; every `Engine::tick` compares it with the one it has seen (`seenBroadcast`, taken at construction, so a new instance does not get an old copy) and, if it is for its group and not from itself, raises `broadcastPending`; `processBlock` then `triggerAsyncUpdate`s and `handleAsyncUpdate` (message thread) takes the values with `takeBroadcast` and sets the parameters with a gesture each. Two broadcasts at once may mix their numbers: a copy is a rare, deliberate click.

**UNDO and A / B.** `ab` uses `hestia::keepsParameter`, and `ab.loadFrom` runs before `replaceState` in `setStateInformation` so a session from before A / B opens on A. `setCurrentProgram` and COPY TO MIX (`handleAsyncUpdate`, its four settings) each begin one undo step.

**Parameters.** `hestia::id` (`carve` and `distance` are user-facing Carve and Depth). All are `ParameterID` version 1 except `hold`, version 2 (off; kept by presets) and `spread`, version 3 (0%, off; not kept by presets, see SPREAD above). The order of the roles (`Role`, `roleNames()`) and of the mixes (`mixNames`: the index is the group, 0 off) is stored in sessions: add at the end, never reorder.

## DRY: where each thing lives

- the bands, the roles and their claims -> `Roles.h` (the engine, the display and the tests read them); the anchors are the only place a role is defined
- the rule and its constants -> `Engine.h` (`pressureOn`, `kDominanceSpan`, `kPresenceLoDb` / `kPresenceHiDb`); SPREAD's slopes, reach and weights -> `Engine.h` (`kSpreadDownDbPerBark`, `kSpreadUpDbPerBark`, `kSpreadBands`, `spreadWeight`); the depth's cues and constants -> `Depth.h` (`cuesFor`, `kAirHz`, `kAirMaxCutDb`, `kLevelMaxCutDb`, `kWidthMaxLoss`, `kReflectMax`, `kDistanceYield`)
- defaults -> `EngineParams`; stem colours and scales -> `UI/HestiaUi.h`

## Gotchas already paid for

- `kFloorDb` is -120 in `hestia::` (the bus's silence) and the display's -90 is `gui::kSpectraFloorDb`: do not shadow one with the other.
- All instances of one test process share `sharedBus()`; a UiCheck that makes processors puts the pair it measures in a mix of its own (H) and sets the
  rest to Off, or they would see each other. `HestiaTests` builds its own `Bus` for each scenario.
- A reader that gathers *every* live stem of a group also gets the ones other tests published: use a `Bus` of your own in DSP tests.
