#!/usr/bin/env bash
# SPDX-License-Identifier: AGPL-3.0-or-later
# Copyright (C) 2026 The Pantheon authors
#
# One way to configure, build and check a plugin or app of this repository, as fast as this machine allows.
#
#   scripts/build.sh <name> [what...]
#
#   <name>   a folder of plugins/ or apps/ (hestia, aegis, orpheus, ...)
#   what     check (default)  the DSP tests and the editor check, built together and run
#            tests | ui | snap | host   <Name>Tests, <Name>UiCheck, <Name>Snapshot, <Name>HostCheck (built and run; snap writes docs/screenshot.png)
#            vst3 | au | app            the plugin formats and the standalone (built, not run)
#            all                        everything above that the plugin has
#            anything else              taken as a CMake target name, built (and run if it is a console tool)
#
# What makes it quick: one `cmake --build` for all the targets asked for (so the cores are shared between them, not used
# one target after another); every core; configure only when there is no build folder (CMake re-configures by itself when a
# CMakeLists changes); Ninja for a new build folder, and ccache for every compile, when they are installed (`brew install ninja ccache`).
# Each target compiles its own copy of JUCE, which is most of a cold build: ccache turns the second and later ones into cache hits.
# A build folder made by this script does not install into the system plugin folders (the *_INSTALL_AFTER_BUILD options stay off). A folder that
# was configured with one of them ON keeps it (the script never changes a cache): building its vst3 or au target then copies the plugin into
# ~/Library/Audio/Plug-Ins, replacing the copy there. Harmonia and Hephaestus install on every build, whatever the option.
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
[ $# -ge 1 ] || { sed -n '5,17p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; exit 2; }

name="$1"; shift
dir=""
for base in plugins apps; do [ -f "$root/$base/$name/CMakeLists.txt" ] && dir="$root/$base/$name"; done
[ -n "$dir" ] || { echo "no plugin or app called '$name' (see plugins/ and apps/)" >&2; exit 2; }
Name="$(tr '[:lower:]' '[:upper:]' <<< "${name:0:1}")${name:1}"
[ $# -ge 1 ] || set -- check

jobs="$(sysctl -n hw.ncpu 2>/dev/null || nproc 2>/dev/null || echo 4)"
build="$dir/build"

if [ ! -f "$build/CMakeCache.txt" ]; then
    args=(-S "$dir" -B "$build" -DCMAKE_BUILD_TYPE=Release)
    command -v ninja >/dev/null && args+=(-G Ninja)
    command -v ccache >/dev/null && args+=(-DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache -DCMAKE_OBJCXX_COMPILER_LAUNCHER=ccache)
    cmake "${args[@]}" >/dev/null
    command -v ccache >/dev/null || echo "note: ccache is not installed; \`brew install ccache\` makes every later build of JUCE a cache hit" >&2
fi

targets="$(cmake --build "$build" --target help 2>/dev/null || true)"
has() { grep -qE "(^|[ .])$1( |$)" <<< "$targets"; }

build_list=(); run_list=()
add() { # add <target> <run?>
    if has "$1"; then build_list+=("$1"); [ "$2" = run ] && run_list+=("$1"); return 0; fi
    return 1
}
for w in "$@"; do
    case "$w" in
        check) add "${Name}Tests" run || add "${name}_tests" run || true; add "${Name}UiCheck" run || true ;;
        tests) add "${Name}Tests" run || add "${name}_tests" run || echo "no tests target for $name" >&2 ;;
        ui)    add "${Name}UiCheck" run   || echo "no UiCheck for $name" >&2 ;;
        snap)  add "${Name}Snapshot" run  || echo "no Snapshot for $name" >&2 ;;
        host)  add "${Name}HostCheck" run || echo "no HostCheck for $name" >&2 ;;
        vst3)  add "${Name}_VST3" no      || echo "no VST3 target for $name" >&2 ;;
        au)    add "${Name}_AU" no        || echo "no AU target for $name" >&2 ;;
        app)   add "${Name}_Standalone" no || add "$Name" no || echo "no standalone for $name" >&2 ;;
        all)   for t in Tests UiCheck HostCheck; do add "$Name$t" run || true; done
               for t in _VST3 _AU _Standalone; do add "$Name$t" no || true; done ;;
        *)     has "$w" && { build_list+=("$w"); case "$w" in *Tests|*UiCheck|*Snapshot|*HostCheck|*Smoke) run_list+=("$w");; esac; } || { echo "no target '$w' in $name" >&2; exit 2; } ;;
    esac
done
[ ${#build_list[@]} -gt 0 ] || { echo "nothing to build" >&2; exit 2; }

echo ">> building ${build_list[*]} (-j $jobs)"
cmake --build "$build" --target "${build_list[@]}" -j "$jobs"

status=0
for t in ${run_list[@]+"${run_list[@]}"}; do
    exe="$(find "$build" -type f -perm -u+x -name "$t" -not -path '*/CMakeFiles/*' 2>/dev/null | head -1)"
    [ -n "$exe" ] || { echo "built $t but did not find the executable" >&2; status=1; continue; }
    echo ">> running $t"
    case "$t" in
        *Snapshot) (cd "$dir" && "$exe" docs/screenshot.png) || status=1 ;;
        *)         (cd "$dir" && "$exe") || status=1 ;;
    esac
done
exit $status
