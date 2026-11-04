# CLAUDE.md

Rules for the repository as a whole. Each plugin has its own CLAUDE.md in `plugins/<name>/` (Claude Code reads it when you
work in that folder; Harmonia's is still missing, see the end of this file); it owns that plugin's architecture, commands and invariants. This file owns only what crosses plugins.
The same rule as in the plugins applies here: every fact lives in one place, the others link to it.

## Layout and dependency direction

```
shared/ui/       the hardware-style interface kit (JUCE GUI only, header-only), target `pantheon_ui`
plugins/<name>/  one self-contained plugin each: its own CMakeLists.txt, core/, tests, README, CLAUDE.md
.github/         CI for the whole repository (workflows run per plugin with `working-directory`)
```

- Dependencies point one way: `plugins/*` → `shared/*`, never the reverse, and never from one plugin to another. If two
  plugins need the same code, it moves to `shared/` first and both use it from there.
- `shared/` knows nothing about any plugin: no plugin types, parameters, verdicts or words in it. What a plugin adds on top
  (its name, its number formats, its verdict colours) lives in that plugin; Themis' is `plugin/Source/ThemisUi.h`.
- A plugin builds from its own folder with the commands in its README (`cmake -S . -B build` there); it pulls `shared/` in
  by relative path. There is no root CMake project: each plugin fetches its own JUCE, and they pin different versions (see
  below), which a single root project could not.

## shared/ui

- All three plugins draw with it (Themis, Hephaestus, Harmonia); nothing in a plugin draws its own panel, lamp, knob, button,
  selector or nameplate. What a plugin adds on top is small and lives in it (`ThemisUi.h`, `HephaestusUi.h`, `HarmoniaUi.h`: its
  words, its verdict or mode colours). A plugin's own namespaces must not be called `ui`, which is the kit's.
- It owns every pixel of the interface: [Hardware.h](shared/ui/include/pantheon/ui/Hardware.h) (`colours::`, fonts, `ui::hw`
  panels, glass, lamps, buttons, knobs, the header plate and the nameplate), [HardwareLookAndFeel.h](shared/ui/include/pantheon/ui/HardwareLookAndFeel.h),
  [Controls.h](shared/ui/include/pantheon/ui/Controls.h) (the parameter-bound `Knob`, `Choice` and `Toggle`; a choice takes its items from the parameter, so a plugin never lists them twice; clicking a `Knob`'s read-out opens it for typing a value) and
  [MeterCell.h](shared/ui/include/pantheon/ui/MeterCell.h). Plugins call them; they do not draw their own panels, lamps or
  buttons. The namespaces (`colours`, `ui`, `ui::hw`) are kept short on purpose.
- Everything is drawn from code: no image or font files, nothing random at run time.
- **A change here changes every plugin that uses it.** Build all of them, and regenerate and look at each plugin's
  screenshots (the command is in that plugin's README). A look change that should be only one plugin's belongs in that
  plugin, not in the kit.

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
- **CI** builds and tests Themis only.
- **Harmonia has no CLAUDE.md.** Its architecture, invariants and gotchas are not written down yet; its README is the only description.
- **Shared DSP.** Not extracted on purpose: today only Themis uses its FFT, band grid and target curves, and the two real
  duplicates (Hephaestus' own `Biquad`, and its spectrum analyser against Themis' `TonalAnalyzer`) differ in precision and
  JUCE dependence. Extract when a second plugin actually needs the same code.
