#pragma once
#include <cstdint>
#include <engine/machine/nexus/Modules.h>
#include <memory>
#include <string>
#include <vector>

// A built patch. Parsed, allocated and ordered on a worker thread, then
// handed to the audio thread as one object, like Mosaic's sample map. After
// that only the audio thread touches it.
//
// Per-voice module state lives here and not on the voice, because the graph
// is replaced whenever a cable is added, and a voice would be left holding a
// dangling pointer.
namespace acidulous::machine::nexus {

struct Node {
    int32_t slot = -1;
    int32_t type = TBlank;
    bool poly = true;
    bool unknown = false;       // a type this build doesn't have, kept as a placeholder
    int32_t outBase = 0;        // where this node's outputs start in the port row
    int32_t inCount = 0, outCount = 0;
    int32_t cableFirst = 0, cableCount = 0; // cables arriving here, contiguous
    // Which input ports have a cable in them, one bit each. Only the output
    // block uses it, to mirror the left input to the right when the right
    // isn't wired.
    uint32_t inMask = 0;
    float x = 0.0f, y = 0.0f;   // canvas position; audio ignores it
    std::string text;           // what an `e|` line gives the module, like a formula; empty for most
    std::vector<std::unique_ptr<Module>> inst; // one, or one per voice
};

struct Cable {
    int32_t srcNode = -1, srcPort = 0;
    int32_t dstNode = -1, dstPort = 0;
    int32_t modNode = -1, modPort = 0;
    float modAmount = 0.0f;
    int32_t paramIndex = -1;    // which cNN_a/b pair owns its depth, or -1
    float depthA = 1.0f, depthB = 1.0f; // used when it owns no parameter
    bool delayed = false;       // closes a loop, so it reads the previous sample
    int32_t prevSlot = -1;      // where its source is kept in prev[]
    // per block
    float base = 0.0f, baseInc = 0.0f, baseLast = 0.0f;
};

class Graph {
  public:
    /**
     * Each voice's level from its velocity, or null for all at full. Applied
     * where voices are summed (into a mono module or the output), so it works
     * in any patch without a cable.
     */
    const float *voiceLevel = nullptr;
    /** Worker thread. Returns null and fills `error` if the text is unusable. */
    static Graph *parse(const std::string &text, float sampleRate, std::string &error);

    ~Graph() = default;

    /** Audio thread, at hand-over: carry over state from the old graph. */
    void adoptFrom(Graph &old);
    void reset();

    int32_t nodeCount() const { return static_cast<int32_t>(nodes.size()); }
    const Node &nodeAt(int32_t i) const { return nodes[static_cast<size_t>(i)]; }

    /** One sample for every node, in order. Returns the stereo sink. */
    void step(Context &ctx, const int32_t *activeVoices, int32_t activeCount, float &outL, float &outR);

    void applyKnobs(const float *slotKnobs);
    /**
     * The default knob values for each occupied slot, from the module table.
     * Slot knobs are machine parameters that default to zero, so a graph
     * built from text alone needs these or its oscillators would be silent.
     * Empty slots are left at zero.
     *
     * [out] is kSlots * kKnobs floats. A loaded patch keeps its saved values,
     * a graph parsed from a bank uses these, so the caller decides.
     */
    void defaultKnobs(float *out) const;
    void applyCableDepths(const float *cableParams, float morph, int32_t frames);
    void advanceCables();
    int32_t scopePoints(float *dest, int32_t max) const;

    /**
     * Activity levels per slot and per cable, for the editor to draw.
     *
     * [dest] gets kSlots slot levels followed by kCables cable levels, so
     * the layout doesn't depend on the patch. Slots are indexed by slot
     * number and cables by their order in the patch text (`paramIndex`),
     * which isn't the order they're stored in here since they're sorted by
     * destination.
     *
     * Levels are decaying peaks so audio-rate signals glow steadily instead
     * of flickering.
     */
    int32_t activity(float *dest, int32_t max) const;
    void setInputCursor(int32_t frame);

    const std::string &warning() const { return warn; }

  private:
    float levelOf(int32_t voice) const { return voiceLevel != nullptr ? voiceLevel[voice] : 1.0f; }

    void meter(const int32_t *active, int32_t activeCount);
    float readPort(int32_t node, int32_t port, int32_t voice) const;
    void gather(const Node &n, int32_t nodeIndex, int32_t voice, const int32_t *active, int32_t activeCount,
                float *in) const;

    std::vector<Node> nodes;
    std::vector<Cable> cables;
    std::vector<int32_t> order;      // node indices, topological
    std::vector<float> port;         // (kVoices + 1) rows x totalPorts; row kVoices is mono
    std::vector<float> prev;         // (kVoices + 1) rows x delayedPorts.size()
    std::vector<int32_t> delayedPorts;
    float sampleRate = 48000.0f;
    int32_t totalPorts = 0;
    int32_t outNode = -1;
    int32_t scopeNode = -1;
    // Display only, written by the audio thread. Read over JNI without a
    // lock, since a torn float only makes one cable the wrong brightness for
    // a frame.
    float slotLevel[kSlots] = {};
    float cableLevel[kCables] = {};
    int32_t meterCountdown = 0;
    std::string warn;
};

} // namespace acidulous::machine::nexus
