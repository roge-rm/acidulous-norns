#!/bin/sh
# Copies the Acidulous engine into third_party/acidulous and its factory banks
# into data/banks.
# Usage: tools/vendor-acidulous.sh [PATH_TO_ACIDULOUS]
# The copied files are never edited by hand.
set -eu
src=${1:-$HOME/Projects/AndroidStudioProjects/Acidulous}
root=$(cd "$(dirname "$0")/.." && pwd)
dst=$root/third_party/acidulous
cpp=$src/app/src/main/cpp
rm -rf "$dst" "$root/data/banks"
mkdir -p "$dst/tools" "$root/data/banks"
cp "$src/LICENSE" "$src/NOTICE" "$dst/"
cp -r "$cpp/engine" "$cpp/sequencer" "$dst/"
# The patch bank reader and what it includes.
cp "$src/tools/patchbank.h" "$src/tools/audition_settings.h" "$dst/tools/"
cp "$src"/tools/banks/*.bank "$root/data/banks/"
git -C "$src" log -1 --format='Acidulous %H (%cs)' > "$dst/VERSION"
if [ -n "$(git -C "$src" status --porcelain -- app/src/main/cpp tools/banks tools/patchbank.h tools/audition_settings.h)" ]; then
    echo "with uncommitted changes" >> "$dst/VERSION"
fi
cat "$dst/VERSION"
