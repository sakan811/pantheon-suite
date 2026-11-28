# CLAUDE.md

Rules for the repository as a whole. Each plugin has its own CLAUDE.md in `plugins/<name>/`, and each app in `apps/<name>/` (Claude Code reads it when you
work in that folder); it owns that plugin's architecture, commands and invariants. This file owns only what crosses plugins.
The same rule as in the plugins applies here: every fact lives in one place, the others link to it.

## Layout and dependency direction

```
shared/ui/       the hardware-style interface kit (JUCE GUI only, header-only), target `pantheon_ui`
shared/dsp/      DSP that more than one plugin uses (header-only, namespace `pdsp`), target `pantheon_dsp`
plugins/<name>/  one self-contained plugin each: its own CMakeLists.txt, Source/ (Themis: core/ and plugin/), Tests/ (Themis: tests/), README, CLAUDE.md
apps/<name>/     one self-contained standalone application each (no plugin formats of its own): the same files as a plugin; today Orpheus, the pitch editor, and Olympus, the studio (a DAW)
.github/         CI for the whole repository (workflows run per plugin with `working-directory`)
```

- Dependencies point one way: `plugins/*` and `apps/*` → `shared/*`, never the reverse, and never from one plugin or app to another. If two of them need the same code, it moves to `shared/` first and both use it from there.
- `shared/` knows nothing about any plugin (no plugin types, parameters, verdicts, words, or GUI in `shared/dsp`). What a plugin adds on top (its name, number formats, verdict or mode colours) lives in that plugin; Themis' is `plugin/Source/ThemisUi.h`.
- **An app may host plugins, at run time only.** Olympus hosts any VST3 (and any Audio Unit on macOS) its scan finds, loads them at run time and links none of their code, so the compile-time rule above stands (no CMake dependency on a plugin). Third-party plugins are scanned at run time: the user decided this. `suiteNames()` (in `apps/olympus/Source/Catalog.h`, the suite's product names) still exists and a test still compares it with the plugins' product names, so a new plugin must be added there; the "suite only" setting narrows the scan to them. See [apps/olympus/README.md](apps/olympus/README.md#controls--capabilities) (`### Controls & capabilities`).
- There is no root CMake project: each plugin or app builds from its own folder, pulls `shared/` in by relative path, and fetches its own JUCE at the one version in [cmake/Juce.cmake](cmake/Juce.cmake).
- The 11-stage mastering chain order lives in [README.md](README.md#theme).

## Building and checking: use scripts/build.sh

[scripts/build.sh](scripts/build.sh) is the one way to configure, build and check a plugin or app (`scripts/build.sh hestia`, `scripts/build.sh aegis ui snap`, `scripts/build.sh orpheus app`); its header says what it takes. Use it instead of typing `cmake` by hand: it builds every target asked for in one `cmake --build` on all cores, configures only when the build folder has no `CMakeCache.txt`, and uses Ninja and ccache when installed (each target compiles its own copy of JUCE, so ccache is the biggest saving; `brew install ninja ccache`). If the script's behaviour changes, update its header, not a copy here.

- **After changing anything in `shared/`**, run `scripts/build.sh` for every consumer (for `shared/ui`, build every consumer and regenerate/inspect screenshots where not byte-identical; for `shared/dsp`, run the consumer tests and checks listed in `## shared/dsp`).
- **Installing on build (`*_INSTALL_AFTER_BUILD` / `COPY_PLUGIN_AFTER_BUILD`):** A build folder made by `scripts/build.sh` does not install into `~/Library/Audio/Plug-Ins`, except:
  - Themis on macOS (`THEMIS_INSTALL_AFTER_BUILD` defaults to `ON` there and the script does not change it; other `*_INSTALL_AFTER_BUILD` options stay `OFF`).
  - Any build folder previously configured with `*_INSTALL_AFTER_BUILD=ON` (check `grep INSTALL_AFTER_BUILD build/CMakeCache.txt` before building a `vst3` or `au` target).
  - Harmonia and Hephaestus, which set `COPY_PLUGIN_AFTER_BUILD TRUE` unconditionally (see `## Not yet in line`).

## Module conventions & defaults

Shared defaults across all 14 plugins and 2 apps (module `CLAUDE.md` files only document what is specific to that module):

- **Build & verify defaults:** `scripts/build.sh <name>` is the standard entry point (`check` = `*Tests` + `*UiCheck`; `snap` = `*Snapshot`); `*Tests`, `*UiCheck`, and `*Snapshot` are `EXCLUDE_FROM_ALL`; manual CMake commands and the macOS CLT `SDKROOT` note live in [README.md](README.md#build--develop) and each module's `README.md` (`## Build & develop`), not in `CLAUDE.md`. Definition of done: DSP change → `*Tests` passes and README numbers match; UI/parameter change → `*UiCheck` passes and `docs/screenshot*.png` is regenerated and inspected.
- **Standalone DSP tests:** Modules with JUCE-free `Source/DSP` compile `Tests/Tests.cpp` directly with `g++ -std=c++20 -O2 -I../../shared/dsp/include -ISource -ITests Tests/Tests.cpp` (also test with `-O3 -march=native` and `-O3 -ffast-math` separately).
- **Real-time & NaN/Inf:** `Engine::process` allocates and locks nothing and replaces non-finite (`NaN`/`Inf`) samples with `0` on input; latency changes are reported on the message thread via `handleAsyncUpdate`.
- **UI test gotchas:** `JUCE_MODAL_LOOPS_PERMITTED=1` is required for `runDispatchLoopUntil`; `Button::triggerClick()` is asynchronous (dispatch the message loop or use `press()`).
- **UI & wiring defaults:** Editors use `shared/ui` (`ui::ScaledEditor<...>` / `ui::innerOf<...>`, `ui::Choice` & `ui::Toggle` bound to one parameter for good, `setSize()` called **last** in constructor). `Source/Presets.h` uses `shared/ui/Presets.h` (`ui::Preset`, `ui::Programs`, `ui::PresetBox`; `Undo` before `apvts`, `AbCompare` after `apvts`, active `abSlot` saved, slots unsaved). `Source/Advice.h` owns `adviceFor()` & `guideText()` (`ui::kMaxNoteChars`), restating README `### Limits`. `<NAME>_PLUGIN_NAME` in `CMakeLists.txt` matches `UI/<Name>Ui.h` (`UiCheck` pins it).
- **Parameters & bypass:** Parameter IDs (`<name>::id`) are permanent (never rename or remove; bump `ParameterID` version for additions). Latency-reporting plugins use `pdsp::BypassDelay` in `processBlockBypassed` (except Harmonia).
- **Code & commit conventions:** Allman braces, space before call parentheses, `juce::` qualified in headers; comments explain *why*; README claims backed by a test; commit by topic and end commit messages with `Co-Authored-By`.

## Skills

The procedures that are long and rarely needed live in [.claude/skills/](.claude/skills) (committed alongside [.claude/agents/](.claude/agents); the rest of `.claude/` is ignored, and `.agents/` mirrors them for Antigravity). **Use them without being asked:** when a task matches one, invoke it with the
Skill tool before you start, and follow its steps instead of improvising them. Do not wait for the user to type the name.

- `new-plugin`: a request to create, add or scaffold a plugin.
- `plugin-parameter-change`: any edit to a plugin's `Parameters.h`, or adding a control or a setting (a mode, a look-ahead, a preset-able value).
- `shared-change`: any edit under `shared/`, before the change is called done.
- `install-and-validate`: build, install for a DAW, `auval`. It **replaces the plugins installed in `~/Library/Audio/Plug-Ins`**; the user has allowed Claude to run it itself when a task asks to build and install (the skill says to name what will be replaced before it builds). It
  installs nothing else and never changes the `*_INSTALL_AFTER_BUILD` options.

This file keeps only the rules; do not copy a skill's steps into it. A new skill is listed here with the request it answers, and its `description` says the same in its first sentence (that is what Claude chooses from).

## Sub-agents: the main session is the orchestrator

Eight cheap agents live in [.claude/agents/](.claude/agents) (mirrored for Antigravity in `.agents/plugins/pantheon/agents/`, with that tool's tool names: change both together). `builder` and `fixer` edit files, `commit-writer` and `pr-writer` write to git and GitHub, the rest only read; each file owns its tools, limits, and report format, so this section only says how they run:

- `builder`: one briefed slice of a feature → code, its tests, built with `scripts/build.sh`.
- `bug-hunter`: a symptom, a plugin or app → cause and place.
- `code-checker`: a diff or named files → findings.
- `feature-checker`: a plugin and a feature → what is missing.
- `doc-checker`: a diff, a module or the repo → docs that disagree with the code or with each other (counts, names, links, captions).
- `fixer`: findings → the smallest edit, built with `scripts/build.sh`.
- `commit-writer`: commits by topic, each commit building on its own (it may unstage, never touch the working tree).
- `pr-writer`: opens a PR from a pushed branch, turns on auto-merge when told, reads a PR's failed CI.

**Standing rule (every task):** Always, for every task (a feature, a fix, a review, a doc change, a build-speed change; none is too small), the main session acts as **orchestrator** (plans, splits, writes briefs, passes reports between agents as text, owns every decision, runs final checks itself, and is the only one that talks to the user) and runs the work through a pipeline of **Haiku** agents (`model: haiku`; where the named agents above are unavailable, use a general-purpose agent with `model: haiku`). Task size changes how many stages run, never whether the pipeline is used (a one-line fix is one `fixer` + orchestrator verification). Agents never call each other and never talk to the user; use them without being asked, as with skills.

```
orchestrator: plan, split, brief ─► builder / fixer (one at a time) ─► reviewers (read-only, in parallel) ─► fixer (≤ 2 rounds) ─► orchestrator: re-run checks, inspect PNGs, decide, commit
```

- **Chains & concurrency:**
  - **Fix chain:** `bug-hunter` (skip when cause is known) → `fixer` with its report → `code-checker` on the diff → back to `fixer` with the findings (**at most 2 rounds**; if not clean after the second, stop and tell the user what is left).
  - **Feature chain:** `builder` (one at a time, split along seams; never two builds or two editors on the same files) → `code-checker` and `feature-checker` (and `doc-checker` when a doc or a counted thing changed) in **parallel** → `fixer` for what they report → `code-checker` once more (at most 2 fixer rounds).
  - **Ship chain** (only when the user asks to commit, push or open a PR): orchestrator's checks pass → `commit-writer` (push if asked) → `pr-writer open` (auto-merge only with the method the user named) → on a red CI, `pr-writer status` → the fix chain.
  - **Parallel only for reads:** fan out only when read-only agents read the same state without dependencies (do not fan out for show).
- **What a brief must have:** goal and purpose; files to read first (the module's `CLAUDE.md` and target files); boundaries (what it may and may not touch, e.g. `shared/`, commits); exact foreground `scripts/build.sh ...` commands with a timeout (no background jobs — they die with the agent); definition of done (checks pass, screenshots regenerated and inspected); a test for every defect or feature; concrete numbers and examples (not "make it robust"); and the report format (`git diff --stat`, pass counts, what it looked at, and `"unverified"` for anything not confirmed). Reviewers also get the module's invariants (`CLAUDE.md`) and failure classes to look for (audio thread, locks, real third-party plugins and devices).
- **What the orchestrator does itself (never delegated to an agent alone):** design, `shared/` design and verification, multi-plugin changes, the skills above (`plugin-parameter-change`, `shared-change`, `new-plugin`), regenerating and looking at screenshots, and `install-and-validate` (`fixer` stops and reports when a finding needs one of these). Re-run `scripts/build.sh` yourself, inspect PNGs yourself, read risky code yourself (audio thread, locks, file/device I/O), scrutinize changed test expectations, and drop findings not confirmed in the code. Never claim "done" on an agent's word or with a background/unrun build.
- **Commits, pushes, and final report:** Commits and pushes happen only when the user asks (or a configured hook asks); `commit-writer` (or the orchestrator) runs them after checks pass, one commit per verified step, never commits to `main`, and pushes only when the user explicitly said push; a PR is opened, and auto-merge turned on, only when the user asked for it. End a chain with a short report to the user: what was found, what changed (`git diff --stat`), the check that ran and its result, and what is left.

## shared/ui

- All the plugins and apps draw with it (Themis, Hephaestus, Harmonia, Iris, Atlas, Hestia, Aegis, Apollo, Hera, Castor, Nemesis, Dionysus, Hermes, Selene, Orpheus, Olympus); nothing in one draws its own panel, lamp, knob, button,
  selector or nameplate. What a plugin adds on top is small and lives in it (`ThemisUi.h`, `HephaestusUi.h`, `HarmoniaUi.h`, `IrisUi.h`, `AtlasUi.h`, `HestiaUi.h`, `AegisUi.h`, `ApolloUi.h`, `HeraUi.h`, `CastorUi.h`, `NemesisUi.h`, `DionysusUi.h`, `HermesUi.h`, `SeleneUi.h`: its
  words, its verdict, mode or placement colours). A plugin's own namespaces must not be called `ui`, which is the kit's.
- It owns every pixel of the interface: [Hardware.h](shared/ui/include/pantheon/ui/Hardware.h) (`colours::`, fonts, `ui::hw`
  panels, glass, lamps, buttons, knobs, the header plate and the nameplate), [HardwareLookAndFeel.h](shared/ui/include/pantheon/ui/HardwareLookAndFeel.h),
  [Controls.h](shared/ui/include/pantheon/ui/Controls.h) (the parameter-bound `Knob`, `Choice` and `Toggle`; a choice takes its items from the parameter, so a plugin never lists them twice; clicking a `Knob`'s read-out opens it for typing a value),
  [ValueKnob.h](shared/ui/include/pantheon/ui/ValueKnob.h) (the same knob for a value that is not a plugin parameter, such as the setting of the selected note in an editor: it holds its own value and calls back; Orpheus and Olympus are its only users),
  [MeterCell.h](shared/ui/include/pantheon/ui/MeterCell.h), [Advice.h](shared/ui/include/pantheon/ui/Advice.h) and [Note.h](shared/ui/include/pantheon/ui/Note.h) (the GUIDE button, `ui::drawNotes` and the foot `ui::drawNoteStrip`, and the plain-C++ `ui::Note` a plugin's own `Advice.h` returns: every plugin and app has a GUIDE saying what it is for and what it is not for, and notes that warn when its settings do not suit the job; the rules and the texts are the plugin's, the drawing is the kit's), [Scaled.h](shared/ui/include/pantheon/ui/Scaled.h) (`ui::ScaledEditor<Editor>`: the window of the plugins whose editor has one fixed design size, Atlas, Iris, Hephaestus, Hestia, Aegis, Apollo, Hera, Castor, Nemesis, Dionysus, Hermes and Selene; it opens at 100% of that size (its design width is 780 to 1650 px), scales between 50% and 150% from any of its four corners (`ui::ResizeGrips`) and keeps the scale in the session's state as `uiScale`; the editor inside is laid out and tested at its own size, and tests reach it with `ui::innerOf<Editor>`; Themis and Harmonia scale or lay out by themselves but use the same `ui::ResizeGrips` and the same 16:10 shape; their own limits (Harmonia 75% to 200%, Themis 1400 x 875 to 2080 x 1300, opening at the smallest) are in their own CLAUDE.md), [Aspect.h](shared/ui/include/pantheon/ui/Aspect.h) and [Resize.h](shared/ui/include/pantheon/ui/Resize.h) (**one window shape for every plugin and app: 16:10**, `ui::kEditorAspect` and `ui::editorHeightFor (width)`; an editor's design width is a multiple of 8, its height is `ui::editorHeightFor (kWidth)` and its main display takes what the rest of the layout leaves, never its own ratio, and a new plugin or app does the same; `ui::ResizeGrips` is the four corner handles over an editor: drag any corner to grow or shrink the window with the ratio kept, double-click one for the opening size; the host's own edges keep working through the same constrainer. The ratio was chosen from the plugins' own shapes (1.54 to 1.67 where there is a main display) and the 16:10 screens they run on: no standard exists, the hosts only ask for a fixed ratio with limits), [Presets.h](shared/ui/include/pantheon/ui/Presets.h) (`ui::Preset`, `ui::applyPreset`, `ui::Programs` for the processor and `ui::PresetBox` for the editor: the mechanism of the presets of Aegis, Apollo, Castor, Dionysus, Hera, Hermes, Hestia, Nemesis and Selene; the settings, names and one-line purposes are the plugin's own `Source/Presets.h`), plus [Undo.h](shared/ui/include/pantheon/ui/Undo.h) (`ui::UndoButtons` and `ui::beginUndoStep`: UNDO / REDO and Cmd+Z for the parameters, from a `juce::UndoManager` the processor owns and hands to its APVTS; opt-in, a plugin without one is untouched) and [AbCompare.h](shared/ui/include/pantheon/ui/AbCompare.h) (`ui::AbCompare` in the processor, two complete settings of the APVTS parameters with one live, and `ui::AbButtons` in the editor's header; a switch is one undo step and only the active slot is saved in the session; opt-in; Hephaestus, Harmonia, Hera, Hermes, Hestia, Aegis, Apollo, Iris and Selene use both, in the strip above their header controls, Apollo's in the row above its display), plus [FreqAxis.h](shared/ui/include/pantheon/ui/FreqAxis.h) (the logarithmic frequency axis the spectrum displays use; Themis' tonal view draws its own) and [Spectrogram.h](shared/ui/include/pantheon/ui/Spectrogram.h) (`ui::Spectrogram`: a ring of rows drawn on glass, levels in, the caller's grid; Atlas is its only user so far). Plugins call them; they do not draw their own panels, lamps or
  buttons. The namespaces (`colours`, `ui`, `ui::hw`) are kept short on purpose.
- Everything is drawn from code: no image or font files, nothing random at run time.
- A look change that should be only one plugin's belongs in that plugin, not in the kit.

## shared/dsp

- What two or more plugins both need, and nothing a single plugin owns:
  - [Biquad.h](shared/dsp/include/pantheon/dsp/Biquad.h): double-precision RBJ designs (bell, shelves, notch), Butterworth cascades (12–48 dB/oct), `magnitudeAt` for drawing and tests. Pinned by `ApolloTests`, `AtlasTests`, `CastorTests`, `DionysusTests`, `HeraTests`, `HermesTests`, `HestiaTests`, `IrisTests`, `NemesisTests`, `HephaestusTests`, and Themis `make test`.
  - [AnalyzerTap.h](shared/dsp/include/pantheon/dsp/AnalyzerTap.h) and [SpectrumAnalyzer.h](shared/dsp/include/pantheon/dsp/SpectrumAnalyzer.h): lock-free FIFOs from audio thread to display and calibrated GUI-free FFT analyzer. Pinned by `HephaestusTests` (test [10] pins calibration), `IrisTests`, `ApolloTests` (`ApolloUiCheck` pins processor tap reads), and `AtlasTests`.
  - [TruePeak.h](shared/dsp/include/pantheon/dsp/TruePeak.h): `pdsp::TruePeakReader` (8× 64-tap windowed sinc, plain C++, delayed by `kTruePeakDelay` samples) so plugins agree on true peak. Pinned by `AegisTests`, `HermesTests`, and Themis `make test`.
  - [BandLink.h](shared/dsp/include/pantheon/dsp/BandLink.h): `pdsp::link` clipboard text (`pantheon-bands 1`; add keys, never change existing key meanings, readers ignore unknown keys/kinds). Themis writes (`COPY`, `Copy EQ`); Iris, Apollo, Hephaestus (`decode`), Atlas (`curve` for `TARGET`), Aegis (`decodeLimiter`), and Castor (`decodeWidth`; never pasted alongside a side bell from the same measurement) read; `amount` (`0..1`, default `1`) scales gain change. Pinned by the five `UiCheck`s that paste (`IrisUiCheck`, `HephaestusUiCheck`, `AegisUiCheck`, `ApolloUiCheck`, `CastorUiCheck`) and `ThemisSmoke`.
  - [MusicBands.h](shared/dsp/include/pantheon/dsp/MusicBands.h): `pdsp::bands` (Sub 20–60 Hz, Bass 60–250 Hz, Low-Mid 250–500 Hz, Midrange 500 Hz–2 kHz, Upper-Mid 2–6 kHz, High 6–20 kHz) and flat-summing LR4 `pdsp::BandSplitter`. Used by Themis `AUDITION` (readouts keep `kEnergyRanges`), Iris regions, Hephaestus band solo strip, Atlas 6 bands, and Castor 6 width bands (not Harmonia or Apollo; new plugins with band solo use it). Pinned by `AtlasTests`, `CastorTests`, `HephaestusTests`, `IrisTests`, and Themis `make test`.
  - [SpectrumIssues.h](shared/dsp/include/pantheon/dsp/SpectrumIssues.h): `pdsp::SpectrumGrid` (1/24-octave) and `pdsp::IssueFinder` (sustained narrow peaks and jumping regions, fed from a display timer). Used by Atlas so far; pinned by `AtlasTests` [9].
  - [BypassDelay.h](shared/dsp/include/pantheon/dsp/BypassDelay.h): plain C++ latency delay called from `processBlockBypassed` for AU host bypass (see `## Module conventions & defaults`).
- Namespace is `pdsp` (not `dsp`, which collides with `juce::dsp`). The files are tested through the plugins that use them, not on their own.

## Documentation shows the plugin

- **Every plugin's and app's README embeds its visuals**, with `docs/showcase.gif` at the top and `docs/screenshot*.png` under `## See it`, each followed by an italic caption that says what the picture is (a headless render of the
  editor), which settings and programme it shows, and what to look at in it. A `docs/` image that no README shows does not count: the README is where it is read. Atlas, Iris, Themis and Apollo have several
  (a mode, a feature, a state): each is embedded with its own caption and the command that made it is in `## Build & develop` of the module's `README.md`.
- The images are the output of the plugin's `Snapshot` target (arguments in the file's header), never edited by hand, and a caption states only what the picture shows. **A change to the interface, a parameter or a feature that
  appears in a picture is not done until the pictures are regenerated, looked at, and the caption still says what they show** (`## Build & develop` in the module's `README.md` has the command). A new feature gets a picture of its own when one
  screenshot cannot show it.
- A new plugin or app starts with its screenshot in the README (the `new-plugin` skill's step 6).

## Licence and CI

- AGPL-3.0-or-later throughout, source only, no binaries or releases of compiled plugins. [LICENSE](LICENSE), [NOTICE.md](NOTICE.md)
  (third-party software, trademarks, the source-only policy), [CONTRIBUTING.md](CONTRIBUTING.md) and [SECURITY.md](SECURITY.md) live
  once, at the root, for every plugin; a plugin's README links to them and does not restate them.
- Every source file starts with `SPDX-License-Identifier: AGPL-3.0-or-later` and `Copyright (C) 2026 The Pantheon authors`
  (the same name everywhere; a new file copies the two lines).
- JUCE: one version for the repository, set in [cmake/Juce.cmake](cmake/Juce.cmake) (with the offline `third_party/JUCE`
  override). Docs say "see cmake/Juce.cmake" and never name the version, not even the major one. The JUCE compile switches
  every target sets are `PANTHEON_JUCE_DEFINITIONS` in the same file; a CMakeLists adds them, it does not retype them.
- Third-party code needs a compatible licence and an entry in [NOTICE.md](NOTICE.md). Do not name other companies' products in
  a UI or in source comments; a README may mention one where it is factual and neutral, and NOTICE.md carries the trademark note.
- CI runs per plugin; workflows are SHA-pinned with read-only tokens. Do not add a release or artifact upload.
- **A workflow never filters by `paths:` on its `on:` triggers.** A workflow skipped that way reports no checks at all, so a required check (the per-plugin jobs are named `linux`) waits for ever on a pull request that touches none of its paths. The
  per-plugin workflows start with a `changes` job that diffs against the base commit and passes `relevant` to the real job (`needs: changes`, `if:`): a job skipped by `if:` reports success. A new plugin's workflow copies that job, with its own paths.

## Not yet in line (Hephaestus and Harmonia were imported as they were)

Delete an item here when it is done; this list is the only place that says what is left.

- **Installing on build:** Harmonia and Hephaestus still set `COPY_PLUGIN_AFTER_BUILD TRUE` unconditionally instead of an option like `THEMIS_INSTALL_AFTER_BUILD` (see `## Building and checking`).
- **CI** builds and tests Themis (macOS and Linux), runs Iris' `IrisTests`, `IrisUiCheck` and `IrisHostCheck` on Linux ([iris.yml](.github/workflows/iris.yml)), and the tests, the editor check and the VST3 build on Linux for Atlas ([atlas.yml](.github/workflows/atlas.yml)), Hestia ([hestia.yml](.github/workflows/hestia.yml)), Aegis, Apollo, Hera, Castor, Nemesis, Dionysus, Hermes and Selene ([selene.yml](.github/workflows/selene.yml); one workflow each, named after the plugin), Orpheus ([orpheus.yml](.github/workflows/orpheus.yml)) and Olympus ([olympus.yml](.github/workflows/olympus.yml); tests, window check, the app and the MCP server, no real plugin). Iris, Atlas, Hestia, the seven newest plugins, Selene, Orpheus and Olympus have no macOS job yet (universal binary, AU and `auval` for the plugins); only Iris and Hephaestus have a `HostCheck` (Hephaestus' is in no workflow); Atlas, Hestia and the seven newest were built on macOS by hand on 2026-11-13 (VST3 and AU) and `auval -v` passes for the nine AUs, which no workflow repeats; the workflows of Atlas, Hestia, Orpheus, Olympus and the seven newest have run and passed on main (checked 2026-11-20 with the GitHub run list; the same checks were first run by hand on Linux: tests, editor check, VST3 build and screenshot); and Hephaestus and Harmonia are in no workflow.
- **Presets in Atlas, Hephaestus and Iris are their own code** (a list, `applyPreset`, the program methods and a combo box each, written before `shared/ui/.../Presets.h` existed); the eight plugins that came after use the kit's. Move them onto it when one of the three is next touched.
- **The spectrogram and its finder in Iris and Themis.** `ui::Spectrogram` and `pdsp::IssueFinder` are in `shared/` for all three, but only Atlas uses them. Iris still has its own
  `ResonanceFinder` (the same idea as the finder's peaks, on its own grid) and no spectrogram or JUMPS markers; Themis has its own spectrogram (`core/include/themis/Spectrogram.h`, `plugin/Source/HistoryViews.h`, on its `BandGrid`) with no markers.
- **Shared DSP.** Only what a second plugin actually needed has been extracted (see shared/dsp). Themis' FFT, band grid and target
  curves, and Harmonia's pitch tracking, are used by one plugin each and stay there; so are Orpheus' offline pitch tracker and PSOLA ([apps/orpheus/Source/Dsp](apps/orpheus/Source/Dsp)), which are whole-file and share no code with Harmonia's real-time ones (if a second consumer needs either, extract then); Themis' `TonalAnalyzer` is not the same code as
  the shared spectrum analyser (it differs in precision and JUCE dependence). Extract when a second plugin needs the same code.
