# Olympus as a basic DAW: roadmap and checklist

Working document, not user documentation. Delete it when the last box is ticked and move what stays true into `README.md` and `CLAUDE.md`.
Goal (from the user): turn Olympus from a stem host into a **basic DAW**: scan third-party plugins, grid, audio editing, a tempo system, markers, faders, panning, and the rest of a basic audio workflow.
Status: the **engine and the window are on the clip model** (steps 1, 3 and 4 are done). The mixer (step 2) is not.

## Decisions already made (keep them; they are what the code does)

- Third-party plugins are scanned and hosted (VST3 everywhere, AU also on macOS). `suiteNames()` still exists and a test still compares it with `plugins/*`. `NOTICE.md:41-47` has the VST3 and Audio Unit hosting note.
- Everything on the timeline is in **samples at the session's rate**. No time-stretching: audio does not follow tempo changes, the grid does.
- **Tempo**: `TempoMap` (`Source/Tempo.h`, plain C++): tempo points (beat in quarter notes, 20..300 bpm, first at beat 0) and one time signature for the project. `Grid` = Bar, 1/2, 1/4, 1/8, 1/16, 1/32.
- **Clip**: a piece of an audio file on a track (`start`, `offset`, `length`, `gainDb`, `fadeIn`, `fadeOut`, `mute`, shared `audio` buffer). Overlapping clips on a track are summed.
- **Track**: clips -> chain -> fader (`gainDb`) -> balance pan (the far side is attenuated; centre is unity) -> master. **Master** `masterDb`: applied to the mix only, never to the stems.
- Markers `{name, start}`, loop `loopStart/loopEnd/loopOn`, metronome, snap + grid are saved in the project (`kProjectVersion = 2`; a v1 file loads as one clip at 0).
- Meters: `Track::peakL/peakR` and `Session::masterPeakL/R`, max since last read; track meters are read by the UI with `exchange (0.0f)` (Arrange.cpp); the master peak is written by the audio path and not shown (no master meter yet).
- **Undo** is the `Editor` (`Source/Edit.h`): every timeline edit, track mix setting, master, tempo, marker and loop change goes through it; a drag or fader move is one `Editor::Gesture`. Adding or removing tracks or plugins is not undoable: the window calls `editor.clearUndo()` after them.

## Done (committed on `dev`)

- [x] **Plugin scanning** (`Catalog.h`, `Settings.h`): every VST3 (and AU on macOS), `KnownPluginList` + dead-man's pedal, blacklist, XML cache loaded at start, "Rescan plugins" in the PLUGINS menu, `Name (Maker)` for duplicate names, `scanSuiteOnly` setting.
- [x] **Tempo map and grid maths** (`Tempo.h`, `Tests/TempoTests.h`).
- [x] **Session model** (`Session.h`): clips, pan, master, meters, markers, loop, metronome, tempo, project v2, playhead info for plugins (bpm, ppq, time signature, loop), render of clips with pan and master, `captureEdit`/`restoreEdit`.
- [x] **Editor + undo** (`Edit.h`): split, move (also between tracks), trim, duplicate, delete, gain, fades, mute, rename, track mix, master, tempo, signature, loop, markers, snap, undo/redo with gestures. Tests [22]-[28] in `Tests/Tests.cpp`.
- [x] Fixes from review: `copyChain` all-or-nothing, render refuses when there is no audio / clamps the tail / refuses over 3 h, missing-audio clip length clamped, stale delay ring cleared when a track is skipped, `save()` holds the session lock.
- [x] Check: `scripts/build.sh olympus` (OlympusTests + OlympusUiCheck pass); `scripts/build.sh olympus snap app OlympusMcp` compile.

## To do, in this order

Each step is one agent (Haiku was used so far) with a precise brief; only one agent may build at a time. Read `Source/Session.h`, `Source/Edit.h`, `Source/Tempo.h` first; do not edit `shared/`.

1. [x] **ArrangeView** (done: `Source/UI/Arrange.h/.cpp`; in the window since step 3, its file drops go through `Editor::addClip`; its geometry, drags and splits are checked in UiCheck [9]; its menus and drags have not been tried by hand) (`Source/UI/Arrange.h/.cpp`, new, self-contained; tested in `Tests/UiCheck.cpp` on its own): header column per track (name, M/S, pan knob, fader, meter, FX button, remove), LOOP strip, ruler with bars/beats/grid lines and tempo flags, marker lane, clip lanes with waveforms, fades, missing-audio look, playhead, snapping, zoom and scroll. Clip mouse editing through `Editor` gestures: move (also to another track), trim edges, fade handles, right-click menu (split, duplicate, mute, gain, rename, delete); keys (Delete, Cmd+D, S, arrows); drop audio files on a lane.
   - Interface planned: `playhead`, `onSeek`, `onStructure`, `onTrackMenu`, `onFilesDropped` callbacks; `selected`, `refresh()`, `zoom`, `zoomToFit`, `scrollTo`, `followPlayhead`, `splitAtPlayhead`, `deleteSelected`, `duplicateSelected`, `addMarkerAtPlayhead`, `clipAt`, `clipBounds`.
2. [ ] **MixerView** (`Source/UI/Mixer.h/.cpp`): a channel strip per track (vertical fader with dB read-out, pan knob, M/S, stereo meter, the chain as slot buttons: click opens the plugin, shift-click bypasses, right-click menu, `+` adds a plugin) and a master strip (fader, meter). Move `LampButton`/`SlotButton` out of `MainComponent.h` into `Source/UI/Widgets.h`. Fader/pan writes go through `Editor` gestures.
3. [x] **Window integration** (done: `MainComponent` has the transport bar, `ArrangeView` in the body, an `Editor` member, the audio source wraps at the loop's end and stops at the end of the audio, ADD TRACK and IMPORT AUDIO, the keys, the project keeps tempo, markers and loop, the FX menu of a track, the GUIDE and the notes updated, the screenshot regenerated and looked at; the metronome is an undoable `Editor` step; not listened to on the Mac) — the brief was: transport bar (rewind, play/stop, loop on/off, metronome, BPM field, time signature, snap on/off, grid choice, undo/redo, zoom, ARRANGE/MIXER switch, position in bars.beats and time); the audio source wraps at the loop end (split blocks at `loopEnd`) and stops at the end of the audio otherwise; `Editor` member and `clearUndo()` after add/remove track, open, new; ADD TRACK (empty) and IMPORT AUDIO (clips at the playhead); keys (Space, Return, Cmd+Z, Shift+Cmd+Z, S, Delete, Cmd+D, L loop, a key for a marker); project open/save keeps tempo, markers, loop; follow the playhead. Replace `TrackRow`/`TrackList`/`WaveStrip`. Update `Advice.h` (the GUIDE text and "No plugin of the suite found" still describe the old stem host and the suite-only rule) and the 16:10 layout; regenerate the screenshots and look at them (root CLAUDE.md: a picture that shows the interface must be regenerated).
4. [x] **MCP tools** (done: 31 tools, test [29]; `Editor::addClip` added) for the new model (`Source/Mcp.h`): `add_clip`, `move_clip`, `split_clip`, `trim_clip`, `delete_clip`, `set_clip` (gain, fades, mute), `set_tempo`, `set_time_signature`, `add_marker`/`remove_marker`, `set_loop`, `set_master`; `set_track` already takes `pan`. Use the `Editor`. Keep the tool count in `tools()` and the README in step (it said 19 at step 4; it is 31 now).
5. [~] **Docs** (README, app CLAUDE.md, the hosting note is in NOTICE.md:41-47; the screenshot caption and GUIDE text were done with step 3): `README.md` (it still says "Not a DAW", "The suite only" philosophy was reworded for scanning but the feature table, screenshot caption and "no timeline editing" are old), `CLAUDE.md` of the app (layout list, "Not done yet" list, invariants for the Editor and the clip model), root `CLAUDE.md` (the Olympus mentions), `NOTICE.md` (the hosting note, lines 41-46).
6. [ ] **Review pass**: `code-checker` on the whole diff, fix chain at most two rounds (see root CLAUDE.md "Sub-agents").

## Known issues found in review, not fixed yet

- `renderStems` runs on the progress window's worker thread and calls `setBusesLayout`/`prepareToPlay`/`reset` on plugins there (`OfflineScope`); the invariant says only `processBlock` may run off the message thread. Pre-existing. Fix: prepare and restore on the message thread around the job, or amend the invariant.
- A plugin that hangs while being scanned blocks the window until it is quit (in-process scan); the next run blacklists it. There is no UI to switch `scanSuiteOnly` (it is set in `settings.xml`). The blacklist is cleared by PLUGINS > Forget blacklisted plugins.
- First scan with many Audio Units is slow (about two minutes for 98 plugins on the machine it was tried on).
- `Mcp.h` unused-variable warning and a lambda-capture warning in `MainComponent.cpp` are old.
- Checked only by the automated tests: nothing was listened to. Real-time playback with clips, the loop, the metronome, hosting a real third-party plugin and the plugin editors need a person on the Mac (say so in the commit that ships the window).
- No macOS CI job for Olympus; the real-plugin check (`OLYMPUS_PLUGIN_DIR`) was not run (no built suite bundle on the machine).

## How to continue

```bash
scripts/build.sh olympus            # OlympusTests + OlympusUiCheck, must pass
scripts/build.sh olympus snap app OlympusMcp   # all targets compile (snap rewrites docs/screenshot.png: look at it, or git checkout it)
```
Next action: step 2 (MixerView). The brief is the list above plus the interface; give the agent `Source/Session.h`, `Source/Edit.h`, `Source/Tempo.h`, `Source/UI/Arrange.h`, `Tests/UiCheck.cpp` and the kit headers to read first.
