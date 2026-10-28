# Acidulous engine

Copied from the Acidulous app, https://github.com/roge-rm/Acidulous, at commit
dcd06e65d02aff048b73a350299f81a5daaaabf2 (see VERSION), by tools/vendor-acidulous.sh:

- engine/, sequencer/ and EngineHost.cpp/.h from app/src/main/cpp
- from app/src/main/cpp/platform: android/jni_bridge.cpp, web/jni.h (a fake
  JNI), web/LinkOff.cpp (Link always off), desktop/android/log.h (Android's
  log call to stderr), link/LinkTimebase.h and lines/ (EngineLines.h/.cpp
  and lines_dispatch.cpp, not its drivers)
- tools/patchbank.h and tools/audition_settings.h
- the factory banks, in data/banks at the top of this repo

It's GPL-3.0-or-later, see LICENSE and NOTICE. Don't edit these files here;
change them in the app and copy them again.
