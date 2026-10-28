#include <engine/core/PresetFile.h>

#include <cmath>
#include <cstdio>
#include <cstring>

namespace acidulous {
namespace presetfile {
namespace {

// --- CRC-32, as zip wants it -----------------------------------------------------

struct CrcTable {
    uint32_t t[256];
    CrcTable() {
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1u) ? 0xedb88320u ^ (c >> 1) : c >> 1;
            t[i] = c;
        }
    }
};

uint32_t crc32(const uint8_t *p, size_t n) {
    static const CrcTable crc; // built once, safely, on first use
    const uint32_t *table = crc.t;
    uint32_t c = 0xffffffffu;
    for (size_t i = 0; i < n; ++i) c = table[(c ^ p[i]) & 0xffu] ^ (c >> 8);
    return c ^ 0xffffffffu;
}

// --- Inflate (RFC 1951) ----------------------------------------------------------

struct Bits {
    const uint8_t *p;
    size_t n;
    size_t pos = 0;
    uint32_t buf = 0;
    int count = 0;
    bool fail = false;

    int take(int need) {
        uint32_t v = buf;
        while (count < need) {
            if (pos >= n) { fail = true; return 0; }
            v |= static_cast<uint32_t>(p[pos++]) << count;
            count += 8;
        }
        buf = need < 32 ? v >> need : 0;
        count -= need;
        return static_cast<int>(v & ((1u << need) - 1u));
    }
};

struct Huffman {
    int16_t count[16];
    int16_t symbol[288];
};

// Canonical codes from code lengths. Negative for an over-full set.
int build(Huffman &h, const int16_t *length, int n) {
    for (int i = 0; i < 16; ++i) h.count[i] = 0;
    for (int s = 0; s < n; ++s) h.count[length[s]]++;
    if (h.count[0] == n) return 0;
    int left = 1;
    for (int len = 1; len < 16; ++len) {
        left <<= 1;
        left -= h.count[len];
        if (left < 0) return left;
    }
    int16_t offs[16];
    offs[1] = 0;
    for (int len = 1; len < 15; ++len) offs[len + 1] = static_cast<int16_t>(offs[len] + h.count[len]);
    for (int s = 0; s < n; ++s) if (length[s] != 0) h.symbol[offs[length[s]]++] = static_cast<int16_t>(s);
    return left;
}

int decode(Bits &b, const Huffman &h) {
    int code = 0, first = 0, index = 0;
    for (int len = 1; len < 16; ++len) {
        code |= b.take(1);
        if (b.fail) return -1;
        const int count = h.count[len];
        if (code - count < first) return h.symbol[index + (code - first)];
        index += count;
        first += count;
        first <<= 1;
        code <<= 1;
    }
    return -1;
}

const int16_t kLenBase[29] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
                              35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
const int16_t kLenExtra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
const int16_t kDistBase[30] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193,
                               257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
const int16_t kDistExtra[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6,
                                6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

bool codes(Bits &b, std::vector<uint8_t> &out, const Huffman &lens, const Huffman &dists, size_t limit) {
    while (true) {
        int sym = decode(b, lens);
        if (sym < 0) return false;
        if (sym < 256) {
            if (out.size() >= limit) return false;
            out.push_back(static_cast<uint8_t>(sym));
        } else if (sym == 256) {
            return true;
        } else {
            sym -= 257;
            if (sym >= 29) return false;
            const size_t len = static_cast<size_t>(kLenBase[sym] + b.take(kLenExtra[sym]));
            const int d = decode(b, dists);
            if (d < 0 || d >= 30) return false;
            const size_t dist = static_cast<size_t>(kDistBase[d] + b.take(kDistExtra[d]));
            if (b.fail || dist > out.size() || out.size() + len > limit) return false;
            // Byte by byte, since a copy can overlap what it writes.
            const size_t from = out.size() - dist;
            for (size_t i = 0; i < len; ++i) out.push_back(out[from + i]);
        }
    }
}

// The fixed codes of a type 1 block.
struct FixedCodes {
    Huffman lens, dists;
    FixedCodes() {
        int16_t length[288];
        int s = 0;
        for (; s < 144; ++s) length[s] = 8;
        for (; s < 256; ++s) length[s] = 9;
        for (; s < 280; ++s) length[s] = 7;
        for (; s < 288; ++s) length[s] = 8;
        build(lens, length, 288);
        for (s = 0; s < 30; ++s) length[s] = 5;
        build(dists, length, 30);
    }
};

bool inflate(const uint8_t *p, size_t n, std::vector<uint8_t> &out, size_t limit) {
    Bits b{p, n};
    int last = 0;
    do {
        last = b.take(1);
        const int type = b.take(2);
        if (b.fail) return false;
        if (type == 0) {
            b.buf = 0;
            b.count = 0;
            if (b.pos + 4 > n) return false;
            const unsigned len = p[b.pos] | (p[b.pos + 1] << 8);
            const unsigned nlen = p[b.pos + 2] | (p[b.pos + 3] << 8);
            b.pos += 4;
            if (len != (~nlen & 0xffffu) || b.pos + len > n || out.size() + len > limit) return false;
            out.insert(out.end(), p + b.pos, p + b.pos + len);
            b.pos += len;
        } else if (type == 1) {
            static const FixedCodes fixed;
            if (!codes(b, out, fixed.lens, fixed.dists, limit)) return false;
        } else if (type == 2) {
            static const int8_t order[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
            const int nlen = b.take(5) + 257;
            const int ndist = b.take(5) + 1;
            const int ncode = b.take(4) + 4;
            if (b.fail || nlen > 286 || ndist > 30) return false;
            int16_t length[320] = {};
            for (int i = 0; i < ncode; ++i) length[order[i]] = static_cast<int16_t>(b.take(3));
            Huffman lens, dists;
            if (b.fail || build(lens, length, 19) != 0) return false;
            int i = 0;
            while (i < nlen + ndist) {
                int sym = decode(b, lens);
                if (sym < 0) return false;
                if (sym < 16) {
                    length[i++] = static_cast<int16_t>(sym);
                    continue;
                }
                int16_t value = 0;
                int repeat = 0;
                if (sym == 16) {
                    if (i == 0) return false;
                    value = length[i - 1];
                    repeat = 3 + b.take(2);
                } else if (sym == 17) {
                    repeat = 3 + b.take(3);
                } else {
                    repeat = 11 + b.take(7);
                }
                if (b.fail || i + repeat > nlen + ndist) return false;
                while (repeat-- > 0) length[i++] = value;
            }
            if (length[256] == 0) return false;
            // An incomplete set is allowed; a code it lacks fails in decode.
            if (build(lens, length, nlen) < 0) return false;
            if (build(dists, length + nlen, ndist) < 0) return false;
            if (!codes(b, out, lens, dists, limit)) return false;
        } else {
            return false;
        }
    } while (!last);
    return true;
}

// --- Zip ---------------------------------------------------------------------------

uint32_t u16(const uint8_t *p) { return p[0] | (p[1] << 8); }
uint32_t u32(const uint8_t *p) { return u16(p) | (u16(p + 2) << 16); }
void put16(std::vector<uint8_t> &o, uint32_t v) { o.push_back(v & 0xff); o.push_back((v >> 8) & 0xff); }
void put32(std::vector<uint8_t> &o, uint32_t v) { put16(o, v & 0xffff); put16(o, v >> 16); }

// All the entries read from one zip together, inflated. A bank of a few
// hundred patches with long Nexus graphs is well under a megabyte; this keeps
// a hostile zip (one small deflate stream named many times) from asking for
// gigabytes on a small host.
constexpr size_t kMaxTotal = 8u << 20;

struct ZipWriter {
    struct Entry { std::string name; uint32_t crc, size, offset; };
    std::vector<uint8_t> out;
    std::vector<Entry> entries;

    // Stored, dated 1980-01-01 so the same patch gives the same bytes.
    void add(const std::string &name, const std::string &data) {
        const auto *bytes = reinterpret_cast<const uint8_t *>(data.data());
        Entry e{name, crc32(bytes, data.size()), static_cast<uint32_t>(data.size()), static_cast<uint32_t>(out.size())};
        put32(out, 0x04034b50u);
        put16(out, 20);      // version needed
        put16(out, 0x0800);  // names are UTF-8
        put16(out, 0);       // stored
        put16(out, 0);       // time
        put16(out, 0x21);    // date
        put32(out, e.crc);
        put32(out, e.size);
        put32(out, e.size);
        put16(out, static_cast<uint32_t>(name.size()));
        put16(out, 0);
        out.insert(out.end(), name.begin(), name.end());
        out.insert(out.end(), bytes, bytes + data.size());
        entries.push_back(e);
    }

    std::vector<uint8_t> finish() {
        const uint32_t dirAt = static_cast<uint32_t>(out.size());
        for (const Entry &e : entries) {
            put32(out, 0x02014b50u);
            put16(out, 20);
            put16(out, 20);
            put16(out, 0x0800);
            put16(out, 0);
            put16(out, 0);
            put16(out, 0x21);
            put32(out, e.crc);
            put32(out, e.size);
            put32(out, e.size);
            put16(out, static_cast<uint32_t>(e.name.size()));
            put16(out, 0);
            put16(out, 0);
            put16(out, 0);
            put16(out, 0);
            put32(out, 0);
            put32(out, e.offset);
            out.insert(out.end(), e.name.begin(), e.name.end());
        }
        const uint32_t dirSize = static_cast<uint32_t>(out.size()) - dirAt;
        put32(out, 0x06054b50u);
        put16(out, 0);
        put16(out, 0);
        put16(out, static_cast<uint32_t>(entries.size()));
        put16(out, static_cast<uint32_t>(entries.size()));
        put32(out, dirSize);
        put32(out, dirAt);
        put16(out, 0);
        return std::move(out);
    }
};

/** Calls [want] for each entry's name; the ones it says yes to are inflated into [out]. */
template <typename Want>
bool readZip(const uint8_t *p, size_t n, Want want, std::vector<std::pair<std::string, std::vector<uint8_t>>> &out,
             std::string &error, size_t maxTotal) {
    // The end record, from the back: it may be followed by a comment.
    if (n < 22) { error = "not a zip"; return false; }
    size_t end = n - 22;
    const size_t stop = n > 22 + 0xffff ? n - 22 - 0xffff : 0;
    while (u32(p + end) != 0x06054b50u) {
        if (end == stop) { error = "not a zip"; return false; }
        --end;
    }
    const uint32_t count = u16(p + end + 10);
    const uint32_t dirSize = u32(p + end + 12);
    size_t at = u32(p + end + 16);
    if (at + dirSize > end) { error = "a broken zip"; return false; }
    size_t total = 0;
    std::vector<size_t> offsets; // a real zip never has two entries share one
    for (uint32_t k = 0; k < count; ++k) {
        if (at + 46 > end || u32(p + at) != 0x02014b50u) { error = "a broken zip"; return false; }
        const uint32_t flags = u16(p + at + 8);
        const uint32_t method = u16(p + at + 10);
        const uint32_t crc = u32(p + at + 16);
        const uint32_t packed = u32(p + at + 20);
        const uint32_t size = u32(p + at + 24);
        const uint32_t nameLen = u16(p + at + 28);
        const uint32_t skip = u16(p + at + 30) + u16(p + at + 32);
        const size_t local = u32(p + at + 42);
        if (at + 46 + nameLen > end) { error = "a broken zip"; return false; }
        std::string name(reinterpret_cast<const char *>(p + at + 46), nameLen);
        at += 46 + nameLen + skip;
        if (name.empty() || name.back() == '/' || !want(name)) continue;
        for (const size_t o : offsets) {
            if (o == local) { error = name + ": shares its data with another entry"; return false; }
        }
        offsets.push_back(local);
        if ((flags & 1u) != 0) { error = name + ": encrypted"; return false; }
        if (local + 30 > n || u32(p + local) != 0x04034b50u) { error = name + ": a broken zip"; return false; }
        const size_t data = local + 30 + u16(p + local + 26) + u16(p + local + 28);
        if (data + packed > n) { error = name + ": a broken zip"; return false; }
        if (size > maxTotal - total) { error = "too big to be a preset file"; return false; }
        total += size;
        std::vector<uint8_t> bytes;
        if (method == 0) {
            if (packed != size) { error = name + ": a broken zip"; return false; }
            bytes.assign(p + data, p + data + packed);
        } else if (method == 8) {
            bytes.reserve(size);
            if (!inflate(p + data, packed, bytes, size) || bytes.size() != size) {
                error = name + ": couldn't be inflated";
                return false;
            }
        } else {
            error = name + ": compression " + std::to_string(method);
            return false;
        }
        if (crc32(bytes.data(), bytes.size()) != crc) { error = name + ": bad CRC"; return false; }
        out.emplace_back(std::move(name), std::move(bytes));
    }
    return true;
}

// --- JSON ----------------------------------------------------------------------------

struct Json {
    enum Kind { Null, Bool, Number, String, Array, Object } kind = Null;
    bool flag = false;
    double number = 0.0;
    std::string text;
    std::vector<Json> items;
    std::vector<std::pair<std::string, Json>> fields;

    const Json *get(const char *key) const {
        for (const auto &f : fields) if (f.first == key) return &f.second;
        return nullptr;
    }
};

/**
 * Reads a decimal number without the C library, whose strtod follows the
 * locale and would read "0,5" on some desktops. Exact for the floats the app
 * writes, which have at most nine digits.
 */
bool parseNumber(const char *&s, const char *end, double &out) {
    const char *p = s;
    bool negative = false;
    if (p < end && (*p == '-' || *p == '+')) negative = *p++ == '-';
    uint64_t mantissa = 0;
    int digits = 0, exponent = 0;
    bool any = false;
    for (; p < end && *p >= '0' && *p <= '9'; ++p) {
        any = true;
        if (digits < 19) { mantissa = mantissa * 10 + static_cast<uint64_t>(*p - '0'); if (mantissa) ++digits; }
        else ++exponent;
    }
    if (p < end && *p == '.') {
        for (++p; p < end && *p >= '0' && *p <= '9'; ++p) {
            any = true;
            if (digits < 19) { mantissa = mantissa * 10 + static_cast<uint64_t>(*p - '0'); if (mantissa) ++digits; --exponent; }
        }
    }
    if (!any) return false;
    if (p < end && (*p == 'e' || *p == 'E')) {
        ++p;
        bool down = false;
        if (p < end && (*p == '-' || *p == '+')) down = *p++ == '-';
        int e = 0;
        bool expDigits = false;
        for (; p < end && *p >= '0' && *p <= '9'; ++p) { expDigits = true; if (e < 10000) e = e * 10 + (*p - '0'); }
        if (!expDigits) return false;
        exponent += down ? -e : e;
    }
    double v = static_cast<double>(mantissa);
    // Below 2^53 and 10^22 both are exact, so this rounds once.
    static const double kPow[23] = {1e0, 1e1, 1e2, 1e3, 1e4, 1e5, 1e6, 1e7, 1e8, 1e9, 1e10, 1e11,
                                    1e12, 1e13, 1e14, 1e15, 1e16, 1e17, 1e18, 1e19, 1e20, 1e21, 1e22};
    if (mantissa == 0) v = 0.0;
    else if (exponent >= -22 && exponent < 0 && mantissa < (1ull << 53)) v /= kPow[-exponent];
    else if (exponent >= 0 && exponent <= 22 && mantissa < (1ull << 53)) v *= kPow[exponent];
    else v *= std::pow(10.0, exponent);
    out = negative ? -v : v;
    s = p;
    return true;
}

void putUtf8(std::string &o, uint32_t c) {
    if (c < 0x80) o += static_cast<char>(c);
    else if (c < 0x800) { o += static_cast<char>(0xc0 | (c >> 6)); o += static_cast<char>(0x80 | (c & 0x3f)); }
    else if (c < 0x10000) {
        o += static_cast<char>(0xe0 | (c >> 12));
        o += static_cast<char>(0x80 | ((c >> 6) & 0x3f));
        o += static_cast<char>(0x80 | (c & 0x3f));
    } else {
        o += static_cast<char>(0xf0 | (c >> 18));
        o += static_cast<char>(0x80 | ((c >> 12) & 0x3f));
        o += static_cast<char>(0x80 | ((c >> 6) & 0x3f));
        o += static_cast<char>(0x80 | (c & 0x3f));
    }
}

struct Parser {
    const char *p, *end;
    int depth = 0;

    void space() { while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) ++p; }
    bool word(const char *w) {
        const size_t n = std::strlen(w);
        if (static_cast<size_t>(end - p) < n || std::memcmp(p, w, n) != 0) return false;
        p += n;
        return true;
    }
    bool hex4(uint32_t &v) {
        if (end - p < 4) return false;
        v = 0;
        for (int i = 0; i < 4; ++i, ++p) {
            const char c = *p;
            v <<= 4;
            if (c >= '0' && c <= '9') v |= c - '0';
            else if (c >= 'a' && c <= 'f') v |= c - 'a' + 10;
            else if (c >= 'A' && c <= 'F') v |= c - 'A' + 10;
            else return false;
        }
        return true;
    }
    bool string(std::string &o) {
        if (p >= end || *p != '"') return false;
        ++p;
        while (p < end && *p != '"') {
            if (*p != '\\') { o += *p++; continue; }
            if (++p >= end) return false;
            const char c = *p++;
            switch (c) {
                case '"': o += '"'; break;
                case '\\': o += '\\'; break;
                case '/': o += '/'; break;
                case 'b': o += '\b'; break;
                case 'f': o += '\f'; break;
                case 'n': o += '\n'; break;
                case 'r': o += '\r'; break;
                case 't': o += '\t'; break;
                case 'u': {
                    uint32_t v;
                    if (!hex4(v)) return false;
                    if (v >= 0xd800 && v < 0xdc00 && end - p >= 6 && p[0] == '\\' && p[1] == 'u') {
                        const char *back = p;
                        p += 2;
                        uint32_t low;
                        if (hex4(low) && low >= 0xdc00 && low < 0xe000) v = 0x10000 + ((v - 0xd800) << 10) + (low - 0xdc00);
                        else p = back;
                    }
                    // A surrogate without its pair can't be UTF-8, so it becomes U+FFFD.
                    if (v >= 0xd800 && v < 0xe000) v = 0xfffd;
                    putUtf8(o, v);
                    break;
                }
                default: return false;
            }
        }
        if (p >= end) return false;
        ++p;
        return true;
    }
    bool value(Json &v) {
        space();
        if (p >= end || ++depth > 64) return false;
        bool ok = true;
        if (*p == '{') {
            v.kind = Json::Object;
            ++p;
            space();
            if (p < end && *p == '}') ++p;
            else while (true) {
                std::string key;
                space();
                if (!string(key)) { ok = false; break; }
                space();
                if (p >= end || *p++ != ':') { ok = false; break; }
                Json item;
                if (!value(item)) { ok = false; break; }
                v.fields.emplace_back(std::move(key), std::move(item));
                space();
                if (p < end && *p == ',') { ++p; continue; }
                if (p < end && *p == '}') { ++p; break; }
                ok = false;
                break;
            }
        } else if (*p == '[') {
            v.kind = Json::Array;
            ++p;
            space();
            if (p < end && *p == ']') ++p;
            else while (true) {
                Json item;
                if (!value(item)) { ok = false; break; }
                v.items.push_back(std::move(item));
                space();
                if (p < end && *p == ',') { ++p; continue; }
                if (p < end && *p == ']') { ++p; break; }
                ok = false;
                break;
            }
        } else if (*p == '"') {
            v.kind = Json::String;
            ok = string(v.text);
        } else if (word("true")) {
            v.kind = Json::Bool;
            v.flag = true;
        } else if (word("false")) {
            v.kind = Json::Bool;
        } else if (word("null")) {
            v.kind = Json::Null;
        } else {
            v.kind = Json::Number;
            ok = parseNumber(p, end, v.number);
        }
        --depth;
        return ok;
    }
};

bool parseJson(const std::vector<uint8_t> &bytes, Json &out) {
    Parser parser{reinterpret_cast<const char *>(bytes.data()), reinterpret_cast<const char *>(bytes.data()) + bytes.size()};
    if (!parser.value(out)) return false;
    parser.space();
    return parser.p == parser.end;
}

void quote(std::string &o, const std::string &s) {
    o += '"';
    for (const char ch : s) {
        const auto c = static_cast<unsigned char>(ch);
        switch (c) {
            case '"': o += "\\\""; break;
            case '\\': o += "\\\\"; break;
            case '\n': o += "\\n"; break;
            case '\r': o += "\\r"; break;
            case '\t': o += "\\t"; break;
            case '\b': o += "\\b"; break;
            case '\f': o += "\\f"; break;
            default:
                if (c < 0x20) {
                    char hex[8];
                    std::snprintf(hex, sizeof hex, "\\u%04x", c);
                    o += hex;
                } else {
                    o += ch;
                }
        }
    }
    o += '"';
}

/** The shortest decimal that reads back as [v], as Kotlin writes a Float. */
void number(std::string &o, float v) {
    if (!std::isfinite(v)) v = 0.0f;
    char text[32];
    for (int digits = 1; digits <= 9; ++digits) {
        std::snprintf(text, sizeof text, "%.*g", digits, static_cast<double>(v));
        for (char *c = text; *c; ++c) if (*c == ',') *c = '.'; // a locale's decimal comma
        const char *s = text;
        double back = 0.0;
        if (parseNumber(s, text + std::strlen(text), back) && static_cast<float>(back) == v) break;
    }
    o += text;
}

void indent(std::string &o, int level) { o.append(static_cast<size_t>(level) * 4, ' '); }

template <typename Value, typename Put>
void object(std::string &o, const std::vector<std::pair<std::string, Value>> &fields, int level, Put put) {
    if (fields.empty()) { o += "{}"; return; }
    o += "{\n";
    for (size_t i = 0; i < fields.size(); ++i) {
        indent(o, level + 1);
        quote(o, fields[i].first);
        o += ": ";
        put(fields[i].second);
        o += i + 1 < fields.size() ? ",\n" : "\n";
    }
    indent(o, level);
    o += '}';
}

std::string patchJson(const PresetPatch &patch, const std::string &key) {
    std::string o = "{\n";
    const auto field = [&](const char *name) { indent(o, 1); quote(o, name); o += ": "; };
    const auto floats = [&](const std::vector<std::pair<std::string, float>> &f, int level) {
        object(o, f, level, [&](float v) { number(o, v); });
    };
    const auto words = [&](const std::vector<std::pair<std::string, std::string>> &f, int level) {
        object(o, f, level, [&](const std::string &v) { quote(o, v); });
    };
    field("machine"); quote(o, key); o += ",\n";
    field("name"); quote(o, patch.name); o += ",\n";
    field("params"); floats(patch.params, 1); o += ",\n";
    field("settings"); words(patch.settings, 1); o += ",\n";
    field("low"); o += std::to_string(patch.low); o += ",\n";
    field("high"); o += std::to_string(patch.high); o += ",\n";
    field("family"); quote(o, patch.family); o += ",\n";
    field("version"); o += std::to_string(patch.version);
    if (!patch.chain.inserts.empty()) {
        o += ",\n";
        field("inserts");
        o += "[\n";
        for (size_t i = 0; i < patch.chain.inserts.size(); ++i) {
            const PatchInsert &fx = patch.chain.inserts[i];
            indent(o, 2); o += "{\n";
            indent(o, 3); o += "\"type\": "; quote(o, fx.type); o += ",\n";
            indent(o, 3); o += "\"params\": "; floats(fx.params, 3); o += ",\n";
            indent(o, 3); o += "\"bypass\": "; o += fx.bypass ? "true" : "false"; o += ",\n";
            indent(o, 3); o += "\"settings\": "; words(fx.settings, 3); o += "\n";
            indent(o, 2); o += i + 1 < patch.chain.inserts.size() ? "},\n" : "}\n";
        }
        indent(o, 1);
        o += ']';
    }
    if (patch.chain.hasSends) {
        o += ",\n";
        field("sendReverb"); number(o, patch.chain.sendReverb); o += ",\n";
        field("sendDelay"); number(o, patch.chain.sendDelay);
    }
    o += "\n}";
    return o;
}

bool readFloats(const Json *j, std::vector<std::pair<std::string, float>> &out) {
    if (j == nullptr || j->kind == Json::Null) return true;
    if (j->kind != Json::Object) return false;
    for (const auto &f : j->fields) {
        if (f.second.kind != Json::Number) return false;
        out.emplace_back(f.first, static_cast<float>(f.second.number));
    }
    return true;
}

bool readWords(const Json *j, std::vector<std::pair<std::string, std::string>> &out) {
    if (j == nullptr || j->kind == Json::Null) return true;
    if (j->kind != Json::Object) return false;
    for (const auto &f : j->fields) {
        if (f.second.kind != Json::String) return false;
        out.emplace_back(f.first, f.second.text);
    }
    return true;
}

bool readInt(const Json *j, int32_t &out) {
    if (j == nullptr) return true;
    if (j->kind != Json::Number || j->number != std::floor(j->number) || std::fabs(j->number) > 2147483647.0) return false;
    out = static_cast<int32_t>(j->number);
    return true;
}

bool readString(const Json *j, std::string &out) {
    if (j == nullptr) return true;
    if (j->kind != Json::String) return false;
    out = j->text;
    return true;
}

// What the app's Patch reader would take: name and params are required, the rest has defaults.
bool readPatch(const Json &j, PresetPatch &p) {
    if (j.kind != Json::Object) return false;
    const Json *name = j.get("name");
    const Json *machine = j.get("machine");
    if (name == nullptr || name->kind != Json::String || machine == nullptr || machine->kind != Json::String) return false;
    p.name = name->text;
    const Json *params = j.get("params");
    if (params == nullptr || !readFloats(params, p.params)) return false;
    if (!readWords(j.get("settings"), p.settings)) return false;
    p.version = 1; // the app's default: a patch from before 0.9.12 doesn't say
    if (!readInt(j.get("low"), p.low) || !readInt(j.get("high"), p.high) || !readInt(j.get("version"), p.version)) return false;
    if (!readString(j.get("family"), p.family)) return false;
    if (const Json *inserts = j.get("inserts")) {
        if (inserts->kind != Json::Array) return false;
        for (const Json &slot : inserts->items) {
            if (slot.kind != Json::Object) return false;
            PatchInsert fx;
            if (!readString(slot.get("type"), fx.type)) return false;
            if (!readFloats(slot.get("params"), fx.params) || !readWords(slot.get("settings"), fx.settings)) return false;
            if (const Json *bypass = slot.get("bypass")) {
                if (bypass->kind != Json::Bool) return false;
                fx.bypass = bypass->flag;
            }
            // An empty slot is nothing to put on a track.
            if (!fx.type.empty()) p.chain.inserts.push_back(std::move(fx));
        }
    }
    // A patch with either send has both.
    const Json *reverb = j.get("sendReverb");
    const Json *delay = j.get("sendDelay");
    const bool hasReverb = reverb != nullptr && reverb->kind != Json::Null;
    const bool hasDelay = delay != nullptr && delay->kind != Json::Null;
    if ((hasReverb && reverb->kind != Json::Number) || (hasDelay && delay->kind != Json::Number)) return false;
    p.chain.hasSends = hasReverb || hasDelay;
    p.chain.sendReverb = hasReverb ? static_cast<float>(reverb->number) : 0.0f;
    p.chain.sendDelay = hasDelay ? static_cast<float>(delay->number) : 0.0f;
    return true;
}

/** The app's rule for a unit name: [A-Za-z0-9 _.-]{1,40}, not starting with a dot. */
bool goodUnit(const std::string &unit) {
    if (unit.empty() || unit.size() > 40 || unit[0] == '.') return false;
    for (const char c : unit) {
        const bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                        c == ' ' || c == '_' || c == '.' || c == '-';
        if (!ok) return false;
    }
    return true;
}

const char *kManifest = "preset.json";
constexpr int kManifestVersion = 1;
const char *kEffectPrefix = "fx."; // PatchStore.FX

} // namespace

std::vector<uint8_t> write(const Preset &preset) {
    const bool effect = preset.kind == "effect";
    const std::string key = effect ? kEffectPrefix + preset.unit : preset.unit;
    ZipWriter zip;
    std::string list;
    for (size_t i = 0; i < preset.patches.size(); ++i) {
        const std::string entry = "patches/" + std::to_string(i + 1) + ".json";
        zip.add(entry, patchJson(preset.patches[i], key));
        if (!list.empty()) list += ",\n";
        list += "        ";
        quote(list, entry);
    }
    std::string manifest = "{\n    \"version\": " + std::to_string(kManifestVersion) + ",\n    \"kind\": ";
    quote(manifest, effect ? "effect" : "machine");
    manifest += ",\n    \"unit\": ";
    quote(manifest, preset.unit);
    manifest += ",\n    \"patches\": [" + (list.empty() ? std::string() : "\n" + list + "\n    ") + "]\n}";
    zip.add(kManifest, manifest);
    return zip.finish();
}

bool unzip(const uint8_t *data, size_t size, std::vector<std::pair<std::string, std::vector<uint8_t>>> &out,
           std::string &error, size_t maxTotal) {
    return readZip(data, size, [](const std::string &) { return true; }, out, error, maxTotal);
}

bool read(const uint8_t *data, size_t size, Preset &out, std::string &error) {
    out = Preset();
    std::vector<std::pair<std::string, std::vector<uint8_t>>> entries;
    const auto wanted = [](const std::string &name) {
        return name == kManifest ||
               (name.compare(0, 8, "patches/") == 0 && name.size() > 13 && name.compare(name.size() - 5, 5, ".json") == 0);
    };
    if (!readZip(data, size, wanted, entries, error, kMaxTotal)) return false;
    const std::vector<uint8_t> *manifestBytes = nullptr;
    for (const auto &e : entries) if (e.first == kManifest) manifestBytes = &e.second;
    Json manifest;
    if (manifestBytes == nullptr) { error = "not a preset file"; return false; }
    if (!parseJson(*manifestBytes, manifest) || manifest.kind != Json::Object) { error = "the manifest couldn't be read"; return false; }
    int32_t version = 1;
    if (!readInt(manifest.get("version"), version) || version > kManifestVersion) {
        error = "made by a newer version";
        return false;
    }
    const Json *kind = manifest.get("kind");
    const Json *unit = manifest.get("unit");
    const Json *patches = manifest.get("patches");
    if (kind == nullptr || kind->kind != Json::String || (kind->text != "machine" && kind->text != "effect")) {
        error = "not a machine or effect preset";
        return false;
    }
    if (unit == nullptr || unit->kind != Json::String || !goodUnit(unit->text)) { error = "a bad unit name"; return false; }
    if (patches == nullptr || patches->kind != Json::Array || patches->items.empty()) { error = "no patches"; return false; }
    out.kind = kind->text;
    out.unit = unit->text;
    for (const Json &item : patches->items) {
        if (item.kind != Json::String) { error = "a bad patch list"; return false; }
        const std::vector<uint8_t> *bytes = nullptr;
        for (const auto &e : entries) if (e.first == item.text) bytes = &e.second;
        Json j;
        PresetPatch patch;
        if (bytes == nullptr || !parseJson(*bytes, j) || !readPatch(j, patch)) {
            error = item.text + " is missing or couldn't be read";
            out = Preset();
            return false;
        }
        patch.machine = out.unit;
        out.patches.push_back(std::move(patch));
    }
    return true;
}

} // namespace presetfile
} // namespace acidulous
