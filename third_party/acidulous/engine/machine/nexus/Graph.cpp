#include "Graph.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <sstream>

namespace acidulous::machine::nexus {

namespace {
/** Split "a|b|c" into fields. */
std::vector<std::string> split(const std::string &line, char sep) {
    std::vector<std::string> out;
    size_t start = 0;
    while (true) {
        const size_t at = line.find(sep, start);
        out.push_back(line.substr(start, at == std::string::npos ? std::string::npos : at - start));
        if (at == std::string::npos) break;
        start = at + 1;
    }
    return out;
}
float toFloat(const std::string &s, float fallback = 0.0f) {
    try { return std::stof(s); } catch (...) { return fallback; }
}
int32_t toInt(const std::string &s, int32_t fallback = -1) {
    try { return static_cast<int32_t>(std::stol(s)); } catch (...) { return fallback; }
}
/** "07.2" -> slot 7, port 2. */
bool parseJack(const std::string &s, int32_t &slot, int32_t &port) {
    const size_t dot = s.find('.');
    if (dot == std::string::npos) return false;
    slot = toInt(s.substr(0, dot));
    port = toInt(s.substr(dot + 1));
    return slot >= 0 && port >= 0 && port < kPorts;
}
/** A whole number with an optional sign and nothing else, as the editor reads one. */
bool wholeInt(const std::string &s, int32_t &out) {
    size_t i = 0;
    if (!s.empty() && (s[0] == '+' || s[0] == '-')) i = 1;
    if (i >= s.size()) return false;
    for (size_t k = i; k < s.size(); ++k) if (s[k] < '0' || s[k] > '9') return false;
    try { out = static_cast<int32_t>(std::stol(s)); } catch (...) { return false; }
    return true;
}
/** A number that is the whole field, else [fallback], as the editor reads a depth. */
float wholeFloat(const std::string &s, float fallback) {
    try {
        size_t used = 0;
        const float v = std::stof(s, &used);
        return used == s.size() ? v : fallback;
    } catch (...) { return fallback; }
}
/** A jack as the editor keeps it: "slot.port", both whole numbers; anything after a second point is ignored. */
bool jackShape(const std::string &s, int32_t &slot, int32_t &port) {
    const size_t dot = s.find('.');
    if (dot == std::string::npos) return false;
    const size_t end = s.find('.', dot + 1);
    return wholeInt(s.substr(0, dot), slot) &&
           wholeInt(s.substr(dot + 1, end == std::string::npos ? std::string::npos : end - dot - 1), port);
}
} // namespace

Graph *Graph::parse(const std::string &text, float sampleRate, std::string &error) {
    auto *g = new Graph();
    g->sampleRate = sampleRate;
    int32_t version = 1;
    std::vector<std::string> lines;
    {
        std::istringstream stream(text);
        std::string line;
        while (std::getline(stream, line)) lines.push_back(line);
    }

    // Pass one: the modules, so a cable can name a slot that appears later.
    std::vector<int32_t> slotToNode(kSlots, -1);
    // A module's text: `e|07|t*(t>>5)`. Everything after the slot is the text,
    // bars and all, since a formula can have `|` in it.
    std::vector<std::string> textOf(kSlots);
    for (const auto &line : lines) {
        if (line.size() < 2 || line[0] != 'e' || line[1] != '|') continue;
        const size_t bar = line.find('|', 2);
        if (bar == std::string::npos) continue;
        const int32_t slot = toInt(line.substr(2, bar - 2));
        if (slot >= 0 && slot < kSlots) textOf[static_cast<size_t>(slot)] = line.substr(bar + 1);
    }
    for (const auto &line : lines) {
        if (line.empty() || line[0] == '#') continue;
        const auto f = split(line, '|');
        if (f[0] == "v" && f.size() >= 2) {
            // 2: wtosc's table knob spans 32 tables, not 8. The
            // knob is a parameter, so the app rescales it when it reads a 1.
            version = toInt(f[1], 1);
            if (version > 2) {
                error = "this patch was written by a newer version";
                delete g;
                return nullptr;
            }
        } else if (f[0] == "m" && f.size() >= 3) {
            const int32_t slot = toInt(f[1]);
            if (slot < 0 || slot >= kSlots || slotToNode[static_cast<size_t>(slot)] >= 0) continue;
            Node n;
            n.slot = slot;
            n.type = typeFromName(f[2].c_str());
            if (n.type < 0) {
                // A module this build doesn't have. Keep the slot, its knobs
                // and its cables and warn, so re-saving doesn't lose it.
                n.type = TBlank;
                n.unknown = true;
                g->warn = "unknown module \"" + f[2] + "\"";
            }
            const ModuleInfo &info = infoFor(n.type);
            n.text = textOf[static_cast<size_t>(slot)];
            n.perNote = n.type == TVoice || n.type == TTouch;
            n.poly = f.size() >= 4 ? (f[3] == "poly") : true;
            if (!n.poly && (info.cap & CapMono) == 0) n.poly = true;
            if (n.poly && (info.cap & CapPoly) == 0) n.poly = false;
            for (int i = 0; i < kPorts; ++i) {
                if (info.in[i] != nullptr) n.inCount = i + 1;
                if (info.out[i] != nullptr) n.outCount = i + 1;
            }
            slotToNode[static_cast<size_t>(slot)] = static_cast<int32_t>(g->nodes.size());
            g->nodes.push_back(std::move(n));
        }
    }
    if (g->nodes.empty()) {
        error = "the patch has no modules";
        delete g;
        return nullptr;
    }

    // Ports, and the instances that write them.
    for (auto &n : g->nodes) {
        n.outBase = g->totalPorts;
        g->totalPorts += n.outCount;
        const int32_t copies = n.poly ? kVoices : 1;
        for (int32_t i = 0; i < copies; ++i) {
            n.inst.emplace_back(makeModule(n.unknown ? TBlank : n.type));
            std::string why;
            if (!n.inst.back()->setText(n.text, why) && i == 0) {
                g->warn = std::string(infoFor(n.type).name) + " in slot " + std::to_string(n.slot) + ": " + why;
            }
            n.inst.back()->prepare(sampleRate, kVoices);
        }
        if (n.type == TOut && g->outNode < 0) g->outNode = static_cast<int32_t>(&n - g->nodes.data());
        if (n.type == TScope && g->scopeNode < 0) g->scopeNode = static_cast<int32_t>(&n - g->nodes.data());
    }
    g->port.assign(static_cast<size_t>(kVoices + 1) * static_cast<size_t>(g->totalPorts), 0.0f);

    // Pass two: cables.
    int32_t cableIndex = 0;
    for (const auto &line : lines) {
        const auto f = split(line, '|');
        // A cable line is one the editor keeps: `c`, then two jacks that read
        // as slot.port. Those are numbered by their place, the ones wired to
        // nothing too, so the editor's cable list and the cNN parameters agree.
        if (f.size() < 3 || f[0] != "c") continue;
        int32_t fromSlot = 0, fromPort = 0, toSlot = 0, toPort = 0;
        if (!jackShape(f[1], fromSlot, fromPort) || !jackShape(f[2], toSlot, toPort)) continue;
        const int32_t index = cableIndex++;
        auto wired = [&](int32_t slot, int32_t port) {
            return slot >= 0 && slot < kSlots && port >= 0 && port < kPorts && slotToNode[static_cast<size_t>(slot)] >= 0;
        };
        if (!wired(fromSlot, fromPort) || !wired(toSlot, toPort)) continue;
        Cable c;
        c.srcNode = slotToNode[static_cast<size_t>(fromSlot)];
        c.srcPort = fromPort;
        c.dstNode = slotToNode[static_cast<size_t>(toSlot)];
        c.dstPort = toPort;
        // A missing depth is 1 and a missing B is the A, as the editor reads them.
        c.depthA = f.size() >= 4 ? wholeFloat(f[3], 1.0f) : 1.0f;
        c.depthB = f.size() >= 5 ? wholeFloat(f[4], c.depthA) : c.depthA;
        int32_t slot = 0, p = 0;
        if (f.size() >= 6 && parseJack(f[5], slot, p) && slot < kSlots && slotToNode[static_cast<size_t>(slot)] >= 0) {
            c.modNode = slotToNode[static_cast<size_t>(slot)];
            c.modPort = p;
        }
        if (f.size() >= 7) c.modAmount = toFloat(f[6], 0.0f);
        // The first kCables cables get a pair of depth parameters, so they
        // can be changed and automated without rebuilding the graph.
        c.paramIndex = index < kCables ? index : -1;
        g->cables.push_back(c);
    }

    // Layout. The audio thread never reads it but the editor needs it.
    for (const auto &line : lines) {
        if (line.empty() || line[0] != 'p') continue;
        const auto f = split(line, '|');
        if (f.size() < 4) continue;
        const int32_t slot = toInt(f[1]);
        if (slot < 0 || slot >= kSlots || slotToNode[static_cast<size_t>(slot)] < 0) continue;
        Node &n = g->nodes[static_cast<size_t>(slotToNode[static_cast<size_t>(slot)])];
        n.x = toFloat(f[2]);
        n.y = toFloat(f[3]);
    }

    // Order the nodes so everything is computed before it's read (Kahn's
    // algorithm). Whatever is left over is in a cycle, and the cables that
    // close those cycles read the previous sample instead.
    const int32_t count = static_cast<int32_t>(g->nodes.size());
    std::vector<int32_t> indegree(static_cast<size_t>(count), 0);
    for (const auto &c : g->cables) {
        if (c.srcNode != c.dstNode) ++indegree[static_cast<size_t>(c.dstNode)];
    }
    std::vector<int32_t> queue;
    for (int32_t i = 0; i < count; ++i) if (indegree[static_cast<size_t>(i)] == 0) queue.push_back(i);
    std::vector<bool> placed(static_cast<size_t>(count), false);
    while (!queue.empty()) {
        const int32_t n = queue.front();
        queue.erase(queue.begin());
        g->order.push_back(n);
        placed[static_cast<size_t>(n)] = true;
        for (const auto &c : g->cables) {
            if (c.srcNode != n || c.dstNode == n) continue;
            if (--indegree[static_cast<size_t>(c.dstNode)] == 0) queue.push_back(c.dstNode);
        }
    }
    for (int32_t i = 0; i < count; ++i) if (!placed[static_cast<size_t>(i)]) g->order.push_back(i);

    std::vector<int32_t> position(static_cast<size_t>(count), 0);
    for (size_t i = 0; i < g->order.size(); ++i) position[static_cast<size_t>(g->order[i])] = static_cast<int32_t>(i);
    for (auto &c : g->cables) {
        // A cable is delayed only if its source is computed after its
        // destination, which is exactly the cables that close a loop.
        c.delayed = c.srcNode == c.dstNode ||
                    position[static_cast<size_t>(c.srcNode)] >= position[static_cast<size_t>(c.dstNode)];
        if (c.delayed) {
            const int32_t portIndex = g->nodes[static_cast<size_t>(c.srcNode)].outBase + c.srcPort;
            auto found = std::find(g->delayedPorts.begin(), g->delayedPorts.end(), portIndex);
            if (found == g->delayedPorts.end()) {
                c.prevSlot = static_cast<int32_t>(g->delayedPorts.size());
                g->delayedPorts.push_back(portIndex);
            } else {
                c.prevSlot = static_cast<int32_t>(found - g->delayedPorts.begin());
            }
        }
    }
    g->prev.assign(static_cast<size_t>(kVoices + 1) * (g->delayedPorts.size() + 1), 0.0f);

    // A poly control block (math, quant, env and the like) fed only by one
    // note's controls and by mono sources puts out one note's control too,
    // like a quantised pitch or an envelope on the note's gate, so a mono
    // module follows its lead voice as well. Anything with a poly source
    // that isn't per note, like an oscillator, is still summed. Walked in
    // order so a chain of such blocks carries it through.
    for (const int32_t ni : g->order) {
        Node &n = g->nodes[static_cast<size_t>(ni)];
        if (!n.poly || n.perNote) continue;
        switch (n.type) {
            case TMath: case TQuant: case TEnv: case TSlew: case TSnh:
            case TLogic: case TProb: case TRand: break;
            default: continue;
        }
        bool fromNote = false, mixed = false;
        for (const auto &c : g->cables) {
            if (c.dstNode != ni) continue;
            for (const int32_t s : {c.srcNode, c.modNode}) {
                if (s < 0) continue;
                const Node &src = g->nodes[static_cast<size_t>(s)];
                if (src.perNote) fromNote = true;
                else if (src.poly) mixed = true;
            }
        }
        n.perNote = fromNote && !mixed;
    }

    // Group cables by destination so the inner loop walks a contiguous span.
    std::sort(g->cables.begin(), g->cables.end(),
              [](const Cable &a, const Cable &b) { return a.dstNode < b.dstNode; });
    for (int32_t i = 0; i < count; ++i) {
        Node &n = g->nodes[static_cast<size_t>(i)];
        n.cableFirst = 0;
        n.cableCount = 0;
        for (size_t c = 0; c < g->cables.size(); ++c) {
            if (g->cables[c].dstNode != i) continue;
            if (n.cableCount == 0) n.cableFirst = static_cast<int32_t>(c);
            ++n.cableCount;
            const int32_t dp = g->cables[c].dstPort;
            if (dp >= 0 && dp < 32) n.inMask |= 1u << dp;
        }
    }
    return g;
}

void Graph::adoptFrom(Graph &old) {
    // A slot that still holds the same kind of module keeps its instance, so
    // adding a cable doesn't cut off ringing strings and delay tails. Pointer
    // moves only.
    for (auto &n : nodes) {
        for (auto &o : old.nodes) {
            // A changed text is a different module: a new formula starts clean.
            if (o.slot != n.slot || o.type != n.type || o.poly != n.poly || o.text != n.text) continue;
            if (o.inst.size() != n.inst.size()) continue;
            for (size_t i = 0; i < n.inst.size(); ++i) n.inst[i] = std::move(o.inst[i]);
            break;
        }
    }
    // Anything the old graph couldn't supply starts clean.
    for (auto &n : nodes) {
        for (auto &i : n.inst) {
            if (i) continue;
            i.reset(makeModule(n.unknown ? TBlank : n.type));
            std::string why;
            i->setText(n.text, why);
            i->prepare(sampleRate, kVoices);
        }
    }
}

void Graph::restartClocks() {
    for (auto &n : nodes) for (auto &i : n.inst) if (i) i->restart();
}

void Graph::reset() {
    for (auto &n : nodes) for (auto &i : n.inst) if (i) i->reset();
    for (auto &v : port) v = 0.0f;
    for (auto &v : prev) v = 0.0f;
    for (auto &v : slotLevel) v = 0.0f;
    for (auto &v : cableLevel) v = 0.0f;
}

/**
 * The editor meters, read every kMeterEvery samples to keep the cost on the
 * audio thread tiny. That's still far more often than the screen redraws.
 */
void Graph::meter(const int32_t *active, int32_t activeCount) {
    for (auto &v : slotLevel) v *= kMeterDecay;
    for (auto &v : cableLevel) v *= kMeterDecay;

    const auto peakAt = [&](int32_t node, int32_t portIndex) {
        float peak = 0.0f;
        if (nodes[static_cast<size_t>(node)].poly) {
            for (int32_t a = 0; a < activeCount; ++a) {
                peak = std::fmax(peak, std::fabs(readPort(node, portIndex, active[a])));
            }
        } else {
            peak = std::fabs(readPort(node, portIndex, kVoices));
        }
        return peak;
    };

    for (size_t i = 0; i < nodes.size(); ++i) {
        const Node &n = nodes[i];
        if (n.slot < 0 || n.slot >= kSlots) continue;
        float peak = 0.0f;
        for (int32_t p = 0; p < n.outCount; ++p) {
            peak = std::fmax(peak, peakAt(static_cast<int32_t>(i), p));
        }
        float &slot = slotLevel[n.slot];
        slot = std::fmax(slot, peak);
    }
    for (const auto &c : cables) {
        if (c.paramIndex < 0 || c.srcNode < 0) continue;
        float &lvl = cableLevel[c.paramIndex];
        lvl = std::fmax(lvl, peakAt(c.srcNode, c.srcPort));
    }
}

int32_t Graph::activity(float *dest, int32_t max) const {
    int32_t n = 0;
    for (int32_t i = 0; i < kSlots && n < max; ++i) dest[n++] = slotLevel[i];
    for (int32_t i = 0; i < kCables && n < max; ++i) dest[n++] = cableLevel[i];
    return n;
}

void Graph::applyKnobs(const float *slotKnobs) {
    for (auto &n : nodes) {
        const float *k = slotKnobs + static_cast<size_t>(n.slot) * kKnobs;
        for (auto &i : n.inst) if (i) { i->setKnobs(k); i->setConnected(n.inMask); }
    }
}

void Graph::defaultKnobs(float *out) const {
    for (int32_t i = 0; i < kSlots * kKnobs; ++i) out[i] = 0.0f;
    for (const auto &n : nodes) {
        const ModuleInfo &info = infoFor(n.type);
        float *k = out + static_cast<size_t>(n.slot) * kKnobs;
        for (int32_t i = 0; i < kKnobs; ++i) k[i] = info.def[i];
    }
}

void Graph::applyCableDepths(const float *cableParams, float morph, int32_t frames) {
    for (auto &c : cables) {
        const float a = c.paramIndex >= 0 ? cableParams[c.paramIndex * 2] : c.depthA;
        const float b = c.paramIndex >= 0 ? cableParams[c.paramIndex * 2 + 1] : c.depthB;
        const float want = a + (b - a) * morph;
        c.base = c.baseLast;
        c.baseInc = frames > 0 ? (want - c.baseLast) / static_cast<float>(frames) : 0.0f;
        c.baseLast = want;
    }
}

void Graph::advanceCables() {
    for (auto &c : cables) c.base += c.baseInc;
}

float Graph::readPort(int32_t node, int32_t p, int32_t voice) const {
    const Node &n = nodes[static_cast<size_t>(node)];
    const int32_t row = n.poly ? voice : kVoices;
    return port[static_cast<size_t>(row) * static_cast<size_t>(totalPorts) +
                static_cast<size_t>(n.outBase + p)];
}

void Graph::gather(const Node &n, int32_t nodeIndex, int32_t voice, const int32_t *active, int32_t activeCount,
                   float *in) const {
    for (int32_t p = 0; p < kPorts; ++p) in[p] = 0.0f;
    const bool monoDest = !n.poly;
    for (int32_t c = n.cableFirst; c < n.cableFirst + n.cableCount; ++c) {
        const Cable &k = cables[static_cast<size_t>(c)];
        if (k.dstNode != nodeIndex) continue;
        const Node &src = nodes[static_cast<size_t>(k.srcNode)];

        float x = 0.0f;
        if (src.poly && monoDest && src.perNote) {
            // One note's controls aren't summed: a pitch or gate summed over
            // voices is no note at all. The mono module follows the lead voice.
            if (leadVoice >= 0) {
                x = k.delayed ? prev[static_cast<size_t>(leadVoice) * (delayedPorts.size() + 1) +
                                     static_cast<size_t>(k.prevSlot)]
                              : readPort(k.srcNode, k.srcPort, leadVoice);
            }
        } else if (k.delayed) {
            const int32_t row = src.poly ? (monoDest ? -1 : voice) : kVoices;
            if (row < 0) {
                for (int32_t a = 0; a < activeCount; ++a) {
                    x += prev[static_cast<size_t>(active[a]) * (delayedPorts.size() + 1) +
                              static_cast<size_t>(k.prevSlot)] * levelOf(active[a]);
                }
            } else {
                x = prev[static_cast<size_t>(row) * (delayedPorts.size() + 1) + static_cast<size_t>(k.prevSlot)];
            }
        } else if (src.poly && monoDest) {
            // A poly source feeding a mono module sums across the voices.
            for (int32_t a = 0; a < activeCount; ++a) x += readPort(k.srcNode, k.srcPort, active[a]) * levelOf(active[a]);
        } else {
            x = readPort(k.srcNode, k.srcPort, voice);
        }

        // The cable is a VCA: its depth can be modulated by any other output.
        float depth = k.base;
        if (k.modNode >= 0) {
            const Node &m = nodes[static_cast<size_t>(k.modNode)];
            float mv = 0.0f;
            if (!m.poly || !monoDest) mv = readPort(k.modNode, k.modPort, voice);
            else if (m.perNote) mv = leadVoice >= 0 ? readPort(k.modNode, k.modPort, leadVoice) : 0.0f;
            else mv = readPort(k.modNode, k.modPort, active[0]);
            depth += mv * k.modAmount;
        }
        in[k.dstPort] += x * depth;
    }
}

void Graph::step(Context &ctx, const int32_t *active, int32_t activeCount, float &outL, float &outR) {
    float in[kPorts];
    for (int32_t oi = 0; oi < static_cast<int32_t>(order.size()); ++oi) {
        const int32_t index = order[static_cast<size_t>(oi)];
        Node &n = nodes[static_cast<size_t>(index)];
        if (n.poly) {
            for (int32_t a = 0; a < activeCount; ++a) {
                const int32_t v = active[a];
                ctx.voice = v;
                gather(n, index, v, active, activeCount, in);
                Module *m = n.inst[static_cast<size_t>(v)].get();
                if (m != nullptr) {
                    m->step(in, &port[static_cast<size_t>(v) * static_cast<size_t>(totalPorts) +
                                      static_cast<size_t>(n.outBase)], ctx);
                }
            }
        } else {
            ctx.voice = -1;
            gather(n, index, kVoices, active, activeCount, in);
            Module *m = n.inst[0].get();
            if (m != nullptr) {
                m->step(in, &port[static_cast<size_t>(kVoices) * static_cast<size_t>(totalPorts) +
                                  static_cast<size_t>(n.outBase)], ctx);
            }
        }
    }

    outL = outR = 0.0f;
    if (outNode >= 0) {
        const Node &o = nodes[static_cast<size_t>(outNode)];
        if (o.poly) {
            for (int32_t a = 0; a < activeCount; ++a) {
                outL += readPort(outNode, 0, active[a]) * levelOf(active[a]);
                outR += readPort(outNode, 1, active[a]) * levelOf(active[a]);
            }
        } else {
            outL = readPort(outNode, 0, kVoices);
            outR = readPort(outNode, 1, kVoices);
        }
    }

    if (--meterCountdown <= 0) {
        meterCountdown = kMeterEvery;
        meter(active, activeCount);
    }

    // Only the ports a feedback cable reads are remembered.
    const size_t stride = delayedPorts.size() + 1;
    for (size_t d = 0; d < delayedPorts.size(); ++d) {
        const int32_t pi = delayedPorts[d];
        for (int32_t row = 0; row <= kVoices; ++row) {
            prev[static_cast<size_t>(row) * stride + d] =
                port[static_cast<size_t>(row) * static_cast<size_t>(totalPorts) + static_cast<size_t>(pi)];
        }
    }
}

int32_t Graph::scopePoints(float *dest, int32_t max) const {
    if (scopeNode < 0) return 0;
    const Node &n = nodes[static_cast<size_t>(scopeNode)];
    if (n.inst.empty() || !n.inst[0]) return 0;
    const auto *s = static_cast<const ScopeMod *>(n.inst[0].get());
    return s->copyTo(dest, max);
}

void Graph::setInputCursor(int32_t frame) {
    for (auto &n : nodes) {
        if (n.type != TAudioIn) continue;
        for (auto &i : n.inst) if (i) static_cast<AudioInMod *>(i.get())->setCursor(frame);
    }
}

} // namespace acidulous::machine::nexus
