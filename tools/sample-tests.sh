#!/bin/bash
# Makes test files with host/tests/make_material.py, then renders patches of
# the sample machines with them loaded into host/build/renders and prints each
# one's peak, RMS and load. Fails if a load or render fails, or a render is
# silent (RMS under -60 dB).
# Usage: tools/sample-tests.sh [--block N]
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
render=$root/host/build/acidulous-render
mat=$root/host/build/material
out=$root/host/build/renders
mkdir -p "$out"
[ -f "$mat/test.sf2" ] || python3 "$root/host/tests/make_material.py" "$mat" >/dev/null
failed=0
# machine, phrase, patch, then the load line's kind, path and arg
while IFS='|' read -r machine phrase patch kind path arg; do
    name=$(echo "$machine-$patch-$kind-$(basename "$path")" | tr ' ' '_')
    script=$out/$name.txt
    {
        printf '0 patch 0 %s "%s"\n' "$machine" "$patch"
        printf '0 load 0 %s "%s" %s\n' "$kind" "$mat/$path" "$arg"
        cat "$root/host/tests/$phrase.txt"
    } > "$script"
    if ! text=$("$render" "$@" "$script" "$out/$name.wav" 2>&1); then
        echo "$text"
        echo "FAIL: $name: render failed" >&2
        failed=1
        continue
    fi
    if grep -q '^  /error\|NOT FINITE' <<<"$text" || ! grep -q '^  /loaded' <<<"$text"; then
        echo "$text"
        echo "FAIL: $name: error, no load or non-finite output" >&2
        failed=1
        continue
    fi
    rms=$(grep -o 'rms [0-9.]* ([-0-9.]* dB)' <<<"$text" | grep -o '(.*dB' | tr -d '( dB')
    if awk "BEGIN{exit !($rms < -60)}"; then
        echo "$text"
        echo "FAIL: $name: silent" >&2
        failed=1
        continue
    fi
    grep -v "^  /\|^[IWE]/" <<<"$text"
    grep '^  /loaded' <<<"$text" | tail -1
done <<LIST
Forage|beat|Init|kit|kit|0
Forage|beat|Body|kit|kit|0
Forage|beat|Init|pad|kit/01-kick.wav|0
Mosaic|keys|Init|sf2|test.sf2|0
Mosaic|keys|Mallets|sf2|test.sf2|1
Mosaic|keys|Init|folder|keys|0
Mosaic|keys|Felt|folder|plain|0
Dice|slices|Init|sample|loop.wav|0
Dice|slices|Stutter|sample|loop.wav|0
Pollen|pad|Init|sample|loop.wav|0
Pollen|pad|Drift|sample|voice.flac|0
Molt|keys|Init|sample|voice.wav|0
Molt|keys|Choir|sample|voice.flac|0
LIST
exit $failed
