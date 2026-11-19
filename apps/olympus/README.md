# Olympus Session Host

A **session host for the Pantheon plugins**, as a standalone macOS app. Add the stems of a song, put a chain of the suite's plugins on each
stem, save the project, and write the stems out again with their chains. An AI agent can do the placing for you through the built-in
**MCP server**; you open the saved project here and edit what is left by hand.

## Name origin

*Olympus* is the mountain where the Greek gods lived together; here it is the one place where the suite's plugins (Hermes, Hestia, Aegis and the others) sit side by side on a song.
(CMake targets and namespaces use the short name "Olympus". It is an app, not a plugin: it lives in `apps/`.)

![Olympus Session Host](docs/screenshot.png)

*Headless render of the window (the hardware-style interface shared with the plugins) over four synthetic stems, with stand-ins that carry the names of the suite's
plugins (the stand-ins only change the level; a real session loads the built plugins). Each row is one stem: its name and file, the fader (the knob), mute and solo, the waveform
with the playhead (click it to move the playhead), and the chain, one lit button per plugin and `+` to add another. The Vocal chain has five plugins, one of them reporting
96 samples of latency, which the foot of the window says is aligned for in playback and in the stems. The Chords stem is muted (red M): it is not played and no stem is written for it.
Regenerate with the command under "Development".*

## Core philosophy

- **Place now, edit later.** The point is to get the chains onto the stems quickly, by hand or by an agent, and to keep every setting in a project file, so nothing is final.
  Open any plugin from its chain, change it, save.
- **Any plugin it finds.** It hosts every VST3 (and, on macOS, Audio Unit) plugin the folders hold, the suite's own and third-party ones alike, and nothing else. Hestia's instances
  (which listen to each other) still work across the stems. The setting *scanSuiteOnly* in `settings.xml` narrows this to the suite's own plugins.
- **Not a DAW.** There is no recording, no MIDI, no instruments, no timeline editing and no automation. It plays the stems from the start and writes them out. Arranging and mixing stay in your own DAW.
- **Nothing is hidden from the agent.** The server reads and sets every setting of a plugin by the names the plugin gives them, loads its presets, and says why anything it cannot do failed.
- **Free, public, source only.** The repository holds the code, not a download, under the AGPL-3.0-or-later (see [the repository notes](../../NOTICE.md)).

## Features

| Area | What it does |
|---|---|
| **Tracks** | Audio files (WAV, AIFF, FLAC, MP3, M4A, Ogg, CAF), one track each, mono or stereo at any rate. A track becomes stereo at the session's rate, which is the first file's; others are resampled. Drop files on the window or press ADD TRACK. |
| **Chains** | Any plugin Olympus found, in any order, several of the same kind if you like. Click a plugin to open its own window, shift-click to bypass it, right-click to move or remove it. A plugin that is not installed on this machine stays in the chain (red), passes the audio through and keeps its settings. |
| **Fader, mute, solo** | A fader for each track after its chain. A muted track, or any track not soloed when another is, is not played and gets no stem. |
| **Playback** | From the playhead to the end of the stems, through the chains, on the audio device. The plugins read the same playhead. A plugin's reported latency is compensated across the tracks, so the stems stay in time. |
| **Project** | `.olympus`: a small XML file with the tracks (a relative path to each audio file, and the absolute one as a fallback), faders, and every plugin's complete state. A project opens without its plugins or audio (each is a warning) and saves again without losing what they held. |
| **Render stems** | One WAV per audible track after its chain and fader, aligned with the audio (the chain's latency is taken off the front), named `01-Vocal.wav`, and `mix.wav`, the sum. 32-bit float by default, so a stem cannot clip and can be edited later; 16 or 24 bit on request, and a tail for reverbs and delays. All tracks run together block by block, which Hestia's bus needs. |
| **MCP server** | `OlympusMcp`: the same session without a window, 19 tools, over stdio (see below). |

## Where it finds the plugins

Olympus hosts **VST3** plugins and, on macOS, **Audio Units**, loading them at run time and linking none of their code. A scan reads what it finds in the system's VST3 folders, in the
folders you give it with **PLUGINS > Look in a folder...** (the choice is remembered, and the MCP server uses it too), in `OLYMPUS_PLUGIN_PATH`, and in `--plugins` for the server; the
Audio Units are the ones the system registers. Point it at this repository's `plugins/` folder and it finds the build folders under it, so `scripts/build.sh hermes vst3` is enough.

Third-party plugins are scanned on your machine, as you install them; nothing is sent anywhere. The result is kept beside the settings (`plugins.xml`), so the window starts from it and
reads the folders again only when the cache is empty or you choose **PLUGINS > Rescan plugins**. A plugin that crashes while it is being read is written to `plugin-scan.pedal` first, and
the next scan blacklists it and skips it (the blacklist is kept in `plugins.xml`; delete that file to try it again). A plugin that hangs in its read is only caught by the next run, once you
have quit. Two plugins with one name are both listed as `Name (Maker)`; a VST3 and an Audio Unit of one name show as the VST3.
`suiteNames()` in `Source/Catalog.h` still lists the suite's own plugins, which a test compares with `plugins/`.

## The MCP server

```
cmake --build build --target OlympusMcp       # in apps/olympus; or: scripts/build.sh olympus OlympusMcp
build/OlympusMcp_artefacts/Release/OlympusMcp [--plugins <folder>] [--project <file.olympus>]
```

For Claude Code, register it once (the path is where you built it):

```
claude mcp add olympus -- /path/to/pantheon-suite/apps/olympus/build/OlympusMcp_artefacts/Release/OlympusMcp
```

It speaks MCP (JSON-RPC 2.0, one message per line on stdin and stdout; diagnostics go to stderr). The tools: `list_plugins`, `project_info`, `new_project`, `open_project`, `save_project`,
`add_track`, `remove_track`, `set_track` (fader, mute, solo, name), `add_plugin`, `remove_plugin`, `move_plugin`, `bypass_plugin`, `set_chain`, `copy_chain` (the same chain, with its settings,
onto other tracks), `list_parameters`, `set_parameter` (by value 0..1 or by the text the plugin shows), `list_presets`, `load_preset` and `render_stems`. Tracks and slots are numbered from 0; a track may be
named and a plugin given by the start of its name. A failed call comes back as an error result with the reason. It edits and renders; it does not play. A typical request: *"Add the four stems in
`~/song/`, put Hestia and Hermes on each, Aegis on the vocal, save it as `~/song/song.olympus` and render the stems."* Then open the project here.

## Development

Build and check with the script (it builds the targets in one go and runs the checks):

```bash
scripts/build.sh olympus            # OlympusTests and OlympusUiCheck
scripts/build.sh olympus snap       # docs/screenshot.png
scripts/build.sh olympus app        # the app
OLYMPUS_PLUGIN_DIR=$PWD/plugins/hermes/build scripts/build.sh olympus tests   # also hosts a built plugin
```

`OlympusTests` (session, render, alignment, project file, MCP server) and `OlympusUiCheck` (the window driven with synthetic events) use plugins made in code, so they need no built plugin; set
`OLYMPUS_PLUGIN_DIR` to add the check against real ones. The same checks run in CI on Linux ([.github/workflows/olympus.yml](../../.github/workflows/olympus.yml)); a macOS build is not covered there.

Licence, contributions and security: see the repository root ([LICENSE](../../LICENSE), [NOTICE.md](../../NOTICE.md), [CONTRIBUTING.md](../../CONTRIBUTING.md), [SECURITY.md](../../SECURITY.md)).
