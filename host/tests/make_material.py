#!/usr/bin/env python3
"""Makes test files for the sample machines in OUT (default host/build/material):

  kit/          13 drum sounds, 01-kick.wav to 13-clave.wav, two of them AIFF
                and one FLAC
  keys/         three WAVs named by note (C3, C4, C5) for a Mosaic folder
  plain/        three WAVs with no note names
  loop.wav      two bars of a beat at 120 bpm, for Dice and Pollen
  voice.wav     a sung vowel gliding over a fifth, for Molt
  voice.flac    the same as FLAC
  test.sf2      a SoundFont with two presets, "Sine Keys" and "Pluck"

  --big MB      also big.sf2, about MB megabytes of sample data in one preset
"""
import math
import os
import random
import struct
import sys
import wave

RATE = 48000


def clip16(x):
    return max(-32767, min(32767, int(round(x * 32767))))


def write_wav(path, samples, rate=RATE, channels=1):
    with wave.open(path, "wb") as w:
        w.setnchannels(channels)
        w.setsampwidth(2)
        w.setframerate(rate)
        w.writeframes(b"".join(struct.pack("<h", clip16(s)) for s in samples))


def ieee_extended(x):
    # 80-bit float for AIFF's sample rate
    e = int(math.floor(math.log2(x)))
    m = int(x / 2 ** e * (1 << 63))
    return struct.pack(">HQ", 16383 + e, m)


def write_aiff(path, samples, rate=RATE):
    data = b"".join(struct.pack(">h", clip16(s)) for s in samples)
    comm = struct.pack(">hIh", 1, len(samples), 16) + ieee_extended(rate)
    ssnd = struct.pack(">II", 0, 0) + data
    body = b"AIFF" + b"COMM" + struct.pack(">I", len(comm)) + comm + b"SSND" + struct.pack(">I", len(ssnd)) + ssnd
    with open(path, "wb") as f:
        f.write(b"FORM" + struct.pack(">I", len(body)) + body)


def crc8(data):
    c = 0
    for b in data:
        c ^= b
        for _ in range(8):
            c = ((c << 1) ^ 0x07) & 0xFF if c & 0x80 else (c << 1) & 0xFF
    return c


def crc16(data):
    c = 0
    for b in data:
        c ^= b << 8
        for _ in range(8):
            c = ((c << 1) ^ 0x8005) & 0xFFFF if c & 0x8000 else (c << 1) & 0xFFFF
    return c


def utf8_num(n):
    if n < 0x80:
        return bytes([n])
    out = []
    while True:
        out.insert(0, 0x80 | (n & 0x3F))
        n >>= 6
        if n < (0x40 >> len(out)):
            out.insert(0, ((0xFF00 >> len(out)) & 0xFF) | n)
            return bytes(out)


def write_flac(path, samples, rate=RATE):
    """Mono 16-bit FLAC with verbatim subframes, blocks of 4096."""
    block = 4096
    ints = [clip16(s) for s in samples]
    info = struct.pack(">HH", block, block)
    info += (0).to_bytes(3, "big") + (0).to_bytes(3, "big")
    # rate 20 bits, channels-1 3 bits, bps-1 5 bits, total samples 36 bits
    v = (rate << 44) | (0 << 41) | (15 << 36) | len(ints)
    info += v.to_bytes(8, "big") + bytes(16)
    out = b"fLaC" + bytes([0x80]) + len(info).to_bytes(3, "big") + info
    for n, start in enumerate(range(0, len(ints), block)):
        chunk = ints[start:start + block]
        bs_code, extra = (7, struct.pack(">H", len(chunk) - 1))
        if len(chunk) == block:
            bs_code, extra = 12, b""  # 256 * 2^(12-8) = 4096
        hdr = bytes([0xFF, 0xF8, (bs_code << 4) | 0x0, (0 << 4) | (4 << 1)])
        # rate code 0: from STREAMINFO; channels 0 (mono); bps 4 (16 bits)
        hdr += utf8_num(n) + extra
        hdr += bytes([crc8(hdr)])
        sub = bytes([0x02]) + b"".join(struct.pack(">h", s) for s in chunk)  # verbatim
        frame = hdr + sub
        frame += struct.pack(">H", crc16(frame))
        out += frame
    with open(path, "wb") as f:
        f.write(out)


def env(n, attack, decay):
    a = max(1, int(attack * RATE))
    return [min(1.0, i / a) * math.exp(-i / (decay * RATE)) for i in range(n)]


def drum(kind, rnd):
    n = int(0.6 * RATE)
    out = []
    phase = 0.0
    for i in range(n):
        t = i / RATE
        noise = rnd.uniform(-1, 1)
        if kind == "kick":
            f = 50 + 120 * math.exp(-t * 30)
            phase += 2 * math.pi * f / RATE
            v = math.sin(phase) * math.exp(-t * 7)
        elif kind == "rim":
            v = (math.sin(2 * math.pi * 1700 * t) * 0.6 + noise * 0.4) * math.exp(-t * 60)
        elif kind == "snare":
            v = (math.sin(2 * math.pi * 190 * t) * 0.5 * math.exp(-t * 25) + noise * 0.6 * math.exp(-t * 14))
        elif kind == "clap":
            burst = sum(math.exp(-max(0, t - d) * 90) if t >= d else 0 for d in (0, 0.012, 0.024))
            v = noise * 0.5 * burst + noise * 0.3 * math.exp(-t * 9)
        elif kind.startswith("tom"):
            base = {"tom1": 90, "tom2": 130, "tom3": 180}[kind]
            f = base * (1 + 0.5 * math.exp(-t * 20))
            phase += 2 * math.pi * f / RATE
            v = math.sin(phase) * math.exp(-t * 6)
        elif kind == "chat":
            v = noise * math.exp(-t * 45) * (1 if i % 2 else -1)
        elif kind == "ohat":
            v = noise * math.exp(-t * 5) * (1 if i % 2 else -1) * 0.6
        elif kind == "crash":
            v = noise * math.exp(-t * 3) * 0.6
        elif kind == "ride":
            v = (noise * 0.3 + math.sin(2 * math.pi * 3200 * t) * 0.3) * math.exp(-t * 4)
        elif kind == "cowbell":
            v = (math.copysign(1, math.sin(2 * math.pi * 540 * t)) + math.copysign(1, math.sin(2 * math.pi * 800 * t))) * 0.25 * math.exp(-t * 12)
        else:  # clave
            v = math.sin(2 * math.pi * 2500 * t) * math.exp(-t * 40)
        out.append(0.8 * v)
    return out


def tone(freq, seconds, harmonics=6, decay=1.5):
    n = int(seconds * RATE)
    e = env(n, 0.005, decay)
    return [0.5 * e[i] * sum(math.sin(2 * math.pi * freq * h * i / RATE) / h for h in range(1, harmonics + 1)) for i in range(n)]


def loop():
    rnd = random.Random(2)
    kick, snare, hat = drum("kick", rnd), drum("snare", rnd), drum("chat", rnd)
    n = int(4.0 * RATE)  # two bars at 120 bpm
    out = [0.0] * n
    step = RATE // 4  # sixteenths... eighth notes at 120 bpm are 0.25 s
    pattern = ["k", "h", "s", "h", "k", "k", "s", "h"] * 2
    for i, p in enumerate(pattern):
        src = {"k": kick, "s": snare, "h": hat}[p]
        at = i * step
        for j, v in enumerate(src[: n - at]):
            out[at + j] += v * (0.7 if p == "h" else 1.0)
    return [max(-1, min(1, v)) for v in out]


def voice():
    # a glottal pulse train through three vowel formants, gliding A2 to E3
    n = int(3.0 * RATE)
    out = []
    phase = 0.0
    y = [[0.0, 0.0] for _ in range(3)]
    formants = [(700, 80), (1220, 90), (2600, 120)]
    coefs = []
    for f, bw in formants:
        r = math.exp(-math.pi * bw / RATE)
        coefs.append((2 * r * math.cos(2 * math.pi * f / RATE), -r * r))
    for i in range(n):
        t = i / RATE
        f0 = 110 * (2 ** ((7 * min(1, max(0, t - 1.0))) / 12)) * (1 + 0.01 * math.sin(2 * math.pi * 5 * t))
        phase += f0 / RATE
        pulse = 0.0
        if phase >= 1:
            phase -= 1
            pulse = 1.0
        s = 0.0
        for k, (a1, a2) in enumerate(coefs):
            v = pulse + a1 * y[k][0] + a2 * y[k][1]
            y[k][1], y[k][0] = y[k][0], v
            s += v
        out.append(s)
    peak = max(abs(v) for v in out)
    e = env(n, 0.05, 100)
    return [0.7 * v / peak * e[i] for i, v in enumerate(out)]


# ---- SoundFont ---------------------------------------------------------------

def chunk(cid, data):
    if len(data) % 2:
        data += b"\0"
    return cid + struct.pack("<I", len(data)) + data


def lst(kind, data):
    return chunk(b"LIST", kind + data)


def name20(s):
    return s.encode()[:19].ljust(20, b"\0")


def write_sf2(path, samples, instruments, presets):
    """samples: [(name, int16 bytes, root, loop)], instruments: [(name,
    [(lo, hi, sample index)])], presets: [(name, number, instrument index)]."""
    smpl = b""
    shdr = b""
    for name, data, root, looped in samples:
        start = len(smpl) // 2
        frames = len(data) // 2
        smpl += data + bytes(46 * 2)
        end = start + frames
        ls, le = (start + frames // 4, end - 8) if looped else (start, start)
        shdr += name20(name) + struct.pack("<IIIIIBbHH", start, end, ls, le, RATE, root, 0, 0, 1)
    shdr += name20("EOS") + bytes(26)

    inst = ibag = igen = b""
    bag = gen = 0
    for name, zones in instruments:
        inst += name20(name) + struct.pack("<H", bag)
        for lo, hi, si in zones:
            ibag += struct.pack("<HH", gen, 0)
            bag += 1
            igen += struct.pack("<HBB", 43, lo, hi)  # keyRange
            igen += struct.pack("<HH", 54, 1 if samples[si][3] else 0)  # sampleModes
            igen += struct.pack("<Hh", 36, -1200)  # releaseVolEnv, about half a second
            igen += struct.pack("<HH", 53, si)  # sampleID, last
            gen += 4
    inst += name20("EOI") + struct.pack("<H", bag)
    ibag += struct.pack("<HH", gen, 0)
    igen += bytes(4)

    phdr = pbag = pgen = b""
    bag = gen = 0
    for name, number, ii in presets:
        phdr += name20(name) + struct.pack("<HHHIII", number, 0, bag, 0, 0, 0)
        pbag += struct.pack("<HH", gen, 0)
        bag += 1
        pgen += struct.pack("<HH", 41, ii)  # instrument
        gen += 1
    phdr += name20("EOP") + struct.pack("<HHHIII", 0, 0, bag, 0, 0, 0)
    pbag += struct.pack("<HH", gen, 0)
    pgen += bytes(4)

    info = chunk(b"ifil", struct.pack("<HH", 2, 1)) + chunk(b"isng", b"EMU8000\0") + chunk(b"INAM", b"acidulous test\0")
    pdta = (chunk(b"phdr", phdr) + chunk(b"pbag", pbag) + chunk(b"pmod", bytes(10)) + chunk(b"pgen", pgen) +
            chunk(b"inst", inst) + chunk(b"ibag", ibag) + chunk(b"imod", bytes(10)) + chunk(b"igen", igen) +
            chunk(b"shdr", shdr))
    body = b"sfbk" + lst(b"INFO", info) + lst(b"sdta", chunk(b"smpl", smpl)) + lst(b"pdta", pdta)
    with open(path, "wb") as f:
        f.write(b"RIFF" + struct.pack("<I", len(body)) + body)


def pcm(samples):
    return b"".join(struct.pack("<h", clip16(s)) for s in samples)


def main():
    args = sys.argv[1:]
    big = 0
    if "--big" in args:
        k = args.index("--big")
        big = int(args[k + 1])
        del args[k:k + 2]
    out = args[0] if args else os.path.join(os.path.dirname(__file__), "..", "build", "material")
    os.makedirs(os.path.join(out, "kit"), exist_ok=True)
    os.makedirs(os.path.join(out, "keys"), exist_ok=True)
    os.makedirs(os.path.join(out, "plain"), exist_ok=True)
    rnd = random.Random(1)
    names = ["kick", "rim", "snare", "clap", "tom1", "tom2", "tom3", "chat", "ohat", "crash", "ride", "cowbell", "clave"]
    for i, name in enumerate(names):
        s = drum(name, rnd)
        base = os.path.join(out, "kit", "%02d-%s" % (i + 1, name))
        if name in ("rim", "tom2"):
            write_aiff(base + ".aif", s)
        elif name == "cowbell":
            write_flac(base + ".flac", s)
        else:
            write_wav(base + ".wav", s)
    for note, f in (("C3", 130.81), ("C4", 261.63), ("C5", 523.25)):
        write_wav(os.path.join(out, "keys", "Keys %s.wav" % note), tone(f, 1.5))
    for i, f in enumerate((220, 330, 440)):
        write_wav(os.path.join(out, "plain", "sound %d.wav" % (i + 1)), tone(f, 1.0, harmonics=3))
    write_wav(os.path.join(out, "loop.wav"), loop())
    v = voice()
    write_wav(os.path.join(out, "voice.wav"), v)
    write_flac(os.path.join(out, "voice.flac"), v)

    sf_samples = [("sine48", pcm(tone(261.63, 1.0, harmonics=2, decay=50)), 60, True),
                  ("sine72", pcm(tone(523.25, 1.0, harmonics=2, decay=50)), 72, True),
                  ("pluck", pcm(tone(220.0, 1.2, harmonics=8, decay=0.3)), 57, False)]
    write_sf2(os.path.join(out, "test.sf2"), sf_samples,
              [("Sine", [(0, 66, 0), (67, 127, 1)]), ("Pluck", [(0, 127, 2)])],
              [("Sine Keys", 0, 0), ("Pluck", 1, 1)])

    if big:
        # one preset over many 10 s samples of noisy tone, so it decodes all of them
        frames = 10 * RATE
        count = max(1, big * 1024 * 1024 // (frames * 2))
        r = random.Random(3)
        samples, zones = [], []
        for k in range(count):
            data = bytes(r.getrandbits(8) for _ in range(256)) * (frames * 2 // 256)
            samples.append(("big%d" % k, data, 60, True))
            lo = k * 128 // count
            hi = (k + 1) * 128 // count - 1
            zones.append((lo, max(lo, hi), k))
        write_sf2(os.path.join(out, "big.sf2"), samples, [("Big", zones)], [("Big", 0, 0)])
    print("made", os.path.abspath(out))


if __name__ == "__main__":
    main()
