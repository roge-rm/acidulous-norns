#!/bin/bash
# Renders a Reflux through a Venue and a Vessel in its own insert slot into
# host/build/renders. Fails if the default room, another room, "no impulse"
# and a shape change don't all differ as they should, if a missing room or file
# isn't an error, or if the Vessel renders the same as the dry sound.
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
render=$root/host/build/acidulous-render
out=$root/host/build/renders
mkdir -p "$out"
failed=0

run() { # name, lines...
    local name=$1
    shift
    { printf '0 patch 0 Reflux "Classic"\n'; printf '%s\n' "$@"; cat "$root/host/tests/keys.txt"; } > "$out/$name.txt"
    "$render" --tail 2 "$out/$name.txt" "$out/$name.wav" 2>&1 > "$out/$name.log" || { cat "$out/$name.log"; echo "FAIL: $name: render failed" >&2; failed=1; }
}
differ() { # a b what
    if cmp -s "$out/$1.wav" "$out/$2.wav"; then
        echo "FAIL: $3" >&2
        failed=1
    fi
}

run venue-dry
run venue-default '0 insert 0 2 Venue'
run venue-plate '0 insert 0 2 Venue' '0 impulse 0 2 metal-plate'
run venue-factory '0 insert 0 2 Venue' '0 impulse 0 2 factory:metal-plate'
run venue-none '0 insert 0 2 Venue' '0.5 impulse 0 2 ""'
run venue-short '0 insert 0 2 Venue' '0 fx 0 2 length 0.1'
run venue-missing '0 insert 0 2 Venue' '0 impulse 0 2 factory:nowhere' '0 impulse 0 2 /nonexistent.wav'
run venue-slot0 '0 insert 0 3 Venue' '0 fx 0 3 stretch 0.2'
run vessel '0 insert 0 2 Vessel'

for n in default plate factory short; do
    differ venue-dry venue-$n "Venue $n sounds like the dry sound"
done
differ venue-default venue-plate "metal plate sounds like the default room"
differ venue-default venue-short "a short length sounds like the full one"
differ venue-dry vessel "Vessel sounds like the dry sound"
if ! cmp -s "$out/venue-plate.wav" "$out/venue-factory.wav"; then
    echo "FAIL: metal-plate and factory:metal-plate differ" >&2
    failed=1
fi
# The room rings out for 0.3 s after "no impulse". After that the level is the
# dry sound's (not the same samples: an insert keeps the track running through
# silences, which moves a free-running oscillator's phase).
if ! python3 -I - "$out/venue-dry.wav" "$out/venue-none.wav" <<'PY'
import sys
import numpy as np
def read(n):
    d = open(n, "rb").read()
    return np.frombuffer(d[d.find(b"data") + 8:], dtype="<f4")
a, b = read(sys.argv[1])[96000:], read(sys.argv[2])[96000:]
rms = lambda x: float(np.sqrt(np.mean(x * x)))
sys.exit(0 if abs(rms(a) - rms(b)) < 0.01 * rms(a) else 1)
PY
then
    echo "FAIL: Venue with no impulse isn't at the dry sound's level" >&2
    failed=1
fi
if [ "$(grep -c '^  /impulse_loaded .* "[^"]' "$out/venue-missing.log")" -lt 2 ] ||
    [ "$(grep -c '^  /error' "$out/venue-missing.log")" -lt 2 ]; then
    grep '/impulse_loaded\|/error' "$out/venue-missing.log" || true
    echo "FAIL: a missing room and file weren't errors" >&2
    failed=1
fi
[ $failed = 0 ] && echo "venue: ok"
exit $failed
