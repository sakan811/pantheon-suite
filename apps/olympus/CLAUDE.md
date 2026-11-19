# CLAUDE.md

Olympus Session Host: a JUCE standalone app (macOS, builds on Linux for CI) that holds the stems of a song, a chain of the suite's own plugins on each, and writes the stems out; plus an MCP server over the same
session so an agent can place the chains. See `README.md` for what it does; this file is what you need to change it safely. It is an **app**, so it lives in `apps/`; the dependency rule of the root CLAUDE.md
applies (`apps/*` -> `shared/*`, never a plugin or another app). It **hosts** plugins, but only at run time: it loads built VST3 bundles and links none of their code, so no dependency on `plugins/*` exists in
CMake. The one place it knows about the plugins is `suiteNames()` in `Source/Catalog.h` (their product names), which a test compares with every `plugins/*/CMakeLists.txt`.

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
Source/Catalog.h      the suite's plugin names, finding built .vst3 bundles by file name (only those are opened), making instances; internal plugins for tests
Source/Session.h      tracks, chains, fader/mute/solo, real-time processBlock with latency compensation, offline renderStems, project XML, describe() (JSON for the server)
Source/Mcp.h          the MCP server: JSON-RPC lines in and out, the 19 tools, parameters by id/name/index
Source/Settings.h     plugin folders and the last project, in one file shared by the window and the server
Source/Advice.h       the GUIDE text and the notes at the foot (rules only; the kit draws them)
Source/Main.cpp       the app and its 16:10 window;  Source/McpMain.cpp  the server's main
Source/UI/            MainComponent (the window, the audio source, actions), TrackRow / TrackList / WaveStrip / LampButton / SlotButton / PluginWindow
Tests/                Tests.cpp (session, render, project, server), UiCheck.cpp, Snapshot.cpp, TestPlugin.h, Check.h (a copy of Orpheus' tiny helper: apps share nothing)
```

## Invariants

- **One Session, three users.** The window, the server and the tests all call `Session`; none keeps a copy of its state. Anything that changes tracks or chains holds `Session::lock`; the audio callback only *tries* it (silence for that block if it cannot).
- **All tracks render together, block by block**, never one after another: Hestia's instances read each other through a bus in the plugin binary, so a track rendered alone would hear no neighbours. Keep it so.
- **A stem is aligned with its audio**: the chain's latency is taken off the front (and the run is longer by the longest latency); the real-time path delays the other tracks instead (`Track::delaySamples`). Test [4] and [5] pin both.
- **Stems are 32-bit float by default**, so they cannot clip and can be edited later; the mix is the sum of the written stems; a muted track (or one not soloed while another is) writes no stem.
- **A project never loses what it cannot use.** A missing plugin stays as a slot with its saved state (`Slot::savedState`), a missing audio file keeps the track and its path; both are `warnings`, never errors. A project from a newer version is refused.
- **The project format is versioned** (`version` on `OlympusProject`); add attributes, never change what one means, and ignore what is unknown, as Orpheus' file does.
- **The server writes only what it is told to** (a project path, a render folder) and prints nothing but protocol on stdout. Every tool failure is an `isError` result with a reason an agent can act on; keep the 19 tools in `tools()` and in README in step.
- **Plugins must be created and edited on the message thread.** The window and the server run on it; the render thread of the progress window only calls `processBlock`.
- **Everything is drawn from the kit** (`shared/ui`): `LampButton` exists only because the kit's look-and-feel lights every switch in one colour; if the kit grows per-button colours, use them and delete it. The window is 16:10 (`kDesignWidth` a multiple of 8).
- **Adding a plugin to the suite** means adding its product name to `suiteNames()` (a test fails until you do).
- AU is not hosted (JUCE finds AUs through the system registry, which would load every installed one); say so wherever hosting is described.

## Not done yet (do not describe it as done)

- No macOS CI job, and the app has not been run against the other twelve built plugins by a person; only Hermes was hosted by hand on Linux.
- No inline plugin parameter panel: a plugin is edited in its own window.
- No undo in the window (the plugins have their own), no loop, no tempo, no per-slot wet/dry.
