# CLAUDE.md

Orpheus Pitch Editor: a JUCE standalone app (macOS) that edits the pitch of a solo vocal offline, note by note. It reads a recording, finds the
notes, lets the user move them, and rebuilds the audio with pitch-synchronous overlap-add. See `README.md` for what it does and what has been measured; this
file is what you need to change it safely. It is an **app**, so it lives in `apps/`; the dependency rule of the root CLAUDE.md applies to it as to a plugin
(`apps/*` -> `shared/*`, never the reverse, never to a plugin). It uses `shared/ui` and none of `shared/dsp`.

## Build and verify

```bash
# Required on this machine: the Command Line Tools ship an SDK that Xcode's linker cannot read (also for plain clang++ in a scratch file).
export SDKROOT=$(xcrun --sdk macosx --show-sdk-path)

cmake -S . -B build -DCMAKE_BUILD_TYPE=Release     # JUCE: version in cmake/Juce.cmake, or third_party/JUCE at the repository root
cmake --build build --target OrpheusTests -j 10 && ./build/OrpheusTests_artefacts/Release/OrpheusTests
cmake --build build --target OrpheusUiCheck -j 10 && ./build/OrpheusUiCheck_artefacts/Release/OrpheusUiCheck
cmake --build build --target Orpheus -j 10
cmake --build build --target OrpheusSnapshot && ./build/OrpheusSnapshot_artefacts/Release/OrpheusSnapshot docs/screenshot.png select=3
```

- `Source/Dsp/` is plain C++ (no JUCE), so `Tests/Tests.cpp` also compiles with a bare `clang++ -std=c++20 -O2 -ISource -ITests Tests/Tests.cpp` (with `SDKROOT` set):
  much faster than CMake when iterating on the signal processing.
- `OrpheusTests`, `OrpheusUiCheck` and `OrpheusSnapshot` are `EXCLUDE_FROM_ALL`; build them by name.
- Definition of done for a DSP change: `OrpheusTests` passes **and** the numbers the README quotes still match its output. For a UI change: `OrpheusUiCheck`
  passes and the screenshots in `docs/` are regenerated and looked at. A change to `shared/ui` (this app added `ValueKnob.h`) means building every plugin too.
- There is no listening test in the suite. Anything that depends on how it *sounds* (a different grain window, a different crossfade) has to be heard by a person; say so in the commit.

## Layout

```
Source/Dsp/          no JUCE; everything here is covered by Tests/Tests.cpp
  Pitch.h             MIDI <-> Hz, Contour (the detected pitch per 5 ms frame)
  PitchAnalysis.h     PitchAnalyzer: decimate, YIN candidates, Viterbi over the whole file, full-rate refinement
  Segmenter.h         the cut into notes: exact piecewise-constant fit on the vibrato-free pitch, boundary refinement, loudness dips
  NoteModel.h         Note (what is edited: target, drift, vibrato, formant, gain), steadyCentre
  Scales.h            scales, Key (snap, grid offset), detectKey, detectTuning, detectKeyAuto
  Edits.h             snap / split / merge on a list of notes
  Plan.h              notes -> numbers per frame (planned pitch, formant, gain, "needs resynthesis"): the only thing the renderer reads of the notes
  Psola.h             findMarks (once per file), render (the stretches an edit touches), matchLoudness, applyGain
Source/Session.h     the recording and everything done to it: audio, contour, marks, notes, key, plan, output, undo, "unsaved" (dirty), project XML, fingerprint, WAV export, output peak, the voice ranges
Source/Playback.h    the audio-thread reader of the session (corrected / original, loop, reference tone)
Source/UI/           Advice.h (the notes of the strip at the foot and the GUIDE text; plain C++, no JUCE), OrpheusUi.h (words, note text, verdict colours), PitchRoll.h (the display and its mouse), MainComponent.{h,cpp} (window, panels, keys, files, analysis thread), Recent.h (the list of recent files)
Source/Main.cpp      the application and its window
../../shared/ui/     the look (and ValueKnob.h, the knob for a value that is not a plugin parameter, added for this app)
Tests/               Tests.cpp (DSP and what the engine refuses), UiCheck.cpp (session, player, window), Snapshot.cpp (window -> PNG), TestSignals.h (the synthetic singer), Check.h
```

## Rules that keep it correct

**One direction of data.** `Contour` (found once) + `notes` (edited) -> `Plan::build` -> `Psola::render` -> `Session::output`. Nothing downstream reads the notes
directly and nothing upstream knows about the plan. A new control is a field on `Note`, a use of it in `Plan::build`, a knob in `MainComponent`, and a line in
`toXml` / `applyXml` and `Note::identicalTo` (undo compares notes with it: a field missing there is an edit that undo cannot see).

**Untouched is bit-exact.** A frame is `active` (resynthesised) only if its planned shift is above `Plan::activeCents` or its formant moved; everything else is copied
from the source. Do not "simplify" this to running the PSOLA everywhere with a ratio of 1: it would change every sample of the take and the tests [3] and the UiCheck
would (rightly) fail. Gain is a separate multiplication (`applyGain`), not a reason to resynthesise.

**The pitch out is the pitch planned, by construction.** `Psola::stretch` spaces the output marks by the *plan's* pitch (`plan.outMidi`), not by a ratio to the detected
pitch, so an error in the detector cancels (the plan is `detected + shift`, and the shift was computed from the same detected value). Keep it that way: a ratio-based
spacing would make every tracker error an error in the output. The input marks only choose which grain to take.

**A stretch is rebuilt whole.** `Psola::render` grows the dirty range to whole stretches of active frames and returns the range it actually rebuilt; the caller
(`Session::renderRange`) copies exactly that range. A stretch that is rebuilt from different starting points gives different grains, so partial and full renders agree
only because both start a stretch at the same mark (test [6], UiCheck [1]). The start mark must belong to the **same voiced run** as the stretch (within one and a half
periods of its start), never the last mark of an earlier run across a gap: that bug left every note after a gap unshifted, and only a test with many onset phases caught it.

**Loudness is matched, then Gain is applied.** `matchLoudness` brings the energy of a rebuilt stretch back to the input's over 24 ms; without it, re-spaced grains lose up to 9 dB
at +5 semitones. It runs inside `stretch`, before `applyGain`.

**Marks need room.** `markRun` starts a run at least one period into the file (its template window must fit). A run at the very start of a file used to get one mark and no render.

**Segmentation settings have reasons** (`SegmenterSettings`): the pitch is averaged twice over 125 ms before the fit because once leaves 38 % of a 5.5 Hz vibrato; a piece
under 24 frames at the edge of a run is merged because the averaging runs out there and follows the vibrato; the loudness is measured over 16 ms (`Wlevel` in the
analyser) because 32 ms hides a re-articulation. Change one with the tests [2] open.

**Threads.** The `Session` is used from the message thread. The audio thread reads `source` and `output` only through `Playback`, under `Session::outputLock`; anything that
replaces or writes them (`adoptAudio`, `renderRange`) takes the same lock for the swap or the copy-in only (`adoptAudio` builds its copy before the lock), and the player reads the length *inside* it. Only the message thread writes `output`, so `exportWav` and `outputPeak` read it without the lock. `Session::analyse` is a pure function of copies, run on a
`juce::Thread` (`MainComponent::Job`); its result is adopted on the message thread, and a result is accepted only if its serial number is the current job's (a pointer compare
once let a stale result through when a new job was allocated at the old one's address).

**Undo is whole-state.** `pushUndo()` copies notes, key and glide; call it once before a change (at `onGestureStart` for a knob, at the first move of a drag), then change, then
`session.changed (firstFrame, lastFrame)`, which rebuilds the plan and the stretches in range. `undo` re-renders only the frames where the two note lists differ.

**A project file is notes plus where the audio is.** The audio is named by an absolute path, a path relative to the project (`Session::sourceOf` tries it first) and a fingerprint
(`Session::fingerprintOf`). `loadFile` reads the audio and checks the fingerprint *before* it touches the open session (a refused or unreadable file changes nothing, a running analysis
included); `applyXml` also refuses another fingerprint or frame count (an older project has no fingerprint), recomputes each note's centre from the contour so a target that was an exact pitch
stays one, sorts the notes, drops overlaps and holds every value to what the editor can make. A key written into the file keeps its old meaning (add attributes, never change one). Analysis is
deterministic: do not make it depend on anything that can differ between runs. The voice range (`Session::voiceRange`, saved as `range`) is part of what the notes are.

**"Unsaved" is a comparison, not a flag.** `Session::dirty()` compares notes, key and glide with the state at the last `markSaved()` (called by load, analysis, applyXml and every save), so undoing
back to the saved state is clean. Everything that would lose edits (Open, drop, Re-analyse, Quit) goes through `MainComponent::askToDiscard`, and the questions and errors go through the
replaceable `ask` and `reportError`, so the checks open no dialog.

**A note is held to two octaves of its sung pitch.** `Note::clampTarget()` (`kMaxShiftSemitones`, MIDI 0..127) is applied by every path that sets a target: a drag, the arrow keys, `applyToSelection`,
split and merge, and `applyXml`; `Psola::stretch` refuses a period ratio over `kMaxPeriodRatio` on its own. `Segmenter::segment` takes a cancel callback (the fit is quadratic in a voiced stretch);
`Session::analyse` passes the job's progress as it.

**Tuning is not believed without evidence.** `detectTuning` returns a strength; below `kTuningStrengthNeeded` the key detection ignores it and the DETECT A4 button refuses and says
so. Notes that are each off in their own way give a "reference" that is only rounding noise.

## DRY: where each thing lives (change it there only)

- scale list and masks, key snapping, the grid offset of a reference tuning -> `Scales.h` (the combo boxes, the display's rows and the engine all read it)
- the planned pitch of a frame -> `Plan::build` (the display draws `plan.outMidi`, the renderer spaces marks by it, the tests measure against it)
- what the app warns about and what GUIDE says -> `adviceFor()` and `guideText()` in `UI/Advice.h`; `MainComponent::adviceInput()` fills it and `ui::drawNoteStrip` draws it. The window's size is `MainComponent::kDesignWidth/Height` (the strip is part of it: tests and the snapshot use it too)
- the A4 range (the Tuning knob, a loaded project) -> `kMinA4Hz` / `kMaxA4Hz` in `Scales.h`; the voice ranges -> `analysisSettingsFor` in `Session.h`
- how a note is written ("A3 +12ct"), the time, the verdict colours -> `UI/OrpheusUi.h`
- the project's two words ("Orpheus", "Pitch Editor") -> `kProjectWord` / `kSecondWord` in `UI/OrpheusUi.h`
- every pixel of the interface and the knobs -> `shared/ui`; `PitchRoll` is the display (like Iris' `EqView`) and draws with the kit's colours and glass
- the synthetic singer, and the phrase the screenshots show -> `Tests/TestSignals.h`

## Gotchas already paid for

- `juce::String (x, 0)` is "as many decimals as it takes", not none: use `numberText()` (it printed "-33.9989ct").
- `juce::Font::getStringWidth` is gone in JUCE 8: `textWidth()` in `OrpheusUi.h`.
- `HardwareLookAndFeel` finds the lit colour of a button on the look and feel, not on the button, so `button.setColour (buttonOnColourId, ...)` does nothing: every lit switch is amber.
- `Button::setToggleState (x, sendNotification)` runs `onClick`; the key handlers rely on it. A `ComboBox` is different: `sendNotification` is *asynchronous* there, so a check that needs `onChange` now uses `sendNotificationSync`.
- `AlertWindow::showOkCancelBox` with a callback gives 1 for the first button and 0 for the second; the window's `ask` hides it. Anything that would open a dialog in a check goes through `ask` / `reportError`.
- Shift means "add to the selection" and "drag finely". Which one is known only at mouse-up, so a Shift-press on a selected note starts a drag and removes the note on release if nothing moved
  (`Drag::toggleOff`). Doing it on press made a fine drag of a selected note impossible.
- Mouse positions are whole pixels: a drag of half a row cannot be exactly half a semitone. Tests allow for it.
- A slider with a step rounds what it is given: the SHIFT knob shows whole cents, so `shiftKnob.getValue()` is within half a cent of the note's shift.
- In an editor constructor call `setSize()` last (`MainComponent` does): `resized()` runs inside it and only positions controls that exist.
- `AudioTransportSource` stops itself at the end of the source and resamples to the device; `Playback` keeps its own position (`Playback::position()`), which is what the display and the clock read.
- Screenshots need no display (`createComponentSnapshot`); a running app cannot be captured in the sandbox this was written in.

## Conventions

- Match the surrounding JUCE style: Allman braces, a space before call parentheses, `juce::` qualified in headers.
- Comments explain *why* (a measured failure, a constraint), not what the code does.
- Claims in the README must be backed by a test or a measurement; re-run the suite and update the numbers. Things measured by hand are labelled as such.
- Do not name other companies' products in the UI or in source comments (see the root CLAUDE.md).
- Commit by topic (DSP / app / UI / tests / docs / build); end commit messages with the Co-Authored-By trailer.
