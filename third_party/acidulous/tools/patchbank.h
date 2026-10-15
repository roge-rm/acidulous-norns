#pragma once
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <istream>
#include <sstream>
#include <algorithm>
#include <string>
#include <vector>

#include <engine/core/Params.h>

// A machine's factory patches, as text.
//
// Values are written in real units (`cutoff 620 Hz`) and resolved through
// the engine's own ParamDef, so there's no second copy of the parameter
// ranges to drift. The audition harness reads these files and
// tools/gen_patches.sh turns the same files into the Kotlin that ships, so
// what's auditioned is what's in the app.
//
// The format:
//
//     # comments run to the end of the line
//     machine  Reflux          # or `effect Delay`
//     role     bass             # the phrase these patches are auditioned with
//     material none             # what to mount; see audition_material.h
//     input    none             # what to put on the input bus
//
//     patch Init                # the first patch, always, and always empty
//
//     patch "Classic Acid"  role=bass
//     patch Oboe  note=70 range=60..84   # measured at 70; played 60..84
//       wave       0            # a stepped parameter by value...
//       mode       #1           # ...or by step index
//       cutoff     620 Hz       # the unit is checked against the engine's
//       decay      320 ms
//       resonance  0.62         # a 0..1 parameter's range is 0..1
//       set formula "t * (t >> 5)"
//
// Values are always in the parameter's own units. The optional unit suffix
// enforces that: `620` in a parameter measured in ms would go unnoticed, but
// `620 Hz` is a parse error.

namespace acidulous::audition {

struct BankValue {
    std::string name;
    double number = 0.0;
    std::string unit;   // as written, checked against ParamDef::unit
    int stepIndex = -1; // >= 0 when written as #N
    int line = 0;
};

struct BankPatch {
    std::string name;
    std::string role;     // overrides the bank's
    // What kind of sound this is, for grouping. It prefixes the rendered
    // wav's name so a folder sorts into families. Defaults to the role, which
    // is usually the same thing, except for Cumulus where ten families share
    // four demo phrases.
    std::string family;
    std::string material; // overrides the bank's
    std::string input;    // overrides the bank's
    int note = -1;        // overrides the phrase's default
    // The range the instrument is played in, as MIDI notes, or -1 for none.
    // The audition phrase stays inside it, and the app puts the keyboard at
    // its bottom when the patch is loaded, so a bassoon is played where a
    // bassoon is.
    int low = -1, high = -1;
    std::vector<BankValue> values;
    std::vector<std::pair<std::string, std::string>> settings;
    int line = 0;
};

struct Bank {
    std::string unit;     // "Reflux", or "fx.Delay"
    std::string role = "note";
    std::string material = "none";
    std::string input = "none";
    std::vector<BankPatch> patches;
    std::string path;

    bool isEffect() const { return unit.rfind("fx.", 0) == 0; }
    /** The engine's type name: "Delay" for "fx.Delay". */
    std::string typeName() const { return isEffect() ? unit.substr(3) : unit; }
};

namespace detail {

inline std::string trim(const std::string &s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
    return s.substr(a, b - a);
}

/** Strips a trailing comment, honouring quotes so a `#` inside a string survives. */
inline std::string stripComment(const std::string &s) {
    bool inQuote = false;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '"') inQuote = !inQuote;
        else if (s[i] == '#' && !inQuote) {
            // A `#` that opens a step index is not a comment: `mode #1`.
            if (i + 1 < s.size() && std::isdigit(static_cast<unsigned char>(s[i + 1]))) continue;
            return s.substr(0, i);
        }
    }
    return s;
}

/** The next token, honouring double quotes. Advances [at]. */
inline bool token(const std::string &s, size_t &at, std::string &out) {
    while (at < s.size() && std::isspace(static_cast<unsigned char>(s[at]))) ++at;
    if (at >= s.size()) return false;
    if (s[at] == '"') {
        const size_t end = s.find('"', at + 1);
        if (end == std::string::npos) {
            out = s.substr(at + 1);
            at = s.size();
        } else {
            out = s.substr(at + 1, end - at - 1);
            at = end + 1;
        }
        return true;
    }
    const size_t start = at;
    while (at < s.size() && !std::isspace(static_cast<unsigned char>(s[at]))) ++at;
    out = s.substr(start, at - start);
    return true;
}

} // namespace detail

/** Reads one bank file. Returns false and fills [error] on a malformed line. */
/**
 * A bank from any stream: [path] is only used to name it in errors and
 * `bank.path`.
 *
 * The browser build has no files to open, so it carries the bank texts in the
 * binary and reads them from memory. Everything else uses readBank.
 */
inline bool readBankFrom(std::istream &in, const std::string &path, Bank &bank, std::string &error) {
    bank = Bank();
    bank.path = path;
    std::string raw;
    int lineNo = 0;
    auto fail = [&](const std::string &why) {
        error = path + ":" + std::to_string(lineNo) + ": " + why;
        return false;
    };
    while (std::getline(in, raw)) {
        ++lineNo;
        const std::string line = detail::trim(detail::stripComment(raw));
        if (line.empty()) continue;
        size_t at = 0;
        std::string head;
        detail::token(line, at, head);

        if (head == "machine" || head == "effect") {
            std::string name;
            if (!detail::token(line, at, name)) return fail("a name is wanted after " + head);
            bank.unit = (head == "effect" ? "fx." : "") + name;
            continue;
        }
        if (head == "role" && bank.patches.empty()) {
            if (!detail::token(line, at, bank.role)) return fail("a phrase is wanted after role");
            continue;
        }
        if (head == "material" && bank.patches.empty()) {
            if (!detail::token(line, at, bank.material)) return fail("a kind is wanted after material");
            continue;
        }
        if (head == "input" && bank.patches.empty()) {
            if (!detail::token(line, at, bank.input)) return fail("a kind is wanted after input");
            continue;
        }
        if (head == "patch") {
            BankPatch p;
            p.line = lineNo;
            if (!detail::token(line, at, p.name)) return fail("a name is wanted after patch");
            std::string attr;
            while (detail::token(line, at, attr)) {
                const size_t eq = attr.find('=');
                if (eq == std::string::npos) return fail("expected key=value, got '" + attr + "'");
                const std::string k = attr.substr(0, eq), v = attr.substr(eq + 1);
                if (k == "role") p.role = v;
                // Every patch should have one. The browser groups a bank by
                // family, and one long list is hard to find anything in. Aim
                // for four to seven families of at least three patches each.
                else if (k == "family") p.family = v;
                else if (k == "material") p.material = v;
                else if (k == "input") p.input = v;
                else if (k == "note") p.note = std::atoi(v.c_str());
                else if (k == "range") {
                    const size_t dots = v.find("..");
                    if (dots == std::string::npos) return fail("range wants lo..hi, got '" + v + "'");
                    p.low = std::atoi(v.c_str());
                    p.high = std::atoi(v.c_str() + dots + 2);
                    if (p.low < 0 || p.high > 127 || p.low >= p.high) {
                        return fail("range " + v + " is not a range of notes");
                    }
                } else return fail("unknown attribute '" + k + "'");
            }
            bank.patches.push_back(std::move(p));
            continue;
        }
        if (bank.patches.empty()) return fail("'" + head + "' before any patch");
        BankPatch &p = bank.patches.back();

        if (head == "set") {
            std::string key, value;
            if (!detail::token(line, at, key)) return fail("a key is wanted after set");
            detail::token(line, at, value); // an empty string is legal: it clears
            // `\n` is a newline, because some settings are whole documents.
            // A Nexus patch is its graph (a line per module and per cable)
            // and Mosaic's zone map is the same, but a bank file is one
            // setting per line.
            std::string out;
            out.reserve(value.size());
            for (size_t i = 0; i < value.size(); ++i) {
                if (value[i] == '\\' && i + 1 < value.size()) {
                    const char c = value[i + 1];
                    if (c == 'n') { out += '\n'; ++i; continue; }
                    if (c == '\\') { out += '\\'; ++i; continue; }
                }
                out += value[i];
            }
            p.settings.emplace_back(key, out);
            continue;
        }

        BankValue v;
        v.name = head;
        v.line = lineNo;
        std::string num;
        if (!detail::token(line, at, num)) return fail("no value for '" + head + "'");
        if (num[0] == '#') {
            v.stepIndex = std::atoi(num.c_str() + 1);
        } else {
            char *end = nullptr;
            v.number = std::strtod(num.c_str(), &end);
            if (end == num.c_str()) return fail("'" + num + "' is not a number");
            // A unit written without a space, as in `620Hz`.
            if (end != nullptr && *end != '\0') v.unit = end;
        }
        if (v.unit.empty()) detail::token(line, at, v.unit);
        p.values.push_back(std::move(v));
    }
    if (bank.unit.empty()) {
        error = path + ": no `machine` or `effect` line";
        return false;
    }
    return true;
}

inline bool readBank(const std::string &path, Bank &bank, std::string &error) {
    std::ifstream in(path);
    if (!in) {
        error = "cannot open " + path;
        return false;
    }
    return readBankFrom(in, path, bank, error);
}

/**
 * One patch resolved against the engine's own parameter table: normalised
 * values by index, with everything the patch left out at its default.
 *
 * Defaulting the rest matches ParamBinding.applyAll in the app and is what
 * lets a patch list only what it changes. Without it a patch could sound
 * right in the harness only because of what the previous patch left behind.
 */
struct Resolved {
    std::vector<float> norm;                 // one per parameter, all of them
    std::vector<std::pair<std::string, std::string>> settings;
    std::vector<std::string> problems;       // named parameters that do not exist, values out of range
};

inline Resolved resolve(const BankPatch &patch, const ParamDef *defs, int32_t count) {
    Resolved r;
    r.norm.resize(static_cast<size_t>(count));
    for (int32_t i = 0; i < count; ++i) r.norm[static_cast<size_t>(i)] = defs[i].unmap(defs[i].def);
    r.settings = patch.settings;

    // One `*` in a name stands for any run of characters, and the value is
    // applied to every parameter it matches. For example Forage has thirteen
    // pads of fourteen parameters, so `p*_cutoff 2200 Hz` sets them all at
    // once. Dice's sixteen slices work the same way.
    //
    // A name with no star must match exactly, and a star that matches nothing
    // is an error, so typos are still caught.
    const auto matching = [&](const std::string &name, std::vector<int32_t> &out) {
        const size_t star = name.find('*');
        if (star == std::string::npos) {
            for (int32_t i = 0; i < count; ++i) {
                if (std::strcmp(defs[i].name, name.c_str()) == 0) { out.push_back(i); return; }
            }
            return;
        }
        const std::string pre = name.substr(0, star), post = name.substr(star + 1);
        for (int32_t i = 0; i < count; ++i) {
            const std::string n = defs[i].name;
            if (n.size() < pre.size() + post.size()) continue;
            if (n.compare(0, pre.size(), pre) != 0) continue;
            if (n.compare(n.size() - post.size(), post.size(), post) != 0) continue;
            out.push_back(i);
        }
    };

    for (const BankValue &v : patch.values) {
        std::vector<int32_t> matches;
        matching(v.name, matches);
        if (matches.empty()) {
            // The most important check. A name that doesn't exist does
            // nothing in the app (applyAll fills the default and moves on),
            // so a typo in a patch would otherwise go unnoticed.
            r.problems.push_back("line " + std::to_string(v.line) + ": no parameter named '" + v.name + "'");
            continue;
        }
        for (const int32_t index : matches) {
        const ParamDef &d = defs[index];
        float v01;
        if (v.stepIndex >= 0) {
            if (d.curve != Curve::Stepped) {
                r.problems.push_back("line " + std::to_string(v.line) + ": '" + v.name +
                                     "' is not stepped, so #" + std::to_string(v.stepIndex) + " means nothing");
                continue;
            }
            if (v.stepIndex >= d.steps) {
                r.problems.push_back("line " + std::to_string(v.line) + ": '" + v.name + "' has " +
                                     std::to_string(d.steps) + " steps, not #" + std::to_string(v.stepIndex));
                continue;
            }
            v01 = d.steps > 1 ? static_cast<float>(v.stepIndex) / static_cast<float>(d.steps - 1) : 0.0f;
        } else {
            if (!v.unit.empty() && d.unit != nullptr && d.unit[0] != '\0' && v.unit != d.unit) {
                r.problems.push_back("line " + std::to_string(v.line) + ": '" + v.name + "' is in " + d.unit +
                                     ", not " + v.unit);
                continue;
            }
            const auto value = static_cast<float>(v.number);
            const float lo = d.min < d.max ? d.min : d.max, hi = d.min < d.max ? d.max : d.min;
            if (value < lo - 1e-4f || value > hi + 1e-4f) {
                char buf[192];
                std::snprintf(buf, sizeof(buf), "line %d: '%s' is %g, outside %g..%g%s%s", v.line,
                              v.name.c_str(), v.number, static_cast<double>(d.min), static_cast<double>(d.max),
                              d.unit != nullptr && d.unit[0] != '\0' ? " " : "", d.unit != nullptr ? d.unit : "");
                r.problems.emplace_back(buf);
                continue;
            }
            v01 = d.unmap(value);
        }
        if (v01 < 0.0f) v01 = 0.0f;
        if (v01 > 1.0f) v01 = 1.0f;
        r.norm[static_cast<size_t>(index)] = v01;
        }
    }
    // A bad value behind a star would fail once per matched parameter, so
    // report each message only once.
    std::vector<std::string> once;
    for (const std::string &p : r.problems) {
        if (std::find(once.begin(), once.end(), p) == once.end()) once.push_back(p);
    }
    r.problems.swap(once);
    return r;
}

} // namespace acidulous::audition
