---
name: install-and-validate
description: Build a plugin's VST3 and AU, install it for the user's DAW and validate the AU with auval. Use when asked to build and install, add a plugin to the DAW, or run auval.
disable-model-invocation: true
argument-hint: "[plugin name, or several]"
allowed-tools: Bash(scripts/build.sh *) Bash(auval -v *)
---

For each plugin in `$ARGUMENTS` (none given: ask which):

1. **Find out whether building installs.** `grep INSTALL_AFTER_BUILD plugins/<name>/build/CMakeCache.txt`. Where it is ON, building the `vst3` or `au` target copies the plugin into
   `~/Library/Audio/Plug-Ins/VST3` and `.../Components`, replacing the copies there. Harmonia and Hephaestus install on every build whatever the option says. Say so before you build; never switch the option
   on or off, and never copy bundles by hand, unless the user asked. Where it is OFF (or there is no build folder), tell the user what would install and ask.
2. **Check, then build the formats.** `scripts/build.sh <name> check` first; if it does not pass, stop and report it. Then `scripts/build.sh <name> vst3 au`. A failed build is reported with its output, not
   worked around.
3. **Validate the AU.** The codes are `PLUGIN_CODE` and `PLUGIN_MANUFACTURER_CODE` in the plugin's `CMakeLists.txt`: `auval -v aufx <PLUGIN_CODE> <MANUFACTURER_CODE>`. The result is "AU VALIDATION SUCCEEDED" or it
   is not; quote the failing lines when it is not.
4. **Confirm what is installed is what was built.** `cmp` the binary in `build/<Name>_artefacts/Release/{VST3,AU}/` with the one in `~/Library/Audio/Plug-Ins/`, and `codesign -v` the installed bundle
   (Apple Silicon will not load an unsigned one; Harmonia's own `scripts/build.sh` signs its copies).
5. **Say what is left for the user:** rescan plug-ins in the DAW (it caches the list), and which plugin copies were replaced. Report faithfully: what built, what was validated, what was not run.
