#pragma once
// Replays engine lines (engine.txt): the engine calls the app made to build a
// song, as text. The format is described in EngineLines.kt.
//
// Each line becomes the same Java_com_rm_acidulous_engine_EngineNative_* call
// the app makes, through the browser build's stand-in JNI
// (platform/web/jni.h), so the host needs jni_bridge.cpp, EngineHost and the
// engine, and no JVM. The per-call code is generated from EngineNative's list
// into lines_dispatch.cpp.

#include <jni.h>

#include <cstdint>
#include <functional>
#include <initializer_list>
#include <map>
#include <string>
#include <vector>

namespace acidulous::lines {

/** What a replay needs and what it found. */
struct Replay {
    /** The JNI stand-in passed to every call. */
    JNIEnv *env = nullptr;
    /** Where `zip:` paths are found: the unpacked zip. */
    std::string root;
    /** The handles made so far (SnapshotBegin), which `@n` picks from. */
    std::vector<jlong> handles;
    /** Called after every line with its number, e.g. to let the audio thread drain the queues. */
    std::function<void(int)> afterLine;
    /** Where problems go: a line that doesn't parse, a load that failed. Stderr if unset. */
    std::function<void(const std::string &)> log;
    /**
     * Where to put what the lines mount and set, or -1 to leave them as
     * written. A preset's lines are for rack 0 and effect slot 0 (see the
     * `# target` line); these replay them onto another rack and slot.
     */
    int targetRack = -1;
    int targetSlot = -1;
    jint rack(jint written) const { return targetRack >= 0 ? targetRack : written; }
    jint slot(jint written) const { return targetSlot >= 0 ? targetSlot : written; }
    /** A unit name, "effect1" and so on, moved to the target slot. */
    jstring unit(jstring written) const;
    int calls = 0;
    int skipped = 0;
    /** Calls that returned false, which is normal for some (SnapshotSetClipCached). */
    int refused = 0;

    /** A call returned an error message, or "" for none. */
    void said(const char *call, jstring error);
    void answered(const char *call, bool ok);
    void problem(const std::string &what);
    int line = 0;

    /** The file's version, from its first line. */
    int version = 3;
    /**
     * An older line on today's counts: Trinity's destinations rescaled, and
     * for version 1 its waves and Nexus's wtosc tables too
     * (engine/machine/TableCounts.h). Tracks what each rack holds, as
     * written, to know which parameters those are.
     */
    std::string upgradeLine(const std::string &line);
    /** Notes the wtosc slots of a LoadNexusPatch line; run over a version 1 file before replaying it. */
    void noteTables(const std::string &line);
    std::map<int, std::string> machineOf;
    /** Per rack, a bit for each Nexus slot holding a wtosc in any of the file's patches. */
    std::map<int, uint32_t> tableSlotsOf;
};

/** Reads one line's arguments, in order. A bad argument sets !ok and reads as zero. */
class Reader {
  public:
    /** The most items an array may say it has. */
    static constexpr int kMaxArray = 1 << 24;

    Reader(const std::string &text, size_t at, Replay &r) : s(text), i(at), r(r) {}
    jint i32();
    jlong i64();
    jfloat f32();
    jdouble f64();
    jboolean flag();
    /** A quoted string, or null, in modified UTF-8 as the JVM hands strings to JNI. */
    jstring str();
    /** A string with `zip:` resolved against the root. */
    jstring path();
    /** Lines of `|` fields, with `zip:` resolved in the fields numbered in [paths]. */
    jstring spec(std::initializer_list<int> paths);
    /** `@n`, the n-th handle made. */
    jlong handle();
    jfloatArray floats();
    jintArray ints();
    jlongArray longs();
    jobjectArray strings();
    /** True if every argument read and nothing is left. */
    bool finish();

  private:
    const std::string &s;
    size_t i;
    Replay &r;
    bool ok = true;
    std::string word();
    bool quoted(std::string &out);
    /** Reads `k[n:` and returns n, or -1 for null or a mismatch. */
    int arrayStart(char kind);
    /** Reads the word null, if that's what's next. */
    bool null();
    bool arrayEnd();
    void skipSpace();
    std::string resolve(const std::string &p) const;
};

/** One recorded call: its name in the file and what replays it. */
struct Call {
    const char *name;
    bool (*run)(Reader &, Replay &);
};
extern const Call kCalls[];
extern const int kCallCount;

/**
 * Replays [text], a whole engine.txt. Returns false if it isn't engine lines
 * (wrong first line). A line that fails is reported and skipped.
 */
bool replay(const std::string &text, Replay &r);

} // namespace acidulous::lines
