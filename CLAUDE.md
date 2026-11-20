# CLAUDE.md

Rules for the repository as a whole. Each plugin has its own CLAUDE.md in `plugins/<name>/`, and each app in `apps/<name>/` (Claude Code reads it when you
work in that folder); it owns that plugin's architecture, commands and invariants. This file owns only what crosses plugins.
The same rule as in the plugins applies here: every fact lives in one place, the others link to it.

## Layout and dependency direction

```
shared/ui/       the hardware-style interface kit (JUCE GUI only, header-only), target `pantheon_ui`
shared/dsp/      DSP that more than one plugin uses (header-only, namespace `pdsp`), target `pantheon_dsp`
plugins/<name>/  one self-contained plugin each: its own CMakeLists.txt, Source/ (Themis: core/ and plugin/), Tests/ (Themis: tests/), README, CLAUDE.md
apps/<name>/     one self-contained standalone application each (no plugin formats of its own): the same files as a plugin; today Orpheus, the pitch editor, and Olympus, the basic DAW
.github/         CI for the whole repository (workflows run per plugin with `working-directory`)
```

- Dependencies point one way: `plugins/*` and `apps/*` → `shared/*`, never the reverse, and never from one plugin or app to another. If two
  of them need the same code, it moves to `shared/` first and both use it from there.
- `shared/` knows nothing about any plugin: no plugin types, parameters, verdicts or words in it. What a plugin adds on top
  (its name, its number formats, its verdict colours) lives in that plugin; Themis' is `plugin/Source/ThemisUi.h`.
- **An app may host plugins, at run time only.** Olympus hosts any VST3 (and any Audio Unit on macOS) its scan finds, loads them at run time and links none of their code, so the compile-time rule above stands (no CMake dependency on a plugin). Third-party plugins are scanned at run time: the user decided this. `suiteNames()` (in `apps/olympus/Source/Catalog.h`, the suite's product names) still exists and a test still compares it with the plugins' product names, so a new plugin must be added there; the "suite only" setting narrows the scan to them. See apps/olympus/README.md "Where it finds the plugins".
- A plugin or app builds from its own folder with the commands in its README (`cmake -S . -B build` there); it pulls `shared/` in
  by relative path. There is no root CMake project: each plugin fetches its own JUCE, all at the one version in [cmake/Juce.cmake](cmake/Juce.cmake)
  (see below).

## Building and checking: use scripts/build.sh

[scripts/build.sh](scripts/build.sh) is the one way to configure, build and check a plugin or app (`scripts/build.sh hestia`, `scripts/build.sh aegis ui snap`, `scripts/build.sh orpheus app`); its header says what it takes. Use it instead of
typing `cmake` by hand: it builds every target asked for in one `cmake --build` on all cores, configures only when the build folder has no CMakeCache.txt, and uses Ninja and ccache when they are installed (each target compiles its own copy of
JUCE, so ccache is the biggest saving; `brew install ninja ccache`). A build folder it makes does not install into the system plugin folders (except Themis' on macOS: `THEMIS_INSTALL_AFTER_BUILD` defaults to ON there and the script does not change it; the other *_INSTALL_AFTER_BUILD options stay off); one configured earlier with `*_INSTALL_AFTER_BUILD=ON` still does (check the cache, `grep INSTALL_AFTER_BUILD build/CMakeCache.txt`, before building a vst3 or au target), and Harmonia and Hephaestus always do. The default (`check`) builds and runs the DSP tests and the editor check. After a change in `shared/`, run it for every plugin that uses
the changed file (see the sections below). The plugins' READMEs keep the plain `cmake` commands for people without the script; if the script's behaviour changes, change its header, not a copy of it here.

## Skills

The procedures that are long and rarely needed live in [.claude/skills/](.claude/skills) (committed; the rest of `.claude/` is ignored). **Use them without being asked:** when a task matches one, invoke it with the
Skill tool before you start, and follow its steps instead of improvising them. Do not wait for the user to type the name.

- `new-plugin`: a request to create, add or scaffold a plugin.
- `plugin-parameter-change`: any edit to a plugin's `Parameters.h`, or adding a control or a setting (a mode, a look-ahead, a preset-able value).
- `shared-change`: any edit under `shared/`, before the change is called done.
- `install-and-validate`: build, install for a DAW, `auval`. It **replaces the plugins installed in `~/Library/Audio/Plug-Ins`**; the user has allowed Claude to run it itself when a task asks to build and install (the skill says to name what will be replaced before it builds). It
  installs nothing else and never changes the `*_INSTALL_AFTER_BUILD` options.

This file keeps only the rules; do not copy a skill's steps into it. A new skill is listed here with the request it answers, and its `description` says the same in its first sentence (that is what Claude chooses from).

## Sub-agents: the main session is the orchestrator

Five cheap agents live in [.claude/agents/](.claude/agents) (read-only except the fixer; each file owns its tools, limits and report format, so this section only says how they are chained). **The main session is the orchestrator:** it
decides which agents run, passes one agent's report to the next as text, and owns every decision; the agents never call each other and never talk to the user. Use them without being asked, as with the skills.

```
bug-hunter ──┐
             ├─► fixer ─► code-checker ─► feature-checker ─► commit-writer
code-checker ┘   (edits)   (the diff)      (completeness)     (only on request)
```

- `bug-hunter`: a symptom, a plugin or app → cause and place. `code-checker`: a diff or named files → findings. `fixer`: findings → the smallest edit, built with `scripts/build.sh`. `feature-checker`: a plugin and a feature → what is missing. `commit-writer`: commits by topic.
- **Fix chain** (a bug or a review finding): `bug-hunter` (skip it when the cause is already known) → `fixer` with its report → `code-checker` on the diff → back to `fixer` with the findings, **at most two rounds**; if it is not clean after the second, stop and
  tell the user what is left. **Feature chain** (after a change is made): `code-checker` and `feature-checker` in parallel → `fixer` for what they report → `code-checker` once more.
- Run agents in parallel only when they read the same state and do not depend on each other (the two checkers). Never run two `fixer`s at once, and never one while another agent reads the same files.
- The agents are cheap and narrow: they find and check, they do not decide. The orchestrator reads a report before acting on it, drops what it cannot verify in the code (an agent's guess is marked "unverified"), and does the judgement work itself: design, a `shared/` change, anything that touches several plugins.
- **What the orchestrator does itself, not through an agent:** the skills above (`plugin-parameter-change`, `shared-change`, `new-plugin`), regenerating and looking at screenshots, and `install-and-validate`. `fixer` stops and reports when a finding needs one of these.
- **Commits and pushes happen only when the user asks.** Then `commit-writer` does them, after the checks pass: say "push" to it only when the user said push. It never commits to `main`.
- A fix chain ends with a short report to the user: what was found, what changed (`git diff --stat`), the check that ran and its result, and what is left for them. Do not claim "done" on an agent's word: the build or test output decides.

### Every task runs through a pipeline of Haiku agents (standing rule from the user, 2026-11-20)

**Always, for every task** (a feature, a fix, a review, a doc change, a build-speed change; none is too small to skip it): the main session plans and writes the briefs, and the work itself is done by a pipeline of **Haiku** agents (`model: haiku`). Where the named agents above are not in the checkout (`.claude/` is ignored, a cloud clone does not have them), use a general-purpose agent with `model: haiku` and the brief below. The size of the task changes how many stages run, never whether the pipeline is used: a one-line fix is one `fixer` and the orchestrator's own check.

```
orchestrator: plan, split, write briefs ─► builder (one at a time) ─► reviewers (read-only, in parallel) ─► fixer (≤ 2 rounds) ─► orchestrator: re-run the checks, look at the pictures, decide, commit
```

- **Roles.** The orchestrator (main session) designs, splits the task, owns every decision, runs the final checks itself, and is the only one that talks to the user. Haiku agents implement, review and fix inside a brief; they decide nothing that is not in it. A `shared/` change or a change across plugins is still *designed* and *verified* by the orchestrator, and *edited* by an agent with the `shared-change` skill's steps in its brief.
- **One step, one agent, one concern.** Split a large task along its seams (this session: build speed → engine → window → devices and recording → monitoring → two review rounds), one builder at a time, never two builds or two editors on the same files. Reviewers only read; two reviewers (diff review and bug hunt) run in parallel because they read the same state.
- **A brief has everything the agent cannot guess**: the goal and what it is for; the files to read first (CLAUDE.md of the plugin or app, the files it will change); what it may and may not touch (`shared/`, commits); the exact commands to build and check (`scripts/build.sh ...`, foreground, with a timeout, no background jobs: a background job dies with the agent); the definition of done (checks pass, picture regenerated and looked at); a test for every defect or feature; and the report format (what changed with `git diff --stat`, pass counts, what it looked at, what it could **not** verify). Name the numbers and the examples, not "make it robust".
- **Verification is the orchestrator's, never the agent's word.** Re-run `scripts/build.sh` yourself, read the PNGs yourself, read the risky code yourself (audio thread, locks, anything that touches a file or a device), and check a changed test expectation (an agent that changes what a test expects may be hiding a bug). Drop a finding that cannot be confirmed in the code. A report that says "done" with a background job still running, or a build that was not run, is not done.
- **Reviewers find what builders miss: always run them.** After a builder, run a diff reviewer and a bug hunter before the fixer. Each reviewer gets the invariants of the code under review (the plugin's or app's CLAUDE.md) and a list of the failure classes to look for (audio thread, threads and locks, real third-party plugins and devices that the stand-in test plugin cannot show), and must say "unverified" for what it did not confirm. At most two fixer rounds; what is left after the second goes to the user as a list.
- **Parallel only for reads.** Fan out when agents read the same state and do not depend on each other; the cost of a multi-agent run is high, so do not fan out for show.
- **Commits and pushes** stay as in the list above (the user asks, or a hook the user configured asks); the orchestrator or `commit-writer` does them after its own check passes, one commit per verified step so a bad step can be reverted alone.

**Why this shape (research, checked 2026-11-20; read the weight of each source, most are practitioner reports):**

- Anthropic's own multi-agent research system is an orchestrator that plans and hands parts to subagents that run in parallel; with a stronger lead and cheaper subagents it beat a single agent by 90.2% on their internal eval, at about 15x the tokens of a chat, so it pays on high-value work. Each subagent needs an objective, an output format, guidance on tools and boundaries; vague briefs caused duplicated work, and effort rules had to be written into the prompts. Their own note: domains where all agents share one context or have many dependencies between agents fit badly. [Anthropic, How we built our multi-agent research system](https://www.anthropic.com/engineering/multi-agent-research-system). That test used Opus and Sonnet, **not** Haiku: nobody has shown for this repository that Haiku alone is enough, which is why the reviewers, the fixer rounds and the orchestrator's own verification are not optional.
- A chain of fixed, well-bounded steps with a check between them trades some latency for accuracy; start simple and add stages only when needed. [Anthropic, Building effective agents](https://www.anthropic.com/research/building-effective-agents) (prompt chaining and orchestrator-workers). Our stages are chained and gated by the orchestrator's checks.
- Why multi-agent systems fail: a study of traces found 14 failure modes in three groups, system design, agents misaligned with each other, and **task verification** (skipping or botching it); better prompts alone fixed only part. [Cemri et al., Why Do Multi-Agent LLM Systems Fail? (arXiv 2503.13657)](https://arxiv.org/abs/2503.13657). Hence: independent reviewers, tests per change, and the orchestrator re-running the checks.
- Cost: practitioner reports (self-measured, not controlled studies) put most of a pipeline's cost in an expensive orchestrator's accumulated context and little in cheap workers (one tally: Opus orchestrator 94%, Haiku researchers a few per cent), and report large savings from moving bounded subagent work to Haiku, with quality kept by gates. [Arize, How cheap models changed multi-agent economics](https://arize.com/blog/how-cheap-models-changed-multi-agent-economics/); [a Sonnet to Haiku hybrid write-up](https://valpere.github.io/blog/2026/04/27/ai-agent-cost-optimization/); [an Opus-orchestrator post-mortem](https://dev.to/shimo4228/i-tried-an-opus-orchestrator-and-killed-it-the-roi-of-multi-agent-systems-1f7g). Hence: keep the orchestrator's own context lean (briefs and reports, not file dumps) and hand the bulk reading and editing to Haiku.
- Subagent quality matters more than extra budget (Anthropic found a model upgrade of the subagents worth more than doubling their token budget), and Haiku does best on narrow, well-specified, verifiable work. Hence the narrow steps and the exact briefs above.

**What this session showed** (the first run of the rule: Olympus, 2026-11-20): Haiku built eight steps from precise briefs and every one passed its own checks, **and** the two review passes still found a dozen real defects the builders' tests could not see (a take placed late near time 0, silent takes when an input is switched off, plugins with sidechain buses, a render closing windows on a worker thread, the session rate not following the device); a builder left an unverified build running when it stopped; one changed a test's expectation (it was right, and was flagged). Lessons already in the rules above: brief with numbers, run the reviewers every time, verify as the orchestrator.

## shared/ui

- All the plugins and apps draw with it (Themis, Hephaestus, Harmonia, Iris, Atlas, Hestia, Aegis, Apollo, Hera, Castor, Nemesis, Dionysus, Hermes, Orpheus, Olympus); nothing in one draws its own panel, lamp, knob, button,
  selector or nameplate. What a plugin adds on top is small and lives in it (`ThemisUi.h`, `HephaestusUi.h`, `HarmoniaUi.h`, `IrisUi.h`, `AtlasUi.h`, `HestiaUi.h`, `AegisUi.h`, `ApolloUi.h`, `HeraUi.h`, `CastorUi.h`, `NemesisUi.h`, `DionysusUi.h`, `HermesUi.h`: its
  words, its verdict, mode or placement colours). A plugin's own namespaces must not be called `ui`, which is the kit's.
- It owns every pixel of the interface: [Hardware.h](shared/ui/include/pantheon/ui/Hardware.h) (`colours::`, fonts, `ui::hw`
  panels, glass, lamps, buttons, knobs, the header plate and the nameplate), [HardwareLookAndFeel.h](shared/ui/include/pantheon/ui/HardwareLookAndFeel.h),
  [Controls.h](shared/ui/include/pantheon/ui/Controls.h) (the parameter-bound `Knob`, `Choice` and `Toggle`; a choice takes its items from the parameter, so a plugin never lists them twice; clicking a `Knob`'s read-out opens it for typing a value),
  [ValueKnob.h](shared/ui/include/pantheon/ui/ValueKnob.h) (the same knob for a value that is not a plugin parameter, such as the setting of the selected note in an editor: it holds its own value and calls back; Orpheus and Olympus are its only users) and
  [MeterCell.h](shared/ui/include/pantheon/ui/MeterCell.h), [Advice.h](shared/ui/include/pantheon/ui/Advice.h) and [Note.h](shared/ui/include/pantheon/ui/Note.h) (the GUIDE button, `ui::drawNotes` and the foot `ui::drawNoteStrip`, and the plain-C++ `ui::Note` a plugin's own `Advice.h` returns: every plugin and app has a GUIDE saying what it is for and what it is not for, and notes that warn when its settings do not suit the job; the rules and the texts are the plugin's, the drawing is the kit's), [Scaled.h](shared/ui/include/pantheon/ui/Scaled.h) (`ui::ScaledEditor<Editor>`: the window of the plugins whose editor has one fixed design size, Atlas, Iris, Hephaestus, Hestia, Aegis, Apollo, Hera, Castor, Nemesis, Dionysus and Hermes; it opens at 100% of that size (its design width is 780 to 1650 px), scales between 50% and 150% from any of its four corners (`ui::ResizeGrips`) and keeps the scale in the session's state as `uiScale`; the editor inside is laid out and tested at its own size, and tests reach it with `ui::innerOf<Editor>`; Themis and Harmonia scale or lay out by themselves but use the same `ui::ResizeGrips` and the same 16:10 shape; their own limits (Harmonia 75% to 200%, Themis 1400 x 875 to 2080 x 1300, opening at the smallest) are in their own CLAUDE.md), [Aspect.h](shared/ui/include/pantheon/ui/Aspect.h) and [Resize.h](shared/ui/include/pantheon/ui/Resize.h) (**one window shape for every plugin and app: 16:10**, `ui::kEditorAspect` and `ui::editorHeightFor (width)`; an editor's design width is a multiple of 8, its height is `ui::editorHeightFor (kWidth)` and its main display takes what the rest of the layout leaves, never its own ratio, and a new plugin or app does the same; `ui::ResizeGrips` is the four corner handles over an editor: drag any corner to grow or shrink the window with the ratio kept, double-click one for the opening size; the host's own edges keep working through the same constrainer. The ratio was chosen from the plugins' own shapes (1.54 to 1.67 where there is a main display) and the 16:10 screens they run on: no standard exists, the hosts only ask for a fixed ratio with limits), [Presets.h](shared/ui/include/pantheon/ui/Presets.h) (`ui::Preset`, `ui::applyPreset`, `ui::Programs` for the processor and `ui::PresetBox` for the editor: the mechanism of the presets of Aegis, Apollo, Castor, Dionysus, Hera, Hermes, Hestia and Nemesis; the settings, names and one-line purposes are the plugin's own `Source/Presets.h`), plus [Undo.h](shared/ui/include/pantheon/ui/Undo.h) (`ui::UndoButtons` and `ui::beginUndoStep`: UNDO / REDO and Cmd+Z for the parameters, from a `juce::UndoManager` the processor owns and hands to its APVTS; opt-in, a plugin without one is untouched) and [AbCompare.h](shared/ui/include/pantheon/ui/AbCompare.h) (`ui::AbCompare` in the processor, two complete settings of the APVTS parameters with one live, and `ui::AbButtons` in the editor's header; a switch is one undo step and only the active slot is saved in the session; opt-in; Hephaestus, Harmonia, Hera, Hermes, Hestia, Aegis, Apollo and Iris use both, in the strip above their header controls, Apollo's in the row above its display), plus [FreqAxis.h](shared/ui/include/pantheon/ui/FreqAxis.h) (the logarithmic frequency axis the spectrum displays use; Themis' tonal view draws its own) and [Spectrogram.h](shared/ui/include/pantheon/ui/Spectrogram.h) (`ui::Spectrogram`: a ring of rows drawn on glass, levels in, the caller's grid; Atlas is its only user so far). Plugins call them; they do not draw their own panels, lamps or
  buttons. The namespaces (`colours`, `ui`, `ui::hw`) are kept short on purpose.
- Everything is drawn from code: no image or font files, nothing random at run time.
- **A change here changes every plugin that uses it.** Build all of them, and regenerate and look at each plugin's
  screenshots (the command is in that plugin's README; a regenerated picture that is byte-identical to the committed one needs no look). A look change that should be only one plugin's belongs in that
  plugin, not in the kit.

## shared/dsp

- What two or more plugins both need, and nothing a single plugin owns: today the double-precision [Biquad.h](shared/dsp/include/pantheon/dsp/Biquad.h)
  (RBJ designs including the parametric-EQ bell, shelves and notch, Butterworth cascades of 12 to 48 dB per octave, `magnitudeAt`
  for drawing and tests), [AnalyzerTap.h](shared/dsp/include/pantheon/dsp/AnalyzerTap.h) (the lock-free FIFOs from the audio thread to a display) and
  [SpectrumAnalyzer.h](shared/dsp/include/pantheon/dsp/SpectrumAnalyzer.h) (the calibrated FFT analyzer, GUI-free), used by Hephaestus, Iris, Apollo and Atlas (its display's analyser: `ApolloUiCheck` pins that it reads what the processor's taps send); and [TruePeak.h](shared/dsp/include/pantheon/dsp/TruePeak.h) (`pdsp::TruePeakReader`: the peak between the samples, read at 8x by a 64-tap windowed sinc, plain C++, delayed by `kTruePeakDelay` samples), which Aegis limits by and Hermes meters by, and which Themis' clip meter and limiter read by too, so the plugins cannot disagree about what a true peak is; and
  [BandLink.h](shared/dsp/include/pantheon/dsp/BandLink.h) (`pdsp::link`: bands handed between plugins as clipboard text, plain C++), which
  Themis writes (COPY, Copy EQ) and Iris, Apollo and Hephaestus read (PASTE; Atlas reads only its `curve` line, for TARGET); its `limiter` line (a ceiling to set and a gain change, from Themis' plan items) is read by Aegis' PASTE (`decodeLimiter`), and `decode` skips it; an `amount` key (0..1, default 1) scales a band's or a limiter's gain change, and a `width` line (the percent of the side signal per band of `pdsp::bands`, read by `decodeWidth` and skipped by `decode`) is read by Castor's PASTE, the one reader of it, from the same measurement as the side bell Iris and Apollo read (a width item hands both: one of them is pasted, not both). Its format is versioned (`pantheon-bands 1`): add keys, never change
  what one means, and keep the readers ignoring what they do not know. It also carries one `curve` line (a spectrum shape on a log-frequency
  grid: Themis' Copy EQ writes its target, Iris and Apollo read it as their reference; a reader without curves skips the line like any unknown kind).
  And [MusicBands.h](shared/dsp/include/pantheon/dsp/MusicBands.h) (`pdsp::bands`: the six main frequency bands of music, Sub 20–60 Hz, Bass 60–250 Hz,
  Low-Mid 250–500 Hz, Midrange 500 Hz–2 kHz, Upper-Mid 2–6 kHz, High 6–20 kHz, and `pdsp::BandSplitter`, the LR4 splitter whose bands sum flat), which every
  plugin that solos or splits by band uses: Themis' AUDITION ranges, Iris' regions (their default crossovers), Hephaestus' band solo strip, Atlas' six compressor bands (the splitter is its zero-latency split, the edges its default crossovers) and Castor's six width bands. The plugins with no band solo (Harmonia, Apollo, whose LISTEN is a band-pass at the band's own frequency, and the rest) do not use it. A new plugin with a band solo uses it
  too; Themis' readouts keep their own four `kEnergyRanges` (their limits were set on them). It is plain C++, so Themis' JUCE-free core includes it.
  And [SpectrumIssues.h](shared/dsp/include/pantheon/dsp/SpectrumIssues.h) (`pdsp::SpectrumGrid`, the 1/24-octave grid of a spectrogram, and `pdsp::IssueFinder`: narrow peaks that stay, and regions
  whose level keeps jumping over its average; plain C++, fed from a display's timer), written to be shared and used by Atlas so far; AtlasTests [9] pins it.
  And [BypassDelay.h](shared/dsp/include/pantheon/dsp/BypassDelay.h) (`pdsp::BypassDelay`: the delay a bypassed plugin applies by its reported latency, called from its `processBlockBypassed`, so a host bypass (AU; a VST3 host bypasses by itself) keeps the track in time; plain C++), used by every plugin that reports latency except Harmonia, whose host bypass is its own `bypass` parameter (mix 0: the input delayed by the latency, see its CLAUDE.md).
- It knows nothing about any plugin and has no GUI. The namespace is `pdsp` (not `dsp`, which would collide with `juce::dsp`).
- **A change here changes every plugin that uses it.** Run each one's DSP tests (Hephaestus: `HephaestusTests`; Iris: `IrisTests`; Atlas: `AtlasTests`, for MusicBands.h and Biquad.h; Aegis: `AegisTests`, Hermes: `HermesTests` and Themis: `make test`, for TruePeak.h; Castor: `CastorTests`, for MusicBands.h (its `BandSplitter`); Apollo, Dionysus, Hera, Hermes, Hestia and Nemesis: `ApolloTests`, `DionysusTests`, `HeraTests`, `HermesTests`, `HestiaTests` and `NemesisTests`, for Biquad.h (Castor's too); Themis: `make test`, for MusicBands.h and Biquad.h); the
  analyzer's calibration is pinned by Hephaestus' test [10]; BandLink by the five `UiCheck`s that paste (Iris, Hephaestus, Aegis, Apollo, Castor) and Themis' `ThemisSmoke` build. The files are
  tested through the plugins that use them, not on their own.

## Documentation shows the plugin

- **Every plugin's and app's README embeds its screenshot**, right after "Name origin" (`![Name Word](docs/screenshot.png)`), followed by an italic caption that says what the picture is (a headless render of the
  editor), which settings and programme it shows, and what to look at in it. A `docs/` image that no README shows does not count: the README is where it is read. Atlas, Iris, Themis and Apollo have several
  (a mode, a feature, a state): each is embedded with its own caption and the command that made it is in the README's "Development".
- The images are the output of the plugin's `Snapshot` target (arguments in the file's header), never edited by hand, and a caption states only what the picture shows. **A change to the interface, a parameter or a feature that
  appears in a picture is not done until the pictures are regenerated, looked at, and the caption still says what they show** (the plugin's CLAUDE.md has the command). A new feature gets a picture of its own when one
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

- **Installing on build.** Both set `COPY_PLUGIN_AFTER_BUILD TRUE` unconditionally, so building a plugin target copies it into
  `~/Library/Audio/Plug-Ins`. Themis makes this an option (`THEMIS_INSTALL_AFTER_BUILD`), which CI relies on.
- **CI** builds and tests Themis (macOS and Linux), runs Iris' `IrisTests`, `IrisUiCheck` and `IrisHostCheck` on Linux ([iris.yml](.github/workflows/iris.yml)), and the tests, the editor check and the VST3 build on Linux for Atlas ([atlas.yml](.github/workflows/atlas.yml)), Hestia ([hestia.yml](.github/workflows/hestia.yml)), Aegis, Apollo, Hera, Castor, Nemesis, Dionysus and Hermes (one workflow each, named after the plugin) Orpheus ([orpheus.yml](.github/workflows/orpheus.yml)) and Olympus ([olympus.yml](.github/workflows/olympus.yml); tests, window check, the app and the MCP server, no real plugin). Iris, Atlas, Hestia, the seven newest plugins, Orpheus and Olympus have no macOS job yet (universal binary, AU and `auval` for the plugins); only Iris and Hephaestus have a `HostCheck` (Hephaestus' is in no workflow); Atlas, Hestia and the seven newest were built on macOS by hand on 2026-11-13 (VST3 and AU) and `auval -v` passes for the nine AUs, which no workflow repeats; the workflows of Atlas, Hestia, Orpheus, Olympus and the seven newest have run and passed on main (checked 2026-11-20 with the GitHub run list; the same checks were first run by hand on Linux: tests, editor check, VST3 build and screenshot); and Hephaestus and Harmonia are in no workflow.
- **Presets in Atlas, Hephaestus and Iris are their own code** (a list, `applyPreset`, the program methods and a combo box each, written before `shared/ui/.../Presets.h` existed); the eight plugins that came after use the kit's. Move them onto it when one of the three is next touched.
- **The spectrogram and its finder in Iris and Themis.** `ui::Spectrogram` and `pdsp::IssueFinder` are in `shared/` for all three, but only Atlas uses them. Iris still has its own
  `ResonanceFinder` (the same idea as the finder's peaks, on its own grid) and no spectrogram or JUMPS markers; Themis has its own spectrogram (`core/include/themis/Spectrogram.h`, `plugin/Source/HistoryViews.h`, on its `BandGrid`) with no markers.
- **Shared DSP.** Only what a second plugin actually needed has been extracted (see shared/dsp). Themis' FFT, band grid and target
  curves, and Harmonia's pitch tracking, are used by one plugin each and stay there; so are Orpheus' offline pitch tracker and PSOLA ([apps/orpheus/Source/Dsp](apps/orpheus/Source/Dsp)), which are whole-file and share no code with Harmonia's real-time ones (if a second consumer needs either, extract then); Themis' `TonalAnalyzer` is not the same code as
  the shared spectrum analyser (it differs in precision and JUCE dependence). Extract when a second plugin needs the same code.
