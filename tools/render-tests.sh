#!/bin/sh
# Renders a test phrase for each first machine with two factory patches into
# host/build/renders and prints each one's peak, RMS and load.
# Usage: tools/render-tests.sh [--block N]
set -eu
root=$(cd "$(dirname "$0")/.." && pwd)
render=$root/host/build/acidulous-render
out=$root/host/build/renders
mkdir -p "$out"
while read -r machine phrase patch; do
    name=$(echo "$machine-$patch" | tr ' ' '_')
    script=$out/$name.txt
    printf '0 patch 0 %s "%s"\n' "$machine" "$patch" > "$script"
    cat "$root/host/tests/$phrase.txt" >> "$script"
    "$render" "$@" "$script" "$out/$name.wav" | grep -v '^  /'
done <<LIST
Reflux acid Classic
Reflux acid Squelch
Hexbeat beat Straight
Hexbeat beat Punchy
Trinity pad Wide Saws
Trinity keys Bell Keys
Ratio keys Tine
Ratio keys Bell
Formulate chip Pulse Lead
Formulate chip Major Arp
LIST
