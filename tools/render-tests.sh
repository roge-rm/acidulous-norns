#!/bin/bash
# Renders a test phrase for each first machine with two factory patches into
# host/build/renders and prints each one's peak, RMS and load. Fails if a
# patch is missing, a render isn't finite or the tool fails.
# Usage: tools/render-tests.sh [--block N]
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
render=$root/host/build/acidulous-render
out=$root/host/build/renders
mkdir -p "$out"
failed=0
while read -r machine phrase patch; do
    name=$(echo "$machine-$patch" | tr ' ' '_')
    script=$out/$name.txt
    printf '0 patch 0 %s "%s"\n' "$machine" "$patch" > "$script"
    cat "$root/host/tests/$phrase.txt" >> "$script"
    if ! text=$("$render" "$@" "$script" "$out/$name.wav" 2>&1); then
        echo "$text"
        echo "FAIL: $machine $patch: render failed" >&2
        failed=1
        continue
    fi
    if grep -q '^  /error\|NOT FINITE' <<<"$text"; then
        echo "$text"
        echo "FAIL: $machine $patch: error or non-finite output" >&2
        failed=1
        continue
    fi
    grep -v '^  /' <<<"$text"
done <<LIST
Reflux acid Classic
Reflux acid Squelch
Hexbeat beat Squared
Hexbeat beat Punchy
Trinity pad Wide Saws
Trinity keys Bell Keys
Ratio keys Bell Piano
Ratio keys Bell
Formulate chip Pulse Lead
Formulate chip Major Arp
LIST
exit $failed
