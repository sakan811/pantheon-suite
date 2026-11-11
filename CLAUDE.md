# CLAUDE.md

Rules for the repository as a whole. Each plugin has its own CLAUDE.md in `plugins/<name>/`, and each app in `apps/<name>/` (Claude Code reads it when you
work in that folder; Harmonia's is still missing, see the end of this file); it owns that plugin's architecture, commands and invariants. This file owns only what crosses plugins.
The same rule as in the plugins applies here: every fact lives in one place, the others link to it.

## Layout and dependency direction

```
shared/ui/       the hardware-style interface kit (JUCE GUI only, header-only), target `pantheon_ui`
shared/dsp/      DSP that more than one plugin uses (header-only, namespace `pdsp`), target `pantheon_dsp`
plugins/<name>/  one self-contained plugin each: its own CMakeLists.txt, core/, tests, README, CLAUDE.md
apps/<name>/     one self-contained standalone application each (no plugin formats, no host): the same files as a plugin; today Orpheus, the pitch editor
.github/         CI for the whole repository (workflows run per plugin with `working-directory`)
```

- Dependencies point one way: `plugins/*` and `apps/*` → `shared/*`, never the reverse, and never from one plugin or app to another. If two
  of them need the same code, it moves to `shared/` first and both use it from there.
- `shared/` knows nothing about any plugin: no plugin types, parameters, verdicts or words in it. What a plugin adds on top
  (its name, its number formats, its verdict colours) lives in that plugin; Themis' is `plugin/Source/ThemisUi.h`.
- A plugin or app builds from its own folder with the commands in its README (`cmake -S . -B build` there); it pulls `shared/` in
  by relative path. There is no root CMake project: each plugin fetches its own JUCE, and they pin different versions (see
  below), which a single root project could not.

## shared/ui

- All the plugins and apps draw with it (Themis, Hephaestus, Harmonia, Iris, Atlas, Orpheus); nothing in one draws its own panel, lamp, knob, button,
  selector or nameplate. What a plugin adds on top is small and lives in it (`ThemisUi.h`, `HephaestusUi.h`, `HarmoniaUi.h`, `IrisUi.h`, `AtlasUi.h`: its
  words, its verdict, mode or placement colours). A plugin's own namespaces must not be called `ui`, which is the kit's.
- It owns every pixel of the interface: [Hardware.h](shared/ui/include/pantheon/ui/Hardware.h) (`colours::`, fonts, `ui::hw`
  panels, glass, lamps, buttons, knobs, the header plate and the nameplate), [HardwareLookAndFeel.h](shared/ui/include/pantheon/ui/HardwareLookAndFeel.h),
  [Controls.h](shared/ui/include/pantheon/ui/Controls.h) (the parameter-bound `Knob`, `Choice` and `Toggle`; a choice takes its items from the parameter, so a plugin never lists them twice; clicking a `Knob`'s read-out opens it for typing a value),
  [ValueKnob.h](shared/ui/include/pantheon/ui/ValueKnob.h) (the same knob for a value that is not a plugin parameter, such as the setting of the selected note in an editor: it holds its own value and calls back; Orpheus is its only user) and
  [MeterCell.h](shared/ui/include/pantheon/ui/MeterCell.h), [Advice.h](shared/ui/include/pantheon/ui/Advice.h) and [Note.h](shared/ui/include/pantheon/ui/Note.h) (the GUIDE button, `ui::drawNotes` and the foot `ui::drawNoteStrip`, and the plain-C++ `ui::Note` a plugin's own `Advice.h` returns: every plugin and app has a GUIDE saying what it is for and what it is not for, and notes that warn when its settings do not suit the job; the rules and the texts are the plugin's, the drawing is the kit's), plus [FreqAxis.h](shared/ui/include/pantheon/ui/FreqAxis.h) (the logarithmic frequency axis every spectrum display uses) and [Spectrogram.h](shared/ui/include/pantheon/ui/Spectrogram.h) (`ui::Spectrogram`: a ring of rows drawn on glass, levels in, the caller's grid; Atlas is its only user so far). Plugins call them; they do not draw their own panels, lamps or
  buttons. The namespaces (`colours`, `ui`, `ui::hw`) are kept short on purpose.
- Everything is drawn from code: no image or font files, nothing random at run time.
- **A change here changes every plugin that uses it.** Build all of them, and regenerate and look at each plugin's
  screenshots (the command is in that plugin's README). A look change that should be only one plugin's belongs in that
  plugin, not in the kit.

## shared/dsp

- What two or more plugins both need, and nothing a single plugin owns: today the double-precision [Biquad.h](shared/dsp/include/pantheon/dsp/Biquad.h)
  (RBJ designs including the parametric-EQ bell, shelves and notch, Butterworth cascades of 12 to 48 dB per octave, `magnitudeAt`
  for drawing and tests), [AnalyzerTap.h](shared/dsp/include/pantheon/dsp/AnalyzerTap.h) (the lock-free FIFOs from the audio thread to a display) and
  [SpectrumAnalyzer.h](shared/dsp/include/pantheon/dsp/SpectrumAnalyzer.h) (the calibrated FFT analyzer, GUI-free), used by Hephaestus and Iris; and
  [BandLink.h](shared/dsp/include/pantheon/dsp/BandLink.h) (`pdsp::link`: bands handed between plugins as clipboard text, plain C++), which
  Themis writes (COPY, Copy EQ) and Iris and Hephaestus read (PASTE). Its format is versioned (`pantheon-bands 1`): add keys, never change
  what one means, and keep the readers ignoring what they do not know. It also carries one `curve` line (a spectrum shape on a log-frequency
  grid: Themis' Copy EQ writes its target, Iris reads it as its reference; a reader without curves skips the line like any unknown kind).
  And [MusicBands.h](shared/dsp/include/pantheon/dsp/MusicBands.h) (`pdsp::bands`: the six main frequency bands of music, Sub 20–60 Hz, Bass 60–250 Hz,
  Low-Mid 250–500 Hz, Midrange 500 Hz–2 kHz, Upper-Mid 2–6 kHz, High 6–20 kHz, and `pdsp::BandSplitter`, the LR4 splitter whose bands sum flat), which every
  plugin but Harmonia solos by: Themis' AUDITION ranges, Iris' regions (their default crossovers), Hephaestus' band solo strip and Atlas' six compressor bands (the splitter is its zero-latency split, the edges its default crossovers). A new plugin with a band solo uses it
  too; Themis' readouts keep their own four `kEnergyRanges` (their limits were set on them). It is plain C++, so Themis' JUCE-free core includes it.
  And [SpectrumIssues.h](shared/dsp/include/pantheon/dsp/SpectrumIssues.h) (`pdsp::SpectrumGrid`, the 1/24-octave grid of a spectrogram, and `pdsp::IssueFinder`: narrow peaks that stay, and regions
  whose level keeps jumping over its average; plain C++, fed from a display's timer), written to be shared and used by Atlas so far; AtlasTests [9] pins it.
- It knows nothing about any plugin and has no GUI. The namespace is `pdsp` (not `dsp`, which would collide with `juce::dsp`).
- **A change here changes every plugin that uses it.** Run each one's DSP tests (Hephaestus: `HephaestusTests`; Iris: `IrisTests`; Atlas: `AtlasTests`, for MusicBands.h and Biquad.h; Themis: `make test`, for MusicBands.h); the
  analyzer's calibration is pinned by Hephaestus' test [10]; BandLink by both `UiCheck`s (PASTE) and Themis' `ThemisSmoke` build. The files are
  tested through the plugins that use them, not on their own.

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

## Not yet in line (Hephaestus and Harmonia were imported as they were)

Delete an item here when it is done; this list is the only place that says what is left.

- **Installing on build.** Both set `COPY_PLUGIN_AFTER_BUILD TRUE` unconditionally, so building a plugin target copies it into
  `~/Library/Audio/Plug-Ins`. Themis makes this an option (`THEMIS_INSTALL_AFTER_BUILD`), which CI relies on.
- **CI** builds and tests Themis (macOS and Linux), runs Iris' `IrisTests`, `IrisUiCheck` and `IrisHostCheck` on Linux ([iris.yml](.github/workflows/iris.yml)) Atlas' `AtlasTests` and `AtlasUiCheck` on Linux ([atlas.yml](.github/workflows/atlas.yml)) and Orpheus' `OrpheusTests` and `OrpheusUiCheck` on Linux ([orpheus.yml](.github/workflows/orpheus.yml)). Iris, Atlas and Orpheus have no macOS job yet (universal binary, AU and `auval` for Iris and Atlas), Atlas has no `HostCheck` and has not been built on macOS or run through `auval`, the Atlas and Orpheus workflows have not run yet, and Hephaestus and Harmonia are in no workflow.
- **Harmonia has no CLAUDE.md.** Its architecture, invariants and gotchas are not written down yet; its README is the only description.
- **The spectrogram and its finder in Iris and Themis.** `ui::Spectrogram` and `pdsp::IssueFinder` are in `shared/` for all three, but only Atlas uses them. Iris still has its own
  `ResonanceFinder` (the same idea as the finder's peaks, on its own grid) and no spectrogram or JUMPS markers; Themis has its own spectrogram (`core/.../Spectrogram.h`, `HistoryViews.h`, on its `BandGrid`) with no markers.
- **Shared DSP.** Only what a second plugin actually needed has been extracted (see shared/dsp). Themis' FFT, band grid and target
  curves, and Harmonia's pitch tracking, are used by one plugin each and stay there; so are Orpheus' offline pitch tracker and PSOLA ([apps/orpheus/Source/Dsp](apps/orpheus/Source/Dsp)), which are whole-file and share no code with Harmonia's real-time ones (if a second consumer needs either, extract then); Themis' `TonalAnalyzer` is not the same code as
  the shared spectrum analyser (it differs in precision and JUCE dependence). Extract when a second plugin needs the same code.
