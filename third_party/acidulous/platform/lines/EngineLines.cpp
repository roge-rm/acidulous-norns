#include "EngineLines.h"

#include <engine/machine/TableCounts.h>
#include <engine/machine/filament/SymLevel.h>

#include <cctype>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace acidulous::lines {

namespace {
constexpr const char *kHeader = "acidulous-engine 4";
// Version 1 stored Trinity's waves over 12 steps and Nexus's wtosc table over
// 8, versions 1 and 2 Trinity's destinations over 34, and versions 1 to 3
// Filament's sympathetic level for a quieter bank; upgradeLine moves them
// onto today's.
constexpr const char *kHeaderV1 = "acidulous-engine 1";
constexpr const char *kHeaderV2 = "acidulous-engine 2";
constexpr const char *kHeaderV3 = "acidulous-engine 3";
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

namespace {

/** A line's words, with a quoted string as one word. Each is [from, to) in the line. */
std::vector<std::pair<size_t, size_t>> words(const std::string &line) {
    std::vector<std::pair<size_t, size_t>> out;
    size_t i = 0;
    while (i < line.size()) {
        while (i < line.size() && isSpace(line[i])) ++i;
        if (i >= line.size()) break;
        const size_t from = i;
        if (line[i] == '"') {
            ++i;
            while (i < line.size() && line[i] != '"') i += line[i] == '\\' ? 2 : 1;
            if (i < line.size()) ++i;
        } else {
            while (i < line.size() && !isSpace(line[i])) ++i;
        }
        out.emplace_back(from, i < line.size() ? i : line.size());
    }
    return out;
}

/** A quoted word's text with its escapes undone. Only what the upgrade compares needs to be exact. */
std::string unquote(const std::string &w) {
    if (w.size() < 2 || w.front() != '"') return w;
    std::string out;
    for (size_t k = 1; k + 1 < w.size(); ++k) {
        if (w[k] != '\\' || k + 2 >= w.size()) { out += w[k]; continue; }
        const char e = w[++k];
        out += e == 'n' ? '\n' : e == 'r' ? '\r' : e == 't' ? '\t' : e;
    }
    return out;
}

/** A float as text that reads back as the same float. */
std::string floatText(float v) {
    char b[32];
    std::snprintf(b, sizeof b, "%.9g", static_cast<double>(v));
    return b;
}

} // namespace

void Replay::noteTables(const std::string &line) {
    const auto w = words(line);
    if (w.size() < 3) return;
    auto text = [&](size_t k) { return line.substr(w[k].first, w[k].second - w[k].first); };
    const std::string call = text(0);
    if (call == "LoadNexusPatch") {
        // Which slots hold a wtosc, so their first knob can be moved.
        uint32_t slots = 0;
        const std::string spec = unquote(text(2));
        size_t at = 0;
        while (at < spec.size()) {
            size_t end = spec.find('\n', at);
            if (end == std::string::npos) end = spec.size();
            const std::string l = spec.substr(at, end - at);
            at = end + 1;
            if (l.size() > 2 && l[0] == 'm' && l[1] == '|') {
                const size_t bar = l.find('|', 2);
                if (bar == std::string::npos) continue;
                std::string type = l.substr(bar + 1);
                type = type.substr(0, type.find('|'));
                const int slot = std::atoi(l.substr(2, bar - 2).c_str());
                if (type == "wtosc" && slot >= 0 && slot < 32) slots |= 1u << slot;
            }
        }
        tableSlotsOf[std::atoi(text(1).c_str())] |= slots;
    }
}

std::string Replay::upgradeLine(const std::string &line) {
    if (version >= 4 && line.find("\"pingpong\"") == std::string::npos) return line;
    const auto w = words(line);
    if (w.size() < 2) return line;
    auto text = [&](size_t k) { return line.substr(w[k].first, w[k].second - w[k].first); };
    const std::string call = text(0);
    // The Delay's pingpong switch became `mode`, with on as ping-pong
    // (model/DelayMigration.kt). Lines from any version can hold it, and
    // only the Delay had a param by that name.
    auto delayMode = [](float v) { return v >= 0.5f ? 1.0f / 3.0f : 0.0f; };
    if (call == "SetParam" && w.size() >= 5 && unquote(text(2)) != "machine" && unquote(text(3)) == "pingpong") {
        const float v = std::strtof(text(4).c_str(), nullptr);
        return line.substr(0, w[3].first) + "\"mode\" " + floatText(delayMode(v)) + line.substr(w[4].second);
    }
    if (call == "SnapshotSetLane" && w.size() >= 9 && unquote(text(5)) != "machine" && unquote(text(6)) == "pingpong") {
        std::string out = line.substr(0, w[6].first) + "\"mode\"" + line.substr(w[6].second, w[8].second - w[6].second);
        // Points are pairs from word 9 on; every second one is a value, and -1 means the knob's own value.
        size_t last = w[8].second;
        for (size_t k = 9; k < w.size(); ++k) {
            std::string word = text(k);
            const bool closes = !word.empty() && word.back() == ']';
            if (closes) word.pop_back();
            if ((k - 9) % 2 == 1) {
                const float v = std::strtof(word.c_str(), nullptr);
                if (v != -1.0f) word = floatText(delayMode(v));
            }
            out += line.substr(last, w[k].first - last) + word + (closes ? "]" : "");
            last = w[k].second;
        }
        return out + line.substr(last);
    }
    if (version >= 4) return line;
    if (call == "MountMachine" && w.size() >= 3) {
        const int rack = std::atoi(text(1).c_str());
        machineOf[rack] = unquote(text(2));
        filamentOf.erase(rack);
        // A Filament that never sets its level plays it at the default,
        // which was for the quieter bank.
        if (version < 4 && machineOf[rack] == "Filament")
            return line + "\nSetParam " + text(1) + " \"machine\" \"symlevel\" " +
                   floatText(filament_levels::level(filament_levels::kLevelDefault)) + " false 0";
        return line;
    }
    if (call == "UnmountMachine") {
        machineOf.erase(std::atoi(text(1).c_str()));
        filamentOf.erase(std::atoi(text(1).c_str()));
        return line;
    }
    // Which rule a parameter of a machine follows, if any.
    auto ruleFor = [&](const std::string &machine, int rack, const std::string &name) -> float (*)(float) {
        if (version >= 3) return nullptr;
        if (machine == "Trinity" && tables::isTrinityDest(name)) return tables::trinityDest;
        if (version >= 2) return nullptr;
        if (machine == "Trinity" && tables::isTrinityWave(name)) return tables::trinityWave;
        if (machine == "Nexus") {
            const int slot = tables::nexusFirstKnobSlot(name);
            const auto it = tableSlotsOf.find(rack);
            if (slot >= 0 && it != tableSlotsOf.end() && ((it->second >> slot) & 1u) != 0) return tables::nexusTable;
        }
        return nullptr;
    };
    if (call == "SetParam" && w.size() >= 5) {
        const int rack = std::atoi(text(1).c_str());
        if (unquote(text(2)) != "machine") return line;
        const auto m = machineOf.find(rack);
        if (m == machineOf.end()) return line;
        if (version < 4 && m->second == "Filament") {
            // A depth follows whether its slot is aimed at the bank, and the
            // slot can be aimed after the depth is set, so both are kept as
            // written and the depth set again when the aim moves.
            const std::string name = unquote(text(3));
            const float v = std::strtof(text(4).c_str(), nullptr);
            auto &f = filamentOf[rack];
            if (name == "symlevel") return line.substr(0, w[4].first) + floatText(filament_levels::level(v)) + line.substr(w[4].second);
            if (const int slot = filament_levels::matrixSlot(name, "amt")) {
                f.amt[slot - 1] = v;
                f.amtSet[slot - 1] = true;
                if (!f.toBank[slot - 1]) return line;
                return line.substr(0, w[4].first) + floatText(filament_levels::depth(v)) + line.substr(w[4].second);
            }
            if (const int slot = filament_levels::matrixSlot(name, "dst")) {
                const bool before = f.toBank[slot - 1];
                f.toBank[slot - 1] = filament_levels::aimsAtBank(v);
                if (before == f.toBank[slot - 1] || !f.amtSet[slot - 1]) return line;
                const float amt = f.amt[slot - 1];
                char amtName[8];
                std::snprintf(amtName, sizeof amtName, "m%d_amt", slot);
                return line + "\nSetParam " + text(1) + " \"machine\" \"" + amtName + "\" " +
                       floatText(f.toBank[slot - 1] ? filament_levels::depth(amt) : amt) + line.substr(w[4].second);
            }
            return line;
        }
        const auto rule = ruleFor(m->second, rack, unquote(text(3)));
        if (rule == nullptr) return line;
        const float v = std::strtof(text(4).c_str(), nullptr);
        return line.substr(0, w[4].first) + floatText(rule(v)) + line.substr(w[4].second);
    }
    if (call == "SnapshotSetLane" && w.size() >= 9) {
        // @h rack scene "machine" "unit" "name" linear f[n: tick value ...]
        const int rack = std::atoi(text(2).c_str());
        if (unquote(text(5)) != "machine") return line;
        float (*rule)(float) = nullptr;
        if (unquote(text(4)) == "Filament") {
            const std::string name = unquote(text(6));
            if (version >= 4) return line;
            if (name == "symlevel") rule = filament_levels::level;
            else if (const int slot = filament_levels::matrixSlot(name, "amt")) {
                const auto f = filamentOf.find(rack);
                if (f != filamentOf.end() && f->second.toBank[slot - 1]) rule = filament_levels::depth;
            }
        } else {
            rule = ruleFor(unquote(text(4)), rack, unquote(text(6)));
        }
        if (rule == nullptr) return line;
        std::string out = line.substr(0, w[8].second);
        // Points are pairs from word 9 on; every second one is a value, and -1 means the knob's own value.
        size_t last = w[8].second;
        for (size_t k = 9; k < w.size(); ++k) {
            std::string word = text(k);
            const bool closes = !word.empty() && word.back() == ']';
            if (closes) word.pop_back();
            if ((k - 9) % 2 == 1) {
                const float v = std::strtof(word.c_str(), nullptr);
                if (v != -1.0f) word = floatText(rule(v));
            }
            out += line.substr(last, w[k].first - last) + word + (closes ? "]" : "");
            last = w[k].second;
        }
        return out + line.substr(last);
    }
    return line;
}

namespace {

/** Replays one call. */
void replayOne(const std::string &line, Replay &r) {
    size_t from = 0;
    while (from < line.size() && isSpace(line[from])) ++from;
    if (from == line.size() || line[from] == '#') return;
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
} // namespace

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
            if (line == kHeaderV1) {
                r.version = 1;
                // The app loads a Nexus patch after setting its knobs, so
                // find every rack's wtosc slots before replaying any line.
                for (size_t from = 0; from < text.size();) {
                    size_t to = text.find('\n', from);
                    if (to == std::string::npos) to = text.size();
                    r.noteTables(text.substr(from, to - from));
                    from = to + 1;
                }
            }
            else if (line == kHeaderV2) r.version = 2;
            else if (line == kHeaderV3) r.version = 3;
            else if (line == kHeader) r.version = 4;
            else {
                r.problem("not engine lines (the first line should be \"" + std::string(kHeader) + "\")");
                return false;
            }
            continue;
        }
        line = r.upgradeLine(line);
        // An upgrade can add lines after the one it was given.
        for (size_t part = 0; part <= line.size();) {
            size_t partEnd = line.find('\n', part);
            if (partEnd == std::string::npos) partEnd = line.size();
            replayOne(line.substr(part, partEnd - part), r);
            part = partEnd + 1;
        }
    }
    return true;
}

} // namespace acidulous::lines
