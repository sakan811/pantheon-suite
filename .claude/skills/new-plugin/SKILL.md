---
name: new-plugin
description: Add a new plugin to this repository, start to finish (CMake, parameters, engine, editor, advice, tests, README, CLAUDE.md, workflow, root lists). Use when asked to create, add or scaffold a plugin.
disable-model-invocation: true
argument-hint: "[name] [what it does in one line]"
allowed-tools: Bash(scripts/build.sh *)
---

Make plugin `$ARGUMENTS`. The seven newest plugins were all made this way; the rules are in the root CLAUDE.md, the steps are here.

1. **Name and codes.** A Greek name for the plugin and one word for what it is (Hera Bond, Nemesis Hush). The plugin code and the manufacturer code are four letters and must not repeat one
   already used: `grep -hE "PLUGIN_CODE|PLUGIN_MANUFACTURER_CODE|BUNDLE_ID" plugins/*/CMakeLists.txt`.
2. **Start from a copy, not from memory.** Copy `plugins/nemesis` (small: one detector, one gain) or `plugins/hera` (a sidechain, presets, a latency) to `plugins/<name>` without `build/`, then rename the
   namespace, the targets (`<Name>`, `<Name>Tests`, `<Name>UiCheck`, `<Name>Snapshot`), the parameter ids and the words. Read the copied plugin's CLAUDE.md first: it says why each piece is there.
3. **What the plugin has** (each one a file or a section of the base plugin): `Source/DSP/Engine.h` with no JUCE in it; `Parameters.h` (the single source of truth, every `ParameterID` version 1);
   `Presets.h` using `shared/ui/.../Presets.h`; `Advice.h` (notes under the controls and a GUIDE that says what it is for and what it is not for); `PluginProcessor.*`; `PluginEditor.*` and `UI/<Name>Ui.h`
   (only its words and colours: every pixel is `shared/ui`); `createEditor()` returns a `ui::ScaledEditor<Editor>`; `Tests/Tests.cpp` (DSP), `UiCheck.cpp`, `Snapshot.cpp`, `Check.h`.
4. **Rules that are easy to forget.** Every source file starts with the two SPDX lines (copy them). The JUCE switches come from `PANTHEON_JUCE_DEFINITIONS`, not retyped, and docs never name the JUCE
   version. `<NAME>_INSTALL_AFTER_BUILD` is an option that defaults to OFF. No other company's product is named in the UI or in source comments. The engine allocates nothing in `process()`, replaces NaN and
   infinity by 0, and is bit-exact when it does nothing. Every number in the README is measured by a test.
5. **Outside the folder.** Copy `.github/workflows/nemesis.yml` to `<name>.yml` and rename only the names and paths (leave the pinned action SHAs alone). Add the plugin to the lists it belongs in: the root
   `README.md`, `NOTICE.md` where it lists plugins, and the root `CLAUDE.md` (the shared/ui list, the CI paragraph, any `shared/dsp` consumer list). `grep -rIl Nemesis --exclude-dir=build --exclude-dir=third_party .`
   shows every place a plugin is named.
6. **Check.** `scripts/build.sh <name> check snap`, then look at `docs/screenshot.png` (the plugin's CLAUDE.md says a UI change is not done until you have). For the AU, VST3 and `auval`, use the
   `install-and-validate` skill.
7. Do not commit unless asked. When you do: by topic, with the Co-Authored-By trailer.
