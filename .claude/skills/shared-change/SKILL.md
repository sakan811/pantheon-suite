---
name: shared-change
description: Check every plugin that uses a file in shared/ after changing it. Use after editing anything under shared/ui or shared/dsp, before saying the change is done.
paths: shared/**
allowed-tools: Bash(scripts/build.sh *) Bash(grep *) Bash(tail *) Bash(git status *) Bash(git diff *)
model: haiku
---

A change in `shared/` changes every plugin and app that includes it (the root CLAUDE.md, "shared/ui" and "shared/dsp"). Do not rely on a list in this file: find the users.

1. **Which files changed:** `git diff --name-only -- shared/`.
2. **Who uses them.** For a `shared/dsp` header: `grep -rlE "pantheon/dsp/<Header>" plugins apps --include=*.h --include=*.cpp | cut -d/ -f1-2 | sort -u`. `shared/ui` is used by every plugin and app
   (list `plugins/*` and `apps/*`). `BandLink.h` is also read by Themis' smoke build and both `UiCheck`s that paste.
3. **Run each user's checks.** `scripts/build.sh <name>` for each (tests and the editor check). Themis builds with its own `make test` in `plugins/themis`; Harmonia has `HarmoniaDevTool selftest` (see its CLAUDE.md).
   Run them one plugin at a time (the script takes one plugin per call). Do not read whole logs: pipe each run through `tail -n 30` (the pass/fail summary is at the end) and `grep -nE "FAIL|error"` only for a plugin that failed.
4. **For a `shared/ui` change, look, but only where the picture changed.** `scripts/build.sh <name> snap` for each, then `git status --porcelain -- '*/docs/*.png'`. The images are deterministic, so one that is byte-identical needs no look; open only the changed ones. A look change that should be one plugin's belongs in that plugin, not in the kit.
5. **Report per plugin:** passed, failed with the failing line, or not run and why. If a check needs numbers the README quotes (the analyser calibration is Hephaestus' test [10]), compare them.
