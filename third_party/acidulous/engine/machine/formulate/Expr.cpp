#include "Expr.h"
#include <cctype>
#include <cmath>
#include <cstring>

namespace acidulous::machine::formulate {

namespace {

// A sine table with 256 steps to the cycle, values -127..127.
struct SineTable {
    int8_t v[256];
    SineTable() {
        for (int i = 0; i < 256; ++i) v[i] = static_cast<int8_t>(std::lround(127.0 * std::sin(2.0 * M_PI * i / 256.0)));
    }
};
const SineTable kSine;

struct Token {
    enum Kind { End, Number, Ident, Punct } kind = End;
    int32_t number = 0;
    std::string text;
};

} // namespace

/** A precedence climbing parser over the C operator table. */
class Parser {
  public:
    Parser(const std::string &src, Expr &out) : s(src), e(out) {}

    bool run(std::string &error) {
        next();
        if (!expr(0)) { error = err; return false; }
        if (tok.kind != Token::End) { error = "unexpected '" + tok.text + "'"; return false; }
        if (e.ops.empty()) { error = "nothing to evaluate"; return false; }
        return true;
    }

  private:
    using Op = Expr::Op;

    void emit(Op op, int32_t arg = 0) { e.ops.push_back({op, arg}); }

    void next() {
        while (pos < s.size() && std::isspace(static_cast<unsigned char>(s[pos]))) ++pos;
        tok = Token{};
        if (pos >= s.size()) return;
        const char ch = s[pos];
        if (std::isdigit(static_cast<unsigned char>(ch))) {
            int32_t value = 0;
            if (ch == '0' && pos + 1 < s.size() && (s[pos + 1] == 'x' || s[pos + 1] == 'X')) {
                pos += 2;
                while (pos < s.size() && std::isxdigit(static_cast<unsigned char>(s[pos]))) {
                    const char h = s[pos++];
                    value = value * 16 + (std::isdigit(static_cast<unsigned char>(h)) ? h - '0'
                                                                                      : (std::tolower(h) - 'a' + 10));
                }
            } else {
                while (pos < s.size() && std::isdigit(static_cast<unsigned char>(s[pos]))) value = value * 10 + (s[pos++] - '0');
            }
            tok.kind = Token::Number;
            tok.number = value;
            return;
        }
        if (std::isalpha(static_cast<unsigned char>(ch)) || ch == '_') {
            const size_t from = pos;
            while (pos < s.size() && (std::isalnum(static_cast<unsigned char>(s[pos])) || s[pos] == '_')) ++pos;
            tok.kind = Token::Ident;
            tok.text = s.substr(from, pos - from);
            return;
        }
        // Two-character operators first, or "a >> b" becomes "a > (> b)".
        static const char *kTwo[] = {"<<", ">>", "<=", ">=", "==", "!=", "&&", "||"};
        for (const char *two : kTwo) {
            if (s.compare(pos, 2, two) == 0) {
                tok.kind = Token::Punct;
                tok.text = two;
                pos += 2;
                return;
            }
        }
        tok.kind = Token::Punct;
        tok.text = std::string(1, ch);
        ++pos;
    }

    bool is(const char *p) const { return tok.kind == Token::Punct && tok.text == p; }

    static int precedenceOf(const std::string &op) {
        if (op == "*" || op == "/" || op == "%") return 10;
        if (op == "+" || op == "-") return 9;
        if (op == "<<" || op == ">>") return 8;
        if (op == "<" || op == ">" || op == "<=" || op == ">=") return 7;
        if (op == "==" || op == "!=") return 6;
        if (op == "&") return 5;
        if (op == "^") return 4;
        if (op == "|") return 3;
        if (op == "&&") return 2;
        if (op == "||") return 1;
        return -1;
    }

    static Op opcodeOf(const std::string &op) {
        if (op == "*") return Op::Mul;
        if (op == "/") return Op::Div;
        if (op == "%") return Op::Mod;
        if (op == "+") return Op::Add;
        if (op == "-") return Op::Sub;
        if (op == "<<") return Op::Shl;
        if (op == ">>") return Op::Shr;
        if (op == "<") return Op::Lt;
        if (op == ">") return Op::Gt;
        if (op == "<=") return Op::Le;
        if (op == ">=") return Op::Ge;
        if (op == "==") return Op::Eq;
        if (op == "!=") return Op::Ne;
        if (op == "&") return Op::And;
        if (op == "^") return Op::Xor;
        if (op == "|") return Op::Or;
        if (op == "&&") return Op::AndAnd;
        return Op::OrOr;
    }

    bool expr(int minPrecedence) {
        if (!unary()) return false;
        for (;;) {
            if (tok.kind != Token::Punct) break;
            // The ternary sits below every binary operator.
            if (is("?") && minPrecedence <= 0) {
                next();
                if (!expr(0)) return false;
                if (!is(":")) { err = "expected ':' in ?:"; return false; }
                next();
                if (!expr(0)) return false;
                emit(Op::Sel);
                continue;
            }
            const int p = precedenceOf(tok.text);
            if (p < 0 || p < minPrecedence || minPrecedence == 0) {
                if (p < 0 || p < minPrecedence) break;
            }
            const std::string op = tok.text;
            next();
            if (!expr(p + 1)) return false;
            emit(opcodeOf(op));
        }
        return true;
    }

    bool unary() {
        if (is("-")) { next(); if (!unary()) return false; emit(Op::Neg); return true; }
        if (is("+")) { next(); return unary(); }
        if (is("~")) { next(); if (!unary()) return false; emit(Op::BitNot); return true; }
        if (is("!")) { next(); if (!unary()) return false; emit(Op::Not); return true; }
        return primary();
    }

    bool primary() {
        if (tok.kind == Token::Number) { emit(Op::Push, tok.number); next(); return true; }
        if (is("(")) {
            next();
            if (!expr(0)) return false;
            if (!is(")")) { err = "expected ')'"; return false; }
            next();
            return true;
        }
        if (tok.kind == Token::Ident) {
            const std::string name = tok.text;
            next();
            if (is("(")) return call(name);
            static const struct { const char *name; Op op; } kVars[] = {
                {"t", Op::VarT}, {"f", Op::VarF}, {"n", Op::VarN}, {"v", Op::VarV}, {"x", Op::VarX},
                {"a", Op::VarA}, {"b", Op::VarB}, {"c", Op::VarC}, {"s", Op::VarS}, {"r", Op::VarR},
                {"sr", Op::VarSr},
            };
            for (const auto &var : kVars) {
                if (name == var.name) { emit(var.op); return true; }
            }
            err = "no such value '" + name + "'";
            return false;
        }
        err = tok.kind == Token::End ? "the expression stops early" : "unexpected '" + tok.text + "'";
        return false;
    }

    bool call(const std::string &name) {
        next(); // (
        int args = 0;
        if (!is(")")) {
            for (;;) {
                if (!expr(0)) return false;
                ++args;
                if (is(",")) { next(); continue; }
                break;
            }
        }
        if (!is(")")) { err = "expected ')' after " + name + "()"; return false; }
        next();
        auto need = [&](int want) {
            if (args == want) return true;
            err = name + "() takes " + std::to_string(want) + " argument" + (want == 1 ? "" : "s");
            return false;
        };
        if (name == "sin") { if (!need(1)) return false; emit(Op::Sin); return true; }
        if (name == "abs") { if (!need(1)) return false; emit(Op::Abs); return true; }
        if (name == "min") { if (!need(2)) return false; emit(Op::Min); return true; }
        if (name == "max") { if (!need(2)) return false; emit(Op::Max); return true; }
        if (name == "rnd") {
            if (args != 0) { err = "rnd() takes nothing"; return false; }
            emit(Op::VarR);
            return true;
        }
        err = "no such function '" + name + "'";
        return false;
    }

    const std::string &s;
    Expr &e;
    size_t pos = 0;
    Token tok;
    std::string err;
};

bool Expr::parse(const std::string &source, Expr &out, std::string &error) {
    out.ops.clear();
    out.text = source;
    // An empty formula is allowed and just means there's no formula.
    bool anything = false;
    for (char ch : source) if (!std::isspace(static_cast<unsigned char>(ch))) anything = true;
    if (!anything) return true;
    Parser parser(source, out);
    if (!parser.run(error)) { out.ops.clear(); return false; }
    return true;
}

int32_t Expr::eval(const Vars &vars) const {
    int32_t stack[32];
    int sp = 0;
    auto push = [&](int32_t value) { if (sp < 32) stack[sp++] = value; };
    auto pop = [&]() { return sp > 0 ? stack[--sp] : 0; };
    for (const Code &code : ops) {
        switch (code.op) {
        case Op::Push: push(code.arg); break;
        case Op::VarT: push(vars.t); break;
        case Op::VarF: push(vars.f); break;
        case Op::VarN: push(vars.n); break;
        case Op::VarV: push(vars.v); break;
        case Op::VarX: push(vars.x); break;
        case Op::VarA: push(vars.a); break;
        case Op::VarB: push(vars.b); break;
        case Op::VarC: push(vars.c); break;
        case Op::VarS: push(vars.s); break;
        case Op::VarR: push(vars.r); break;
        case Op::VarSr: push(vars.sr); break;
        case Op::Neg: push(-pop()); break;
        case Op::Not: push(pop() == 0 ? 1 : 0); break;
        case Op::BitNot: push(~pop()); break;
        case Op::Sin: push(kSine.v[static_cast<uint8_t>(pop() & 0xff)]); break;
        case Op::Abs: { const int32_t x = pop(); push(x < 0 ? -x : x); break; }
        case Op::Sel: { const int32_t no = pop(), yes = pop(), cond = pop(); push(cond != 0 ? yes : no); break; }
        default: {
            const int32_t rhs = pop(), lhs = pop();
            switch (code.op) {
            case Op::Add: push(lhs + rhs); break;
            case Op::Sub: push(lhs - rhs); break;
            case Op::Mul: push(lhs * rhs); break;
            case Op::Div: push(rhs == 0 ? 0 : lhs / rhs); break;
            case Op::Mod: push(rhs == 0 ? 0 : lhs % rhs); break;
            case Op::And: push(lhs & rhs); break;
            case Op::Or: push(lhs | rhs); break;
            case Op::Xor: push(lhs ^ rhs); break;
            // Shifts outside 0..31 are clamped so "t >> 99" gives zero instead
            // of undefined behaviour.
            case Op::Shl: push(rhs < 0 || rhs > 31 ? 0 : static_cast<int32_t>(static_cast<uint32_t>(lhs) << rhs)); break;
            case Op::Shr: push(rhs < 0 || rhs > 31 ? (lhs < 0 ? -1 : 0) : lhs >> rhs); break;
            case Op::Lt: push(lhs < rhs ? 1 : 0); break;
            case Op::Gt: push(lhs > rhs ? 1 : 0); break;
            case Op::Le: push(lhs <= rhs ? 1 : 0); break;
            case Op::Ge: push(lhs >= rhs ? 1 : 0); break;
            case Op::Eq: push(lhs == rhs ? 1 : 0); break;
            case Op::Ne: push(lhs != rhs ? 1 : 0); break;
            case Op::AndAnd: push(lhs != 0 && rhs != 0 ? 1 : 0); break;
            case Op::OrOr: push(lhs != 0 || rhs != 0 ? 1 : 0); break;
            case Op::Min: push(lhs < rhs ? lhs : rhs); break;
            case Op::Max: push(lhs > rhs ? lhs : rhs); break;
            default: break;
            }
            break;
        }
        }
    }
    return sp > 0 ? stack[sp - 1] : 0;
}

} // namespace acidulous::machine::formulate
