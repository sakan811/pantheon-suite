---
name: plugin-parameter-change
description: Add, change or remove a parameter of a plugin without breaking saved sessions, presets, tests or docs. Use when editing a plugin's Parameters.h, adding a control, or adding a setting such as a look-ahead or a mode.
paths: plugins/*/Source/Parameters.h
allowed-tools: Bash(scripts/build.sh *)
---

A parameter's id is stored in every saved session and every automation lane. Work through this in order; the plugin's own CLAUDE.md has the details of its engine.

1. **Never rename or remove an id, and never reorder a choice list** (names follow the enum or the engine's list; add at the end). A parameter that is no longer wanted stays, unused.
2. **A new parameter is `ParameterID { id, 2 }`** (a higher version than the others), so a session saved before it opens with its default. That default must be the old behaviour (a new look-ahead is Off), and the
   engine at that default must still pass every existing test, bit for bit where it was bit-exact.
3. **The four places:** the `id::` name, the entry in `createLayout()`, `readParams()` (clamp a choice to its list), and the engine's params struct. The editor reads a choice's items from the parameter: never list them twice.
4. **The editor.** A `ui::Knob`, `ui::Choice` or `ui::Toggle` bound to the id (declare it with the others, initialise it in the same order, add it to the `addAndMakeVisible` list, give it a tooltip that says what it does and
   what it costs). `setSize()` stays last. If it needs room the design size changes: check it still fits and that nothing overlaps.
5. **If it moves the latency** (a look-ahead, an oversampling, a phase mode): the engine reports it through an atomic and the processor hands it to the host on the message thread (`AsyncUpdater`, as Hera and
   Apollo do); changing it while playing restarts the delay line, and the README and a note say so.
6. **Presets.** Go through each preset in `Source/Presets.h`: does it set the new parameter? Anything it does not list goes back to the default. A session-level parameter goes in the plugin's `keepsParameter`.
7. **Tests.** The parameter count and the default in `UiCheck`; a DSP test that measures what the parameter does (not just that it is stored); a processor-level check if the host is told something.
8. **Docs.** The README table row, the "Measured" line with the number the test prints, and every sentence in the README, `Advice.h` (notes and GUIDE) and the plugin's CLAUDE.md that says the plugin does not have it. A note is at
   most `ui::kMaxNoteChars` characters. The CLAUDE.md "Parameters" rule lists which ids are not version 1.
9. **Check.** `scripts/build.sh <name> check snap`, then look at `docs/screenshot.png`. Not done until the check passes and you have looked.
