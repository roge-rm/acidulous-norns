#!/bin/sh
# Copies the Acidulous engine into third_party/acidulous and its factory banks
# into data/banks, and the Venue rooms into data/rooms.
# Usage: tools/vendor-acidulous.sh [PATH_TO_ACIDULOUS]
# The copied files are never edited by hand.
set -eu
src=${1:-$HOME/Projects/AndroidStudioProjects/Acidulous}
root=$(cd "$(dirname "$0")/.." && pwd)
dst=$root/third_party/acidulous
cpp=$src/app/src/main/cpp
rm -rf "$dst" "$root/data/banks" "$root/data/rooms"
mkdir -p "$dst/tools" "$root/data/banks" "$root/data/rooms"
cp "$src/LICENSE" "$src/NOTICE" "$dst/"
cp -r "$cpp/engine" "$cpp/sequencer" "$dst/"
# The app's engine host and its JNI bridge, with the shims the desktop and web
# builds use: Android's log call, the web build's fake JNI and Link always off.
# The audio driver is the plugin's own (host/src/norns).
cp "$cpp/EngineHost.cpp" "$cpp/EngineHost.h" "$dst/"
mkdir -p "$dst/platform/android" "$dst/platform/web" "$dst/platform/desktop/android" "$dst/platform/link"
cp "$cpp/platform/android/jni_bridge.cpp" "$dst/platform/android/"
cp "$cpp/platform/web/jni.h" "$cpp/platform/web/LinkOff.cpp" "$dst/platform/web/"
cp "$cpp/platform/desktop/android/log.h" "$dst/platform/desktop/android/"
cp "$cpp/platform/link/LinkTimebase.h" "$dst/platform/link/"
# The engine lines replayer and its dispatch table, for replaying songs.
mkdir -p "$dst/platform/lines"
cp "$cpp/platform/lines/EngineLines.h" "$cpp/platform/lines/EngineLines.cpp" "$cpp/platform/lines/lines_dispatch.cpp" "$dst/platform/lines/"
# The patch bank reader and what it includes.
cp "$src/tools/patchbank.h" "$src/tools/audition_settings.h" "$dst/tools/"
cp "$src"/tools/banks/*.bank "$root/data/banks/"
# Venue's rooms, as the app ships them (impulse-<id>.wav).
cp "$src"/shared/src/commonMain/files/impulse-*.wav "$root/data/rooms/"
git -C "$src" log -1 --format='Acidulous %H (%cs)' > "$dst/VERSION"
if [ -n "$(git -C "$src" status --porcelain -- app/src/main/cpp/engine app/src/main/cpp/sequencer app/src/main/cpp/EngineHost.cpp app/src/main/cpp/EngineHost.h app/src/main/cpp/platform tools/banks tools/patchbank.h tools/audition_settings.h)" ]; then
    echo "with uncommitted changes" >> "$dst/VERSION"
fi
commit=$(git -C "$src" log -1 --format=%H)
cat > "$dst/README.md" <<EOF
# Acidulous engine

Copied from the Acidulous app, https://github.com/roge-rm/Acidulous, at commit
$commit (see VERSION), by tools/vendor-acidulous.sh:

- engine/, sequencer/ and EngineHost.cpp/.h from app/src/main/cpp
- from app/src/main/cpp/platform: android/jni_bridge.cpp, web/jni.h (a fake
  JNI), web/LinkOff.cpp (Link always off), desktop/android/log.h (Android's
  log call to stderr), link/LinkTimebase.h and lines/ (EngineLines.h/.cpp
  and lines_dispatch.cpp, not its drivers)
- tools/patchbank.h and tools/audition_settings.h
- the factory banks, in data/banks at the top of this repo, and the Venue
  rooms (impulse-<id>.wav) in data/rooms

It's GPL-3.0-or-later, see LICENSE and NOTICE. Don't edit these files here;
change them in the app and copy them again.
EOF
cat "$dst/VERSION"
