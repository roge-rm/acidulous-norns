# Acidulous engine

Copied from the Acidulous app, https://github.com/roge-rm/Acidulous, at commit
8ded5657bb33238bcd1ed6f129f6fe184a30c47e (see VERSION), by tools/vendor-acidulous.sh:

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
