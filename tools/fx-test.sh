#!/bin/bash
# Renders Trinity's Saw Lead as it comes and with its Distortion's drive
# turned up by /fx, into host/build/renders. Fails if /inserts doesn't list
# the patch's Distortion and Delay, if the two renders are the same, or if
# /fx takes a param the effect doesn't have, or if a /patch naming a missing
# patch after Saw Lead doesn't end with an empty /inserts.
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
render=$root/host/build/acidulous-render
out=$root/host/build/renders
mkdir -p "$out"
failed=0

printf '0 patch 0 Trinity "Saw Lead"\n' > "$out/fx-plain.txt"
cat "$root/host/tests/keys.txt" >> "$out/fx-plain.txt"
printf '0 patch 0 Trinity "Saw Lead"\n0 fx 0 0 drive 1\n' > "$out/fx-drive.txt"
cat "$root/host/tests/keys.txt" >> "$out/fx-drive.txt"
printf '0 patch 0 Trinity "Saw Lead"\n0 fx 0 0 nope 1\n0 fx 0 1 drive 1\n' > "$out/fx-bad.txt"
printf '0 patch 0 Trinity "Saw Lead"\n0 patch 0 Trinity "No Such Patch"\n' > "$out/fx-missing.txt"

plain=$("$render" "$out/fx-plain.txt" "$out/fx-plain.wav" 2>&1)
drive=$("$render" "$out/fx-drive.txt" "$out/fx-drive.wav" 2>&1)
bad=$("$render" "$out/fx-bad.txt" "$out/fx-bad.wav" 2>&1)
missing=$("$render" "$out/fx-missing.txt" "$out/fx-missing.wav" 2>&1)

if ! grep -q '^  /inserts 0 0 "Distortion" 6 .* 1 "Delay" 8 ' <<<"$plain"; then
    grep '^  /inserts' <<<"$plain" || true
    echo "FAIL: /inserts doesn't list Saw Lead's Distortion and Delay" >&2
    failed=1
fi
if cmp -s "$out/fx-plain.wav" "$out/fx-drive.wav"; then
    echo "FAIL: drive moved by /fx made no difference" >&2
    failed=1
fi
if [ "$(grep -c '^  /error' <<<"$bad")" != 2 ]; then
    grep '^  /error' <<<"$bad" || true
    echo "FAIL: /fx took a param the effect doesn't have" >&2
    failed=1
fi
if [ "$(grep -c '^  /inserts' <<<"$missing")" != 2 ] || [ "$(grep '^  /inserts' <<<"$missing" | tail -n 1)" != "  /inserts 0" ]; then
    grep '^  /inserts' <<<"$missing" || true
    echo "FAIL: a missing patch after Saw Lead didn't end with an empty /inserts" >&2
    failed=1
fi
grep -v '^  /' <<<"$plain" | grep -v '^I/' || true
grep -v '^  /' <<<"$drive" | grep -v '^I/' || true
[ $failed = 0 ] && echo "fx: ok"
exit $failed
