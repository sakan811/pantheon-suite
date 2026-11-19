<!--
SPDX-License-Identifier: AGPL-3.0-or-later
Copyright (C) 2026 The Pantheon authors
-->

# Assessment: a custom DAW with MCP for the suite?

Status: proposal for a decision, no code. Written 2026-11-19 from three read-only research passes (repository reuse, DAW and MCP landscape,
design options). Items marked *unverified* were not confirmed against a primary source.

## The question

Should the suite get its own DAW that (1) has an MCP server, (2) integrates with the suite's plugins, (3) saves projects and (4) exports stems easily?

## Short answer

**Not a full DAW. Yes to the four goals, delivered smaller:** a headless render engine with an MCP server and a stem-export CLI first, and a small
session host (GUI) only if real use of the first one asks for it.

## What the repository has and lacks

- **Reusable:** the UI kit (`shared/ui`), the DSP (`shared/dsp`), Orpheus' project file (`apps/orpheus/Source/Session.h`: versioned XML, unknown attributes
  ignored) and WAV export, every plugin's APVTS state, and the BandLink clipboard format.
- **Missing for a DAW:** an audio graph, track and clip model, multitrack timeline, plugin hosting (no app links `juce_audio_processors`), a multitrack
  project format, stem render, a tempo/MIDI model, and any MCP code.
- **Rules a DAW would bend:** root CLAUDE.md says apps have "no plugin formats, no host"; dependencies point `apps/*` → `shared/*` only; the licence is
  source only (a DAW is a distributable program). Plugins are `juce_add_plugin` targets with the processor compiled inside, so hosting them in another
  process-level program needs a new library target per plugin: a new dependency direction.
- **Stems must render in one process.** Hestia's instances see each other through a shared bus (`plugins/hestia/Source/Bus.h`, `sharedBus()` is the
  plugin binary's one), so a render that runs each stem in its own process would change Hestia's result.

## The landscape

- Community MCP servers already drive Ableton Live (e.g. ahujasid/ableton-mcp) and REAPER (e.g. bonfire-systems/reaper-mcp); they are not official,
  and most control the session without hearing the audio. Bitwig and Logic servers were not confirmed.
- REAPER (ReaScript, OSC), Ardour (Lua, API "subject to change") and Bitwig (controller scripts) are scriptable already. So *"MCP for a DAW"* is
  available for the DAW the user already uses; what is not available is an agent that can render the suite's own plugins deterministically.
- A from-scratch DAW is the usual large scope (graph, hosting, timeline, automation, undo, save, export, latency compensation). No effort source was
  found; the estimates below are the planning agent's *unverified* guesses. Tracktion Engine covers much of it but is GPL/commercial (*terms of use with
  this AGPL repository unverified*).
- Licensing: the VST3 SDK moved to MIT with 3.8 (October 2025, from search results, *not fetched*), which is compatible with AGPL. For AU, Apple's
  documented licence covers only the host logo, and whether AU SDK headers are compatible is *unverified*. Get a licence review before shipping a host.

## Options

| | Scope (guess) | Four goals | Fit with the repository's rules |
|---|---|---|---|
| **A. Full DAW** (`apps/daw`) | 40+ person-weeks | all four, the last only after most of the work | breaks "no host"; ships a binary; timeline widgets missing from the kit |
| **B. Session host / stem bouncer** (GUI) | 10 to 16 | all four | needs a host tier; GUI means kit additions and a README screenshot; AU is macOS only |
| **C. Headless render library + CLI + MCP server** | 4 to 7 | MCP yes; plugins yes; stems best; project = a render spec, not a DAW project | needs a host tier; no window, so testable on Linux in CI like the `*Tests` targets |

## Recommendation: C, built so B can sit on top

C matches how the suite is used today (the plugins live in the owner's own DAW), is deterministic and checkable by an agent, runs on the Linux CI, and
its render spec becomes B's project format, so nothing is thrown away. Do not start A.

## Decisions needed before any code

1. **A host tier.** Today nothing may depend on a plugin. Hosting needs something that links the plugins' processors, e.g. `tools/`, which may depend on
   `plugins/*` and which nothing depends on. This amends the root CLAUDE.md layout section and `scripts/build.sh`.
2. **How plugins expose a processor.** A static library target per plugin, next to its `juce_add_plugin`, starting with Hestia, Hermes and one simple plugin.
3. **What a tool shows in its README** in place of the screenshot rule, since a CLI has no window.
4. **Licensing review** for hosting AU/VST3 (above), if the tool ever loads installed third-party plugins; hosting only the suite's own processors avoids it.

## Milestones for C

1. Decide and amend the rules (above); one workflow with the `changes` job.
2. Headless host library: load processors offline at a fixed block size, read and write WAV, set parameters sample-accurately; tested on a sine, on Linux CI.
3. Render spec (versioned, ignores unknown keys, like Orpheus' file) and a `render` CLI that writes stems; null test against in-process reference output.
4. MCP server over stdio: list plugins, get and set parameters, load a preset, render; tested with a fake client.
5. All thirteen plugins, plus a fixture for Hestia's multi-stem bus.
6. Decide on B from real use of the stem export.

## Open risks

- Faster-than-real-time render with plugins that read the playhead (Hermes, Themis).
- Latency compensation across stems (plugins report latency; `pdsp::BypassDelay` exists for bypass).
- Parameter lists differ per plugin: expose them generically from the APVTS.
