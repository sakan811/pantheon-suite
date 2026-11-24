#!/usr/bin/env bash
# SPDX-License-Identifier: AGPL-3.0-or-later
# Copyright (C) 2026 The Pantheon authors
#
# Launch the standalone app for an application or plugin, building it first if needed.
#
#   scripts/open.sh <app> [file...]
#
#   <app>      olympus, orpheus, or any plugin in plugins/ that has a standalone target
#   [file...]  extra arguments passed to the app (e.g. a .olympus project, .orpheus project, or .wav file)
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
[ $# -ge 1 ] || { sed -n '5,10p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; exit 2; }

name="$1"; shift
dir=""
for base in apps plugins; do
    [ -f "$root/$base/$name/CMakeLists.txt" ] && dir="$root/$base/$name"
done
[ -n "$dir" ] || { echo "no app or plugin called '$name' (see apps/ and plugins/)" >&2; exit 2; }

Name="$(tr '[:lower:]' '[:upper:]' <<< "${name:0:1}")${name:1}"

find_bundle() {
    [ -d "$dir/build" ] || return 0
    find "$dir/build" -maxdepth 4 -type d \( -name "${Name}.app" -o -name "${Name} *.app" \) -not -path '*/CMakeFiles/*' 2>/dev/null | head -1 || true
}

find_bin() {
    [ -d "$dir/build" ] || return 0
    find "$dir/build" -maxdepth 4 -type f -perm -u+x \( -name "$Name" -o -name "${Name}_Standalone" -o -name "${Name} *" \) -not -path '*/CMakeFiles/*' 2>/dev/null | head -1 || true
}

if [ "$(uname -s)" = "Darwin" ]; then
    app_bundle="$(find_bundle)"
    if [ -z "$app_bundle" ]; then
        "$root/scripts/build.sh" "$name" app
        app_bundle="$(find_bundle)"
    fi
    [ -n "$app_bundle" ] || { echo "could not find .app bundle for '$name' in $dir/build" >&2; exit 1; }
    if [ $# -eq 0 ]; then
        exec open "$app_bundle"
    else
        exec open -a "$app_bundle" -- "$@"
    fi
else
    bin="$(find_bin)"
    if [ -z "$bin" ]; then
        "$root/scripts/build.sh" "$name" app
        bin="$(find_bin)"
    fi
    [ -n "$bin" ] || { echo "could not find standalone executable for '$name' in $dir/build" >&2; exit 1; }
    exec "$bin" "$@"
fi
