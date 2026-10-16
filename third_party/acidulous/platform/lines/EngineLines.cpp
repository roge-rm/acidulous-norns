#include "EngineLines.h"

#include <cctype>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace acidulous::lines {

namespace {
constexpr const char *kHeader = "acidulous-engine 1";
constexpr const char *kZip = "zip:";

bool isSpace(char c) { return c == ' ' || c == '\t' || c == '\r'; }

/**
 * UTF-8 as the JVM's GetStringUTFChars gives it: U+0000 as C0 80, and a
 * character past U+FFFF as its two UTF-16 halves, three bytes each.
 */
std::string modifiedUtf8(const std::string &in) {
    std::string out;
    out.reserve(in.size());
    for (size_t k = 0; k < in.size(); ++k) {
        const auto b = static_cast<unsigned char>(in[k]);
        if (b == 0) {
            out += '\xC0';
            out += '\x80';
        } else if ((b & 0xF8) == 0xF0 && k + 3 < in.size()) {
            const uint32_t cp = ((b & 0x07u) << 18) | ((static_cast<unsigned char>(in[k + 1]) & 0x3Fu) << 12) |
                                ((static_cast<unsigned char>(in[k + 2]) & 0x3Fu) << 6) |
                                (static_cast<unsigned char>(in[k + 3]) & 0x3Fu);
            const uint32_t v = cp - 0x10000;
            for (uint32_t half : {0xD800 + (v >> 10), 0xDC00 + (v & 0x3FF)}) {
                out += static_cast<char>(0xE0 | (half >> 12));
                out += static_cast<char>(0x80 | ((half >> 6) & 0x3F));
                out += static_cast<char>(0x80 | (half & 0x3F));
            }
            k += 3;
        } else {
            out += static_cast<char>(b);
        }
    }
    return out;
}
} // namespace

void Replay::problem(const std::string &what) {
    const std::string text = "line " + std::to_string(line) + ": " + what;
    if (log) log(text);
    else std::fprintf(stderr, "engine lines: %s\n", text.c_str());
}

jstring Replay::unit(jstring written) const {
    if (targetSlot < 0 || targetSlot > 8 || written == nullptr) return written;
    std::string &t = written->text;
    if (t.size() == 7 && t.compare(0, 6, "effect") == 0 && t[6] >= '1' && t[6] <= '9') t[6] = static_cast<char>('1' + targetSlot);
    return written;
}

void Replay::said(const char *call, jstring error) {
    if (error != nullptr && !error->text.empty()) problem(std::string(call) + ": " + error->text);
}

void Replay::answered(const char *, bool ok) {
    if (!ok) ++refused;
}

void Reader::skipSpace() {
    while (i < s.size() && isSpace(s[i])) ++i;
}

std::string Reader::word() {
    skipSpace();
    const size_t from = i;
    while (i < s.size() && !isSpace(s[i]) && s[i] != '\n' && s[i] != ']') ++i;
    if (i == from) ok = false;
    return s.substr(from, i - from);
}

jint Reader::i32() {
    const jlong v = i64();
    if (v < INT32_MIN || v > INT32_MAX) { ok = false; return 0; }
    return static_cast<jint>(v);
}

jlong Reader::i64() {
    const std::string w = word();
    char *end = nullptr;
    errno = 0;
    const long long v = std::strtoll(w.c_str(), &end, 10);
    if (w.empty() || *end != '\0' || errno == ERANGE) ok = false;
    return static_cast<jlong>(v);
}

jfloat Reader::f32() {
    const std::string w = word();
    char *end = nullptr;
    // strtof rounds correctly, so the shortest text Kotlin writes reads back as the same float.
    const float v = std::strtof(w.c_str(), &end);
    if (w.empty() || *end != '\0') ok = false;
    return v;
}

jdouble Reader::f64() {
    const std::string w = word();
    char *end = nullptr;
    const double v = std::strtod(w.c_str(), &end);
    if (w.empty() || *end != '\0') ok = false;
    return v;
}

jboolean Reader::flag() {
    const std::string w = word();
    if (w == "true") return JNI_TRUE;
    if (w != "false") ok = false;
    return JNI_FALSE;
}

bool Reader::quoted(std::string &out) {
    skipSpace();
    if (i >= s.size() || s[i] != '"') { ok = false; return false; }
    ++i;
    while (i < s.size() && s[i] != '"' && s[i] != '\n') {
        char c = s[i++];
        if (c == '\\' && i < s.size()) {
            c = s[i++];
            switch (c) {
                case 'n': c = '\n'; break;
                case 'r': c = '\r'; break;
                case 't': c = '\t'; break;
                case 'x': {
                    const std::string hex = s.substr(i, 2);
                    char *end = nullptr;
                    const long v = std::strtol(hex.c_str(), &end, 16);
                    if (hex.size() != 2 || *end != '\0' || !std::isxdigit(static_cast<unsigned char>(hex[0]))) {
                        ok = false;
                        return false;
                    }
                    c = static_cast<char>(v);
                    i += 2;
                    break;
                }
                case '\\':
                case '"': break;
                default: ok = false; return false;
            }
        }
        out += c;
    }
    if (i >= s.size() || s[i] != '"') { ok = false; return false; }
    ++i;
    return true;
}

bool Reader::null() {
    skipSpace();
    if (s.compare(i, 4, "null") != 0) return false;
    const size_t after = i + 4;
    if (after < s.size() && !isSpace(s[after]) && s[after] != '\n' && s[after] != ']') return false;
    i = after;
    return true;
}

jstring Reader::str() {
    if (null()) return nullptr;
    std::string text;
    if (!quoted(text)) return nullptr;
    return r.env->NewStringUTF(modifiedUtf8(text).c_str());
}

std::string Reader::resolve(const std::string &p) const {
    if (p.compare(0, std::strlen(kZip), kZip) != 0) return p;
    const std::string rest = p.substr(std::strlen(kZip));
    if (r.root.empty()) return rest;
    return r.root.back() == '/' ? r.root + rest : r.root + "/" + rest;
}

jstring Reader::path() {
    jstring v = str();
    if (v != nullptr) v->text = resolve(v->text);
    return v;
}

jstring Reader::spec(std::initializer_list<int> paths) {
    jstring v = str();
    if (v == nullptr) return v;
    std::string out, field;
    int index = 0;
    for (size_t k = 0; k <= v->text.size(); ++k) {
        const bool last = k == v->text.size();
        const char c = last ? '\0' : v->text[k];
        if (c == '|' || c == '\n' || last) {
            bool isPath = false;
            for (int p : paths) isPath = isPath || p == index;
            out += isPath ? resolve(field) : field;
            if (!last) out += c;
            index = c == '|' ? index + 1 : 0;
            field.clear();
        } else {
            field += c;
        }
    }
    v->text = out;
    return v;
}

jlong Reader::handle() {
    skipSpace();
    if (i >= s.size() || s[i] != '@') { ok = false; return 0; }
    ++i;
    const jlong n = i64();
    if (n < 0 || n >= static_cast<jlong>(r.handles.size())) { ok = false; return 0; }
    return r.handles[static_cast<size_t>(n)];
}

int Reader::arrayStart(char kind) {
    if (null()) return -1;
    if (i + 1 >= s.size() || s[i] != kind || s[i + 1] != '[') { ok = false; return -1; }
    i += 2;
    const size_t from = i;
    while (i < s.size() && s[i] >= '0' && s[i] <= '9') ++i;
    if (i == from || i - from > 9 || i >= s.size() || s[i] != ':') { ok = false; return -1; }
    const long n = std::strtol(s.substr(from, i - from).c_str(), nullptr, 10);
    ++i;
    // Each value takes at least two characters, so a count the line can't hold is wrong.
    if (n > kMaxArray || static_cast<size_t>(n) > (s.size() - i) / 2 + 1) { ok = false; return -1; }
    return static_cast<int>(n);
}

bool Reader::arrayEnd() {
    skipSpace();
    if (i >= s.size() || s[i] != ']') { ok = false; return false; }
    ++i;
    return true;
}

jfloatArray Reader::floats() {
    const int n = arrayStart('f');
    if (n < 0) return nullptr;
    jfloatArray a = r.env->NewFloatArray(n);
    for (int k = 0; k < n && ok; ++k) a->items[static_cast<size_t>(k)] = f32();
    arrayEnd();
    return a;
}

jintArray Reader::ints() {
    const int n = arrayStart('i');
    if (n < 0) return nullptr;
    jintArray a = r.env->NewIntArray(n);
    for (int k = 0; k < n && ok; ++k) a->items[static_cast<size_t>(k)] = i32();
    arrayEnd();
    return a;
}

jlongArray Reader::longs() {
    const int n = arrayStart('l');
    if (n < 0) return nullptr;
    jlongArray a = r.env->NewLongArray(n);
    for (int k = 0; k < n && ok; ++k) a->items[static_cast<size_t>(k)] = i64();
    arrayEnd();
    return a;
}

jobjectArray Reader::strings() {
    const int n = arrayStart('s');
    if (n < 0) return nullptr;
    jobjectArray a = r.env->NewObjectArray(n, r.env->FindClass("java/lang/String"), nullptr);
    for (int k = 0; k < n && ok; ++k) a->items[static_cast<size_t>(k)] = str();
    arrayEnd();
    return a;
}

bool Reader::finish() {
    skipSpace();
    return ok && (i >= s.size() || s[i] == '\n');
}

bool replay(const std::string &text, Replay &r) {
    size_t at = 0;
    r.line = 0;
    while (at <= text.size()) {
        size_t end = text.find('\n', at);
        if (end == std::string::npos) end = text.size();
        ++r.line;
        std::string line = text.substr(at, end - at);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        at = end + 1;
        if (r.line == 1) {
            if (line != kHeader) {
                r.problem("not engine lines (the first line should be \"" + std::string(kHeader) + "\")");
                return false;
            }
            continue;
        }
        size_t from = 0;
        while (from < line.size() && isSpace(line[from])) ++from;
        if (from == line.size() || line[from] == '#') continue;
        size_t nameEnd = from;
        while (nameEnd < line.size() && !isSpace(line[nameEnd])) ++nameEnd;
        const std::string name = line.substr(from, nameEnd - from);
        const Call *call = nullptr;
        for (int k = 0; k < kCallCount; ++k) {
            if (name == kCalls[k].name) { call = &kCalls[k]; break; }
        }
        if (call == nullptr) {
            r.problem("no call named " + name);
            ++r.skipped;
        } else {
            Reader in(line, nameEnd, r);
            if (call->run(in, r)) ++r.calls;
            else { r.problem("can't read the arguments of " + name); ++r.skipped; }
        }
        // Everything this line made, the arguments and the result.
        jniweb::arena().clear();
        if (r.afterLine) r.afterLine(r.line);
    }
    return true;
}

} // namespace acidulous::lines
