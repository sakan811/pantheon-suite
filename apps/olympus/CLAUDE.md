# CLAUDE.md

Olympus Session Host: a JUCE standalone app (macOS, builds on Linux for CI) that holds a timeline of clips on tracks (with a tempo map, markers, a loop and undo), a chain of plugins on each track, and writes the stems out; plus an MCP server over the same
session so an agent can place the chains. See `README.md` for what it does; this file is what you need to change it safely. It is an **app**, so it lives in `apps/`; the dependency rule of the root CLAUDE.md
applies (`apps/*` -> `shared/*`, never a plugin or another app). It **hosts** plugins, but only at run time: it loads VST3 bundles (and Audio Units on macOS) that its scan finds, and links none of their code, so no dependency on `plugins/*` exists in
CMake. Third-party plugins are scanned at run time; that was decided by the user. The one place it knows about the suite's own plugins is `suiteNames()` in `Source/Catalog.h` (their product
names), which a test compares with every `plugins/*/CMakeLists.txt`, and which the "suite only" setting (`scanSuiteOnly`) keeps.

## Build and verify

```bash
scripts/build.sh olympus                 # OlympusTests + OlympusUiCheck, built and run
scripts/build.sh olympus snap            # docs/screenshot.png and docs/screenshot-record.png (the pictures the README embeds)
scripts/build.sh olympus app             # the window;  scripts/build.sh olympus OlympusMcp  # the server
OLYMPUS_PLUGIN_DIR=$PWD/plugins/hermes/build scripts/build.sh olympus tests     # also hosts a real built plugin (VST3)
```

- Definition of done: both checks pass, the screenshot is regenerated and looked at when the window changed, and the README's claims (tool count, targets, behaviour) still match. A change in `shared/ui` means building every plugin and app.
- `OlympusTests` and `OlympusUiCheck` need no built plugin and no audio device: plugins are `test::GainPlugin` (`Tests/TestPlugin.h`, a gain with two presets and an optional latency). Anything that touches the
  real hosting path (scan, load, state, render of an actual plugin) is the `OLYMPUS_PLUGIN_DIR` check: run it by hand after touching `Catalog.h` and say so.
- `UiCheck` runs headless with no display: it replaces `makeEditorWindow` so no native window is opened (a native window under a bare X server aborts the process). JUCE's `Button::triggerClick` acts asynchronously: use `press()` there.
- There is no listening test. Real-time playback, the audio device and the plugin editors in their own windows are checked by a person on the Mac; say so in the commit.

## Layout

```
Source/Catalog.h      the scan (juce::KnownPluginList + PluginDirectoryScanner: VST3 bundles of the folders, AU on macOS; dead-man's pedal, blacklist), the cache (loadCache/saveCache), names and resolve, making instances; internal plugins for tests
Source/Session.h      tracks of clips, chains, fader/pan/mute/solo, master, markers, loop, metronome, tempo; real-time processBlock with latency compensation, offline renderStems, project XML (version 2), describe() (JSON for the server), captureEdit/restoreEdit (the undo state)
Source/Tempo.h        TempoMap (tempo points, one time signature, beat and sample maths) and Grid: plain C++, no JUCE
Source/Edit.h         Editor: the one place the timeline is edited and undone; a Gesture groups a drag into one undo step
Source/Record.h       Recorder (one 32-bit float WAV per armed track, written by a TimeSliceThread; push() from the audio thread), beginRecording (the armed tracks) and finishRecording (the takes as clips, one undo step)
Source/Mcp.h          the MCP server: JSON-RPC lines in and out, the 31 tools, parameters by id/name/index
Source/Settings.h     plugin folders, scanSuiteOnly and the last project, in one file shared by the window and the server; the plugin cache and the scan's pedal sit beside it
Source/Advice.h       the GUIDE text and the notes at the foot (rules only; the kit draws them)
Source/Main.cpp       the app and its 16:10 window;  Source/McpMain.cpp  the server's main
Source/UI/MainComponent.h / .cpp  the window: the project buttons, the transport bar and the top bar's device line; the audio device's callback (`DeviceCallback`: the inputs to the recorder, the transport `Source` to the outputs), REC and the takes, the AUDIO dialog and the IN menu, the actions, the keys, the FX menu of a track; owns `Editor editor`
Source/UI/Arrange.h / .cpp  ArrangeView, the arrangement in the window's body: track headers (name, FX, X, M, S, pan, fader, meter), ruler, loop and marker lanes, clip lanes, the playhead; its drags go through Editor gestures
Source/UI/                 LampButton, SlotButton, PluginWindow (SlotButton is kept, unused: the FX menu lists a track's plugins); the header of a track (in Arrange.cpp) has the R (arm) and IN (input) buttons, and its meter shows the input of an armed track while the transport is stopped or recording
Tests/                Tests.cpp (session, render, project, server, the Editor's edits [22]-[28]), TempoTests.h (tempo and grid, included by Tests.cpp), UiCheck.cpp, Snapshot.cpp, TestPlugin.h, Check.h (a copy of Orpheus' tiny helper: apps share nothing)
```

## Invariants

- **One Session, three users.** The window, the server and the tests all call `Session` (timeline edits through `Editor`, below); none keeps a copy of its state. Anything that changes tracks, clips or chains holds `Session::lock`; the audio callback only *tries* it (silence for that block if it cannot).
- **All tracks render together, block by block**, never one after another: Hestia's instances read each other through a bus in the plugin binary, so a track rendered alone would hear no neighbours. Keep it so.
- **A stem is aligned with its audio**: the chain's latency is taken off the front (and the run is longer by the longest latency); the real-time path delays the other tracks instead (`Track::delaySamples`). Test [4] and [5] pin both.
- **Stems are 32-bit float by default**, so they cannot clip and can be edited later; the mix is the sum of the written stems; a muted track (or one not soloed while another is) writes no stem.
- **A project never loses what it cannot use.** A missing plugin stays as a slot with its saved state (`Slot::savedState`), a missing audio file keeps the track and its path; both are `warnings`, never errors. A project from a newer version is refused.
- **The project format is versioned** (`version` on `OlympusProject`); add attributes, never change what one means, and ignore what is unknown, as Orpheus' file does.
- **The server writes only what it is told to** (a project path, a render folder) and prints nothing but protocol on stdout. Every tool failure is an `isError` result with a reason an agent can act on; keep the 31 tools in `tools()` and in README in step.
- **The monitor chain is playback only, never in a render, not in the project, saved in `monitor.xml`**: `Session::processMonitor` runs after `processBlock` on the audio callback's playback block and nowhere else; `renderStems` and `processBlock` never touch it (test [31]), and it is not an undo step.
- **Recording is real-time safe, its files are written by a thread, and a take lands minus the latency**: `Recorder::push` allocates nothing, takes no lock and does no file IO (the `ThreadedWriter`s do, on a `TimeSliceThread`); `stop()` waits for a push in flight before it closes a file; `finishRecording` puts each take at `startSample - latencyCompensation` (never before 0) as one undo step.
- **One audio path, the device's.** `MainComponent::DeviceCallback::deviceBlock` is what the device calls and what the checks call with their own buffers: the inputs go to `Recorder::push` first (always, so the meters work), then `Source::getNextAudioBlock` fills the outputs (it clears them first, so unused outputs are silent). `processMonitor` runs on the playback block after `processBlock`, as its invariant says. A device with no outputs plays nothing and its takes still get their inputs. `audioDeviceAboutToStart` (message thread, before the callbacks start) ends a running take, because a take cannot go on across a new rate or block size, and then prepares the recorder; `audioDeviceStopped` (device thread) only sets a flag that the timer reads and ends the take.
- **A take starts where the playhead was at its first block.** `deviceBlock` sets `Source::recordStart` from the position at the first block the recorder runs in (the block is then pushed, so the take and the position agree); REC clears it first. `stopRecording` places each take at `recordStart - latency`, with the latency fixed at REC (device input + output latency + `extraLatencySamples` + `Session::monitorLatency()`). While a take runs the loop does not wrap, the end of the audio does not stop it, `seek` is refused (the take is one continuous recording), and a track cannot be removed.
- **Plugins must be created and edited on the message thread.** The window and the server run on it; the render thread of the progress window only calls `processBlock`.
- **Everything is drawn from the kit** (`shared/ui`): `LampButton` exists only because the kit's look-and-feel lights every switch in one colour; if the kit grows per-button colours, use them and delete it. The window is 16:10 (`kDesignWidth` a multiple of 8).
- **Adding a plugin to the suite** means adding its product name to `suiteNames()` (a test fails until you do).
- **Every timeline edit goes through `Editor`** (`Source/Edit.h`): clips, track gain, pan, mute, solo and name, the master, tempo, time signature, loop, metronome, markers, snap and grid. It takes the session lock, refuses a bad argument with a reason and changes nothing, and pushes one undo step per change; a `Gesture` makes a drag one step. Snap and grid are saved but not undone. Nothing else writes those fields: the window, the arrangement and the server all call `Editor`.
- **The audio callback has its own copy of the transport** (`MainComponent::Source`: the position, the loop, the end of the audio, all atomics). `syncTransport` copies the loop and the end from the session; it is called by every change that moves them (`refreshNow`, the loop, the arrangement's `onStructure`), never by the 30 Hz timer, because the callback's try-lock would otherwise miss a block now and then. A block is cut at the loop's end (`processBlock` is never called across it), and playing stops at the end of the audio unless the loop is on. `renderAudioBlock` runs the same code for the checks.
- **Adding or removing a track or a plugin is not undoable.** It is not an `Editor` step: the window calls `Editor::clearUndo()` after it, and after opening or starting a project. Undo and redo also refuse a step recorded with another track count.
- **Clips are samples at the session's rate.** An imported file is resampled to it (the first file sets it), and nothing is time-stretched: a tempo change moves the grid, not the audio. Overlapping clips on a track are summed, with their gain and fades. A clip whose file is missing keeps its place and plays silence (its `audio` is null). A version 1 project loads as one clip at 0.
- **The master applies to the mix only.** `masterDb` is applied to playback and to `mix.wav`; the stems are written without it (`renderStems`).
- Audio Units are hosted on macOS only (`JUCE_PLUGINHOST_AU`, set under `if(APPLE)`): the scan finds them through the system registry, so it loads each one to read it, under the dead-man's pedal. A VST3 and an AU of one name: the VST3 is kept.
- The scan reads plugins on the message thread and blocks it while it runs; a plugin that hangs in its read is caught only by the next run (the pedal). Say so wherever scanning is described.

## Not done yet (do not describe it as done)

- No mixer view (roadmap step 2). The fader and pan of a track are in the arrangement's header only.
- A track's plugins are not drawn on the arrangement: the FX button lists them (open, bypass, move, remove).
- The MCP server's timeline tools (clips, tempo, time signature, markers, loop, master) are in, but its undo is not exposed: it has no undo tool, so an agent's edits cannot be undone from it.
- No macOS CI job, and the app has not been run against the other twelve built plugins by a person; only Hermes was hosted by hand on Linux.
- No inline plugin parameter panel: a plugin is edited in its own window.
- No per-slot wet/dry.
- Nothing has been listened to. Real-time playback of clips, the loop wrap and the metronome, the audio device, hosting a real third-party plugin and the plugin editors are checked only by the automated tests (the callback's own code, `renderAudioBlock`, `DeviceCallback::deviceBlock` with the checks' buffers), and a person must check them on the Mac. The real-plugin check (`OLYMPUS_PLUGIN_DIR`) has not been run on the clip engine.
- **Recording has never run on a real interface.** No check opens a device: the take, its placement and its undo are checked with a fake device (`setDeviceForTest`) and fed blocks. Not checked: the AUDIO dialog (`AudioDeviceSelectorComponent`, a non-modal `DialogWindow`), the macOS microphone prompt (`MICROPHONE_PERMISSION_*` in CMake), a device's real latency values, a stop or change of the device under a real driver (its handling is the flag and `audioDeviceAboutToStart`), and the take files on a real disk under a long stall (six seconds of buffer, then dropped samples are counted by `Recorder::samplesDropped`, which no window shows).
- No software input monitoring (use the interface's direct monitoring), no punch-in or loop recording, no comping of takes, no take lanes: a take is a clip, and recording again on a track adds another clip over the first.
- The arrangement's drags, menus and the look at other sizes are checked by its methods and one screenshot, not by a person's hand.
