#include "Sf2Reader.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <tuple>

namespace acidulous {

namespace {

// --- Raw structures, as they sit in the file (little endian, packed) --------------

struct Phdr { char name[20]; uint16_t preset, bank, bagNdx; uint32_t library, genre, morphology; };
struct Bag { uint16_t genNdx, modNdx; };
struct Mod { uint16_t src, dest, amtSrc, trans; int16_t amount; };
struct Gen { uint16_t oper; uint16_t amount; };
struct Inst { char name[20]; uint16_t bagNdx; };
struct Shdr {
    char name[20];
    uint32_t start, end, startLoop, endLoop, sampleRate;
    uint8_t originalPitch;
    int8_t pitchCorrection;
    uint16_t sampleLink, sampleType;
};

// Generator operators this reader understands.
enum GenOp : uint16_t {
    GenStartAddrs = 0, GenEndAddrs = 1, GenStartLoop = 2, GenEndLoop = 3, GenStartAddrsCoarse = 4,
    GenEndAddrsCoarse = 12, GenPan = 17, GenAttackVol = 34, GenHoldVol = 35, GenDecayVol = 36,
    GenSustainVol = 37, GenReleaseVol = 38, GenInstrument = 41, GenKeyRange = 43, GenVelRange = 44,
    GenStartLoopCoarse = 45, GenInitialAttenuation = 48, GenEndLoopCoarse = 50, GenCoarseTune = 51,
    GenFineTune = 52, GenSampleID = 53, GenSampleModes = 54, GenOverridingRootKey = 58,
};

inline uint16_t rd16(const uint8_t *p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }
inline uint32_t rd32(const uint8_t *p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}
inline int16_t asSigned(uint16_t v) { return static_cast<int16_t>(v); }

std::string trimmed(const char *raw, size_t max) {
    size_t n = 0;
    while (n < max && raw[n] != '\0') ++n;
    while (n > 0 && (raw[n - 1] == ' ' || raw[n - 1] == '\t')) --n;
    return std::string(raw, n);
}

// Reads the whole file into memory to keep the parser simple. The size cap
// stops a huge file from crashing the app.
bool readFile(const std::string &path, std::vector<uint8_t> &bytes, std::string &error) {
    FILE *f = std::fopen(path.c_str(), "rb");
    if (f == nullptr) { error = "cannot open " + path; return false; }
    std::fseek(f, 0, SEEK_END);
    const long size = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (size <= 0) { std::fclose(f); error = "empty file"; return false; }
    if (static_cast<int64_t>(size) > Sf2Reader::kMaxFileBytes) {
        std::fclose(f);
        error = "SoundFont is larger than 320 MB";
        return false;
    }
    bytes.resize(static_cast<size_t>(size));
    const size_t got = std::fread(bytes.data(), 1, bytes.size(), f);
    std::fclose(f);
    if (got != bytes.size()) { error = "short read"; return false; }
    return true;
}

struct Chunks {
    const uint8_t *smpl = nullptr; size_t smplBytes = 0;
    const uint8_t *sm24 = nullptr; size_t sm24Bytes = 0;
    const uint8_t *phdr = nullptr; size_t phdrBytes = 0;
    const uint8_t *pbag = nullptr; size_t pbagBytes = 0;
    const uint8_t *pgen = nullptr; size_t pgenBytes = 0;
    const uint8_t *pmod = nullptr; size_t pmodBytes = 0;
    const uint8_t *imod = nullptr; size_t imodBytes = 0;
    const uint8_t *inst = nullptr; size_t instBytes = 0;
    const uint8_t *ibag = nullptr; size_t ibagBytes = 0;
    const uint8_t *igen = nullptr; size_t igenBytes = 0;
    const uint8_t *shdr = nullptr; size_t shdrBytes = 0;
    std::string name;
};

void scanList(const uint8_t *p, const uint8_t *end, Chunks &c) {
    while (end - p >= 8) {
        const char *id = reinterpret_cast<const char *>(p);
        const size_t size = rd32(p + 4);
        const uint8_t *body = p + 8;
        // Against what's left, never as body + size, which wraps on 32 bits.
        if (size > static_cast<size_t>(end - body)) break;
        auto take = [&](const char *want, const uint8_t *&dst, size_t &dstSize) {
            if (std::memcmp(id, want, 4) == 0) { dst = body; dstSize = size; }
        };
        take("smpl", c.smpl, c.smplBytes);
        take("sm24", c.sm24, c.sm24Bytes);
        take("phdr", c.phdr, c.phdrBytes);
        take("pbag", c.pbag, c.pbagBytes);
        take("pgen", c.pgen, c.pgenBytes);
        take("pmod", c.pmod, c.pmodBytes);
        take("imod", c.imod, c.imodBytes);
        take("inst", c.inst, c.instBytes);
        take("ibag", c.ibag, c.ibagBytes);
        take("igen", c.igen, c.igenBytes);
        take("shdr", c.shdr, c.shdrBytes);
        if (std::memcmp(id, "INAM", 4) == 0) c.name = trimmed(reinterpret_cast<const char *>(body), size);
        if ((size & 1) != 0 && size == static_cast<size_t>(end - body)) break; // no room for the pad byte
        p = body + size + (size & 1); // chunks are word aligned
    }
}

bool parseChunks(const std::vector<uint8_t> &bytes, Chunks &c, std::string &error) {
    if (bytes.size() < 12 || std::memcmp(bytes.data(), "RIFF", 4) != 0 ||
        std::memcmp(bytes.data() + 8, "sfbk", 4) != 0) {
        error = "not a SoundFont (no RIFF/sfbk header)";
        return false;
    }
    const uint8_t *p = bytes.data() + 12;
    const uint8_t *end = bytes.data() + bytes.size();
    while (end - p >= 8) {
        const char *id = reinterpret_cast<const char *>(p);
        const size_t size = rd32(p + 4);
        const uint8_t *body = p + 8;
        if (size > static_cast<size_t>(end - body)) break;
        if (std::memcmp(id, "LIST", 4) == 0 && size >= 4) scanList(body + 4, body + size, c);
        if ((size & 1) != 0 && size == static_cast<size_t>(end - body)) break;
        p = body + size + (size & 1);
    }
    if (c.phdr == nullptr || c.pbag == nullptr || c.pgen == nullptr || c.inst == nullptr ||
        c.ibag == nullptr || c.igen == nullptr || c.shdr == nullptr) {
        error = "SoundFont is missing its preset or instrument tables";
        return false;
    }
    if (c.smpl == nullptr) { error = "SoundFont has no sample data"; return false; }
    return true;
}

Phdr readPhdr(const uint8_t *p) {
    Phdr h{};
    std::memcpy(h.name, p, 20);
    h.preset = rd16(p + 20); h.bank = rd16(p + 22); h.bagNdx = rd16(p + 24);
    return h;
}
Bag readBag(const uint8_t *p) { return {rd16(p), rd16(p + 2)}; }
Mod readMod(const uint8_t *p) { return {rd16(p), rd16(p + 2), rd16(p + 6), rd16(p + 8), asSigned(rd16(p + 4))}; }

// --- Modulators -------------------------------------------------------------------
//
// A modulator is a controller, a curve and an amount aimed at a generator.
// The spec defines ten default ones present in every instrument unless the
// file overrides them. They're where a SoundFont's velocity response comes
// from, so a bank whose velocity layers all use one sample depends on them.

// The source field is a bitfield: index, a flag saying whether the index is a
// MIDI CC, then direction, polarity and curve type.
struct ModSourceBits {
    uint8_t index;
    bool isCc, decreasing, bipolar;
    uint8_t type;
};

ModSourceBits decodeSource(uint16_t v) {
    ModSourceBits b{};
    b.index = static_cast<uint8_t>(v & 0x7f);
    b.isCc = (v & 0x80) != 0;
    b.decreasing = (v & 0x100) != 0;
    b.bipolar = (v & 0x200) != 0;
    b.type = static_cast<uint8_t>((v >> 10) & 0x3f);
    return b;
}

// General controller numbers, used when the CC flag is clear.
enum GeneralController : uint8_t { CtrlNone = 0, CtrlVelocity = 2, CtrlKeyNumber = 3, CtrlPolyPressure = 10,
                                   CtrlChannelPressure = 13, CtrlPitchWheel = 14, CtrlPitchWheelSens = 16 };

/**
 * Converts a file modulator into one the engine can use, or returns false.
 * It's dropped when the engine has nothing matching its destination (the
 * SoundFont LFOs, effect sends) or when it has a second amount source.
 */
bool resolveMod(const Mod &m, ZoneMod &out) {
    const ModSourceBits src = decodeSource(m.src);
    if (src.type > 3) return false;
    // A second amount source multiplies two controllers. Only the pitch-wheel
    // default uses it, and the machine's own bend handles that.
    if (m.amtSrc != 0) return false;

    if (src.isCc) {
        out.source = ZoneMod::SrcCc;
        out.cc = src.index;
    } else {
        switch (src.index) {
        case CtrlNone: out.source = ZoneMod::SrcNone; break;
        case CtrlVelocity: out.source = ZoneMod::SrcVelocity; break;
        case CtrlKeyNumber: out.source = ZoneMod::SrcKeyNumber; break;
        case CtrlPolyPressure: out.source = ZoneMod::SrcPolyPressure; break;
        case CtrlChannelPressure: out.source = ZoneMod::SrcChannelPressure; break;
        case CtrlPitchWheel: return false; // the machine's own bend range governs pitch
        default: return false;
        }
    }
    switch (m.dest) {
    case GenInitialAttenuation: out.dest = ZoneMod::DstAttenuation; break;
    case 8 /* initialFilterFc */: out.dest = ZoneMod::DstFilterCutoff; break;
    case GenPan: out.dest = ZoneMod::DstPan; break;
    case GenCoarseTune: out.dest = ZoneMod::DstTuning; out.amount = static_cast<float>(m.amount) * 100.0f; break;
    case GenFineTune: out.dest = ZoneMod::DstTuning; break;
    default: return false;
    }
    if (out.dest != ZoneMod::DstTuning || m.dest == GenFineTune) out.amount = static_cast<float>(m.amount);
    out.curve = src.type <= 3 ? src.type : 0;
    out.decreasing = src.decreasing;
    out.bipolar = src.bipolar;
    return true;
}

/** The spec's default modulators, minus the ones the engine can't use. */
const Mod kDefaultMods[] = {
    // velocity -> attenuation, 960 cB, concave, decreasing, unipolar
    {0x0502, GenInitialAttenuation, 0, 0, 960},
    // velocity -> filter cutoff, -2400 cents, linear, decreasing, unipolar
    {0x0102, 8, 0, 0, -2400},
    // CC7 volume -> attenuation, concave, decreasing
    {0x0587, GenInitialAttenuation, 0, 0, 960},
    // CC11 expression -> attenuation, concave, decreasing
    {0x058b, GenInitialAttenuation, 0, 0, 960},
    // CC10 pan -> pan, linear, bipolar
    {0x028a, GenPan, 0, 0, 1000},
};

bool sameModTarget(const Mod &a, const Mod &b) { return a.src == b.src && a.dest == b.dest && a.amtSrc == b.amtSrc; }

Gen readGen(const uint8_t *p) { return {rd16(p), rd16(p + 2)}; }
Inst readInst(const uint8_t *p) {
    Inst h{};
    std::memcpy(h.name, p, 20);
    h.bagNdx = rd16(p + 20);
    return h;
}
Shdr readShdr(const uint8_t *p) {
    Shdr h{};
    std::memcpy(h.name, p, 20);
    h.start = rd32(p + 20); h.end = rd32(p + 24); h.startLoop = rd32(p + 28); h.endLoop = rd32(p + 32);
    h.sampleRate = rd32(p + 36);
    h.originalPitch = p[40];
    h.pitchCorrection = static_cast<int8_t>(p[41]);
    h.sampleLink = rd16(p + 42);
    h.sampleType = rd16(p + 44);
    return h;
}

// The generators of one zone, with the defaults a global zone supplied.
struct GenSet {
    std::map<uint16_t, uint16_t> g;
    bool has(uint16_t op) const { return g.find(op) != g.end(); }
    uint16_t raw(uint16_t op, uint16_t fallback = 0) const {
        auto it = g.find(op);
        return it == g.end() ? fallback : it->second;
    }
    int16_t sign(uint16_t op, int16_t fallback = 0) const {
        auto it = g.find(op);
        return it == g.end() ? fallback : asSigned(it->second);
    }
};

inline float timecentsToSeconds(int16_t tc) { return std::pow(2.0f, static_cast<float>(tc) / 1200.0f); }

} // namespace

bool Sf2Reader::listPresets(const std::string &path, std::vector<PresetInfo> &out, std::string &error) {
    std::vector<uint8_t> bytes;
    if (!readFile(path, bytes, error)) return false;
    Chunks c;
    if (!parseChunks(bytes, c, error)) return false;
    const size_t count = c.phdrBytes / 38;
    if (count < 2) { error = "SoundFont has no presets"; return false; }
    out.clear();
    for (size_t i = 0; i + 1 < count; ++i) { // the last entry is the EOP terminator
        const Phdr h = readPhdr(c.phdr + i * 38);
        PresetInfo info;
        info.bank = h.bank;
        info.preset = h.preset;
        info.name = trimmed(h.name, 20);
        out.push_back(info);
    }
    return true;
}

std::unique_ptr<SampleMap> Sf2Reader::load(const std::string &path, int32_t presetIndex, std::string &error) {
    std::vector<uint8_t> bytes;
    if (!readFile(path, bytes, error)) return nullptr;
    Chunks c;
    if (!parseChunks(bytes, c, error)) return nullptr;

    const size_t presetCount = c.phdrBytes / 38;
    const size_t pbagCount = c.pbagBytes / 4, pgenCount = c.pgenBytes / 4, pmodCount = c.pmodBytes / 10;
    const size_t instCount = c.instBytes / 22;
    const size_t ibagCount = c.ibagBytes / 4, igenCount = c.igenBytes / 4, imodCount = c.imodBytes / 10;
    const size_t shdrCount = c.shdrBytes / 46;
    if (presetIndex < 0 || static_cast<size_t>(presetIndex) + 1 >= presetCount) {
        error = "no such preset";
        return nullptr;
    }

    const Phdr preset = readPhdr(c.phdr + static_cast<size_t>(presetIndex) * 38);
    const Phdr nextPreset = readPhdr(c.phdr + (static_cast<size_t>(presetIndex) + 1) * 38);

    auto map = std::make_unique<SampleMap>();
    map->name = trimmed(preset.name, 20);

    // Which windows of the source samples the preset actually touches, so
    // only those decode. Keyed by the window and not just the sample, since
    // two zones can trim one sample differently.
    std::map<std::tuple<uint16_t, int64_t, int64_t>, int32_t> sampleIndexOf;
    int64_t decodedFrames = 0;

    auto gensOf = [](const uint8_t *gen, size_t genCount, size_t from, size_t to, GenSet &set) {
        for (size_t i = from; i < to && i < genCount; ++i) {
            const Gen g = readGen(gen + i * 4);
            set.g[g.oper] = g.amount;
        }
    };

    for (size_t pb = preset.bagNdx; pb < nextPreset.bagNdx && pb + 1 <= pbagCount; ++pb) {
        const Bag bag = readBag(c.pbag + pb * 4);
        const size_t genEnd = (pb + 1 < pbagCount) ? readBag(c.pbag + (pb + 1) * 4).genNdx : pgenCount;
        const size_t pModEnd = (pb + 1 < pbagCount) ? readBag(c.pbag + (pb + 1) * 4).modNdx : pmodCount;
        GenSet pset;
        gensOf(c.pgen, pgenCount, bag.genNdx, genEnd, pset);
        if (!pset.has(GenInstrument)) continue; // a global preset zone, rarely used, skip it
        const uint16_t instIndex = pset.raw(GenInstrument);
        if (instIndex + 1 >= instCount) continue;

        const Inst inst = readInst(c.inst + instIndex * 22);
        const Inst nextInst = readInst(c.inst + (static_cast<size_t>(instIndex) + 1) * 22);

        GenSet globalInst;
        std::vector<Mod> globalInstMods;
        bool haveGlobal = false;
        for (size_t ib = inst.bagNdx; ib < nextInst.bagNdx && ib + 1 <= ibagCount; ++ib) {
            const Bag ibag = readBag(c.ibag + ib * 4);
            const size_t iEnd = (ib + 1 < ibagCount) ? readBag(c.ibag + (ib + 1) * 4).genNdx : igenCount;
            const size_t iModEnd = (ib + 1 < ibagCount) ? readBag(c.ibag + (ib + 1) * 4).modNdx : imodCount;
            GenSet iset;
            if (haveGlobal) iset = globalInst;
            gensOf(c.igen, igenCount, ibag.genNdx, iEnd, iset);
            if (!iset.has(GenSampleID)) { // the instrument's global zone: defaults for the rest
                globalInst = iset;
                globalInstMods.clear();
                for (size_t mi = ibag.modNdx; mi < iModEnd && mi < imodCount; ++mi)
                    globalInstMods.push_back(readMod(c.imod + mi * 10));
                haveGlobal = true;
                continue;
            }
            const uint16_t sampleId = iset.raw(GenSampleID);
            if (sampleId + 1 > shdrCount) continue;
            const Shdr sh = readShdr(c.shdr + static_cast<size_t>(sampleId) * 46);
            if (sh.end <= sh.start) continue;

            MapZone zone;
            // Ranges intersect between the preset and instrument levels.
            uint8_t kLo = 0, kHi = 127, vLo = 1, vHi = 127;
            if (iset.has(GenKeyRange)) { kLo = iset.raw(GenKeyRange) & 0xff; kHi = (iset.raw(GenKeyRange) >> 8) & 0xff; }
            if (pset.has(GenKeyRange)) {
                kLo = std::max<uint8_t>(kLo, pset.raw(GenKeyRange) & 0xff);
                kHi = std::min<uint8_t>(kHi, (pset.raw(GenKeyRange) >> 8) & 0xff);
            }
            if (iset.has(GenVelRange)) { vLo = iset.raw(GenVelRange) & 0xff; vHi = (iset.raw(GenVelRange) >> 8) & 0xff; }
            if (pset.has(GenVelRange)) {
                vLo = std::max<uint8_t>(vLo, pset.raw(GenVelRange) & 0xff);
                vHi = std::min<uint8_t>(vHi, (pset.raw(GenVelRange) >> 8) & 0xff);
            }
            if (kHi < kLo || vHi < vLo) continue;
            zone.lowKey = kLo; zone.highKey = kHi; zone.lowVel = vLo; zone.highVel = vHi;

            const int16_t rootOverride = iset.sign(GenOverridingRootKey, -1);
            zone.rootKey = rootOverride >= 0 && rootOverride < 128 ? static_cast<uint8_t>(rootOverride)
                                                                   : sh.originalPitch;
            // Tuning adds up across the levels, as the spec says for relative
            // generators. The sample's own correction is always included.
            zone.tuneCents = static_cast<float>(sh.pitchCorrection) +
                             100.0f * static_cast<float>(iset.sign(GenCoarseTune) + pset.sign(GenCoarseTune)) +
                             static_cast<float>(iset.sign(GenFineTune) + pset.sign(GenFineTune));
            const float centibels = static_cast<float>(iset.sign(GenInitialAttenuation)) +
                                    static_cast<float>(pset.sign(GenInitialAttenuation));
            zone.gain = std::pow(10.0f, -std::max(0.0f, centibels) / 200.0f);
            zone.pan = static_cast<float>(iset.sign(GenPan) + pset.sign(GenPan)) / 500.0f;
            if (zone.pan < -1.0f) zone.pan = -1.0f;
            if (zone.pan > 1.0f) zone.pan = 1.0f;

            // Sample window and loop, with both the fine and coarse offsets.
            const int32_t startOff = iset.sign(GenStartAddrs) + 32768 * iset.sign(GenStartAddrsCoarse);
            const int32_t endOff = iset.sign(GenEndAddrs) + 32768 * iset.sign(GenEndAddrsCoarse);
            const int32_t loopStartOff = iset.sign(GenStartLoop) + 32768 * iset.sign(GenStartLoopCoarse);
            const int32_t loopEndOff = iset.sign(GenEndLoop) + 32768 * iset.sign(GenEndLoopCoarse);
            const int64_t srcStart = static_cast<int64_t>(sh.start) + startOff;
            const int64_t srcEnd = static_cast<int64_t>(sh.end) + endOff;
            if (srcEnd <= srcStart) continue;
            const int64_t available = static_cast<int64_t>(c.smplBytes / 2);
            if (srcStart < 0 || srcEnd > available) continue;

            const uint16_t modes = iset.raw(GenSampleModes, 0);
            if (modes == 1 || modes == 3) {
                zone.loopStart = static_cast<int32_t>(static_cast<int64_t>(sh.startLoop) + loopStartOff - srcStart);
                zone.loopEnd = static_cast<int32_t>(static_cast<int64_t>(sh.endLoop) + loopEndOff - srcStart);
                if (zone.loopEnd <= zone.loopStart + 1 || zone.loopStart < 0) { zone.loopStart = zone.loopEnd = -1; }
            }

            if (iset.has(GenAttackVol) || iset.has(GenReleaseVol) || iset.has(GenDecayVol)) {
                zone.hasEnvelope = true;
                zone.attack = timecentsToSeconds(iset.sign(GenAttackVol, -12000));
                zone.decay = timecentsToSeconds(iset.sign(GenDecayVol, -12000));
                const float sustainCb = static_cast<float>(iset.sign(GenSustainVol, 0));
                zone.sustain = std::pow(10.0f, -std::max(0.0f, sustainCb) / 200.0f);
                zone.release = timecentsToSeconds(iset.sign(GenReleaseVol, -12000));
            }

            // Modulators: start from the ten defaults, then the instrument's
            // global zone, this zone and the preset zone can each replace one
            // by target or add their own.
            {
                std::vector<Mod> effective(std::begin(kDefaultMods), std::end(kDefaultMods));
                auto merge = [&effective](const Mod &m) {
                    for (auto &e : effective) {
                        if (sameModTarget(e, m)) { e = m; return; }
                    }
                    effective.push_back(m);
                };
                for (const Mod &m : globalInstMods) merge(m);
                for (size_t mi = ibag.modNdx; mi < iModEnd && mi < imodCount; ++mi) merge(readMod(c.imod + mi * 10));
                for (size_t mi = bag.modNdx; mi < pModEnd && mi < pmodCount; ++mi) merge(readMod(c.pmod + mi * 10));
                for (const Mod &m : effective) {
                    if (m.amount == 0) continue;
                    ZoneMod zm;
                    if (!resolveMod(m, zm)) { ++map->droppedMods; continue; }
                    if (zm.source == ZoneMod::SrcNone) continue;
                    if (zone.modCount >= kMaxZoneMods) { ++map->droppedMods; continue; }
                    if (zm.source == ZoneMod::SrcVelocity && zm.dest == ZoneMod::DstAttenuation) {
                        zone.velocityToLevel = true;
                    }
                    zone.mods[zone.modCount++] = zm;
                }
            }

            // Decode the source window once, however many zones point at it.
            const auto key = std::make_tuple(sampleId, srcStart, srcEnd);
            auto known = sampleIndexOf.find(key);
            if (known == sampleIndexOf.end()) {
                const int64_t frames = srcEnd - srcStart;
                if (decodedFrames + frames > kMaxDecodedFrames) {
                    error = "preset needs more sample data than the limit allows";
                    return nullptr;
                }
                decodedFrames += frames;
                SampleData data;
                data.name = trimmed(sh.name, 20);
                data.stereo = false; // SF2 keeps stereo halves as two mono samples, panned
                data.frames = static_cast<int32_t>(frames);
                data.rate = static_cast<int32_t>(sh.sampleRate > 0 ? sh.sampleRate : 44100);
                data.left.resize(static_cast<size_t>(frames));
                const uint8_t *pcm = c.smpl + srcStart * 2;
                const bool have24 = c.sm24 != nullptr && c.sm24Bytes >= static_cast<size_t>(available);
                for (int64_t i = 0; i < frames; ++i) {
                    const int16_t hi = static_cast<int16_t>(rd16(pcm + i * 2));
                    if (have24) {
                        const int32_t full = (static_cast<int32_t>(hi) << 8) | c.sm24[srcStart + i];
                        data.left[static_cast<size_t>(i)] = static_cast<float>(full) / 8388608.0f;
                    } else {
                        data.left[static_cast<size_t>(i)] = static_cast<float>(hi) / 32768.0f;
                    }
                }
                map->samples.push_back(std::move(data));
                known = sampleIndexOf.emplace(key, static_cast<int32_t>(map->samples.size()) - 1).first;
            }
            zone.sample = known->second;
            map->zones.push_back(zone);
        }
    }

    if (map->zones.empty()) { error = "preset has no playable zones"; return nullptr; }
    return map;
}

} // namespace acidulous
