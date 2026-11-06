#!/usr/bin/env python3
"""Makes the .acpreset fixtures with packed media/ files, as the app writes
them, into host/tests/acpreset:
  zones-packed.acpreset  Mosaic zones on three short tones in media/keys/
  sf2-packed.acpreset    Mosaic on a small SoundFont in media/sf/
  bad-name.acpreset      Dice whose sample is media/../evil.wav
  big.acpreset           Dice whose sample unpacks to over 64 MB
  big-other.acpreset     Dice on a small sample, with over 64 MB of other files
  slices.acpreset        Forage whose slice_sample is media/loops/break.wav
  voices.acpreset        Diction singing one recorded voice and morphing to another
  arp.acpreset           two Arp presets as the app writes modifier presets
  chord.acpreset         a Chord preset with key, scale and latch, which a load keeps
  scale.acpreset         a Scale preset, which the app refuses
Usage: host/tests/make_packed.py [modifiers]  (modifiers: only the last three)"""
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


def modifier(name, unit, patches):
    """A modifier preset as the app writes one: patches keyed mod.<unit>, no media or engine lines."""
    entries = ["patches/%d.json" % (k + 1) for k in range(len(patches))]
    with zipfile.ZipFile(os.path.join(OUT, name), "w", zipfile.ZIP_DEFLATED, compresslevel=9) as z:
        for entry, (patch, params) in zip(entries, patches):
            z.writestr(entry, json.dumps({"machine": "mod." + unit, "name": patch, "params": params, "version": 6}, indent=4))
        z.writestr("preset.json", json.dumps({"version": 1, "kind": "modifier", "unit": unit, "patches": entries}, indent=4))


def modifiers():
    modifier("arp.acpreset", "Arp", [("Up 16ths", {"rate": 0.7777778, "gate": 0.23076923}), ("Slow", {"rate": 0.0})])
    modifier("chord.acpreset", "Chord", [("Wide", {"type": 0.5, "strum": 0.25, "key": 0.5, "scale": 0.5, "latch": 1.0})])
    modifier("scale.acpreset", "Scale", [("Mine", {"key": 0.5})])


def bank(name, takes):
    """A voice's bank.json: takes as (prompt id, file, cut fields)."""
    cuts = {}
    for pid, _, cut in takes:
        c = {"problem": "", "start": 0, "end": 0, "holdFrom": 0, "holdTo": 0, "glideFrom": 0, "glideTo": 0,
             "consonantFrom": 0, "consonantTo": 0, "rootHz": 0.0, "centsOff": 0.0, "by": 0, "hand": False}
        c.update(cut)
        cuts[pid] = c
    return json.dumps({"name": name, "note": 57, "takes": {pid: f for pid, f, _ in takes}, "cuts": cuts}, indent=4)


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
    write("big-other.acpreset", "Dice", "Big Other", {"sample": "media/small.wav"},
          [("media/small.wav", wav_bytes(mm.tone(330, 0.2))), ("media/huge.bin", bytes((65 << 20) + 4096))])
    write("slices.acpreset", "Forage", "Sliced", {"slice_sample": "media/loops/break.wav", "slice_count": "4"},
          [("media/loops/break.wav", wav_bytes(mm.tone(110, 1.0, decay=0.2)))])
    held = {"holdFrom": 2400, "holdTo": 19200}
    bo = [("v-aa", "aa.wav", held), ("v-ay", "ay.wav", dict(held, glideFrom=14400, glideTo=21600)),
          ("aa-s", "aa-s.wav", {"consonantFrom": 9600, "consonantTo": 14400}),
          ("v-iy", "bad.wav", dict(held, problem="too short"))]
    cy = [("v-iy", "iy.wav", held)]
    media = [("media/voices/Bo/bank.json", bank("Bo", bo).encode()),
             ("media/voices/Cy/bank.json", bank("Cy", cy).encode())]
    for voice, takes, freq in (("Bo", bo, 220), ("Cy", cy, 330)):
        for _, f, _ in takes:
            media.append(("media/voices/%s/%s" % (voice, f), wav_bytes(mm.tone(freq, 0.5, decay=0.1))))
    write("voices.acpreset", "Diction", "Two Voices",
          {"voice": "media/voices/Bo/bank.json", "morphvoice": "media/voices/Cy/bank.json"}, media)
    print("made", OUT)


if __name__ == "__main__":
    if sys.argv[1:] == ["modifiers"]:
        modifiers()
    else:
        main()
        modifiers()
