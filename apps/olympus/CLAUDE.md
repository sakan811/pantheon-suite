# CLAUDE.md

Olympus Session Host: a JUCE standalone app (macOS, builds on Linux for CI) that holds a timeline of clips on tracks (with a tempo map, markers, a loop and undo), a chain of plugins on each track, and writes the stems out; plus an MCP server over the same
session so an agent can place the chains. See `README.md` for what it does; this file is what you need to change it safely. It is an **app**, so it lives in `apps/`; the dependency rule of the root CLAUDE.md
applies (`apps/*` -> `shared/*`, never a plugin or another app). It **hosts** plugins, but only at run time: it loads VST3 bundles (and Audio Units on macOS) that its scan finds, and links none of their code, so no dependency on `plugins/*` exists in
CMake. Third-party plugins are scanned at run time; that was decided by the user. The one place it knows about the suite's own plugins is `suiteNames()` in `Source/Catalog.h` (their product
names), which a test compares with every `plugins/*/CMakeLists.txt`, and which the "suite only" setting (`scanSuiteOnly`) keeps.

## Build and verify

```bash
scripts/build.sh olympus                 # OlympusTests + OlympusUiCheck, built and run
scripts/build.sh olympus snap            # docs/screenshot.png (the picture the README embeds)
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
Source/Mcp.h          the MCP server: JSON-RPC lines in and out, the 31 tools, parameters by id/name/index
Source/Settings.h     plugin folders, scanSuiteOnly and the last project, in one file shared by the window and the server; the plugin cache and the scan's pedal sit beside it
Source/Advice.h       the GUIDE text and the notes at the foot (rules only; the kit draws them)
Source/Main.cpp       the app and its 16:10 window;  Source/McpMain.cpp  the server's main
Source/UI/            MainComponent (the window, the audio source, actions), TrackRow / TrackList / WaveStrip / LampButton / SlotButton / PluginWindow
Source/UI/Arrange.h / .cpp  the arrange view (the timeline): in progress, not yet in MainComponent
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
- **Plugins must be created and edited on the message thread.** The window and the server run on it; the render thread of the progress window only calls `processBlock`.
- **Everything is drawn from the kit** (`shared/ui`): `LampButton` exists only because the kit's look-and-feel lights every switch in one colour; if the kit grows per-button colours, use them and delete it. The window is 16:10 (`kDesignWidth` a multiple of 8).
- **Adding a plugin to the suite** means adding its product name to `suiteNames()` (a test fails until you do).
- **Every timeline edit goes through `Editor`** (`Source/Edit.h`): clips, track gain, pan, mute, solo and name, the master, tempo, time signature, loop, markers, snap and grid. It takes the session lock, refuses a bad argument with a reason and changes nothing, and pushes one undo step per change; a `Gesture` makes a drag one step. Snap and grid are saved but not undone. Nothing else should write those fields (the window still does in places, see "Not done yet").
- **Adding or removing a track or a plugin is not undoable.** It is not an `Editor` step: the window calls `Editor::clearUndo()` after it, and after opening or starting a project. Undo and redo also refuse a step recorded with another track count.
- **Clips are samples at the session's rate.** An imported file is resampled to it (the first file sets it), and nothing is time-stretched: a tempo change moves the grid, not the audio. Overlapping clips on a track are summed, with their gain and fades. A clip whose file is missing keeps its place and plays silence (its `audio` is null). A version 1 project loads as one clip at 0.
- **The master applies to the mix only.** `masterDb` is applied to playback and to `mix.wav`; the stems are written without it (`renderStems`).
- Audio Units are hosted on macOS only (`JUCE_PLUGINHOST_AU`, set under `if(APPLE)`): the scan finds them through the system registry, so it loads each one to read it, under the dead-man's pedal. A VST3 and an AU of one name: the VST3 is kept.
- The scan reads plugins on the message thread and blocks it while it runs; a plugin that hangs in its read is caught only by the next run (the pedal). Say so wherever scanning is described.

## Not done yet (do not describe it as done)

- The window is not yet on the clip model. It still shows one stem per track and plays the stems, not the clips; it has no timeline, grid, tempo, markers, loop, metronome, pan or undo in the window. The arrange view (`Source/UI/Arrange.h`) is in progress and not in `MainComponent` yet.
- No mixer view.
- The MCP server's timeline tools (clips, tempo, time signature, markers, loop, master) are in, but its undo is not exposed: it has no undo tool, so an agent's edits cannot be undone from it.
- The window still writes some track settings (name, mute, solo, gain) straight into the session, without `Editor` and its lock.
- No macOS CI job, and the app has not been run against the other twelve built plugins by a person; only Hermes was hosted by hand on Linux.
- No inline plugin parameter panel: a plugin is edited in its own window.
- No per-slot wet/dry.
- Nothing has been listened to. Real-time playback of clips, the loop and the metronome, the audio device, hosting a real third-party plugin and the plugin editors are checked only by the automated tests, and a person must check them on the Mac. The real-plugin check (`OLYMPUS_PLUGIN_DIR`) has not been run on the clip engine.
