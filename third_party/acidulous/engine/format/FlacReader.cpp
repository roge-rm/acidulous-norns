#include "FlacReader.h"
#include "Decoded.h"
#include <cstring>

// FLAC decoder, the counterpart of FlacWriter.
//
// Our encoder only uses fixed predictors (orders 0 to 4), but other encoders
// also use LPC, so this reads both, plus escape partitions and all four
// stereo decorrelation modes.
//
// FLAC is lossless, so a round trip through our writer must return the exact
// same samples, and the test checks that.
namespace acidulous {

namespace {

/** Reads bits most significant first, the order FLAC uses. */
class BitReader {
  public:
    BitReader(const unsigned char *data, size_t size) : p(data), n(size) {}

    bool bad() const { return failed; }
    size_t bytePosition() const { return at; }
    void seekByte(size_t to) { at = to; bit = 0; failed = at > n; }
    void align() { if (bit != 0) { bit = 0; ++at; } }
    bool atEnd() const { return at >= n; }

    uint32_t get(int count) {
        uint32_t v = 0;
        for (int i = 0; i < count; ++i) {
            if (at >= n) { failed = true; return v; }
            v = (v << 1) | ((p[at] >> (7 - bit)) & 1u);
            if (++bit == 8) { bit = 0; ++at; }
        }
        return v;
    }

    /** [count] bits as a two's-complement signed value. */
    int32_t getSigned(int count) {
        if (count == 0) return 0;
        const uint32_t v = get(count);
        const uint32_t sign = 1u << (count - 1);
        return static_cast<int32_t>((v ^ sign) - sign);
    }

    /** Counts zeroes until a one, which is how Rice codes a quotient. */
    uint32_t unary() {
        uint32_t count = 0;
        while (true) {
            if (at >= n) { failed = true; return count; }
            if (((p[at] >> (7 - bit)) & 1u) != 0) {
                if (++bit == 8) { bit = 0; ++at; }
                return count;
            }
            ++count;
            if (++bit == 8) { bit = 0; ++at; }
            if (count > (1u << 24)) { failed = true; return count; } // a run this long means corrupt data
        }
    }

  private:
    const unsigned char *p;
    size_t n;
    size_t at = 0;
    int bit = 0;
    bool failed = false;
};

struct StreamInfo {
    int32_t minBlock = 0, maxBlock = 0;
    int32_t rate = 0, channels = 0, bits = 0;
    int64_t totalSamples = 0;
};

const int32_t kRateCode[12] = {0, 88200, 176400, 192000, 8000, 16000, 22050, 24000, 32000, 44100, 48000, 96000};

/** The residual of one subframe, Rice coded in partitions. */
bool readResidual(BitReader &br, int32_t blockSize, int32_t order, int32_t *out) {
    const uint32_t method = br.get(2);
    if (method > 1) return false; // 2 and 3 are reserved
    const int paramBits = method == 0 ? 4 : 5;
    const uint32_t escape = method == 0 ? 15u : 31u;
    const uint32_t partitionOrder = br.get(4);
    const int32_t partitions = 1 << partitionOrder;
    if (blockSize % partitions != 0) return false;
    const int32_t each = blockSize >> partitionOrder;
    if (each < order) return false;

    int32_t at = order;
    for (int32_t part = 0; part < partitions; ++part) {
        const int32_t count = part == 0 ? each - order : each;
        const uint32_t param = br.get(paramBits);
        if (param == escape) {
            // Escape: a raw bit width, then that many bits per sample. Zero
            // means a partition of silence.
            const int raw = static_cast<int>(br.get(5));
            for (int32_t i = 0; i < count; ++i) out[at++] = raw == 0 ? 0 : br.getSigned(raw);
        } else {
            for (int32_t i = 0; i < count; ++i) {
                const uint32_t quotient = br.unary();
                const uint32_t remainder = param > 0 ? br.get(static_cast<int>(param)) : 0;
                const uint32_t folded = (quotient << param) | remainder;
                // Zig-zag: the low bit is the sign, so small negatives stay
                // small.
                out[at++] = static_cast<int32_t>((folded >> 1) ^ (~(folded & 1u) + 1u));
            }
        }
        if (br.bad()) return false;
    }
    return true;
}

/** One channel of one frame. [bps] already includes the side channel's extra bit. */
bool readSubframe(BitReader &br, int32_t blockSize, int bps, int32_t *out) {
    if (br.get(1) != 0) return false; // the padding bit is always zero
    const uint32_t type = br.get(6);
    uint32_t wasted = 0;
    if (br.get(1) != 0) wasted = br.unary() + 1;
    const int effective = bps - static_cast<int>(wasted);
    if (effective <= 0 || effective > 32) return false;

    if (type == 0) { // CONSTANT
        const int32_t v = br.getSigned(effective);
        for (int32_t i = 0; i < blockSize; ++i) out[i] = v;
    } else if (type == 1) { // VERBATIM
        for (int32_t i = 0; i < blockSize; ++i) out[i] = br.getSigned(effective);
    } else if (type >= 8 && type <= 12) { // FIXED, order 0..4
        const int32_t order = static_cast<int32_t>(type) - 8;
        if (order > blockSize) return false;
        for (int32_t i = 0; i < order; ++i) out[i] = br.getSigned(effective);
        if (!readResidual(br, blockSize, order, out)) return false;
        // Undo the differencing of the fixed predictor.
        for (int32_t i = order; i < blockSize; ++i) {
            switch (order) {
            case 0: break;
            case 1: out[i] += out[i - 1]; break;
            case 2: out[i] += 2 * out[i - 1] - out[i - 2]; break;
            case 3: out[i] += 3 * out[i - 1] - 3 * out[i - 2] + out[i - 3]; break;
            default: out[i] += 4 * out[i - 1] - 6 * out[i - 2] + 4 * out[i - 3] - out[i - 4]; break;
            }
        }
    } else if (type >= 32) { // LPC, order 1..32
        const int32_t order = static_cast<int32_t>(type) - 31;
        if (order > blockSize) return false;
        for (int32_t i = 0; i < order; ++i) out[i] = br.getSigned(effective);
        const int precision = static_cast<int>(br.get(4)) + 1;
        if (precision > 32) return false; // 0b1111 + 1 is the invalid marker
        const int shift = br.getSigned(5);
        if (shift < 0) return false;
        int32_t coeff[32];
        for (int32_t i = 0; i < order; ++i) coeff[i] = br.getSigned(precision);
        if (!readResidual(br, blockSize, order, out)) return false;
        // The prediction uses 64 bits, since order 32 with 24-bit coefficients
        // and 24-bit samples overflows 32 bits.
        for (int32_t i = order; i < blockSize; ++i) {
            int64_t sum = 0;
            for (int32_t k = 0; k < order; ++k) {
                sum += static_cast<int64_t>(coeff[k]) * out[i - 1 - k];
            }
            out[i] += static_cast<int32_t>(sum >> shift);
        }
    } else {
        return false; // reserved
    }
    if (wasted > 0) {
        for (int32_t i = 0; i < blockSize; ++i) out[i] = static_cast<int32_t>(static_cast<uint32_t>(out[i]) << wasted);
    }
    return !br.bad();
}
} // namespace

std::unique_ptr<SampleData> FlacReader::read(const std::string &path, int32_t targetRate, std::string &error,
                                            int32_t maxSeconds) {
    std::vector<unsigned char> bytes;
    if (!slurp(path, bytes, error, slurpCeilingFor(maxSeconds))) return nullptr;
    if (bytes.size() < 8 || std::memcmp(bytes.data(), "fLaC", 4) != 0) {
        error = "not a FLAC file";
        return nullptr;
    }

    // --- metadata, only STREAMINFO is used -----------------------------------
    StreamInfo si;
    size_t at = 4;
    bool sawStreamInfo = false;
    while (at + 4 <= bytes.size()) {
        const bool last = (bytes[at] & 0x80u) != 0;
        const uint32_t type = bytes[at] & 0x7Fu;
        const size_t len = (static_cast<size_t>(bytes[at + 1]) << 16) |
                           (static_cast<size_t>(bytes[at + 2]) << 8) | bytes[at + 3];
        at += 4;
        if (at + len > bytes.size()) { error = "truncated metadata"; return nullptr; }
        if (type == 0 && len >= 34) {
            BitReader b(bytes.data() + at, len);
            si.minBlock = static_cast<int32_t>(b.get(16));
            si.maxBlock = static_cast<int32_t>(b.get(16));
            b.get(24); // min frame size
            b.get(24); // max frame size
            si.rate = static_cast<int32_t>(b.get(20));
            si.channels = static_cast<int32_t>(b.get(3)) + 1;
            si.bits = static_cast<int32_t>(b.get(5)) + 1;
            si.totalSamples = (static_cast<int64_t>(b.get(18)) << 18) | b.get(18);
            sawStreamInfo = true;
        }
        at += len;
        if (last) break;
    }
    if (!sawStreamInfo || si.rate <= 0 || si.channels <= 0) { error = "no stream info"; return nullptr; }
    if (si.channels > 2) { error = "more than two channels"; return nullptr; }
    if (si.bits > 32) { error = "unsupported bit depth"; return nullptr; }

    DecodedAudio got;
    got.rate = si.rate;
    got.stereo = si.channels == 2;
    const int64_t cap = static_cast<int64_t>(maxSeconds) * si.rate;
    const float scale = 1.0f / static_cast<float>(1LL << (si.bits - 1));

    // --- frames ------------------------------------------------------------
    BitReader br(bytes.data(), bytes.size());
    br.seekByte(at);
    std::vector<int32_t> plane[2];
    while (!br.atEnd()) {
        const size_t frameStart = br.bytePosition();
        if (br.get(14) != 0x3FFEu) { // the sync code
            // Not a frame: the end of the stream or padding after it.
            break;
        }
        br.get(1); // reserved
        const uint32_t variable = br.get(1);
        const uint32_t blockCode = br.get(4);
        const uint32_t rateCode = br.get(4);
        const uint32_t channelCode = br.get(4);
        const uint32_t sizeCode = br.get(3);
        br.get(1); // reserved

        // The frame or sample number, in a UTF-8-like coding. Its value isn't
        // used but it has to be skipped exactly.
        const uint32_t first = br.get(8);
        int extra = 0;
        if ((first & 0x80u) != 0) {
            uint32_t mask = 0x40u;
            while ((first & mask) != 0 && extra < 6) { ++extra; mask >>= 1; }
            if (extra == 0) { error = "bad frame number"; return nullptr; }
        }
        for (int i = 0; i < extra; ++i) br.get(8);
        (void)variable;

        int32_t blockSize = 0;
        if (blockCode == 1) blockSize = 192;
        else if (blockCode >= 2 && blockCode <= 5) blockSize = 576 << (blockCode - 2);
        else if (blockCode >= 8) blockSize = 256 << (blockCode - 8);
        else if (blockCode == 6) blockSize = static_cast<int32_t>(br.get(8)) + 1;
        else if (blockCode == 7) blockSize = static_cast<int32_t>(br.get(16)) + 1;
        else { error = "reserved block size"; return nullptr; }

        int32_t rate = si.rate;
        if (rateCode >= 1 && rateCode <= 11) rate = kRateCode[rateCode];
        else if (rateCode == 12) rate = static_cast<int32_t>(br.get(8)) * 1000;
        else if (rateCode == 13) rate = static_cast<int32_t>(br.get(16));
        else if (rateCode == 14) rate = static_cast<int32_t>(br.get(16)) * 10;
        else if (rateCode == 15) { error = "bad sample rate"; return nullptr; }
        (void)rate; // the stream's rate is used

        int bps = si.bits;
        switch (sizeCode) {
        case 0: break;
        case 1: bps = 8; break;
        case 2: bps = 12; break;
        case 4: bps = 16; break;
        case 5: bps = 20; break;
        case 6: bps = 24; break;
        case 7: bps = 32; break;
        default: error = "reserved sample size"; return nullptr;
        }

        br.get(8); // the header's CRC-8, not checked
        if (br.bad() || blockSize <= 0 || blockSize > 65536) { error = "bad frame header"; return nullptr; }

        const int32_t channels = channelCode < 8 ? static_cast<int32_t>(channelCode) + 1 : 2;
        if (channels != si.channels) { error = "frame disagrees about channels"; return nullptr; }
        for (int32_t c = 0; c < channels; ++c) plane[c].assign(static_cast<size_t>(blockSize), 0);

        for (int32_t c = 0; c < channels; ++c) {
            // The side channel has one more bit, since the difference of two
            // n-bit numbers needs n+1.
            const bool side = (channelCode == 8 && c == 1) || (channelCode == 9 && c == 0) ||
                              (channelCode == 10 && c == 1);
            if (!readSubframe(br, blockSize, bps + (side ? 1 : 0), plane[c].data())) {
                error = "bad subframe";
                return nullptr;
            }
        }
        br.align();
        br.get(16); // the frame's CRC-16, not checked either

        // Undo whichever decorrelation was used.
        if (channelCode == 8) { // left / side
            for (int32_t i = 0; i < blockSize; ++i) plane[1][i] = plane[0][i] - plane[1][i];
        } else if (channelCode == 9) { // side / right
            for (int32_t i = 0; i < blockSize; ++i) plane[0][i] += plane[1][i];
        } else if (channelCode == 10) { // mid / side
            for (int32_t i = 0; i < blockSize; ++i) {
                const int32_t side = plane[1][i];
                // The mid channel lost its low bit when encoded, and the side's
                // low bit restores it. Shifted as uint32_t because shifting a
                // negative int left is undefined behaviour in C++17.
                const auto mid = static_cast<int32_t>(static_cast<uint32_t>(plane[0][i]) << 1 | (side & 1u));
                plane[0][i] = (mid + side) >> 1;
                plane[1][i] = (mid - side) >> 1;
            }
        }

        // If the block doesn't fit whole, the cap cut it.
        if (static_cast<int64_t>(got.ch[0].size()) + blockSize > cap) got.truncated = true;
        for (int32_t i = 0; i < blockSize; ++i) {
            if (static_cast<int64_t>(got.ch[0].size()) >= cap) break;
            got.ch[0].push_back(static_cast<float>(plane[0][i]) * scale);
            if (got.stereo) got.ch[1].push_back(static_cast<float>(plane[1][i]) * scale);
        }
        if (br.bad()) { error = "truncated"; return nullptr; }
        if (static_cast<int64_t>(got.ch[0].size()) >= cap) break;
        if (br.bytePosition() <= frameStart) { error = "made no progress"; return nullptr; }
    }

    if (got.ch[0].empty()) { error = "no audio in it"; return nullptr; }
    got.frames = static_cast<int32_t>(got.ch[0].size());
    auto out = assemble(got, path, targetRate, maxSeconds);
    if (!out) error = "empty";
    return out;
}

} // namespace acidulous
