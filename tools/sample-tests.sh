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
        case $path in factory:*) file=$path ;; *) file=$mat/$path ;; esac
        printf '0 load 0 %s "%s" %s\n' "$kind" "$file" "$arg"
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
Abacus|keys|Voice Scan|sample|voice.wav|0
Abacus|keys|Voice Scan|sample|voice.flac|0
Abacus|keys|Voice Scan|sample|factory:voice|0
LIST
# Abacus borrowing from part 1 (sidechain 1) sounds different from not
for sc in 0 0.0625; do
    {
        echo '0 patch 0 Trinity "Saw Lead"'
        echo '0 patch 1 Abacus "Voice Scan"'
        echo '0 param 1 borrow 0.5'
        echo "0 param 1 sidechain $sc"
        echo '0.1 note 0 1 48 100'
        echo '0.1 note 1 1 60 100'
        echo '1.5 note 0 1 48 0'
        echo '1.5 note 1 1 60 0'
    } > "$out/Abacus-sidechain-$sc.txt"
    "$render" "$@" "$out/Abacus-sidechain-$sc.txt" "$out/Abacus-sidechain-$sc.wav" >/dev/null 2>&1 || { echo "FAIL: Abacus sidechain render" >&2; failed=1; }
done
if cmp -s "$out/Abacus-sidechain-0.wav" "$out/Abacus-sidechain-0.0625.wav"; then
    echo "FAIL: Abacus sidechain changes nothing" >&2
    failed=1
else
    echo "Abacus sidechain: from track 1 differs from off"
fi
exit $failed
