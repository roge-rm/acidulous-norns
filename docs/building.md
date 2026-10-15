# Building

The plugin must be built for 32-bit ARM, so build it on a norns (or another 32-bit Raspberry Pi OS with SuperCollider's headers). The render tool builds anywhere with cmake and a C++17 compiler.

## On the norns

You need cmake, the SuperCollider plugin headers (on norns in `/usr/local/include/SuperCollider`) and liblo. All are on a stock norns.

```
cd host
mkdir -p build && cd build
cmake ..
make -j3
```

This gives:

- `Acidulous.so`: the SuperCollider plugin
- `acidulous-render`: the offline render tool

On ARM the build targets the Cortex-A53, which covers the Pi 3 and Pi 4. The engine builds at `-O3 -ffast-math -fno-finite-math-only`, as in the app.

To use a new plugin, strip it and put it in `bin/`:

```
strip --strip-unneeded -o ../../bin/Acidulous.so Acidulous.so
```

To try it by hand, copy it to `~/.local/share/SuperCollider/Extensions/acidulous/` and restart SuperCollider with `sudo systemctl restart norns-sclang`. Only the `.so` goes there: SuperCollider already compiles `lib/Acidulous.sc` and `lib/Engine_Acidulous.sc` from `~/dust`, and a second copy would be a duplicate class.

When the last ssh login ends, the norns removes the user's shared memory, JACK's included. Running audio carries on, but SuperCollider restarted after that can't connect to JACK. Restart SuperCollider while still logged in, or restart `norns-jack` (which restarts crone and sclang) and then `norns-matron`, or restart the norns.

## On the desktop

```
cmake -S host -B host/build
cmake --build host/build -j8
```

Without SuperCollider's headers or liblo only `acidulous-render` is built. To build the plugin too, point cmake at them:

```
cmake -S host -B host/build -DSC_INC=PATH/include/plugin_interface \
    -DLO_INC=PATH/include -DLO_LIB=PATH/lib/liblo.so
```

## Updating the engine

```
tools/vendor-acidulous.sh [PATH_TO_ACIDULOUS]
```

copies the engine, the sequencer and the patch bank reader from the app into `third_party/acidulous`, and the factory banks into `data/banks`. It defaults to `~/Projects/AndroidStudioProjects/Acidulous`. The app's commit goes in `third_party/acidulous/VERSION`. Don't edit the copied files.

## OSC access

The plugin listens on UDP port 57150 and drops any message that doesn't come from this machine (liblo can only bind every interface, so it filters instead). To accept other machines, for desktop testing, start scsynth with `ACIDULOUS_OSC_ALL=1` in its environment.

## Testing offline

```
host/build/acidulous-render script.txt out.wav
```

A script has a line per event, `seconds command args...`, with the same commands the plugin takes over OSC:

```
0    patch 0 Reflux Classic
0    note 0 1 36 100
0.5  note 0 1 36 0
```

See the top of `host/src/render.cpp` for the options. `tools/render-tests.sh` renders two patches each of Reflux, Hexbeat, Trinity, Ratio and Formulate into `host/build/renders` and prints their peak, RMS and load.

## OSC

The messages the plugin takes on port 57150 and what it sends back to matron on 10111 are listed at the top of `host/src/bridge.h`. Parts are 0 to 7 and channels 1 to 16.

`acidulous-test BANK_DIR` (built with the plugin) runs the core with its worker thread and the OSC bridge: a note straight after `/patch`, reset, and starting and stopping the bridge and core twice. `tools/render-tests.sh` fails if a patch is missing or an output isn't finite.
