#!/usr/bin/env python3
"""Renders a patch from our own engine one note at a time and writes a small
SoundFont of it, for Mosaic's default sound.

  tools/make-keys-sf2.py [out.sf2]

Needs host/build/acidulous-render. Notes every 6 semitones from C1 to C7,
mono 16-bit at 24 kHz, each with a sustain loop crossfaded into its start.
The renders go in host/build/sf2/.
"""

import math
import os
import struct
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
RENDER = os.path.join(ROOT, "host", "build", "acidulous-render")
WORK = os.path.join(ROOT, "host", "build", "sf2")
OUT = os.path.join(ROOT, "data", "samples", "acidulous-soft-horn.sf2")

MACHINE, PATCH = "Trinity", "Soft Horn"
PRESET = "Soft Horn"
NOTES = list(range(24, 97, 6))  # C1 to C7
VELOCITY = 100
LEAD = 0.1  # seconds before the note, so the patch is in place
SECONDS = 0.75  # kept per note at 24 kHz
LOOP = 0.3  # loop length, seconds, rounded to whole periods
FADE = 0.1  # crossfade at the loop end, seconds
RATE = 24000
GUARD = 8  # samples past the loop end, copied from its start
RELEASE = 0.6  # seconds, for "envfrom file"


def render(note):
    os.makedirs(WORK, exist_ok=True)
    script = os.path.join(WORK, "note-%d.txt" % note)
    wav = os.path.join(WORK, "note-%d.wav" % note)
    with open(script, "w") as f:
        f.write('0 patch 0 %s "%s"\n' % (MACHINE, PATCH))
        f.write("%g note 0 1 %d %d\n" % (LEAD, note, VELOCITY))
        f.write("%g note 0 1 %d 0\n" % (LEAD + SECONDS + 0.2, note))
    subprocess.run([RENDER, "--banks", os.path.join(ROOT, "data", "banks"), "--tail", "0.1", script, wav],
                   check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    return wav


def read_float_wav(path):
    """The render's 48 kHz stereo float WAV, mixed to mono."""
    data = open(path, "rb").read()
    assert data[:4] == b"RIFF" and data[8:12] == b"WAVE"
    p = 12
    pcm = None
    while p + 8 <= len(data):
        cid, size = data[p:p + 4], struct.unpack("<I", data[p + 4:p + 8])[0]
        if cid == b"fmt ":
            fmt, ch, rate, _, _, bits = struct.unpack("<HHIIHH", data[p + 8:p + 24])
            assert fmt == 3 and ch == 2 and rate == 48000 and bits == 32
        elif cid == b"data":
            pcm = data[p + 8:p + 8 + size]
        p += 8 + size + (size & 1)
    lr = struct.unpack("<%df" % (len(pcm) // 4), pcm)
    return [(lr[i] + lr[i + 1]) * 0.5 for i in range(0, len(lr), 2)]


def halve(x):
    """48 to 24 kHz: a windowed-sinc low-pass at 10.5 kHz, every other sample."""
    taps = 63
    mid = taps // 2
    cut = 10500.0 / 48000.0
    h = []
    for k in range(taps):
        n = k - mid
        s = 2 * cut if n == 0 else math.sin(2 * math.pi * cut * n) / (math.pi * n)
        w = 0.42 - 0.5 * math.cos(2 * math.pi * k / (taps - 1)) + 0.08 * math.cos(4 * math.pi * k / (taps - 1))
        h.append(s * w)
    g = sum(h)
    h = [v / g for v in h]
    out = []
    n = len(x)
    for i in range(0, n, 2):
        acc = 0.0
        for k in range(taps):
            j = i + k - mid
            if 0 <= j < n:
                acc += h[k] * x[j]
        out.append(acc)
    return out


def take_note(x, note):
    """From the note's start, SECONDS long, with a crossfaded loop at the end.
    Returns the samples and the loop's start and end (end exclusive)."""
    start = int(LEAD * RATE)
    x = x[start:]
    period = RATE / (440.0 * 2 ** ((note - 69) / 12.0))
    end = int(SECONDS * RATE)
    length = max(1, round(LOOP * RATE / period)) * period
    loop_end = end
    guess = int(round(loop_end - length))
    fade = int(FADE * RATE)
    # the loop start whose run-up best matches the run-up to the loop end
    best, best_err = guess, None
    span = int(period) + 2
    for ls in range(guess - span, guess + span + 1):
        err = 0.0
        for k in range(1, 200):
            d = x[loop_end - k] - x[ls - k]
            err += d * d
        if best_err is None or err < best_err:
            best, best_err = ls, err
    loop_start = best
    assert loop_start - fade > 0
    y = list(x[:loop_end])
    # the end of the loop fades into what comes before its start, so playing
    # on from the loop end into the loop start is continuous
    for k in range(fade):
        w = (k + 1) / fade
        a = loop_end - fade + k
        b = loop_start - fade + k
        y[a] = x[a] * (1 - w) + x[b] * w
    for k in range(GUARD):
        y.append(y[loop_start + k])
    return y, loop_start, loop_end


def pack_name(name):
    return name.encode("ascii")[:20].ljust(20, b"\0")


def chunk(cid, body):
    out = cid + struct.pack("<I", len(body)) + body
    if len(body) & 1:
        out += b"\0"
    return out


def list_chunk(kind, body):
    return chunk(b"LIST", kind + body)


def write_sf2(path, notes):
    """notes: (key, samples, loop_start, loop_end, lo, hi) each."""
    smpl = bytearray()
    shdr = bytearray()
    for i, (key, y, ls, le, lo, hi) in enumerate(notes):
        start = len(smpl) // 2
        for v in y:
            smpl += struct.pack("<h", max(-32768, min(32767, int(round(v * 32767)))))
        end = len(smpl) // 2
        smpl += b"\0\0" * 46  # 46 zero samples after each, as the spec asks
        shdr += pack_name("horn %d" % key) + struct.pack(
            "<IIIIIBbHH", start, end, start + ls, start + le, RATE, key, 0, 0, 1)
    shdr += pack_name("EOS") + b"\0" * 26

    # generators
    GEN_KEY_RANGE, GEN_INSTRUMENT, GEN_SAMPLE_ID, GEN_SAMPLE_MODES = 43, 41, 53, 54
    GEN_ATTACK, GEN_DECAY, GEN_SUSTAIN, GEN_RELEASE = 34, 36, 37, 38

    def tc(seconds):
        return int(round(1200 * math.log2(seconds))) & 0xFFFF

    igen = bytearray()
    ibag = bytearray()
    # global zone: the envelope
    ibag += struct.pack("<HH", 0, 0)
    for op, amt in ((GEN_ATTACK, tc(0.01)), (GEN_DECAY, tc(1.0)), (GEN_SUSTAIN, 0), (GEN_RELEASE, tc(RELEASE))):
        igen += struct.pack("<HH", op, amt)
    for i, (key, y, ls, le, lo, hi) in enumerate(notes):
        ibag += struct.pack("<HH", len(igen) // 4, 0)
        igen += struct.pack("<HBB", GEN_KEY_RANGE, lo, hi)
        igen += struct.pack("<HH", GEN_SAMPLE_MODES, 1)
        igen += struct.pack("<HH", GEN_SAMPLE_ID, i)
    ibag += struct.pack("<HH", len(igen) // 4, 0)
    igen += b"\0\0\0\0"
    inst = pack_name(PRESET) + struct.pack("<H", 0) + pack_name("EOI") + struct.pack("<H", len(ibag) // 4 - 1)

    pgen = struct.pack("<HH", GEN_INSTRUMENT, 0) + b"\0\0\0\0"
    pbag = struct.pack("<HH", 0, 0) + struct.pack("<HH", 1, 0)
    phdr = (pack_name(PRESET) + struct.pack("<HHHIII", 0, 0, 0, 0, 0, 0) +
            pack_name("EOP") + struct.pack("<HHHIII", 0, 0, 1, 0, 0, 0))
    mod_end = b"\0" * 10

    info = list_chunk(b"INFO",
                      chunk(b"ifil", struct.pack("<HH", 2, 1)) +
                      chunk(b"isng", b"EMU8000\0") +
                      chunk(b"INAM", b"Acidulous Soft Horn\0") +
                      chunk(b"ICMT", b"Rendered from Acidulous Trinity, patch Soft Horn\0"))
    sdta = list_chunk(b"sdta", chunk(b"smpl", bytes(smpl)))
    pdta = list_chunk(b"pdta",
                      chunk(b"phdr", phdr) + chunk(b"pbag", pbag) + chunk(b"pmod", mod_end) +
                      chunk(b"pgen", pgen) + chunk(b"inst", inst) + chunk(b"ibag", bytes(ibag)) +
                      chunk(b"imod", mod_end) + chunk(b"igen", bytes(igen)) + chunk(b"shdr", bytes(shdr)))
    body = b"sfbk" + info + sdta + pdta
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "wb") as f:
        f.write(b"RIFF" + struct.pack("<I", len(body)) + body)


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else OUT
    takes = []
    for note in NOTES:
        y, ls, le = take_note(halve(read_float_wav(render(note))), note)
        # each to the same loudness over its loop, so zones don't step
        rms = math.sqrt(sum(v * v for v in y[ls:le]) / (le - ls))
        takes.append((note, [v / rms for v in y], ls, le))
    peak = max(abs(v) for _, y, _, _ in takes for v in y)
    gain = 0.89 / peak  # about -1 dB at the loudest
    notes = []
    for i, (note, y, ls, le) in enumerate(takes):
        lo = 0 if i == 0 else note - 3
        hi = 127 if i == len(takes) - 1 else note + 2
        notes.append((note, [v * gain for v in y], ls, le, lo, hi))
        # the step across the loop seam against the largest step elsewhere
        z = notes[-1][1]
        seam = abs(z[ls] - z[le - 1])
        biggest = max(abs(z[k + 1] - z[k]) for k in range(ls, le - 1))
        print("note %d: %d samples, loop %d-%d, seam step %.4f, largest step in loop %.4f" %
              (note, len(z), ls, le, seam, biggest))
    write_sf2(out, notes)
    print("%s: %d bytes" % (out, os.path.getsize(out)))


if __name__ == "__main__":
    main()
