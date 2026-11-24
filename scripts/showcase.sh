#!/usr/bin/env bash
# SPDX-License-Identifier: AGPL-3.0-or-later
# Copyright (C) 2026 The Pantheon authors
#
# Capture animated frames from <Name>Snapshot (or HarmoniaDevTool / ThemisSmoke)
# and encode docs/showcase.gif with ffmpeg.
#
#   scripts/showcase.sh <name|all>
#
#   <name>  a folder of plugins/ or apps/, or 'all' to run for every plugin and app
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
[ $# -eq 1 ] || { sed -n '5,10p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; exit 2; }
command -v ffmpeg >/dev/null 2>&1 || { echo "ffmpeg is required on PATH" >&2; exit 2; }

name="$1"

if [ "$name" = "all" ]; then
    status=0
    for d in "$root/plugins"/* "$root/apps"/*; do
        [ -d "$d" ] || continue
        "${BASH_SOURCE[0]}" "$(basename "$d")" || status=1
    done
    exit "$status"
fi

dir=""
for base in plugins apps; do
    [ -f "$root/$base/$name/CMakeLists.txt" ] && dir="$root/$base/$name"
done
[ -n "$dir" ] || { echo "no plugin or app called '$name' (see plugins/ and apps/)" >&2; exit 2; }

Name="$(tr '[:lower:]' '[:upper:]' <<< "${name:0:1}")${name:1}"
case "$name" in
    harmonia) target="HarmoniaDevTool" ;;
    themis)   target="ThemisSmoke" ;;
    *)        target="${Name}Snapshot" ;;
esac

jobs="$(sysctl -n hw.ncpu 2>/dev/null || nproc 2>/dev/null || echo 4)"
build="$dir/build"

if [ ! -f "$build/CMakeCache.txt" ]; then
    args=(-S "$dir" -B "$build" -DCMAKE_BUILD_TYPE=Release)
    command -v ninja >/dev/null && args+=(-G Ninja)
    command -v ccache >/dev/null && args+=(-DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache -DCMAKE_OBJCXX_COMPILER_LAUNCHER=ccache)
    [ "$name" = "themis" ] && args+=(-DTHEMIS_BUILD_SMOKE=ON)
    cmake "${args[@]}" >/dev/null
    command -v ccache >/dev/null || echo "note: ccache is not installed; \`brew install ccache\` makes every later build of JUCE a cache hit" >&2
elif [ "$name" = "themis" ] && ! grep -q '^THEMIS_BUILD_SMOKE:BOOL=ON$' "$build/CMakeCache.txt"; then
    cmake -S "$dir" -B "$build" -DTHEMIS_BUILD_SMOKE=ON >/dev/null
fi

echo ">> building $target (-j $jobs)"
cmake --build "$build" --target "$target" -j "$jobs"

exe="$(find "$build" -type f -perm -u+x -name "$target" -not -path '*/CMakeFiles/*' 2>/dev/null | head -1 || true)"
[ -n "$exe" ] || { echo "built $target but did not find the executable" >&2; exit 1; }

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

echo ">> capturing frames with $target"
if [ "$name" = "harmonia" ]; then
    (cd "$dir" && "$exe" snapshot --frames "$tmp")
else
    (cd "$dir" && "$exe" --frames "$tmp")
fi

if ! ls "$tmp"/frame_*.png >/dev/null 2>&1; then
    echo "no frame_*.png files produced by $target" >&2
    exit 1
fi

mkdir -p "$dir/docs"
ffmpeg -y -framerate 12 -i "$tmp/frame_%03d.png" -vf "scale=840:-1:flags=lanczos,palettegen=stats_mode=diff" "$tmp/palette.png"
ffmpeg -y -framerate 12 -i "$tmp/frame_%03d.png" -i "$tmp/palette.png" -lavfi "scale=840:-1:flags=lanczos[x];[x][1:v]paletteuse=dither=bayer:bayer_scale=3" "$dir/docs/showcase.gif"

ls -lh "$dir/docs/showcase.gif"
