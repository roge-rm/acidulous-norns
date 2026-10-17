#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include <engine/core/PatchChain.h>

// Preset files (.acpreset) for a host that drives the engine directly, so a
// patch saved there imports into the app and the app's load here.
//
// The format is the app's (PresetFile.kt, where the zip is described): a zip
// with preset.json, patches/<n>.json and, from the app, media/ and engine/.
// This reads the manifest and the patches and skips the rest. It writes
// stored entries and no media, so file settings stay the paths they were.
//
// A patch's JSON is the app's Patch (PatchStore.kt) with its inserts as
// UnitSlot (Song.kt). Both sides are checked against the same fixture files
// (tools/preset_file_test.cpp and PresetFixtureTest.kt), so keep them alike.
//
// No dependencies: the zip reader inflates on its own.
namespace acidulous {

/**
 * RateMigration.PATCH_VERSION in the app. A patch below it has LFO rates or
 * wavetable numbers the app moves on load and this doesn't, so check
 * [PresetPatch::version] before using one.
 */
constexpr int32_t kPatchVersion = 3;

struct PresetPatch {
    std::string machine;                                         // the machine's type, or the effect's
    std::string name;
    std::vector<std::pair<std::string, float>> params;           // normalised 0..1, by name
    std::vector<std::pair<std::string, std::string>> settings;   // words and documents, like "nexus" or "macro1"
    int32_t low = -1, high = -1;                                 // the note range it's played in, -1 for none
    std::string family;                                          // the bank's group, empty for the user's own
    int32_t version = kPatchVersion;
    PatchChain chain;                                            // inserts and sends
};

struct Preset {
    std::string kind = "machine"; // or "effect"
    std::string unit;             // the machine's or effect's type
    std::vector<PresetPatch> patches;
};

namespace presetfile {

/** [preset] as .acpreset bytes. Each patch's machine is written as the app keeps it ("fx.Chorus" for an effect). */
std::vector<uint8_t> write(const Preset &preset);

/**
 * Reads .acpreset bytes, every patch in it. Turns down what the app would:
 * no manifest, a newer manifest, a bad unit name or a patch missing.
 * Each patch's machine is the manifest's unit. False with [error] set.
 */
bool read(const uint8_t *data, size_t size, Preset &out, std::string &error);

/** A zip's entries by name, inflated. For tests; [error] says what went wrong. */
bool unzip(const uint8_t *data, size_t size, std::vector<std::pair<std::string, std::vector<uint8_t>>> &out,
           std::string &error);

} // namespace presetfile
} // namespace acidulous
