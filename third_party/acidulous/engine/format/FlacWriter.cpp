#include "FlacWriter.h"
#include <cmath>
#include <cstring>

namespace acidulous {

namespace {

constexpr int32_t kChannels = 2;

/** Writes bits most significant first, as FLAC is packed. */
class BitWriter {
  public:
    void clear() {
        bytes.clear();
        bit = 0;
        partial = 0;
    }
    void put(uint32_t value, int32_t count) {
        for (int32_t i = count - 1; i >= 0; --i) {
            partial = static_cast<uint8_t>((partial << 1) | ((value >> i) & 1u));
            if (++bit == 8) {
                bytes.push_back(partial);
                partial = 0;
                bit = 0;
            }
        }
    }
    /** [count] may be over 32, which unary quotients often are. */
    void putZeros(uint32_t count) {
        for (uint32_t i = 0; i < count; ++i) {
            put(0, 1);
        }
    }
    void align() {
        while (bit != 0) {
            put(0, 1);
        }
    }
    const std::vector<uint8_t> &data() const { return bytes; }
    size_t size() const { return bytes.size(); }

  private:
    std::vector<uint8_t> bytes;
    uint8_t partial = 0;
    int32_t bit = 0;
};

uint8_t crc8(const uint8_t *p, size_t n) {
    uint8_t crc = 0;
    for (size_t i = 0; i < n; ++i) {
        crc ^= p[i];
        for (int b = 0; b < 8; ++b) {
            crc = static_cast<uint8_t>((crc & 0x80) ? ((crc << 1) ^ 0x07) : (crc << 1));
        }
    }
    return crc;
}

uint16_t crc16(const uint8_t *p, size_t n) {
    uint16_t crc = 0;
    for (size_t i = 0; i < n; ++i) {
        crc ^= static_cast<uint16_t>(p[i]) << 8;
        for (int b = 0; b < 8; ++b) {
            crc = static_cast<uint16_t>((crc & 0x8000) ? ((crc << 1) ^ 0x8005) : (crc << 1));
        }
    }
    return crc;
}

/** Signed to unsigned so small negatives stay small: -1, 1, -2, 2... */
inline uint32_t zigzag(int64_t v) {
    return static_cast<uint32_t>(v < 0 ? (static_cast<uint64_t>(-v) * 2 - 1) : (static_cast<uint64_t>(v) * 2));
}

/** What a partition costs at this Rice parameter, in bits. */
uint64_t riceCost(const uint32_t *u, int32_t n, int32_t k) {
    uint64_t bits = 0;
    for (int32_t i = 0; i < n; ++i) {
        bits += static_cast<uint64_t>(u[i] >> k) + 1 + static_cast<uint64_t>(k);
    }
    return bits;
}

/** The cheapest parameter, searched around the mean. */
int32_t bestRice(const uint32_t *u, int32_t n, uint64_t &costOut) {
    uint64_t sum = 0;
    for (int32_t i = 0; i < n; ++i) {
        sum += u[i];
    }
    int32_t start = 0;
    while (start < 14 && (static_cast<uint64_t>(n) << (start + 1)) <= sum) {
        ++start;
    }
    int32_t best = start;
    uint64_t bestCost = riceCost(u, n, start);
    for (int32_t k = start > 0 ? start - 1 : 0; k <= start + 1 && k <= 14; ++k) {
        const uint64_t c = riceCost(u, n, k);
        if (c < bestCost) {
            bestCost = c;
            best = k;
        }
    }
    costOut = bestCost;
    return best;
}

/**
 * One fixed-predictor residual. Order 0 is the signal itself and each order
 * after is the difference of the one before, so the coefficients are a row
 * of Pascal's triangle with alternating signs.
 */
void residualFor(const int32_t *x, int32_t n, int32_t order, int64_t *out) {
    for (int32_t i = order; i < n; ++i) {
        switch (order) {
        case 0: out[i] = x[i]; break;
        case 1: out[i] = static_cast<int64_t>(x[i]) - x[i - 1]; break;
        case 2: out[i] = static_cast<int64_t>(x[i]) - 2LL * x[i - 1] + x[i - 2]; break;
        case 3: out[i] = static_cast<int64_t>(x[i]) - 3LL * x[i - 1] + 3LL * x[i - 2] - x[i - 3]; break;
        default: out[i] = static_cast<int64_t>(x[i]) - 4LL * x[i - 1] + 6LL * x[i - 2] - 4LL * x[i - 3] + x[i - 4]; break;
        }
    }
}

struct Residual {
    int32_t partitionOrder = 0;
    std::vector<int32_t> parameters;
    uint64_t bits = 0;
};

/** Picks the partitioning and parameters, and returns the cost. */
Residual planResidual(const uint32_t *u, int32_t n, int32_t predictorOrder) {
    Residual best;
    best.bits = UINT64_MAX;
    for (int32_t order = 0; order <= 6; ++order) {
        const int32_t parts = 1 << order;
        if (n % parts != 0) {
            continue;
        }
        const int32_t each = n / parts;
        if (each <= predictorOrder) {
            continue; // the first partition would be empty or negative
        }
        Residual r;
        r.partitionOrder = order;
        r.bits = 2 + 4 + static_cast<uint64_t>(parts) * 4; // method, order, parameters
        r.parameters.resize(static_cast<size_t>(parts));
        for (int32_t p = 0; p < parts; ++p) {
            const int32_t from = p == 0 ? predictorOrder : p * each;
            const int32_t count = p == 0 ? each - predictorOrder : each;
            uint64_t cost = 0;
            r.parameters[static_cast<size_t>(p)] = bestRice(u + from, count, cost);
            r.bits += cost;
        }
        if (r.bits < best.bits) {
            best = r;
        }
    }
    return best;
}

struct Subframe {
    int32_t order = 0;       // fixed-predictor order, or -1 for constant
    bool verbatim = false;
    Residual residual;
    uint64_t bits = 0;
};

/** The cheapest way to code this channel: constant, a predictor or raw. */
Subframe planSubframe(const int32_t *x, int32_t n, int32_t bits) {
    Subframe out;
    bool constant = true;
    for (int32_t i = 1; i < n; ++i) {
        if (x[i] != x[0]) {
            constant = false;
            break;
        }
    }
    if (constant) {
        out.order = -1;
        out.bits = 8 + static_cast<uint64_t>(bits);
        return out;
    }

    out.verbatim = true;
    out.bits = 8 + static_cast<uint64_t>(n) * bits;

    std::vector<int64_t> r(static_cast<size_t>(n));
    std::vector<uint32_t> u(static_cast<size_t>(n));
    for (int32_t order = 0; order <= 4 && order < n; ++order) {
        residualFor(x, n, order, r.data());
        for (int32_t i = order; i < n; ++i) {
            u[static_cast<size_t>(i)] = zigzag(r[static_cast<size_t>(i)]);
        }
        const Residual plan = planResidual(u.data(), n, order);
        if (plan.bits == UINT64_MAX) {
            continue;
        }
        const uint64_t total = 8 + static_cast<uint64_t>(order) * bits + plan.bits;
        if (total < out.bits) {
            out.bits = total;
            out.order = order;
            out.residual = plan;
            out.verbatim = false;
        }
    }
    return out;
}

void writeResidual(BitWriter &bw, const uint32_t *u, int32_t n, int32_t predictorOrder, const Residual &plan) {
    bw.put(0, 2); // Rice with a 4-bit parameter
    bw.put(static_cast<uint32_t>(plan.partitionOrder), 4);
    const int32_t parts = 1 << plan.partitionOrder;
    const int32_t each = n / parts;
    for (int32_t p = 0; p < parts; ++p) {
        const int32_t k = plan.parameters[static_cast<size_t>(p)];
        bw.put(static_cast<uint32_t>(k), 4);
        const int32_t from = p == 0 ? predictorOrder : p * each;
        const int32_t count = p == 0 ? each - predictorOrder : each;
        for (int32_t i = 0; i < count; ++i) {
            const uint32_t v = u[from + i];
            bw.putZeros(v >> k);
            bw.put(1, 1);
            if (k > 0) {
                bw.put(v & ((1u << k) - 1u), k);
            }
        }
    }
}

void writeSubframe(BitWriter &bw, const int32_t *x, int32_t n, int32_t bits, const Subframe &plan) {
    if (plan.order < 0) {
        bw.put(0, 1);
        bw.put(0, 6); // CONSTANT
        bw.put(0, 1);
        bw.put(static_cast<uint32_t>(x[0]), bits);
        return;
    }
    if (plan.verbatim) {
        bw.put(0, 1);
        bw.put(1, 6); // VERBATIM
        bw.put(0, 1);
        for (int32_t i = 0; i < n; ++i) {
            bw.put(static_cast<uint32_t>(x[i]), bits);
        }
        return;
    }
    bw.put(0, 1);
    bw.put(static_cast<uint32_t>(0x08 | plan.order), 6); // FIXED, 001xxx
    bw.put(0, 1);
    for (int32_t i = 0; i < plan.order; ++i) {
        bw.put(static_cast<uint32_t>(x[i]), bits);
    }
    std::vector<int64_t> r(static_cast<size_t>(n));
    std::vector<uint32_t> u(static_cast<size_t>(n));
    residualFor(x, n, plan.order, r.data());
    for (int32_t i = plan.order; i < n; ++i) {
        u[static_cast<size_t>(i)] = zigzag(r[static_cast<size_t>(i)]);
    }
    writeResidual(bw, u.data(), n, plan.order, plan.residual);
}

/** The frame number, in the same variable-length scheme UTF-8 uses. */
void putCodedNumber(BitWriter &bw, uint64_t v) {
    if (v < 0x80ull) {
        bw.put(static_cast<uint32_t>(v), 8);
        return;
    }
    int32_t bytes;
    if (v < 0x800ull) bytes = 2;
    else if (v < 0x10000ull) bytes = 3;
    else if (v < 0x200000ull) bytes = 4;
    else if (v < 0x4000000ull) bytes = 5;
    else if (v < 0x80000000ull) bytes = 6;
    else bytes = 7;
    const int32_t continuationBits = (bytes - 1) * 6;
    const uint32_t lead = (0xffu << (8 - bytes)) & 0xffu;
    bw.put(lead | static_cast<uint32_t>(v >> continuationBits), 8);
    for (int32_t i = bytes - 2; i >= 0; --i) {
        bw.put(0x80u | static_cast<uint32_t>((v >> (i * 6)) & 0x3full), 8);
    }
}

} // namespace

// --- MD5, so the file can be verified -----------------------------------------

void FlacWriter::Md5::block(const uint8_t *p) {
    static const uint32_t K[64] = {
        0xd76aa478u, 0xe8c7b756u, 0x242070dbu, 0xc1bdceeeu, 0xf57c0fafu, 0x4787c62au, 0xa8304613u, 0xfd469501u,
        0x698098d8u, 0x8b44f7afu, 0xffff5bb1u, 0x895cd7beu, 0x6b901122u, 0xfd987193u, 0xa679438eu, 0x49b40821u,
        0xf61e2562u, 0xc040b340u, 0x265e5a51u, 0xe9b6c7aau, 0xd62f105du, 0x02441453u, 0xd8a1e681u, 0xe7d3fbc8u,
        0x21e1cde6u, 0xc33707d6u, 0xf4d50d87u, 0x455a14edu, 0xa9e3e905u, 0xfcefa3f8u, 0x676f02d9u, 0x8d2a4c8au,
        0xfffa3942u, 0x8771f681u, 0x6d9d6122u, 0xfde5380cu, 0xa4beea44u, 0x4bdecfa9u, 0xf6bb4b60u, 0xbebfbc70u,
        0x289b7ec6u, 0xeaa127fau, 0xd4ef3085u, 0x04881d05u, 0xd9d4d039u, 0xe6db99e5u, 0x1fa27cf8u, 0xc4ac5665u,
        0xf4292244u, 0x432aff97u, 0xab9423a7u, 0xfc93a039u, 0x655b59c3u, 0x8f0ccc92u, 0xffeff47du, 0x85845dd1u,
        0x6fa87e4fu, 0xfe2ce6e0u, 0xa3014314u, 0x4e0811a1u, 0xf7537e82u, 0xbd3af235u, 0x2ad7d2bbu, 0xeb86d391u};
    static const int32_t R[64] = {7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22,
                                  5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20,
                                  4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
                                  6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21};
    uint32_t m[16];
    for (int i = 0; i < 16; ++i) {
        m[i] = static_cast<uint32_t>(p[i * 4]) | (static_cast<uint32_t>(p[i * 4 + 1]) << 8) |
               (static_cast<uint32_t>(p[i * 4 + 2]) << 16) | (static_cast<uint32_t>(p[i * 4 + 3]) << 24);
    }
    uint32_t a = h[0], b = h[1], c = h[2], d = h[3];
    for (int i = 0; i < 64; ++i) {
        uint32_t f;
        int32_t g;
        if (i < 16) {
            f = (b & c) | (~b & d);
            g = i;
        } else if (i < 32) {
            f = (d & b) | (~d & c);
            g = (5 * i + 1) % 16;
        } else if (i < 48) {
            f = b ^ c ^ d;
            g = (3 * i + 5) % 16;
        } else {
            f = c ^ (b | ~d);
            g = (7 * i) % 16;
        }
        const uint32_t tmp = d;
        d = c;
        c = b;
        const uint32_t sum = a + f + K[i] + m[g];
        b = b + ((sum << R[i]) | (sum >> (32 - R[i])));
        a = tmp;
    }
    h[0] += a;
    h[1] += b;
    h[2] += c;
    h[3] += d;
}

void FlacWriter::Md5::update(const uint8_t *data, size_t n) {
    length += n;
    while (n > 0) {
        const size_t room = 64 - have;
        const size_t take = n < room ? n : room;
        std::memcpy(buffer + have, data, take);
        have += take;
        data += take;
        n -= take;
        if (have == 64) {
            block(buffer);
            have = 0;
        }
    }
}

void FlacWriter::Md5::finish(uint8_t out[16]) {
    const uint64_t bits = length * 8;
    const uint8_t one = 0x80;
    update(&one, 1);
    const uint8_t zero = 0;
    while (have != 56) {
        update(&zero, 1);
    }
    uint8_t tail[8];
    for (int i = 0; i < 8; ++i) {
        tail[i] = static_cast<uint8_t>(bits >> (8 * i));
    }
    length -= 8; // the length field isn't part of the message
    update(tail, 8);
    for (int i = 0; i < 4; ++i) {
        for (int j = 0; j < 4; ++j) {
            out[i * 4 + j] = static_cast<uint8_t>(h[i] >> (8 * j));
        }
    }
}

// --- the writer ---------------------------------------------------------------

bool FlacWriter::open(const std::string &path, int32_t sampleRate, int32_t bits, std::string &error) {
    close();
    file = std::fopen(path.c_str(), "wb");
    if (file == nullptr) {
        error = "can't create " + path;
        return false;
    }
    rate = sampleRate;
    bps = bits == 16 ? 16 : 24; // FLAC can't store 32-bit float
    frames = 0;
    frameNumber = 0;
    minFrame = 0xffffffffu;
    maxFrame = 0;
    left.clear();
    right.clear();
    md5 = Md5{};
    std::memset(md5Digest, 0, sizeof md5Digest);
    std::fwrite("fLaC", 1, 4, file);
    writeStreamInfo(); // reserved now, rewritten with the totals on close
    return true;
}

void FlacWriter::writeStreamInfo() {
    const long where = 4;
    std::fseek(file, where, SEEK_SET);
    uint8_t h[4 + 34] = {};
    h[0] = 0x80; // last metadata block, type 0 (STREAMINFO)
    h[1] = 0;
    h[2] = 0;
    h[3] = 34;
    uint8_t *b = h + 4;
    const uint32_t minB = kBlock, maxB = kBlock;
    b[0] = static_cast<uint8_t>(minB >> 8);
    b[1] = static_cast<uint8_t>(minB);
    b[2] = static_cast<uint8_t>(maxB >> 8);
    b[3] = static_cast<uint8_t>(maxB);
    const uint32_t mn = minFrame == 0xffffffffu ? 0 : minFrame;
    b[4] = static_cast<uint8_t>(mn >> 16);
    b[5] = static_cast<uint8_t>(mn >> 8);
    b[6] = static_cast<uint8_t>(mn);
    b[7] = static_cast<uint8_t>(maxFrame >> 16);
    b[8] = static_cast<uint8_t>(maxFrame >> 8);
    b[9] = static_cast<uint8_t>(maxFrame);
    // 20 bits of rate, 3 of channels-1, 5 of bits-1, then 36 of total
    // samples. They cross byte boundaries, so they're packed by hand.
    const uint64_t total = static_cast<uint64_t>(frames);
    const uint32_t r = static_cast<uint32_t>(rate);
    b[10] = static_cast<uint8_t>(r >> 12);
    b[11] = static_cast<uint8_t>(r >> 4);
    b[12] = static_cast<uint8_t>(((r & 0xf) << 4) | ((kChannels - 1) << 1) | ((bps - 1) >> 4));
    b[13] = static_cast<uint8_t>((((bps - 1) & 0xf) << 4) | static_cast<uint8_t>((total >> 32) & 0xf));
    b[14] = static_cast<uint8_t>(total >> 24);
    b[15] = static_cast<uint8_t>(total >> 16);
    b[16] = static_cast<uint8_t>(total >> 8);
    b[17] = static_cast<uint8_t>(total);
    std::memcpy(b + 18, md5Digest, 16);
    std::fwrite(h, 1, sizeof h, file);
}

void FlacWriter::write(const float *interleaved, int32_t framesIn) {
    if (file == nullptr) {
        return;
    }

    for (int32_t i = 0; i < framesIn; ++i) {
        const int32_t li = quantise(interleaved[i * 2], bps);
        const int32_t ri = quantise(interleaved[i * 2 + 1], bps);
        left.push_back(li);
        right.push_back(ri);

        // The MD5 is over the samples as little-endian signed integers, as
        // many bytes as the bit depth needs.
        uint8_t s[6];
        const int32_t n = bps / 8;
        for (int32_t k = 0; k < n; ++k) {
            s[k] = static_cast<uint8_t>(li >> (8 * k));
            s[n + k] = static_cast<uint8_t>(ri >> (8 * k));
        }
        md5.update(s, static_cast<size_t>(n * 2));

        ++frames;
        if (static_cast<int32_t>(left.size()) == kBlock) {
            flushBlock();
        }
    }
}

void FlacWriter::flushBlock() {
    const auto n = static_cast<int32_t>(left.size());
    if (n == 0) {
        return;
    }

    // Stereo decorrelation. The two channels of a mix are nearly the same, so
    // coding one as the difference saves a lot. Try all four modes and keep
    // the cheapest.
    std::vector<int32_t> mid(static_cast<size_t>(n)), side(static_cast<size_t>(n));
    for (int32_t i = 0; i < n; ++i) {
        side[static_cast<size_t>(i)] = left[static_cast<size_t>(i)] - right[static_cast<size_t>(i)];
        mid[static_cast<size_t>(i)] =
            static_cast<int32_t>((static_cast<int64_t>(left[static_cast<size_t>(i)]) + right[static_cast<size_t>(i)]) >> 1);
    }
    const Subframe pl = planSubframe(left.data(), n, bps);
    const Subframe pr = planSubframe(right.data(), n, bps);
    const Subframe ps = planSubframe(side.data(), n, bps + 1);
    const Subframe pm = planSubframe(mid.data(), n, bps);

    struct Choice {
        uint32_t assignment;
        const Subframe *a;
        const Subframe *b;
        const int32_t *xa;
        const int32_t *xb;
        int32_t bitsA;
        int32_t bitsB;
        uint64_t cost;
    };
    Choice options[4] = {
        {0x1, &pl, &pr, left.data(), right.data(), bps, bps, pl.bits + pr.bits},
        {0x8, &pl, &ps, left.data(), side.data(), bps, bps + 1, pl.bits + ps.bits},
        {0x9, &ps, &pr, side.data(), right.data(), bps + 1, bps, ps.bits + pr.bits},
        {0xa, &pm, &ps, mid.data(), side.data(), bps, bps + 1, pm.bits + ps.bits},
    };
    const Choice *pick = &options[0];
    for (const Choice &c : options) {
        if (c.cost < pick->cost) {
            pick = &c;
        }
    }

    BitWriter bw;
    bw.put(0x3ffe, 14); // sync
    bw.put(0, 1);       // reserved
    bw.put(0, 1);       // fixed block size, so the number below is a frame number

    // A full block has its own code. The shorter last block writes its size
    // in 16 bits after the header.
    const bool exact = n == kBlock;
    bw.put(exact ? 0xcu : 0x7u, 4);
    bw.put(rate == 48000 ? 0xau : (rate == 44100 ? 0x9u : 0x0u), 4);
    bw.put(pick->assignment, 4);
    bw.put(bps == 16 ? 0x4u : 0x6u, 3);
    bw.put(0, 1); // reserved
    putCodedNumber(bw, frameNumber);
    if (!exact) {
        bw.put(static_cast<uint32_t>(n - 1), 16);
    }
    // CRC-8 covers the header as written so far, which is byte aligned here.
    bw.align();
    const uint8_t headerCrc = crc8(bw.data().data(), bw.size());
    bw.put(headerCrc, 8);

    writeSubframe(bw, pick->xa, n, pick->bitsA, *pick->a);
    writeSubframe(bw, pick->xb, n, pick->bitsB, *pick->b);
    bw.align();
    const uint16_t frameCrc = crc16(bw.data().data(), bw.size());
    bw.put(frameCrc, 16);

    std::fwrite(bw.data().data(), 1, bw.size(), file);
    const auto size = static_cast<uint32_t>(bw.size());
    if (size < minFrame) minFrame = size;
    if (size > maxFrame) maxFrame = size;

    ++frameNumber;
    left.clear();
    right.clear();
}

bool FlacWriter::close() {
    if (file == nullptr) {
        return true;
    }
    flushBlock();
    md5.finish(md5Digest);
    writeStreamInfo();
    const bool ok = std::fclose(file) == 0;
    file = nullptr;
    return ok;
}

} // namespace acidulous
