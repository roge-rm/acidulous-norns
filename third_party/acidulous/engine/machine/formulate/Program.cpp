#include "Program.h"
#include <cctype>
#include <cstdlib>

namespace acidulous::machine::formulate {

namespace {
bool parseTable(const std::string &text, const char *what, Table &out, std::string &error) {
    out.steps.clear();
    out.loopFrom = 0;
    size_t i = 0;
    bool sawBar = false;
    while (i < text.size()) {
        const char ch = text[i];
        if (std::isspace(static_cast<unsigned char>(ch)) || ch == ',') { ++i; continue; }
        if (ch == '|') {
            if (sawBar) { error = std::string(what) + ": only one '|'"; return false; }
            sawBar = true;
            out.loopFrom = static_cast<int32_t>(out.steps.size());
            ++i;
            continue;
        }
        const size_t from = i;
        if (ch == '-' || ch == '+') ++i;
        bool digits = false;
        while (i < text.size() && std::isdigit(static_cast<unsigned char>(text[i]))) { ++i; digits = true; }
        if (!digits) { error = std::string(what) + ": can't read '" + text.substr(from, 8) + "'"; return false; }
        out.steps.push_back(std::atoi(text.substr(from, i - from).c_str()));
        if (out.steps.size() > 64) { error = std::string(what) + ": more than 64 steps"; return false; }
    }
    // No bar means the table loops whole; a bar at the very end means it
    // plays once and holds.
    if (!sawBar) out.loopFrom = 0;
    return true;
}
} // namespace

std::unique_ptr<Program> compile(const std::string &formula, const std::string &arp,
                                 const std::string &duty, const std::string &vol,
                                 std::string &error) {
    auto program = std::make_unique<Program>();
    if (!Expr::parse(formula, program->formula, error)) return nullptr;
    if (!parseTable(arp, "arp", program->arp, error)) return nullptr;
    if (!parseTable(duty, "duty", program->duty, error)) return nullptr;
    if (!parseTable(vol, "volume", program->vol, error)) return nullptr;
    return program;
}

} // namespace acidulous::machine::formulate
