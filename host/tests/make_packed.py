#!/usr/bin/env python3
"""Makes the .acpreset fixtures with packed media/ files, as the app writes
them, into host/tests/acpreset:
  zones-packed.acpreset  Mosaic zones on three short tones in media/keys/
  sf2-packed.acpreset    Mosaic on a small SoundFont in media/sf/
  bad-name.acpreset      Dice whose sample is media/../evil.wav
  big.acpreset           Dice whose sample unpacks to over 64 MB
Usage: host/tests/make_packed.py"""
import io
import json
import os
import sys
import tempfile
import zipfile

here = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, here)
import make_material as mm  # noqa: E402

OUT = os.path.join(here, "acpreset")


def wav_bytes(samples):
    with tempfile.TemporaryDirectory() as d:
        p = os.path.join(d, "x.wav")
        mm.write_wav(p, samples)
        return open(p, "rb").read()


def sf2_bytes():
    with tempfile.TemporaryDirectory() as d:
        p = os.path.join(d, "x.sf2")
        mm.write_sf2(p, [("sine", mm.pcm(mm.tone(261.63, 0.2, harmonics=2, decay=50)), 60, True)],
                     [("Sine", [(0, 127, 0)])], [("Tiny Sine", 0, 0)])
        return open(p, "rb").read()


def write(name, machine, patch, settings, media):
    patch_json = {"machine": machine, "name": patch, "params": {}, "settings": settings, "version": 6}
    manifest = {"version": 1, "kind": "machine", "unit": machine, "patches": ["patches/1.json"]}
    with zipfile.ZipFile(os.path.join(OUT, name), "w", zipfile.ZIP_DEFLATED, compresslevel=9) as z:
        for entry, data in media:
            z.writestr(entry, data)
        z.writestr("patches/1.json", json.dumps(patch_json, indent=4))
        z.writestr("preset.json", json.dumps(manifest, indent=4))


def main():
    keys = []
    lines = []
    for note, f, lo, hi, root in (("C3", 130.81, 0, 54, 48), ("C4", 261.63, 55, 66, 60),
                                  ("C5", 523.25, 67, 127, 72)):
        entry = "media/keys/Keys %s.wav" % note
        keys.append((entry, wav_bytes(mm.tone(f, 0.3))))
        lines.append("%s|%d|%d|%d|0|127|0|1|0|0" % (entry, lo, hi, root))
    write("zones-packed.acpreset", "Mosaic", "Packed Keys", {"zones": "\n".join(lines)}, keys)
    write("sf2-packed.acpreset", "Mosaic", "Packed Sf2", {"sf2": "media/sf/tiny.sf2", "sf2preset": "0"},
          [("media/sf/tiny.sf2", sf2_bytes())])
    write("bad-name.acpreset", "Dice", "Bad Name", {"sample": "media/../evil.wav"},
          [("media/../evil.wav", wav_bytes(mm.tone(220, 0.1)))])
    write("big.acpreset", "Dice", "Big", {"sample": "media/big.wav"}, [("media/big.wav", bytes((65 << 20) + 4096))])
    print("made", OUT)


if __name__ == "__main__":
    main()
