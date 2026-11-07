#!/usr/bin/env python3
"""Writes lib/cards.lua from the app's machine panels.

Reads shared/.../ui/MachinePanel.kt (and the lists it uses from other files),
finds each <Machine>Panel, and walks it with a small Kotlin evaluator: Group
blocks become cards, PanelKnob, PanelStepKnob, PanelSwitch, PanelOnOff,
PanelVowelKnob and Drawbars become controls, for loops and local vals and
funs are expanded. A section's cards come in section order. A param name that
depends on the selected pad is written with {pad} (or {pad+1}), which Lua fills.

Also writes lib/modcards.lua, the modifiers' cards (Scale, Chord, Arp), from
SlotsPanel.kt: its PANEL_GROUPS, switchLabels and SHORT_LABELS, with the
params of the vendored engine's InputMods.cpp.

And lib/fxcards.lua, the insert effects' cards: SlotsPanel.kt's PANEL_GROUPS
and switchLabels over the vendored engine's effect params, the reverb's
models and their knobs, and the mod card's words from InsertModCard.kt.

Usage: tools/gen-cards.py [APP_REPO] [OUT]
  APP_REPO defaults to ~/Projects/AndroidStudioProjects/Acidulous,
  OUT to lib/cards.lua next to this script.
"""
import os, re, sys

HERE = os.path.dirname(os.path.abspath(__file__))
APP = sys.argv[1] if len(sys.argv) > 1 else os.path.expanduser("~/Projects/AndroidStudioProjects/Acidulous")
OUT = sys.argv[2] if len(sys.argv) > 2 else os.path.join(HERE, "..", "lib", "cards.lua")
SRC = os.path.join(APP, "shared/src/commonMain/kotlin/com/rm/acidulous")
PANEL = os.path.join(SRC, "ui/MachinePanel.kt")
SLOTS = os.path.join(SRC, "ui/SlotsPanel.kt")
MOD_OUT = os.path.join(os.path.dirname(OUT), "modcards.lua")
INPUT_MODS = os.path.join(HERE, "..", "third_party/acidulous/engine/inputmod/InputMods.cpp")
MOD_TYPES = ("Scale", "Chord", "Arp")

CONTROLS = {"PanelKnob": "knob", "PanelStepKnob": "step", "PanelSwitch": "switch",
            "PanelOnOff": "onoff", "PanelVowelKnob": "knob", "PanelPair": "switch"}

# ---- tokens --------------------------------------------------------------------

TOKEN = re.compile(r"""
    (?P<nl>\n) | (?P<ws>[ \t\r]+) | (?P<lc>//[^\n]*) | (?P<bc>/\*.*?\*/) |
    (?P<str>"(?:\\.|\$\{(?:[^{}]|\{[^{}]*\})*\}|[^"\\])*") | (?P<chr>'(?:\\.|[^'\\])') |
    (?P<num>\d+(?:\.\d+)?[fFL]?) | (?P<id>[A-Za-z_][A-Za-z_0-9]*) |
    (?P<op>\.\.|->|::|\?\.|\?:|==|!=|<=|>=|&&|\|\||[-+*/%<>=!?.,;:(){}\[\]@&|^~$#\\])
""", re.S | re.X)


class Tok:
    __slots__ = ("k", "v")

    def __init__(self, k, v): self.k, self.v = k, v

    def __repr__(self): return f"{self.k}:{self.v}"


def tokenize(text):
    out, pos = [], 0
    while pos < len(text):
        m = TOKEN.match(text, pos)
        if not m:
            pos += 1  # anything odd, e.g. non-ASCII outside strings
            continue
        pos = m.end()
        k = m.lastgroup
        if k in ("ws", "lc", "bc"):
            if k == "bc" and "\n" in m.group():
                out.append(Tok("nl", "\n"))
            continue
        out.append(Tok(k, m.group()))
    return out


def match_close(toks, i):
    """Index of the bracket closing toks[i]."""
    pairs = {"(": ")", "{": "}", "[": "]"}
    o = toks[i].v
    c = pairs[o]
    depth = 0
    for j in range(i, len(toks)):
        if toks[j].k == "op":
            if toks[j].v in pairs: depth += 1
            elif toks[j].v in pairs.values():
                depth -= 1
                if depth == 0: return j
    raise ValueError("unbalanced " + o)


def split_args(toks, a, b):
    """Top-level comma-separated ranges in toks[a:b]."""
    args, depth, start = [], 0, a
    for j in range(a, b):
        v = toks[j].v if toks[j].k == "op" else None
        if v in ("(", "{", "["): depth += 1
        elif v in (")", "}", "]"): depth -= 1
        elif v == "," and depth == 0:
            args.append((start, j)); start = j + 1
    if any(t.k != "nl" for t in toks[start:b]): args.append((start, b))
    return args


def statement_end(toks, i, end):
    """End of the statement starting at i: a newline, ; or } at depth 0."""
    depth, j = 0, i
    while j < end:
        t = toks[j]
        if t.k == "op" and t.v in ("(", "{", "["): depth += 1
        elif t.k == "op" and t.v in (")", "}", "]"):
            if depth == 0: return j
            depth -= 1
            if depth == 0 and t.v == "}":
                # a block ends the statement unless an else or a call follows
                k = j + 1
                while k < end and toks[k].k == "nl": k += 1
                # a list joined to more on the same line, `map { } + ...`, carries on too
                if not (k < end and (toks[k].v in ("else", ".") or toks[k].v == "+" and k == j + 1)): return j + 1
        elif depth == 0 and (t.k == "nl" or t.v == ";") and j > i:
            # a line ending in an operator carries on
            prev = toks[j - 1]
            if not (prev.k == "op" and prev.v in ("+", "-", "=", ",", "(", "->", "..", ".")):
                return j
        j += 1
    return end


# ---- values ----------------------------------------------------------------------

class Fail(Exception):
    pass


class Pad:
    """The selected pad, plus an offset."""

    def __init__(self, off=0): self.off = off

    def __add__(self, n): return Pad(self.off + n)

    def __sub__(self, n): return Pad(self.off - n)

    def text(self): return "{pad}" if self.off == 0 else "{pad%+d}" % self.off


class Fun:
    def __init__(self, params, toks, a, b, env):
        self.params, self.toks, self.a, self.b, self.env = params, toks, a, b, env


NOTE_NAMES = ["C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"]


def kformat(fmt, args):
    out, k = [], 0
    for m in re.finditer(r"%(-?0?\d*)(?:\.(\d+))?([dsf%])|[^%]+", fmt):
        if not m.group(3):
            out.append(m.group()); continue
        if m.group(3) == "%":
            out.append("%"); continue
        a = args[k]; k += 1
        if isinstance(a, Pad):
            out.append(a.text())
        elif m.group(3) == "d":
            out.append(("%" + m.group(1) + "d") % int(a))
        elif m.group(3) == "f":
            out.append(("%" + m.group(1) + "." + (m.group(2) or "6") + "f") % float(a))
        else:
            out.append(str(a))
    return "".join(out)


class Evaluator:
    def __init__(self, globals_):
        self.globals = globals_  # name -> (toks, a, b) of a top-level val
        self.cache = {}

    def lookup(self, name, env):
        if name in env:
            v = env[name]
            if v is None: raise Fail(name)
            return v
        if name in self.cache: return self.cache[name]
        if name in self.globals:
            toks, a, b = self.globals[name]
            self.cache[name] = None
            v = self.eval(toks, a, b, {})
            self.cache[name] = v
            return v
        raise Fail(name)

    def string(self, raw, env):
        body = raw[1:-1]
        out, i = [], 0
        while i < len(body):
            c = body[i]
            if c == "\\" and i + 1 < len(body):
                out.append({"n": "\n", "t": "\t"}.get(body[i + 1], body[i + 1])); i += 2
            elif c == "$" and i + 1 < len(body) and body[i + 1] == "{":
                depth, j = 0, i + 1
                while True:
                    if body[j] == "{": depth += 1
                    elif body[j] == "}":
                        depth -= 1
                        if depth == 0: break
                    j += 1
                inner = tokenize(body[i + 2:j])
                out.append(self.show(self.eval(inner, 0, len(inner), env)))
                i = j + 1
            elif c == "$" and i + 1 < len(body) and (body[i + 1].isalpha() or body[i + 1] == "_"):
                m = re.match(r"[A-Za-z_][A-Za-z_0-9]*", body[i + 1:])
                out.append(self.show(self.lookup(m.group(), env)))
                i += 1 + m.end()
            else:
                out.append(c); i += 1
        return "".join(out)

    def show(self, v):
        if isinstance(v, Pad): return v.text()
        if isinstance(v, float) and v == int(v): return str(int(v))
        return str(v)

    def eval(self, toks, a, b, env):
        while a < b and toks[a].k == "nl": a += 1
        while b > a and toks[b - 1].k == "nl": b -= 1
        if a >= b: raise Fail("empty")
        if toks[a].v == "if" and toks[a + 1].v == "(":
            c = match_close(toks, a + 1)
            e = [j for j in self.top_ops(toks, c + 1, b, ("else",))]
            if not e: raise Fail("if without else")
            if test(self, toks, a + 2, c, env): return self.eval(toks, c + 1, e[0], env)
            return self.eval(toks, e[0] + 1, b, env)
        # ranges bind loosest
        for j in self.top_ops(toks, a, b, ("..", "until", "downTo")):
            lo = self.eval(toks, a, j, env)
            hi = self.eval(toks, j + 1, b, env)
            op = toks[j].v
            if op == "..": return list(range(int(lo), int(hi) + 1))
            if op == "until": return list(range(int(lo), int(hi)))
            return list(range(int(lo), int(hi) - 1, -1))
        for j in self.top_ops(toks, a, b, ("to",)):
            return (self.eval(toks, a, j, env), self.eval(toks, j + 1, b, env))
        ops = self.top_ops(toks, a, b, ("+", "-"))
        ops = [j for j in ops if j > a and not (toks[j - 1].k == "op" and toks[j - 1].v in ("(", ",", "=", "+", "-", "*"))]
        if ops:
            j = ops[-1]
            left = self.eval(toks, a, j, env)
            right = self.eval(toks, j + 1, b, env)
            if toks[j].v == "+":
                if isinstance(left, list): return left + (right if isinstance(right, list) else [right])
                if isinstance(left, str) or isinstance(right, str): return self.show(left) + self.show(right)
                return left + right
            return left - right
        if toks[a].v == "-" and toks[a].k == "op":
            return -self.eval(toks, a + 1, b, env)
        return self.postfix(toks, a, b, env)

    def top_ops(self, toks, a, b, names):
        out, depth = [], 0
        for j in range(a, b):
            t = toks[j]
            if t.k == "op" and t.v in ("(", "{", "["): depth += 1
            elif t.k == "op" and t.v in (")", "}", "]"): depth -= 1
            elif depth == 0 and t.v in names and t.k in ("op", "id"): out.append(j)
        return out

    def primary(self, toks, a, b, env):
        """Value of the primary at a, and the index after it."""
        t = toks[a]
        if t.k == "str": return self.string(t.v, env), a + 1
        if t.k == "chr": return t.v[1:-1], a + 1
        if t.k == "num":
            v = t.v.rstrip("fFL")
            return (float(v) if "." in v else int(v)), a + 1
        if t.k == "op" and t.v == "(":
            c = match_close(toks, a)
            return self.eval(toks, a + 1, c, env), c + 1
        if t.k == "id":
            name = t.v
            if a + 1 < b and toks[a + 1].v == "(":
                c = match_close(toks, a + 1)
                args = [self.eval(toks, x, y, env) for x, y in split_args(toks, a + 2, c)]
                return self.call(name, args, env), c + 1
            if name == "true": return True, a + 1
            if name == "false": return False, a + 1
            return self.lookup(name, env), a + 1
        raise Fail(t.v)

    def call(self, name, args, env):
        if name in ("listOf", "arrayOf", "mutableListOf"): return list(args)
        if name == "noteName": return NOTE_NAMES[args[0] % 12] + str(args[0] // 12 - 1)
        f = env.get(name)
        if isinstance(f, Fun):
            e = dict(f.env); e.update(zip(f.params, args))
            return self.eval(f.toks, f.a, f.b, e)
        raise Fail(name)

    def postfix(self, toks, a, b, env):
        v, j = self.primary(toks, a, b, env)
        while j < b:
            if toks[j].v not in (".", "?."): raise Fail("after " + toks[j].v)
            m = toks[j + 1].v
            k = j + 2
            args, lam = [], None
            if k < b and toks[k].v == "(":
                c = match_close(toks, k)
                args = [self.eval(toks, x, y, env) for x, y in split_args(toks, k + 1, c)]
                k = c + 1
            if k < b and toks[k].v == "{":
                c = match_close(toks, k)
                lam = (k + 1, c)
                k = c + 1
            v = self.method(v, m, args, lam, toks, env)
            j = k
        return v

    def method(self, v, m, args, lam, toks, env):
        if m == "format": return kformat(v, args)
        if m == "coerceIn": return v
        if m in ("lowercase", "toString", "trim"): return v if isinstance(v, Pad) else str(v).lower() if m == "lowercase" else str(v)
        if m == "uppercase": return str(v).upper()
        if m == "endsWith": return str(v).endswith(args[0])
        if m == "startsWith": return str(v).startswith(args[0])
        if m == "replace": return v.replace(args[0], args[1])
        if m == "size": return len(v)
        if m in ("first",): return v[0]
        if m in ("drop",): return v[args[0]:]
        if m in ("take",): return v[:args[0]]
        if m == "map" and lam:
            a, b = lam
            name = "it"
            arrow = [j for j in range(a, b) if toks[j].v == "->"]
            if arrow:
                name = toks[a].v if toks[a].k == "id" else "it"
                a = arrow[0] + 1
            out = []
            for x in v:
                e = dict(env); e[name] = x
                out.append(self.eval(toks, a, self.last_expr(toks, a, b), e))
            return out
        raise Fail("." + m)

    def last_expr(self, toks, a, b):
        return b


# ---- walking a panel -------------------------------------------------------------

class Card:
    def __init__(self, title, section):
        self.title, self.section, self.controls = title, section, []


class Walker:
    def __init__(self, ev, toks):
        self.ev, self.toks = ev, toks
        self.cards = []
        self.problems = []

    def card_for(self, ctx):
        if ctx.get("card") is None:
            c = Card(ctx.get("section_name") or "main", ctx.get("section", 0))
            self.cards.append(c)
            ctx["card"] = c
        return ctx["card"]

    def add(self, ctx, kind, name, label, opts=None):
        if isinstance(name, Pad): name = name.text()
        card = self.card_for(ctx)
        if any(c["p"] == name for c in card.controls): return
        card.controls.append({"p": name, "l": label, "k": kind, "o": opts})

    def walk(self, a, b, env, ctx):
        toks, ev = self.toks, self.ev
        i = a
        while i < b:
            t = toks[i]
            v = t.v
            if t.k == "id" and v in ("val", "var") and i + 2 < b and toks[i + 1].k == "id":
                name = toks[i + 1].v
                j = i + 2
                if toks[j].v == ":":  # a type
                    while j < b and toks[j].v != "=" and toks[j].k != "nl": j += 1
                if j < b and toks[j].v == "=":
                    e = statement_end(toks, j + 1, b)
                    if toks[j + 1].v == "remember" or toks[j + 1].v == "rememberSaveable":
                        env[name] = None
                    else:
                        try: env[name] = ev.eval(toks, j + 1, e, env)
                        except (Fail, Exception): env[name] = None
                    i = e; continue
                if j < b and toks[j].v == "by":
                    env[name] = None
                    i = statement_end(toks, j, b); continue
                i += 1; continue
            if t.k == "id" and v == "fun" and toks[i + 1].k == "id" and toks[i + 2].v == "(":
                c = match_close(toks, i + 2)
                params = [toks[x].v for x, y in split_args(toks, i + 3, c)]
                j = c + 1
                if toks[j].v == ":":
                    while toks[j].v not in ("=", "{"): j += 1
                if toks[j].v == "=":
                    e = statement_end(toks, j + 1, b)
                    env[toks[i + 1].v] = Fun(params, toks, j + 1, e, dict(env))
                    i = e; continue
                if toks[j].v == "{":  # a local helper with a body: not a card
                    i = match_close(toks, j) + 1; continue
            if t.k == "id" and v == "for" and toks[i + 1].v == "(":
                i = self.for_loop(i, b, env, ctx); continue
            if t.k == "id" and v == "forEachIndexed" and toks[i - 1].v == ".":
                i += 1; continue
            if t.k == "id" and v == "Group" and toks[i + 1].v == "(":
                c = match_close(toks, i + 1)
                args = split_args(toks, i + 2, c)
                try: title = ev.eval(toks, args[0][0], args[0][1], env)
                except Fail: title = "?"
                j = c + 1
                while toks[j].k == "nl": j += 1
                if toks[j].v != "{": i = j; continue
                e = match_close(toks, j)
                card = Card(title, ctx.get("section", 0))
                self.cards.append(card)
                inner = dict(ctx); inner["card"] = card
                self.walk(j + 1, e, dict(env), inner)
                i = e + 1; continue
            if t.k == "id" and v == "PanelSections" and toks[i + 1].v == "(":
                i = self.sections(i, b, env, ctx); continue
            if t.k == "op" and v == "{":
                # a block or lambda: its vals stay in it
                e = match_close(toks, i)
                self.walk(i + 1, e, dict(env), ctx)
                i = e + 1; continue
            if t.k == "id" and v in CONTROLS and toks[i + 1].v == "(" and not (i > 0 and toks[i - 1].v == "fun"):
                c = match_close(toks, i + 1)
                self.control(v, split_args(toks, i + 2, c), env, ctx)
                i = c + 1; continue
            if t.k == "id" and v == "Drawbars" and toks[i + 1].v == "(" and toks[i - 1].v != "fun":
                c = match_close(toks, i + 1)
                args = split_args(toks, i + 2, c)
                try:
                    prefix = ev.eval(toks, args[1][0], args[1][1], env)
                    names = ev.eval(toks, args[2][0], args[2][1], env)
                    for lab in names:
                        n = lab.replace("5⅓", "513").replace("2⅔", "223").replace("1⅗", "135").replace("1⅓", "113")
                        self.add(ctx, "knob", prefix + n, short(ascii_label(lab)))
                except Fail as f:
                    self.problems.append("drawbars: " + str(f))
                i = c + 1; continue
            i += 1

    def control(self, fn, args, env, ctx):
        toks, ev = self.toks, self.ev
        kind = CONTROLS[fn]
        pos = [r for r in args if not (toks[r[0]].k == "id" and toks[r[0] + 1].v == "=" and toks[r[0] + 1].k == "op")]
        named = {toks[r[0]].v: r for r in args if r not in pos}

        def arg(k, key=None):
            if key and key in named: r = named[key]; return ev.eval(toks, r[0] + 2, r[1], env)
            if k < len(pos): return ev.eval(toks, pos[k][0], pos[k][1], env)
            return None
        try:
            name = arg(1, "name")
        except Fail as f:
            self.problems.append(f"{fn}: name {f}")
            return
        opts = None
        label = None
        try:
            if kind in ("step", "switch"):
                try: opts = arg(2, "labels")
                except Fail: opts = None
                label = arg(3, "label")
            else:
                label = arg(2, "label")
        except Fail:
            label = None
        if not isinstance(label, str): label = None
        shown = name.text() if isinstance(name, Pad) else name
        if label is None: label = re.sub(r"^p\{pad[^}]*\}_|^p\d\d_", "", shown)
        if opts is not None:
            if not (isinstance(opts, list) and all(isinstance(o, (str, int)) for o in opts)): opts = None
            else: opts = [ascii_label(str(o)) for o in opts]
        if kind == "switch" and opts is None: kind = "step"
        self.add(ctx, kind, name, short(ascii_label(label)), opts)

    def for_loop(self, i, b, env, ctx):
        toks, ev = self.toks, self.ev
        c = match_close(toks, i + 1)
        head = i + 2
        if toks[head].v == "(":  # destructuring
            hc = match_close(toks, head)
            names = [toks[x].v for x, y in split_args(toks, head + 1, hc)]
            k = hc + 1
        else:
            names = [toks[head].v]
            k = head + 1
        assert toks[k].v == "in", toks[k]
        body = c + 1
        while toks[body].k == "nl": body += 1
        if toks[body].v == "{":
            e = match_close(toks, body)
            ba, bb, nxt = body + 1, e, e + 1
        else:
            e = statement_end(toks, body, b)
            ba, bb, nxt = body, e, e
        try:
            items = ev.eval(toks, k + 1, c, env)
        except Fail as f:
            self.problems.append("for: " + str(f))
            return nxt
        for item in items:
            e2 = dict(env)
            if len(names) == 1: e2[names[0]] = item
            else:
                for n, x in zip(names, item): e2[n] = x
            self.walk(ba, bb, e2, dict(ctx))
        return nxt

    def sections(self, i, b, env, ctx):
        toks, ev = self.toks, self.ev
        c = match_close(toks, i + 1)
        args = split_args(toks, i + 2, c)
        try: names = ev.eval(toks, args[0][0], args[0][1], env)
        except Fail: names = []
        j = c + 1
        if toks[j].v != "{": return j
        e = match_close(toks, j)
        # { sec -> when (sec) { 0 -> ...; else -> ... } }
        w = j + 1
        while w < e and toks[w].v != "when": w += 1
        if w >= e:
            self.walk(j + 1, e, env, ctx); return e + 1
        wb = w
        while toks[wb].v != "{": wb += 1
        we = match_close(toks, wb)
        branches = []
        k = wb + 1
        while k < we:
            if toks[k].k == "nl": k += 1; continue
            arrow = k
            while toks[arrow].v != "->": arrow += 1
            label = toks[k].v
            body = arrow + 1
            while toks[body].k == "nl": body += 1
            if toks[body].v == "{":
                be = match_close(toks, body)
                branches.append((label, body + 1, be))
                k = be + 1
            else:
                be = statement_end(toks, body, we)
                branches.append((label, body, be))
                k = be
        used = {int(l) for l, _, _ in branches if l.isdigit()}
        spare = [n for n in range(len(names)) if n not in used]
        for label, ba, bb in branches:
            sec = int(label) if label.isdigit() else (spare[0] if spare else len(names))
            inner = dict(ctx); inner["section"] = sec; inner["card"] = None
            inner["section_name"] = names[sec] if sec < len(names) else "more"
            self.walk(ba, bb, dict(env), inner)
        return e + 1


# Short forms for labels over 6 letters, the most a knob's column fits.
SHORT = {
    "sustain": "sus", "release": "rel", "velocity": "vel", "transpose": "transp", "position": "pos",
    "density": "dense", "pressure": "press", "stretch": "strtch", "hardness": "hard",
    "stiffness": "stiff", "reverse": "rev", "pattern": "patt", "feedback": "fdbk", "threshold": "thresh",
    "damping": "damp", "vibrato": "vib", "formant": "formnt", "tension": "tens", "strings": "string",
    "scatter": "scattr", "how many": "count", "glide on": "glide", "by key": "by key", "odd/even": "odd/ev",
    "whistle": "whistl", "whisper": "whispr", "unvoiced": "unvoic", "una corda": "corda",
    "trigger": "trig", "tremolo": "trem", "tracker": "track", "tonebar": "tonbar", "sympathy": "sympth",
    "surface": "surfce", "pitch env": "p env", "balance": "bal", "spread": "spread", "brightness": "bright",
    "envelope": "env", "harmonics": "harm", "frequency": "freq", "resonance": "reso", "amount": "amount",
    "attack": "attack", "detune": "detune", "octave": "octave", "shimmer": "shimmr", "voices": "voices",
    "humanise": "human", "coupling": "couple", "listens": "listen", "bendtime": "b time",
    "up the series": "series", "breath": "breath", "character": "char", "material": "matrl",
    "duration": "length", "distance": "dist", "variation": "vary", "sensitivity": "sens",
}


def short(s):
    """s in at most 6 letters."""
    if len(s) <= 6: return s
    if s in SHORT: return SHORT[s]
    words = s.split()
    if len(words) > 1:
        t = words[0][0] + " " + " ".join(words[1:])
        if len(t) <= 6: return t
    # drop vowels from the end, keeping the first letter
    t = list(s)
    for k in range(len(t) - 1, 0, -1):
        if len(t) <= 6: break
        if t[k] in "aeiou" and t[k - 1] != " ": del t[k]
    return "".join(t)[:6].strip()


def ascii_label(s):
    s = s.split("~")[0]
    s = re.sub(r"^·\s*", "", s)
    s = (s.replace("→", ">").replace("·", "-").replace("×", "x").replace("⅓", " 1/3")
          .replace("⅔", " 2/3").replace("⅗", " 3/5").replace("ä", "a").replace("ö", "o")
          .replace("é", "e").replace("♯", "#").replace("♭", "b").replace("∿", "~"))
    s = s.encode("ascii", "replace").decode().replace("?", "")
    return s.lower()


# ---- conditions ------------------------------------------------------------------

def test(ev, toks, a, b, env):
    """A Kotlin condition in toks[a:b]: ||, &&, ==, !=, <, >, <=, >=, !, brackets."""
    while a < b and toks[a].k == "nl": a += 1
    while b > a and toks[b - 1].k == "nl": b -= 1
    for ops in (("||",), ("&&",)):
        js = ev.top_ops(toks, a, b, ops)
        if js:
            parts, last = [], a
            for j in js + [b]:
                parts.append((last, j)); last = j + 1
            vals = (test(ev, toks, x, y, env) for x, y in parts)
            return any(vals) if ops == ("||",) else all(vals)
    for op in ("==", "!=", "<=", ">=", "<", ">"):
        js = ev.top_ops(toks, a, b, (op,))
        if js:
            j = js[0]
            l, r = ev.eval(toks, a, j, env), ev.eval(toks, j + 1, b, env)
            return {"==": l == r, "!=": l != r, "<=": l <= r, ">=": l >= r, "<": l < r, ">": l > r}[op]
    if toks[a].v == "!": return not test(ev, toks, a + 1, b, env)
    if toks[a].v == "(" and match_close(toks, a) == b - 1: return test(ev, toks, a + 1, b - 1, env)
    if toks[a].k == "id" and toks[a].v in ("true", "false") and a + 1 == b: return toks[a].v == "true"
    if toks[a].k == "id" and toks[a].v == "else": return True
    if any(t.v == "." for t in toks[a:b]):
        v = ev.eval(toks, a, b, env)
        if isinstance(v, bool): return v
    raise Fail("condition " + " ".join(t.v for t in toks[a:b]))


# ---- modifier cards ----------------------------------------------------------------

def block_after(text, head):
    """The text inside the brackets that open right after `head`."""
    i = text.index(head) + len(head)
    while text[i] not in "({": i += 1
    toks = tokenize(text[i:])
    return toks, 1, match_close(toks, 0)


def engine_params(src, type_):
    """The modifier's param names in engine order, from InputMods.cpp."""
    i = src.index("const ParamDef *%s::paramDefs" % type_)
    body = src[i:src.index("count = Count;", i)]
    names = []
    for m in re.finditer(r'\{"(\w+)",\s*-?[\d.]+f?,|stepNames\[\d+\]\s*=\s*\{([^}]*)\}', body):
        if m.group(1): names.append(m.group(1))
        else:
            # the steps come where the loop over stepNames is
            names.append(("*steps", re.findall(r'"(\w+)"', m.group(2))))
    out = []
    for n in names:
        if isinstance(n, tuple): continue
        out.append(n)
    steps = [n for n in names if isinstance(n, tuple)]
    if steps:
        # stepNames is declared before the loop that adds them, after "length"
        at = out.index("length") + 1 if "length" in out else len(out)
        out[at:at] = steps[0][1]
    return out


def switch_labels(ev, toks, a, b):
    """switchLabels's branches as [(cond range, expr range)]."""
    i = a
    while toks[i].v != "when": i += 1
    i += 1
    while toks[i].v != "{": i += 1
    end = match_close(toks, i)
    arrows = []
    depth = 0
    for j in range(i + 1, end):
        t = toks[j]
        if t.k == "op" and t.v in ("(", "{", "["): depth += 1
        elif t.k == "op" and t.v in (")", "}", "]"): depth -= 1
        elif depth == 0 and t.v == "->": arrows.append(j)
    out = []
    for n, j in enumerate(arrows):
        k = j - 1
        while k > i and toks[k].k != "nl": k -= 1
        start = k + 1
        if n + 1 < len(arrows):
            nk = arrows[n + 1] - 1
            while toks[nk].k != "nl": nk -= 1
            stop = nk
        else:
            stop = end
        out.append(((start, j), (j + 1, stop)))
    return out


def mod_cards(ev):
    text = open(SLOTS, encoding="utf-8").read()
    src = open(INPUT_MODS, encoding="utf-8").read()
    toks = tokenize(text)
    # PANEL_GROUPS: type -> [(title, [names])]
    groups = {}
    for m in re.finditer(r'"(\w+)" to listOf\((.*?)\n    \)', text[text.index("private val PANEL_GROUPS"):], re.S):
        if m.group(1) not in MOD_TYPES or m.group(1) in groups: continue
        groups[m.group(1)] = [(t, re.findall(r'"(\w+)"', names))
                              for t, names in re.findall(r'"([\w ]+)" to listOf\(([^)]*)\)', m.group(2))]
    st, sa, sb = block_after(text, "private val SHORT_LABELS")
    shorts = dict(re.findall(r'"(\w+)" to "([^"]+)"', "".join(t.v + " " for t in st[sa:sb])))
    hidden = re.search(r'CHORD_MEMORY_NOTE = Regex\("([^"]+)"\)', text).group(1)
    # switchLabels's body
    fi = next(i for i, t in enumerate(toks) if t.v == "switchLabels" and toks[i - 1].v == "fun")
    branches = switch_labels(ev, toks, fi, len(toks))
    problems = []

    def labels(type_, name, steps):
        env = {"type": type_, "name": name, "steps": steps}
        for (ca, cb), (ea, eb) in branches:
            try:
                if not test(ev, toks, ca, cb, env): continue
            except Fail as e:
                problems.append(f"{type_}.{name}: {e}"); continue
            if toks[ea].v == "null": return None
            try:
                return [str(x) for x in ev.eval(toks, ea, eb, env)]
            except Fail as e:
                problems.append(f"{type_}.{name}: {e}"); return None
        return None

    # name -> steps for the stepped params (0 when the count isn't a plain number)
    stepped = {m.group(1): int(m.group(3)) if m.group(3).isdigit() else 0
               for m in re.finditer(r'\{"(\w+)",[^{}]*?Curve::(Stepped),\s*([^,]+?),', src)}

    out = {}
    for type_ in MOD_TYPES:
        names = engine_params(src, type_)
        steps_p = [n for n in names if type_ == "Arp" and re.fullmatch(r"s\d\d", n)]
        shown = [n for n in names if n not in steps_p and not (type_ == "Chord" and re.fullmatch(hidden, n))]

        def control(n):
            # as the app: a stepped param with words for its steps is a switch
            # of up to 4, or a knob that names its step
            o = labels(type_, n, stepped[n]) if n in stepped else None
            if o: o = [x.replace("↑", " up").replace("↓", " dn") for x in o]
            kind = "knob"
            if o == ["off", "on"]: kind = "onoff"
            elif o and len(o) <= 4: kind = "switch"
            elif o: kind = "step"
            lab = short(ascii_label(shorts.get(n, n)))
            return {"p": n, "l": lab, "k": kind, "o": [ascii_label(x) if kind != "onoff" else x for x in o] if o else None}

        cards, taken = [], set()
        for title, ns in groups.get(type_, []):
            here = [n for n in ns if n in shown]
            if not here: continue
            taken.update(here)
            cards.append((title, [control(n) for n in here]))
        rest = [n for n in shown if n not in taken]
        if rest: cards.append(("more", [control(n) for n in rest]))
        if steps_p:
            cards.append(("steps", [{"p": n, "l": str(int(n[1:])), "k": "onoff", "o": None} for n in steps_p]))
        out[type_] = [((t if type_.lower() in t else type_.lower() + " " + t), c) for t, c in cards]
    return out, problems


def write_mod_cards(ev):
    cards, problems = mod_cards(ev)
    lines = [
        "-- Generated by tools/gen-cards.py from the app's SlotsPanel.kt; don't edit.",
        "-- modifier -> list of cards {t = title, c = controls}, each control",
        "-- {p = param, l = label, k = knob|step|switch|onoff, o = option labels}.",
        "return {",
    ]
    for type_ in MOD_TYPES:
        lines.append(f"  {type_} = {{")
        for title, ctl in cards[type_]:
            lines.append(f"    {{t = {lua_str(title)}, c = {{")
            for x in ctl:
                s = f"{{p = {lua_str(x['p'])}, l = {lua_str(x['l'])}, k = {lua_str(x['k'])}"
                if x["o"]: s += ", o = {" + ", ".join(lua_str(o) for o in x["o"]) + "}"
                lines.append("      " + s + "},")
            lines.append("    }},")
        lines.append("  },")
    lines.append("}")
    with open(MOD_OUT, "w") as f: f.write("\n".join(lines) + "\n")
    print(f"{len(cards)} modifiers, {sum(len(v) for v in cards.values())} cards -> {os.path.relpath(MOD_OUT)}")
    for p in problems: print("  " + p)


# ---- effect cards ------------------------------------------------------------------

EFFECTS_DIR = os.path.join(HERE, "..", "third_party/acidulous/engine/effect")
CARD_KT = os.path.join(SRC, "ui/InsertModCard.kt")
FX_OUT = os.path.join(os.path.dirname(OUT), "fxcards.lua")


def effect_defs():
    """effect name -> [(name, min, max, default, curve, steps, unit)] from the
    vendored engine's paramDefs, in engine order, by EffectRegistry's names."""
    srcs = {}
    for root, _, files in os.walk(EFFECTS_DIR):
        for f in files:
            if f.endswith((".cpp", ".h")):
                srcs[os.path.join(root, f)] = open(os.path.join(root, f), encoding="utf-8").read()
    every = "\n".join(srcs.values())
    consts = {m.group(1): int(m.group(2)) for m in re.finditer(r"constexpr int(?:32_t)? (\w+)\s*=\s*(\d+);", every)}
    for m in re.finditer(r"enum \w+ \{([^}]*)\}", every):
        names = [n.strip().split("=")[0].strip() for n in m.group(1).split(",") if n.strip()]
        if names and names[-1].startswith("k") and all("=" not in n for n in m.group(1).split(",")):
            consts[names[-1]] = len(names) - 1

    def num(v):
        v = v.strip().rstrip("fF")
        try: return float(v)
        except ValueError: return float(consts.get(v, 0))

    by_class = {}
    for path, text in srcs.items():
        # constants of the effect's own .h and .cpp come first: kModes is in several
        stem = os.path.splitext(path)[0]
        own = {}
        for ext in (".h", ".cpp"):
            for m in re.finditer(r"constexpr int(?:32_t)? (\w+)\s*=\s*(\d+)\s*[,;]", srcs.get(stem + ext, "")):
                own[m.group(1)] = int(m.group(2))
        for m in re.finditer(r"const ParamDef \*(\w+)::paramDefs\(int32_t &count\) const \{", text):
            body = text[m.end():text.index("count =", m.end())]
            defs = []
            for d in re.finditer(r'\{"(\w+)",\s*([^,]+),\s*([^,]+),\s*([^,]+),\s*Curve::(\w+),\s*([^,]+),\s*"([^"]*)"\}', body):
                n, lo, hi, de, curve, steps, unit = d.groups()
                steps = steps.strip()
                st = int(steps) if steps.isdigit() else own.get(steps, consts.get(steps, 0))
                defs.append((n, num(lo), num(hi), num(de), curve, st, unit))
            by_class[m.group(1)] = defs
    reg = open(os.path.join(EFFECTS_DIR, "EffectRegistry.cpp"), encoding="utf-8").read()
    out = {}
    for name, cls in re.findall(r'\{"(\w+)", make<effect::(\w+)>\}', reg):
        out[name] = by_class.get(cls, [])
    return out


def fx_cards(ev):
    text = open(SLOTS, encoding="utf-8").read()
    toks = tokenize(text)
    defs = effect_defs()
    groups = {}
    for m in re.finditer(r'"(\w+)" to listOf\((.*?)\n    \)', text[text.index("private val PANEL_GROUPS"):], re.S):
        if m.group(1) not in defs or m.group(1) in groups: continue
        groups[m.group(1)] = [(t, re.findall(r'"(\w+)"', names))
                              for t, names in re.findall(r'"([\w ]+)" to listOf\(([^)]*)\)', m.group(2))]
    fi = next(i for i, t in enumerate(toks) if t.v == "switchLabels" and toks[i - 1].v == "fun")
    branches = switch_labels(ev, toks, fi, len(toks))
    problems = []

    def labels(type_, name, steps):
        env = {"type": type_, "name": name, "steps": steps}
        for (ca, cb), (ea, eb) in branches:
            try:
                if not test(ev, toks, ca, cb, env): continue
            except Fail as e:
                problems.append(f"{type_}.{name}: {e}"); continue
            if toks[ea].v == "null": return None
            try:
                return [str(x) for x in ev.eval(toks, ea, eb, env)]
            except Fail as e:
                problems.append(f"{type_}.{name}: {e}"); return None
        return None

    def words(o):
        return [ascii_label(x.replace("↑", " up").replace("↓", " dn")) for x in o]

    effects = {}
    for type_, ds in defs.items():
        if not ds: problems.append(f"{type_}: no params found")
        opts = {}
        for d in ds:
            if d[4] != "Stepped": continue
            o = labels(type_, d[0], d[5])
            # a list for another type's param of the same name has the wrong count
            if o and d[5] > 0 and len(o) != d[5]: o = None
            if o and o != ["off", "on"]: opts[d[0]] = words(o)
        effects[type_] = {"g": groups.get(type_, []), "o": opts}
    models = [ascii_label(x) for x in ev.lookup("REVERB_MODELS", {})]
    knobs_text = text[text.index("private val REVERB_MODEL_KNOBS"):]
    knobs_text = knobs_text[:knobs_text.index("\n)")]
    knobs = [(m, re.findall(r'"(\w+)"', ns)) for m, ns in re.findall(r'"(\w+)" to listOf\(([^)]*)\)', knobs_text)]
    card = open(CARD_KT, encoding="utf-8").read()
    def kt_list(src, name):
        m = re.search(name + r" = listOf\(([^)]*)\)", src)
        return words(re.findall(r'"([^"]*)"', m.group(1)))
    mod = {"sources": kt_list(card, "SOURCE_WORDS"), "shapes": kt_list(card, "SHAPE_WORDS"),
           "rates": [x.lower() for x in kt_list(text, "internal val NOTE_RATES")]}
    sends = {t: defs[t] for t in ("Reverb", "Delay")}
    return effects, models, knobs, mod, sends, problems


def write_fx_cards(ev):
    effects, models, knobs, mod, sends, problems = fx_cards(ev)
    L = lua_str
    lines = [
        "-- Generated by tools/gen-cards.py from the app's SlotsPanel.kt and InsertModCard.kt",
        "-- and the vendored engine's effects; don't edit.",
        "return {",
        "  -- effect -> {g = cards {title, param names} as the app groups them (none: engine",
        "  -- order), o = stepped param -> option labels}",
        "  effects = {",
    ]
    for type_ in effects:
        e = effects[type_]
        lines.append(f"    {type_} = {{")
        if e["g"]:
            lines.append("      g = {")
            for t, ns in e["g"]:
                lines.append(f"        {{{L(t)}, {{" + ", ".join(L(n) for n in ns) + "}},")
            lines.append("      },")
        if e["o"]:
            lines.append("      o = {")
            for n, o in e["o"].items():
                lines.append(f"        {n} = {{" + ", ".join(L(x) for x in o) + "},")
            lines.append("      },")
        lines.append("    },")
    lines.append("  },")
    lines.append("  -- the effects in the engine's order")
    lines.append("  order = {" + ", ".join(L(t) for t in effects) + "},")
    lines.append("  -- the reverb's models, in the engine's order, and each one's own knobs")
    lines.append("  reverb_models = {" + ", ".join(L(m) for m in models) + "},")
    lines.append("  reverb_knobs = {")
    for m, ns in knobs:
        lines.append(f"    {m} = {{" + ", ".join(L(n) for n in ns) + "},")
    lines.append("  },")
    lines.append("  -- an insert's mod: its sources, LFO shapes and rates")
    lines.append("  mod = {")
    for k in ("sources", "shapes", "rates"):
        lines.append(f"    {k} = {{" + ", ".join(L(x) for x in mod[k]) + "},")
    lines.append("  },")
    lines.append("  -- the send effects' params: {name, min, max, default, curve, steps, unit}")
    lines.append("  sends = {")
    for t, ds in sends.items():
        lines.append(f"    {t} = {{")
        for n, lo, hi, de, curve, st, unit in ds:
            lines.append(f"      {{{L(n)}, {lo:g}, {hi:g}, {de:g}, {L(curve.lower())}, {st}, {L(unit)}}},")
        lines.append("    },")
    lines.append("  },")
    lines.append("}")
    with open(FX_OUT, "w") as f: f.write("\n".join(lines) + "\n")
    print(f"{len(effects)} effects -> {os.path.relpath(FX_OUT)}")
    for p in problems: print("  " + p)


# ---- the file ----------------------------------------------------------------------

def top_vals(toks, into):
    depth = 0
    for i, t in enumerate(toks):
        if t.k == "op" and t.v in ("{", "(", "["): depth += 1
        elif t.k == "op" and t.v in ("}", ")", "]"): depth -= 1
        elif depth == 0 and t.v == "val" and toks[i + 1].k == "id":
            j = i + 2
            if toks[j].v == ":":
                while toks[j].v != "=" and toks[j].k != "nl": j += 1
            if toks[j].v != "=": continue
            into.setdefault(toks[i + 1].v, (toks, j + 1, statement_end(toks, j + 1, len(toks))))


def panels(toks):
    """name -> (start, end) of each private fun <Name>Panel body."""
    out = {}
    for i, t in enumerate(toks):
        if t.v == "fun" and toks[i + 1].k == "id" and toks[i + 1].v.endswith("Panel") and toks[i + 2].v == "(":
            name = toks[i + 1].v[:-5]
            if not name or not name[0].isupper(): continue
            c = match_close(toks, i + 2)
            j = c + 1
            while toks[j].v != "{": j += 1
            params = []
            for x, y in split_args(toks, i + 3, c):
                while toks[x].k == "nl": x += 1
                params.append(toks[x].v)
            out[name] = (j + 1, match_close(toks, j), params)
    return out


ENV_TITLES = ("envelope", "amp env", "amp")
NOT_AMP = re.compile(r"^(op \d+|eg \d+|env \d+|filter env)$")
ENV_LOOSE = re.compile(r"att|atk|dec|rel")
ENV_NAMES = re.compile(r"^(amp)?(attack|atk|decay|dec|release|rel)$|^a_(attack|decay|release)$")


def amp_env_first(cards):
    """Moves the amp envelope card to second place and returns the cards; the
    card is marked env. Titles first, then the first card with an envelope param."""
    def has(c, rx): return any(rx.search(x["p"]) for x in c.controls)
    pick = None
    for c in cards:
        if str(c.title) in ENV_TITLES and has(c, ENV_LOOSE): pick = c; break
    if pick is None:
        for c in cards:
            t = str(c.title)
            if not NOT_AMP.match(t) and has(c, ENV_NAMES): pick = c; break
    if pick is None: return cards
    pick.env = True
    rest = [c for c in cards if c is not pick]
    if cards[0] is pick: return cards
    return rest[:1] + [pick] + rest[1:]


def lua_str(s):
    return '"' + s.replace("\\", "\\\\").replace('"', '\\"') + '"'


def main():
    text = open(PANEL, encoding="utf-8").read()
    toks = tokenize(text)
    globals_ = {}
    top_vals(toks, globals_)
    for root, _, files in os.walk(SRC):
        for f in sorted(files):
            p = os.path.join(root, f)
            if f.endswith(".kt") and p != PANEL:
                t = open(p, encoding="utf-8").read()
                if re.search(r"^(internal |private )?val [A-Z_]+ = (listOf|\d)", t, re.M):
                    top_vals(tokenize(t), globals_)
    ev = Evaluator(globals_)
    # The machines MachinePanel dispatches to
    dispatch = dict(re.findall(r'^\s*"(\w+)" -> (\w+)Panel\(', text, re.M))
    found = panels(toks)
    out, problems = {}, []
    for machine, fn in sorted(dispatch.items()):
        if fn not in found:
            problems.append(f"{machine}: no {fn}Panel"); continue
        a, b, params = found[fn]
        env = {p.split(":")[0].strip(): None for p in params}
        env["pad"] = Pad()
        w = Walker(ev, toks)
        try:
            w.walk(a, b, env, {"section": 0})
        except Exception as e:  # anything the evaluator can't follow
            problems.append(f"{machine}: {type(e).__name__} {e}")
            continue
        cards = [c for c in sorted(w.cards, key=lambda c: c.section) if c.controls]
        for p in w.problems: problems.append(f"{machine}: {p}")
        if cards: out[machine] = amp_env_first(cards)
        else: problems.append(f"{machine}: no cards")
    lines = [
        "-- Generated by tools/gen-cards.py from the app's MachinePanel.kt; don't edit.",
        "-- machine -> list of cards {t = title, e = true on the amp envelope, c = controls}, each control",
        "-- {p = param, l = label, k = knob|step|switch|onoff, o = option labels}.",
        "-- {pad} in a param name is the selected pad as two digits, {pad+1} one more.",
        "return {",
    ]
    for machine in sorted(out):
        lines.append(f"  {machine} = {{")
        for c in out[machine]:
            ctl = []
            for x in c.controls:
                s = f"{{p = {lua_str(x['p'])}, l = {lua_str(x['l'])}, k = {lua_str(x['k'])}"
                if x["o"]: s += ", o = {" + ", ".join(lua_str(o) for o in x["o"]) + "}"
                ctl.append(s + "}")
            e = ", e = true" if getattr(c, "env", False) else ""
            lines.append(f"    {{t = {lua_str(ascii_label(str(c.title)))}{e}, c = {{")
            for s in ctl: lines.append("      " + s + ",")
            lines.append("    }},")
        lines.append("  },")
    lines.append("}")
    with open(OUT, "w") as f: f.write("\n".join(lines) + "\n")
    print(f"{len(out)} machines, {sum(len(v) for v in out.values())} cards -> {os.path.relpath(OUT)}")
    for p in problems: print("  " + p)
    write_mod_cards(ev)
    write_fx_cards(ev)


if __name__ == "__main__":
    if any(a in ("-h", "--help") for a in sys.argv[1:]):
        print(__doc__.strip())
        sys.exit(0)
    if not os.path.isfile(PANEL):
        sys.exit("no app repo at %s (no %s)\n\n%s" % (APP, os.path.relpath(PANEL, APP), __doc__.strip()))
    main()
