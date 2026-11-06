// Saves parts as .acpreset files and loads them back, with the core's worker
// thread and a fake audio thread, as the plugin runs:
// - a Trinity patch with an edit, its macro names and insert, read back with
//   EngineHost::readPreset and loaded over another patch
// - a save straight after a /patch, while its insert mount is still queued
// - the app's version 3 fixture (host/tests/acpreset), a patch too old to load
//   and Trinity destinations moved on a version 3 patch
// - a Filament's sympathetic level and depths aimed at it scaled on a
//   version 4 patch, kept on 5
// - a Delay insert's pingpong moved to mode on a version 5 patch, kept on 6
// - the part's own inserts in slots 2-3 (/insert), insert mods through /fx
//   and their read-back in /inserts, and a saved patch keeping only slots
//   0-1 with their mods
// - Dice's sample and Mosaic's folder packed and found again, a labelled
//   path kept, and a missing file that leaves the rest loaded
// - the app's packed media/ files unpacked (Forage pad from hit.acpreset,
//   Mosaic zones and SoundFont), kept on a second load, a ../ name refused
//   and over 64 MB refused, the patch still loading, a small file from a
//   preset with over 64 MB of others
// - Forage's slice_sample from presets, loaded and saved packed
// - Diction's recorded voices from presets, loaded, saved packed as the app
//   packs them and loaded again
// - every machine's files (Forage pads and slices, Dice, Pollen and Molt
//   samples, Mosaic's SoundFont and zones) packed as media/<path>, two of one
//   name made distinct, loaded back the same, saved over, past 64 MB a path
// - the app's machine presets loaded as their engine lines load them, and
//   its effect presets into a part's own slot as theirs load them
// - effect presets: the slot's mod kept, another type switching the slot,
//   an old pingpong, refusals, a save round trip and the deep scan
// - /preset_scan and /preset_delete
// Usage: acidulous-preset-test BANK_DIR SCRATCH_DIR MATERIAL_DIR FIXTURE_DIR
#include "core.h"

#include <EngineHost.h>
#include <engine/core/PresetFile.h>
#include <engine/machine/filament/SymLevel.h>
#include <engine/effect/EffectRegistry.h>
#include <engine/inputmod/InputModRegistry.h>
#include <engine/machine/MachineRegistry.h>
#include <jni.h>
#include <platform/lines/EngineLines.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <sys/stat.h>
#include <thread>
#include <vector>

using namespace std::chrono_literals;
using core::Value;

namespace
{

struct Reply
{
    std::string path;
    core::Args  args;
};

struct Log : core::Replier
{
    std::mutex              lock;
    std::condition_variable cv;
    std::vector<Reply>      got;
    void reply(const char* path, const core::Args& args) override
    {
        std::lock_guard<std::mutex> l(lock);
        got.push_back({path, args});
        cv.notify_all();
    }
    void clear()
    {
        std::lock_guard<std::mutex> l(lock);
        got.clear();
    }
    // The first reply to [path] since the last clear, waiting up to 10 s.
    bool wait(const char* path, Reply& out)
    {
        std::unique_lock<std::mutex> l(lock);
        return cv.wait_for(l, 10s, [&] {
            for(const auto& r : got)
                if(r.path == path)
                {
                    out = r;
                    return true;
                }
            return false;
        });
    }
    int count(const char* path)
    {
        std::lock_guard<std::mutex> l(lock);
        int n = 0;
        for(const auto& r : got)
            n += r.path == path;
        return n;
    }
    std::vector<Reply> all(const char* path)
    {
        std::lock_guard<std::mutex> l(lock);
        std::vector<Reply> out;
        for(const auto& r : got)
            if(r.path == path)
                out.push_back(r);
        return out;
    }
};

int failures = 0;
void check(bool ok, const std::string& what)
{
    std::printf("%s: %s\n", ok ? "ok  " : "FAIL", what.c_str());
    if(!ok)
        ++failures;
}

bool read_file(const std::string& path, acidulous::Preset& out)
{
    std::ifstream        f(path, std::ios::binary);
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    std::string          why;
    const bool           ok = !bytes.empty() && acidulous::EngineHost::readPreset(bytes.data(), bytes.size(), out, why);
    if(!ok)
        std::printf("  %s: %s\n", path.c_str(), why.c_str());
    return ok;
}

std::string setting(const acidulous::PresetPatch& p, const std::string& key)
{
    for(const auto& kv : p.settings)
        if(kv.first == key)
            return kv.second;
    return "";
}

float param(const acidulous::PresetPatch& p, const std::string& key)
{
    for(const auto& kv : p.params)
        if(kv.first == key)
            return kv.second;
    return -1.f;
}

int index_of(const char* machine, const char* name)
{
    int32_t                    count = 0;
    const acidulous::ParamDef* defs  = acidulous::MachineRegistry::paramDefs(machine, count);
    for(int32_t i = 0; i < count; ++i)
        if(!std::strcmp(defs[i].name, name))
            return i;
    return -1;
}

// An /inserts reply as slot -> type and name -> value, the mod params too.
struct Slot
{
    std::string                  type;
    int                          count = 0, mods = 0;
    std::map<std::string, float> values;
};
std::map<int, Slot> inserts_of(const Reply& r)
{
    std::map<int, Slot> out;
    size_t              k = 1;
    const auto          params = [&](Slot& s, int n) {
        for(int i = 0; i < n && k + 7 < r.args.size(); ++i, k += 8)
            s.values[r.args[k].str] = float(r.args[k + 7].num);
    };
    while(k + 2 < r.args.size())
    {
        Slot& s = out[int(r.args[k].num)];
        s.type  = r.args[k + 1].str;
        s.count = int(r.args[k + 2].num);
        k += 3;
        params(s, s.count);
        if(k >= r.args.size())
            break;
        s.mods = int(r.args[k++].num);
        params(s, s.mods);
    }
    return out;
}

bool exists(const std::string& path)
{
    struct stat st{};
    return stat(path.c_str(), &st) == 0;
}

} // namespace

int main(int argc, char** argv)
{
    if(argc < 5)
    {
        std::fprintf(stderr, "usage: acidulous-preset-test BANK_DIR SCRATCH_DIR MATERIAL_DIR FIXTURE_DIR\n");
        return 2;
    }
    const std::string banks = argv[1], scratch = std::string(argv[2]) + "/presets", mat = argv[3],
                      fixtures = argv[4];
    std::system(("rm -rf '" + scratch + "'").c_str());
    Log   log;
    auto* c = new core::Core(banks, &log, false);

    std::atomic<bool> go{true};
    std::thread       audio([&] {
        float l[64], r[64];
        while(go)
        {
            c->process(nullptr, nullptr, l, r, 64);
            std::this_thread::sleep_for(300us);
        }
    });
    const auto cmd = [&](const char* path, core::Args a) { c->command(path, a); };
    const auto state = [&](int part, int index) {
        log.clear();
        cmd("/state", {Value::i(part)});
        Reply r;
        if(!log.wait("/state", r) || size_t(index + 4) >= r.args.size())
            return -1.0;
        return r.args[size_t(index + 4)].num;
    };
    const auto save = [&](int part, const std::string& path, const std::string& name, core::Args extra) {
        log.clear();
        core::Args a{Value::i(part), Value::s(path), Value::s(name)};
        a.insert(a.end(), extra.begin(), extra.end());
        cmd("/preset_save", a);
        Reply r;
        return log.wait("/preset_saved", r) && r.args.size() > 2 ? r.args[2].str : std::string("no reply");
    };
    const auto load_as = [&](int part, const std::string& path, int index, int files, const std::string& machine,
                             const std::string& name, Reply& r) {
        log.clear();
        cmd("/preset_load", {Value::i(part), Value::s(path), Value::i(index), Value::s("@t"), Value::i(files),
                             Value::s(machine), Value::s(name), Value::s(scratch + "/audio/"), Value::s(mat + "/")});
        return log.wait("/preset_loaded", r) && r.args.size() > 4;
    };
    const auto load = [&](int part, const std::string& path, int index, int files, Reply& r) {
        return load_as(part, path, index, files, "Trinity", "", r);
    };
    const auto no_roots = core::Args{Value::i(0)};

    // A Trinity patch with an edit, its macros and its insert.
    const int  cutoff = index_of("Trinity", "f1_freq");
    const std::string lead = scratch + "/Trinity/my_lead.acpreset";
    cmd("/patch", {Value::i(0), Value::s("Trinity"), Value::s("Saw Lead")});
    cmd("/param", {Value::i(0), Value::s("f1_freq"), Value::f(0.123)});
    const std::string names = (c->drain(), c->macro_names(0));
    check(save(0, lead, "My Lead", no_roots) == "ok", "save Trinity");
    acidulous::Preset p;
    if(read_file(lead, p) && p.patches.size() == 1)
    {
        const auto& pp = p.patches[0];
        check(pp.name == "My Lead" && pp.machine == "Trinity", "name and machine read back");
        check(std::fabs(param(pp, "f1_freq") - 0.123f) < 1e-4f, "the edit read back");
        check(!setting(pp, "macro1").empty() && setting(pp, "macro1") == names.substr(0, names.find('\n')),
              "macro names read back");
        check(!pp.chain.inserts.empty() && pp.chain.inserts[0].type == "Distortion", "the insert read back");
        check(pp.version == acidulous::kPatchVersion, "version");
    }
    else
        check(false, "read the saved Trinity");

    // Another patch, then the saved one back.
    cmd("/patch", {Value::i(0), Value::s("Trinity"), Value::s("Glass Pad")});
    Reply r;
    check(load(0, lead, 0, 1, r) && r.args[2].str == "My Lead" && r.args[3].str.empty(), "load it back");
    check(std::fabs(state(0, cutoff) - 0.123) < 1e-3, "the edit came back");
    check(c->macro_names(0) == names, "macro names came back");

    // A save queued straight behind a /patch, its insert not yet mounted.
    cmd("/patch", {Value::i(0), Value::s("Trinity"), Value::s("Saw Lead")});
    check(save(0, scratch + "/Trinity/quick.acpreset", "Quick", no_roots) == "ok", "save behind a /patch");

    // The app's fixture: two patches, the first with inserts and a send.
    check(load(0, fixtures + "/app-bank.acpreset", 0, 1, r) && r.args[2].str == "Fixture Glass" &&
              r.args[3].str.empty(),
          "load the app's fixture");
    check(log.count("/inserts") == 1, "its inserts");
    check(load(0, fixtures + "/app-bank.acpreset", 1, 1, r) && r.args[2].str == "Fixture Init", "its second patch");
    check(load(0, fixtures + "/native-bank.acpreset", 0, 1, r) && r.args[3].str.empty(), "the engine's fixture");
    check(load(0, fixtures + "/app-bank.acpreset", 5, 1, r) && !r.args[3].str.empty(), "no patch 6");
    check(load_as(0, fixtures + "/app-bank.acpreset", 0, 1, "Trinity", "Fixture Init", r) &&
              r.args[2].str == "Fixture Init" && r.args[4].num == 1,
          "found by name before index");

    // A failed load still gives the part the script's machine, with its values.
    cmd("/unmount", {Value::i(3)});
    check(load_as(3, scratch + "/nowhere.acpreset", 0, 1, "Reflux", "", r) && !r.args[3].str.empty() &&
              log.count("/values") == 1,
          "a missing file still mounts its machine");
    {
        log.clear();
        cmd("/state", {Value::i(3)});
        Reply st;
        check(log.wait("/state", st) && st.args[1].str == "Reflux", "the part has Reflux");
    }
    log.clear();
    cmd("/unmount", {Value::i(4)});
    cmd("/patch", {Value::i(4), Value::s("Reflux"), Value::s("No Such Patch")});
    c->drain();
    {
        log.clear();
        cmd("/state", {Value::i(4)});
        Reply st;
        check(log.wait("/state", st) && st.args[1].str == "Reflux", "a missing bank patch still mounts its machine");
    }

    // A patch from before version 3 isn't loaded.
    {
        acidulous::Preset old;
        old.unit = "Trinity";
        acidulous::PresetPatch pp;
        pp.machine = "Trinity";
        pp.name    = "Old";
        pp.version = 2;
        old.patches.push_back(pp);
        const auto    bytes = acidulous::presetfile::write(old);
        std::ofstream(scratch + "/Trinity/old.acpreset", std::ios::binary)
            .write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()));
        check(load(0, scratch + "/Trinity/old.acpreset", 0, 1, r) && !r.args[3].str.empty(), "an old patch refused");
        std::printf("  %s\n", r.args.size() > 3 ? r.args[3].str.c_str() : "");
    }

    // A version 3 Trinity patch has its destinations moved from 34 steps to
    // 37, the same destination; a version 4 one keeps them.
    {
        const int dest = index_of("Trinity", "m01_dest"), mac = index_of("Trinity", "mac1_d1");
        for(int version : {3, 4})
        {
            acidulous::Preset v;
            v.unit = "Trinity";
            acidulous::PresetPatch pp;
            pp.machine = "Trinity";
            pp.name    = "V" + std::to_string(version);
            pp.version = version;
            pp.params.push_back({"m01_dest", 1.f});
            pp.params.push_back({"mac1_d1", 20.f / 33.f});
            v.patches.push_back(pp);
            const auto        bytes = acidulous::presetfile::write(v);
            const std::string f     = scratch + "/Trinity/v" + std::to_string(version) + ".acpreset";
            std::ofstream(f, std::ios::binary).write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()));
            check(load(0, f, 0, 1, r) && r.args[3].str.empty(), version == 3 ? "a version 3 patch loads" : "a version 4 patch loads");
            const double want_dest = version == 3 ? 33.0 / 36.0 : 1.0, want_mac = version == 3 ? 20.0 / 36.0 : 20.0 / 33.0;
            check(std::fabs(state(0, dest) - want_dest) < 1e-4 && std::fabs(state(0, mac) - want_mac) < 1e-4,
                  version == 3 ? "its destinations moved" : "its destinations kept");
            std::remove(f.c_str());
        }
    }

    // A version 4 Filament patch has its sympathetic level and the depth of a
    // matrix slot aimed at the bank scaled by sqrt(0.2 / 3), and a missing
    // symlevel taken at its old default; a slot aimed elsewhere and a
    // version 5 patch keep theirs.
    {
        namespace fl = acidulous::filament_levels;
        const int level = index_of("Filament", "symlevel"), amt1 = index_of("Filament", "m1_amt"),
                  amt2 = index_of("Filament", "m2_amt");
        struct Case
        {
            int  version;
            bool has_level;
        };
        for(const Case& k : {Case{4, true}, Case{4, false}, Case{5, true}})
        {
            acidulous::Preset v;
            v.unit = "Filament";
            acidulous::PresetPatch pp;
            pp.machine = "Filament";
            pp.name    = "F" + std::to_string(k.version);
            pp.version = k.version;
            if(k.has_level)
                pp.params.push_back({"symlevel", 0.6f});
            pp.params.push_back({"m1_dst", float(fl::kDstSympathetic) / float(fl::kDests - 1)});
            pp.params.push_back({"m1_amt", 0.9f});
            pp.params.push_back({"m2_dst", 0.f});
            pp.params.push_back({"m2_amt", 0.9f});
            v.patches.push_back(pp);
            const auto        bytes = acidulous::presetfile::write(v);
            const std::string f     = scratch + "/filament.acpreset";
            std::ofstream(f, std::ios::binary).write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()));
            const std::string what = "Filament version " + std::to_string(k.version) + (k.has_level ? "" : " without symlevel");
            check(load_as(6, f, 0, 1, "Filament", "", r) && r.args[3].str.empty(), what + " loads");
            const bool   old        = k.version < 5;
            const double want_level = old ? fl::level(k.has_level ? 0.6f : fl::kLevelDefault) : 0.6;
            const double want_amt1  = old ? fl::depth(0.9f) : 0.9;
            const double got_level = state(6, level), got_amt1 = state(6, amt1), got_amt2 = state(6, amt2);
            check(std::fabs(got_level - want_level) < 1e-4 && std::fabs(got_amt1 - want_amt1) < 1e-4 &&
                      std::fabs(got_amt2 - 0.9) < 1e-4,
                  what + ": symlevel " + std::to_string(got_level) + ", m1_amt " + std::to_string(got_amt1) +
                      ", m2_amt " + std::to_string(got_amt2));
            std::remove(f.c_str());
        }
    }

    // A version 5 Delay insert has the pingpong switch: on becomes ping-pong
    // (mode 1/3), off normal (0). A version 6 one keeps its mode.
    {
        struct Case
        {
            int         version;
            const char* name;
            float       value, want;
        };
        for(const Case& k : {Case{5, "pingpong", 1.f, 1.f / 3.f}, Case{5, "pingpong", 0.f, 0.f},
                             Case{6, "mode", 2.f / 3.f, 2.f / 3.f}})
        {
            acidulous::Preset v;
            v.unit = "Trinity";
            acidulous::PresetPatch pp;
            pp.machine = "Trinity";
            pp.name    = "D" + std::to_string(k.version);
            pp.version = k.version;
            acidulous::PatchInsert fx;
            fx.type   = "Delay";
            fx.params = {{k.name, k.value}, {"feedback", 0.7f}};
            pp.chain.inserts.push_back(fx);
            v.patches.push_back(pp);
            const auto        bytes = acidulous::presetfile::write(v);
            const std::string f     = scratch + "/Trinity/delay.acpreset";
            std::ofstream(f, std::ios::binary).write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()));
            const std::string what = "version " + std::to_string(k.version) + " " + k.name + " " + std::to_string(k.value);
            check(load(4, f, 0, 1, r) && r.args[3].str.empty(), what + " loads");
            Reply ins;
            auto  slots = log.wait("/inserts", ins) ? inserts_of(ins) : std::map<int, Slot>{};
            check(slots[0].type == "Delay" && std::fabs(slots[0].values["mode"] - k.want) < 1e-4f &&
                      std::fabs(slots[0].values["feedback"] - 0.7f) < 1e-4f,
                  what + ": mode " + std::to_string(slots[0].values["mode"]));
            // What the engine holds, read back by an /insert in the part's own slot.
            log.clear();
            cmd("/insert", {Value::i(4), Value::i(3), Value::s("Chorus")});
            slots = log.wait("/inserts", ins) ? inserts_of(ins) : std::map<int, Slot>{};
            check(std::fabs(slots[0].values["mode"] - k.want) < 1e-4f && slots[3].type == "Chorus",
                  what + ": the engine has it");
            std::remove(f.c_str());
        }
    }

    // The part's own inserts and insert mods.
    {
        Reply ins;
        log.clear();
        cmd("/patch", {Value::i(4), Value::s("Trinity"), Value::s("Saw Lead")});
        log.wait("/inserts", ins);
        log.clear();
        cmd("/insert", {Value::i(4), Value::i(2), Value::s("Delay")});
        auto  slots = log.wait("/inserts", ins) ? inserts_of(ins) : std::map<int, Slot>{};
        int32_t mod_count = 0;
        acidulous::EffectRegistry::paramDefs("~mod", mod_count);
        check(slots[0].type == "Distortion" && slots[2].type == "Delay" && slots[3].type == "Chorus",
              "the patch's insert in slot 0 and the part's own in 2 and 3, kept over a /patch");
        check(mod_count > 0 && slots[0].mods == mod_count && slots[2].mods == mod_count && slots[2].values.count("mod_step16"),
              "every slot reads back its mod params");
        log.clear();
        cmd("/insert", {Value::i(4), Value::i(1), Value::s("Chorus")});
        check(log.wait("/error", ins) && log.count("/inserts") == 0, "slot 1 is a patch's");
        cmd("/fx", {Value::i(4), Value::i(0), Value::s("mod_source"), Value::f(0.25f)});
        cmd("/fx", {Value::i(4), Value::i(0), Value::s("mod_target1"), Value::f(2.f / 64.f)});
        cmd("/fx", {Value::i(4), Value::i(0), Value::s("mod_depth1"), Value::f(0.8f)});
        cmd("/fx", {Value::i(4), Value::i(2), Value::s("mod_source"), Value::f(0.5f)});
        cmd("/fx", {Value::i(4), Value::i(2), Value::s("mode"), Value::f(1.f)});
        log.clear();
        cmd("/fx", {Value::i(4), Value::i(2), Value::s("mod_nothing"), Value::f(1.f)});
        check(log.wait("/error", ins), "an unknown mod param is an error");
        log.clear();
        cmd("/insert", {Value::i(4), Value::i(3), Value::s("")});
        slots = log.wait("/inserts", ins) ? inserts_of(ins) : std::map<int, Slot>{};
        check(std::fabs(slots[0].values["mod_source"] - 0.25f) < 1e-4f && std::fabs(slots[0].values["mod_depth1"] - 0.8f) < 1e-4f &&
                  std::fabs(slots[2].values["mod_source"] - 0.5f) < 1e-4f && std::fabs(slots[2].values["mode"] - 1.f) < 1e-4f &&
                  !slots.count(3),
              "/fx mod params and the emptied slot 3 read back");
        const std::string f = scratch + "/Trinity/modded.acpreset";
        check(save(4, f, "Modded", no_roots) == "ok", "save with insert mods");
        acidulous::Preset m;
        if(read_file(f, m) && m.patches.size() == 1)
        {
            const auto& ch = m.patches[0].chain.inserts;
            float       src[2] = {-1.f, -1.f};
            for(size_t i = 0; i < ch.size() && i < 2; ++i)
                for(const auto& kv : ch[i].params)
                    if(kv.first == "mod_source")
                        src[i] = kv.second;
            // Saw Lead's Distortion and Delay, without the part's Delay in slot 2
            check(ch.size() == 2 && ch[0].type == "Distortion" && std::fabs(src[0] - 0.25f) < 1e-4f &&
                      ch[1].type == "Delay" && src[1] < 0.f,
                  "only the patch's slots saved, slot 0 with its mod");
        }
        else
            check(false, "read the modded patch");
        // Loading it back keeps the part's own slot 2.
        log.clear();
        check(load(4, f, 0, 1, r) && r.args[3].str.empty(), "load the modded patch");
        slots = log.wait("/inserts", ins) ? inserts_of(ins) : std::map<int, Slot>{};
        check(std::fabs(slots[0].values["mod_source"] - 0.25f) < 1e-4f && slots[2].type == "Delay",
              "its mod came back and slot 2 stayed");
        std::remove(f.c_str());
        cmd("/insert", {Value::i(4), Value::i(2), Value::s("")});
        cmd("/unmount", {Value::i(4)});
    }

    // Dice: its sample, packed.
    cmd("/patch", {Value::i(1), Value::s("Dice"), Value::s("Init")});
    cmd("/load", {Value::i(1), Value::s("sample"), Value::s(mat + "/loop.wav")});
    const std::string dice = scratch + "/Dice/loop.acpreset";
    // a labelled root writes label:path
    check(save(1, scratch + "/Dice/labelled.acpreset", "Labelled",
               {Value::i(1), Value::s("script=" + mat + "/"), Value::s("sample"), Value::s(mat + "/loop.wav")}) == "ok",
          "save with a labelled root");
    if(read_file(scratch + "/Dice/labelled.acpreset", p) && !p.patches.empty())
        check(setting(p.patches[0], "sample") == "script:loop.wav", "a labelled path");
    log.clear();
    cmd("/preset_load", {Value::i(1), Value::s(scratch + "/Dice/labelled.acpreset"), Value::i(0), Value::s("@l"),
                         Value::i(1), Value::s("Dice"), Value::s(""), Value::s("/nowhere/"),
                         Value::s("script=" + mat + "/")});
    {
        Reply x;
        log.wait("/preset_loaded", x);
        bool found = false;
        for(const auto& y : log.all("/loaded"))
            found = found || y.args[2].str == mat + "/loop.wav";
        check(found, "a labelled path found again");
    }
    // norns's script path can end in "//"
    check(save(1, dice, "Loop", {Value::i(1), Value::s(mat + "//"), Value::s("sample"), Value::s(mat + "//loop.wav")}) ==
              "ok",
          "save Dice");
    if(read_file(dice, p) && !p.patches.empty())
        check(setting(p.patches[0], "sample") == "media/loop.wav", "Dice's sample packed by its path");
    cmd("/load", {Value::i(1), Value::s("clear"), Value::s("")});
    check(load(1, dice, 0, 1, r) && r.args[3].str.empty(), "load Dice back");
    {
        bool found = false;
        for(const auto& x : log.all("/loaded"))
            found = found || (x.args[1].str == "sample" &&
                              x.args[2].str == scratch + "/audio/acidulous/presets/Dice/loop/loop.wav");
        check(found, "Dice's sample loaded again");
    }

    // Mosaic: a folder, saved as zones, loaded back as zones.
    cmd("/patch", {Value::i(2), Value::s("Mosaic"), Value::s("Init")});
    cmd("/load", {Value::i(2), Value::s("folder"), Value::s(mat + "/keys")});
    const std::string mosaic = scratch + "/Mosaic/keys.acpreset";
    check(save(2, mosaic, "Keys", {Value::i(1), Value::s(mat + "/")}) == "ok", "save Mosaic");
    if(read_file(mosaic, p) && !p.patches.empty())
    {
        const std::string z = setting(p.patches[0], "zones");
        check(z.rfind("media/keys/Keys C3.wav|", 0) == 0 && z.find(mat) == std::string::npos, "Mosaic's zones packed");
    }
    cmd("/load", {Value::i(2), Value::s("clear"), Value::s("")});
    check(load(2, mosaic, 0, 1, r) && r.args[3].str.empty(), "load Mosaic back");
    {
        bool found = false;
        for(const auto& x : log.all("/loaded"))
            found = found || x.args[1].str == "zones";
        check(found, "Mosaic's zones loaded again");
    }

    // A missing file: the rest still loads, with an /error.
    check(save(1, scratch + "/Dice/gone.acpreset", "Gone",
               {Value::i(1), Value::s(mat + "/"), Value::s("sample"), Value::s(mat + "/nope.wav")}) == "ok",
          "save with a missing file");
    check(load(1, scratch + "/Dice/gone.acpreset", 0, 1, r) && r.args[3].str.empty() && log.count("/error") == 1,
          "a missing file is an /error, the patch still loads");

    // The app's packed media/ files, unpacked under the first unlabelled root.
    {
        const std::string audio = scratch + "/audio/";
        const auto load_packed = [&](int part, const std::string& file, const std::string& machine, Reply& rr) {
            log.clear();
            cmd("/preset_load", {Value::i(part), Value::s(fixtures + "/" + file), Value::i(0), Value::s("@t"),
                                 Value::i(1), Value::s(machine), Value::s(""), Value::s(audio), Value::s(mat + "/")});
            const bool ok = log.wait("/preset_loaded", rr) && rr.args.size() > 4;
            c->drain();
            return ok;
        };
        const auto loaded = [&](const std::string& kind, const std::string& path) {
            for(const auto& x : log.all("/loaded"))
                if(x.args[1].str == kind && x.args[2].str == path)
                    return true;
            return false;
        };
        const auto error_has = [&](const std::string& text) {
            for(const auto& x : log.all("/error"))
                if(x.args[0].str.find(text) != std::string::npos)
                    return true;
            return false;
        };
        const auto inode = [](const std::string& path) {
            struct stat st{};
            return stat(path.c_str(), &st) == 0 ? st.st_ino : ino_t(0);
        };

        // Forage's pad from hit.acpreset, then a second load keeping the file.
        const std::string hit = audio + "acidulous/presets/Forage/hit/samples/hit.wav";
        check(load_packed(3, "hit.acpreset", "Forage", r) && r.args[3].str.empty() && loaded("pad", hit) &&
                  log.count("/error") == 0,
              "Forage's packed pad unpacked and loaded");
        const ino_t first = inode(hit);
        check(first != 0 && load_packed(3, "hit.acpreset", "Forage", r) && loaded("pad", hit) && inode(hit) == first,
              "a second load keeps the unpacked file");
        check(save(3, scratch + "/Forage/hit.acpreset", "Hit",
                   {Value::i(1), Value::s(audio), Value::s("p00_sample"), Value::s(hit)}) == "ok",
              "save the unpacked pad");
        if(read_file(scratch + "/Forage/hit.acpreset", p) && !p.patches.empty())
            check(setting(p.patches[0], "p00_sample") == "media/samples/hit.wav",
                  "packed by its name in the preset it came from");

        // A name with .. in it is refused, the patch still loads.
        check(load_packed(1, "bad-name.acpreset", "Dice", r) && r.args[3].str.empty() && error_has("bad name") &&
                  !exists(audio + "acidulous/presets/Dice/evil.wav"),
              "a ../ entry refused");

        // Over 64 MB unpacked: "too big", the patch still loads.
        check(load_packed(1, "big.acpreset", "Dice", r) && r.args[3].str.empty() && r.args[2].str == "Big" &&
                  error_has("too big") && !exists(audio + "acidulous/presets/Dice/big/big.wav"),
              "over the cap is too big");

        // Mosaic's zones and SoundFont.
        const std::string keys = audio + "acidulous/presets/Mosaic/zones-packed/keys/";
        check(load_packed(2, "zones-packed.acpreset", "Mosaic", r) && r.args[3].str.empty() &&
                  log.count("/error") == 0 && exists(keys + "Keys C3.wav") && exists(keys + "Keys C5.wav"),
              "Mosaic's packed zones unpacked");
        {
            bool found = false;
            for(const auto& x : log.all("/loaded"))
                found = found || x.args[1].str == "zones";
            check(found, "Mosaic's packed zones loaded");
        }
        check(load_packed(2, "sf2-packed.acpreset", "Mosaic", r) && r.args[3].str.empty() && log.count("/error") == 0 &&
                  loaded("sf2", audio + "acidulous/presets/Mosaic/sf2-packed/sf/tiny.sf2"),
              "Mosaic's packed SoundFont unpacked and loaded");

        // Only the named file comes out of a preset with over 64 MB of others.
        check(load_packed(1, "big-other.acpreset", "Dice", r) && r.args[3].str.empty() && log.count("/error") == 0 &&
                  loaded("sample", audio + "acidulous/presets/Dice/big-other/small.wav") &&
                  !exists(audio + "acidulous/presets/Dice/big-other/huge.bin"),
              "a small file from a big preset");

        // Forage's slice_sample, then saved packed.
        const std::string brk = audio + "acidulous/presets/Forage/slices/loops/break.wav";
        check(load_packed(3, "slices.acpreset", "Forage", r) && r.args[3].str.empty() && log.count("/error") == 0 &&
                  loaded("slices", brk),
              "Forage's packed slice_sample unpacked and loaded");
        check(save(3, scratch + "/Forage/sliced.acpreset", "Sliced",
                   {Value::i(1), Value::s(audio), Value::s("slice_sample"), Value::s(brk)}) == "ok",
              "save the slices");
        if(read_file(scratch + "/Forage/sliced.acpreset", p) && !p.patches.empty())
            check(setting(p.patches[0], "slice_sample") == "media/loops/break.wav",
                  "slices packed by their name in the preset they came from");

        // Diction's recorded voice from the app's sing.acpreset: unpacked, and
        // its spec the one in the app's engine lines. Its one take is cut too
        // short for the engine, which says so.
        const std::string ana = audio + "acidulous/presets/Diction/sing/voices/Ana/";
        check(load_packed(4, "sing.acpreset", "Diction", r) && r.args[3].str.empty() && exists(ana + "bank.json") &&
                  exists(ana + "ah.wav") && error_has("IY: too short"),
              "Diction's packed voice unpacked and sent to the engine");
        {
            std::ifstream        f(fixtures + "/sing.acpreset", std::ios::binary);
            std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
            std::vector<std::pair<std::string, std::vector<uint8_t>>> entries;
            std::string                                               why, line;
            acidulous::presetfile::unzip(bytes.data(), bytes.size(), entries, why);
            for(const auto& e : entries)
                if(e.first == "engine/1.txt")
                {
                    const std::string text(e.second.begin(), e.second.end());
                    const size_t      at = text.find("LoadVoice 0 0 \"");
                    if(at != std::string::npos)
                        line = text.substr(at + 15, text.find('"', at + 15) - at - 15);
                }
            const std::string zip = "zip:media/voices/Ana/";
            if(line.find(zip) != std::string::npos)
                line.replace(line.find(zip), zip.size(), ana);
            std::vector<std::string> files;
            int                      missing = 0;
            check(!line.empty() && core::voice_spec(ana + "bank.json", files, missing, why) == line && missing == 0,
                  "the voice's spec is the app's");
        }

        // A voice sung in and one to morph to; a take with a problem isn't unpacked.
        const std::string bo = audio + "acidulous/presets/Diction/voices/voices/Bo/";
        const std::string cy = audio + "acidulous/presets/Diction/voices/voices/Cy/";
        check(load_packed(4, "voices.acpreset", "Diction", r) && r.args[3].str.empty() && log.count("/error") == 0 &&
                  loaded("voice", bo + "bank.json") && loaded("voice", cy + "bank.json") &&
                  exists(bo + "aa-s.wav") && exists(bo + "ay.wav") && !exists(bo + "bad.wav"),
              "two packed voices unpacked and loaded");
        check(save(4, scratch + "/Diction/two.acpreset", "Two",
                   {Value::i(1), Value::s(audio), Value::s("voice"), Value::s(bo + "bank.json"),
                    Value::s("morphvoice"), Value::s(cy + "bank.json")}) == "ok",
              "save the voices");
        // The voices are packed as the app packs them, media/voices/<name>/.
        if(read_file(scratch + "/Diction/two.acpreset", p) && !p.patches.empty())
            check(setting(p.patches[0], "voice") == "media/voices/Bo/bank.json" &&
                      setting(p.patches[0], "morphvoice") == "media/voices/Cy/bank.json",
                  "voices saved as the app's media/voices/<name>/bank.json");
        {
            std::ifstream        f(scratch + "/Diction/two.acpreset", std::ios::binary);
            std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
            std::vector<std::pair<std::string, std::vector<uint8_t>>> entries;
            std::string                                               why;
            std::set<std::string>                                     names;
            bool                                                      same = true;
            const bool ok = acidulous::presetfile::unzip(bytes.data(), bytes.size(), entries, why);
            for(const auto& e : entries)
            {
                names.insert(e.first);
                const std::string rest = e.first.rfind("media/voices/", 0) == 0 ? e.first.substr(13) : "";
                if(rest.empty())
                    continue;
                std::ifstream        g(audio + "acidulous/presets/Diction/voices/voices/" + rest, std::ios::binary);
                std::vector<uint8_t> there((std::istreambuf_iterator<char>(g)), std::istreambuf_iterator<char>());
                same = same && there == e.second;
            }
            check(ok && same && names.count("preset.json") && names.count("patches/1.json") &&
                      names.count("media/voices/Bo/bank.json") && names.count("media/voices/Bo/aa.wav") &&
                      names.count("media/voices/Bo/ay.wav") && names.count("media/voices/Bo/aa-s.wav") &&
                      names.count("media/voices/Cy/bank.json") && names.count("media/voices/Cy/iy.wav") &&
                      !names.count("media/voices/Bo/bad.wav") && names.size() == 8,
                  "the saved zip holds each voice's bank.json and takes");
        }
        // and loading that file unpacks them again, the same voices
        const std::string bo2 = audio + "acidulous/presets/Diction/two/voices/Bo/";
        const std::string cy2 = audio + "acidulous/presets/Diction/two/voices/Cy/";
        log.clear();
        cmd("/preset_load", {Value::i(4), Value::s(scratch + "/Diction/two.acpreset"), Value::i(0), Value::s("@t"),
                             Value::i(1), Value::s("Diction"), Value::s(""), Value::s(audio)});
        check(log.wait("/preset_loaded", r) && (c->drain(), true) && r.args[3].str.empty() &&
                  loaded("voice", bo2 + "bank.json") && loaded("voice", cy2 + "bank.json") &&
                  log.count("/error") == 0,
              "the saved voices load again");
        {
            const auto spec = [](const std::string& dir) {
                std::vector<std::string> files;
                int                      missing = 0;
                std::string              why;
                std::string              s = core::voice_spec(dir + "bank.json", files, missing, why);
                for(size_t at; (at = s.find(dir)) != std::string::npos;)
                    s.erase(at, dir.size());
                return missing == 0 ? s : std::string();
            };
            check(!spec(bo).empty() && spec(bo) == spec(bo2) && !spec(cy).empty() && spec(cy) == spec(cy2),
                  "the reloaded voices are the same");
        }
        // a voice and morph voice from two folders of one name get two names
        {
            const std::string other = audio + "elsewhere/Bo/";
            std::system(("mkdir -p '" + other + "' && cp '" + bo + "bank.json' '" + bo + "aa.wav' '" + other + "'").c_str());
            check(save(4, scratch + "/Diction/same.acpreset", "Same",
                       {Value::i(1), Value::s(audio), Value::s("voice"), Value::s(bo + "bank.json"),
                        Value::s("morphvoice"), Value::s(other + "bank.json")}) == "ok",
                  "save two voices of one name");
            if(read_file(scratch + "/Diction/same.acpreset", p) && !p.patches.empty())
                check(setting(p.patches[0], "voice") == "media/voices/Bo/bank.json" &&
                          setting(p.patches[0], "morphvoice") == "media/voices/Bo 2/bank.json",
                      "the second one is Bo 2");
        }

        // Every machine's files packed as the app packs them, media/<path
        // under the root>, and loaded back from there the same.
        {
            const auto bytes_of = [](const std::string& path) {
                std::ifstream f(path, std::ios::binary);
                return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
            };
            const auto entries_of = [&](const std::string& path) {
                const std::vector<uint8_t> zip = bytes_of(path);
                std::vector<std::pair<std::string, std::vector<uint8_t>>> list;
                std::string                                               why;
                acidulous::presetfile::unzip(zip.data(), zip.size(), list, why, size_t(80) << 20);
                return std::map<std::string, std::vector<uint8_t>>(list.begin(), list.end());
            };
            const std::string pk = audio + "pk/", out = scratch + "/outside/";
            std::system(("mkdir -p '" + pk + "' '" + out + "a' '" + out + "b' && cp -r '" + mat + "/loop.wav' '" + mat +
                         "/voice.wav' '" + mat + "/test.sf2' '" + mat + "/keys' '" + mat + "/kit' '" + pk +
                         "' && cp '" + mat + "/kit/03-snare.wav' '" + out + "a/hit.wav' && cp '" + mat +
                         "/kit/04-clap.wav' '" + out + "b/hit.wav'")
                            .c_str());
            const core::Args root{Value::i(2), Value::s(audio), Value::s("script=" + mat + "/")};
            const auto       with = [&](core::Args a, std::initializer_list<std::string> kv) {
                for(const auto& x : kv)
                    a.push_back(Value::s(x));
                return a;
            };
            const auto reload = [&](int part, const std::string& file, const std::string& machine) {
                log.clear();
                cmd("/preset_load", {Value::i(part), Value::s(file), Value::i(0), Value::s("@t"), Value::i(1),
                                     Value::s(machine), Value::s(""), Value::s(audio), Value::s("script=" + mat + "/")});
                Reply x;
                const bool ok = log.wait("/preset_loaded", x) && x.args.size() > 4 && x.args[3].str.empty();
                c->drain();
                return ok && log.count("/error") == 0;
            };
            // Each [entry] in the zip at [file] holds the bytes of [source].
            const auto holds = [&](const std::map<std::string, std::vector<uint8_t>>& z, const std::string& entry,
                                   const std::string& source) {
                const auto it = z.find(entry);
                return it != z.end() && !it->second.empty() && it->second == bytes_of(source);
            };

            // Forage: pads, one file on two pads, two files of one name from
            // outside the root, one from the script's folder kept as a path.
            cmd("/patch", {Value::i(3), Value::s("Forage"), Value::s("Init")});
            const std::string forage = scratch + "/Forage/pads.acpreset";
            check(save(3, forage, "Pads",
                       with(root, {"p00_sample", pk + "kit/01-kick.wav", "p01_sample", pk + "kit/01-kick.wav",
                                   "p02_sample", out + "a/hit.wav", "p03_sample", out + "b/hit.wav", "p04_sample",
                                   mat + "/kit/05-tom1.wav"})) == "ok",
                  "save Forage's pads");
            auto z = entries_of(forage);
            if(read_file(forage, p) && !p.patches.empty())
            {
                const auto& pp = p.patches[0];
                check(setting(pp, "p00_sample") == "media/pk/kit/01-kick.wav" &&
                          setting(pp, "p01_sample") == "media/pk/kit/01-kick.wav" &&
                          setting(pp, "p02_sample") == "media/hit.wav" &&
                          setting(pp, "p03_sample") == "media/hit (2).wav" &&
                          setting(pp, "p04_sample") == "script:kit/05-tom1.wav",
                      "Forage's pads point into media/, the same name twice made two");
            }
            check(z.size() == 5 && holds(z, "media/pk/kit/01-kick.wav", pk + "kit/01-kick.wav") &&
                      holds(z, "media/hit.wav", out + "a/hit.wav") && holds(z, "media/hit (2).wav", out + "b/hit.wav"),
                  "Forage's pads packed once each");
            const std::string fdir = audio + "acidulous/presets/Forage/pads/";
            check(reload(3, forage, "Forage") && loaded("pad", fdir + "pk/kit/01-kick.wav") &&
                      loaded("pad", fdir + "hit.wav") && loaded("pad", fdir + "hit (2).wav") &&
                      loaded("pad", mat + "/kit/05-tom1.wav") && bytes_of(fdir + "hit (2).wav") == bytes_of(out + "b/hit.wav"),
                  "Forage's pads load back the same files");
            // saved again from where they were unpacked, under the same names
            check(save(3, forage, "Pads",
                       with(root, {"p00_sample", fdir + "pk/kit/01-kick.wav", "p02_sample", fdir + "hit.wav",
                                   "p03_sample", fdir + "hit (2).wav"})) == "ok",
                  "a one-patch file with files is saved over");
            z = entries_of(forage);
            check(z.count("media/pk/kit/01-kick.wav") && z.count("media/hit.wav") && z.count("media/hit (2).wav") &&
                      z.size() == 5,
                  "unpacked files go back in by their names in the preset");
            log.clear();
            cmd("/preset_scan", {Value::s(scratch + "/Forage")});
            {
                bool flag = true;
                if(log.wait("/preset_scan", r))
                    for(size_t k = 1; k + 5 < r.args.size(); k += 6)
                        if(r.args[k].str == "pads.acpreset")
                            flag = r.args[k + 5].num != 0;
                check(!flag, "our own packed file isn't marked packed");
            }

            // Forage's slices.
            check(save(3, scratch + "/Forage/cut.acpreset", "Cut", with(root, {"slice_sample", pk + "loop.wav"})) == "ok",
                  "save Forage's slices");
            z = entries_of(scratch + "/Forage/cut.acpreset");
            if(read_file(scratch + "/Forage/cut.acpreset", p) && !p.patches.empty())
                check(setting(p.patches[0], "slice_sample") == "media/pk/loop.wav" &&
                          holds(z, "media/pk/loop.wav", pk + "loop.wav"),
                      "Forage's slices packed");
            check(reload(3, scratch + "/Forage/cut.acpreset", "Forage") &&
                      loaded("slices", audio + "acidulous/presets/Forage/cut/pk/loop.wav"),
                  "Forage's slices load back");

            // Dice, Pollen and Molt: their sample.
            int part = 0;
            for(const char* m : {"Dice", "Pollen", "Molt"})
            {
                const std::string machine = m, file = scratch + "/" + machine + "/one.acpreset";
                cmd("/patch", {Value::i(part), Value::s(machine), Value::s("Init")});
                check(save(part, file, "One", with(root, {"sample", pk + "voice.wav"})) == "ok", "save " + machine);
                z = entries_of(file);
                if(read_file(file, p) && !p.patches.empty())
                    check(setting(p.patches[0], "sample") == "media/pk/voice.wav" &&
                              holds(z, "media/pk/voice.wav", pk + "voice.wav") && z.size() == 3,
                          machine + "'s sample packed");
                const std::string back = audio + "acidulous/presets/" + machine + "/one/pk/voice.wav";
                check(reload(part, file, machine) && loaded("sample", back) && bytes_of(back) == bytes_of(pk + "voice.wav"),
                      machine + "'s sample loads back the same");
                part = part == 0 ? 1 : 0;
            }

            // Mosaic: a SoundFont, then a folder's zones with their files.
            cmd("/patch", {Value::i(2), Value::s("Mosaic"), Value::s("Init")});
            check(save(2, scratch + "/Mosaic/font.acpreset", "Font",
                       with(root, {"sf2", pk + "test.sf2", "sf2preset", "0"})) == "ok",
                  "save Mosaic's SoundFont");
            z = entries_of(scratch + "/Mosaic/font.acpreset");
            if(read_file(scratch + "/Mosaic/font.acpreset", p) && !p.patches.empty())
                check(setting(p.patches[0], "sf2") == "media/pk/test.sf2" && holds(z, "media/pk/test.sf2", pk + "test.sf2"),
                      "Mosaic's SoundFont packed");
            check(reload(2, scratch + "/Mosaic/font.acpreset", "Mosaic") &&
                      loaded("sf2", audio + "acidulous/presets/Mosaic/font/pk/test.sf2"),
                  "Mosaic's SoundFont loads back");
            log.clear();
            cmd("/load", {Value::i(2), Value::s("folder"), Value::s(pk + "keys")});
            c->drain();
            const std::string zfile = scratch + "/Mosaic/zoned.acpreset";
            check(save(2, zfile, "Zoned", root) == "ok", "save Mosaic's zones");
            z = entries_of(zfile);
            std::string zones;
            if(read_file(zfile, p) && !p.patches.empty())
                zones = setting(p.patches[0], "zones");
            check(zones.rfind("media/pk/keys/Keys C3.wav|", 0) == 0 && zones.find("\nmedia/pk/keys/Keys C5.wav|") != std::string::npos &&
                      holds(z, "media/pk/keys/Keys C3.wav", pk + "keys/Keys C3.wav") &&
                      holds(z, "media/pk/keys/Keys C4.wav", pk + "keys/Keys C4.wav") &&
                      holds(z, "media/pk/keys/Keys C5.wav", pk + "keys/Keys C5.wav") && z.size() == 5,
                  "Mosaic's zones point into media/ with their files packed");
            const std::string zdir = audio + "acidulous/presets/Mosaic/zoned/pk/keys/";
            {
                const bool ok = reload(2, zfile, "Mosaic");
                bool       found = false;
                for(const auto& x : log.all("/loaded"))
                    found = found || x.args[1].str == "zones";
                check(ok && found && bytes_of(zdir + "Keys C4.wav") == bytes_of(pk + "keys/Keys C4.wav"),
                      "Mosaic's zones load back the same files");
            }

            // 64 MB in all: a file that would go past it stays a path.
            const std::string huge = pk + "huge.wav";
            std::system(("truncate -s " + std::to_string((64 << 20) - 100000) + " '" + huge + "'").c_str());
            check(save(3, scratch + "/Forage/full.acpreset", "Full",
                       with(root, {"p00_sample", huge, "p01_sample", pk + "loop.wav", "p02_sample",
                                   pk + "kit/02-rim.aif"})) == "ok",
                  "save past 64 MB");
            z = entries_of(scratch + "/Forage/full.acpreset");
            {
                // too big for readPreset, so the patch's JSON is read as text
                const auto        it   = z.find("patches/1.json");
                const std::string json = it == z.end() ? "" : std::string(it->second.begin(), it->second.end());
                check(json.find("\"media/pk/huge.wav\"") != std::string::npos &&
                          json.find("\"pk/loop.wav\"") != std::string::npos &&
                          json.find("\"media/pk/kit/02-rim.aif\"") != std::string::npos && z.count("media/pk/huge.wav") &&
                          !z.count("media/pk/loop.wav") && z.count("media/pk/kit/02-rim.aif"),
                      "a file past 64 MB in all stays a path");
            }
            std::remove(huge.c_str());
            std::remove((scratch + "/Forage/full.acpreset").c_str());
        }

        // /load: the built-in voice back, a voice with none of its takes, not on Forage.
        const auto load_kind = [&](int part, const std::string& kind, const std::string& path, int arg) {
            log.clear();
            cmd("/load", {Value::i(part), Value::s(kind), Value::s(path), Value::i(arg)});
            c->drain();
        };
        load_kind(4, "voice", "", 0);
        check(loaded("voice", "") && log.count("/error") == 0, "the built-in voice back");
        std::remove((cy + "iy.wav").c_str());
        load_kind(4, "voice", cy + "bank.json", 1);
        check(error_has("no takes"), "a voice with no takes is an /error");
        load_kind(3, "voice", bo + "bank.json", 0);
        check(error_has("has Forage"), "a voice on Forage is refused");
    }

    // The app's machine presets: /preset_load on part 6 against the preset's
    // engine/1.txt replayed onto rack 7, its zip unpacked for the zip: paths.
    // Machine params, macro names, inserts and sends match within 1e-4.
    {
        auto&             host  = acidulous::EngineHost::instance();
        const std::string audio = scratch + "/audio/";
        const int         native = 6, lines = 7;
        // [rack]'s patch, once its mounts are in.
        const auto patch_of = [&](int rack) {
            acidulous::PresetPatch pp;
            std::string            why;
            for(int tries = 0; tries < 200 && pp.machine.empty(); ++tries)
            {
                pp = host.rackPatch(rack, "x", why, true, nullptr);
                if(pp.machine.empty())
                    std::this_thread::sleep_for(5ms);
            }
            return pp;
        };
        const auto blocks = [&](int n) {
            const int64_t until = host.framesRendered() + 64 * n;
            for(int k = 0; k < 2000 && host.framesRendered() < until; ++k)
                std::this_thread::sleep_for(200us);
        };
        for(const char* name : {"reflux", "hit", "sing"})
        {
            const std::string file = fixtures + "/" + name + ".acpreset";
            const std::string root = scratch + "/replay/" + name + "/";
            std::ifstream        f(file, std::ios::binary);
            std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
            std::vector<std::pair<std::string, std::vector<uint8_t>>> entries;
            std::string                                               why, text;
            acidulous::presetfile::unzip(bytes.data(), bytes.size(), entries, why);
            for(const auto& e : entries)
            {
                const std::string out = root + e.first;
                std::system(("mkdir -p '" + out.substr(0, out.rfind('/')) + "'").c_str());
                std::ofstream(out, std::ios::binary)
                    .write(reinterpret_cast<const char*>(e.second.data()), std::streamsize(e.second.size()));
                if(e.first == "engine/1.txt")
                    text.assign(e.second.begin(), e.second.end());
            }

            log.clear();
            cmd("/preset_load", {Value::i(native), Value::s(file), Value::i(0), Value::s("@t"), Value::i(1),
                                 Value::s(""), Value::s(""), Value::s(audio)});
            const bool loaded = log.wait("/preset_loaded", r) && r.args[3].str.empty();
            c->drain();

            static JNIEnv                    env;
            acidulous::lines::Replay         replay;
            std::vector<std::string>         problems;
            replay.env        = &env;
            replay.root       = root;
            replay.targetRack = lines;
            replay.log        = [&](const std::string& p) { problems.push_back(p); };
            replay.afterLine  = [&](int) { blocks(2); };
            const bool replayed = !text.empty() && acidulous::lines::replay(text, replay);
            jniweb::arena().clear();
            for(const auto& p : problems)
                std::printf("  %s lines: %s\n", name, p.c_str());
            blocks(4);

            const auto a = patch_of(native), b = patch_of(lines);
            int        differ = 0;
            const auto say    = [&](const std::string& what) {
                std::printf("  %s: %s\n", name, what.c_str());
                ++differ;
            };
            if(std::string(name) == "reflux" && std::fabs(param(a, "cutoff") - 0.3f) > 1e-4f)
                say("cutoff isn't the preset's 0.3");
            if(a.machine != b.machine)
                say("machine " + a.machine + " vs " + b.machine);
            std::map<std::string, float> pa(a.params.begin(), a.params.end()), pb(b.params.begin(), b.params.end());
            for(const auto& kv : pb)
            {
                const auto it = pa.find(kv.first);
                if(it == pa.end())
                    say(kv.first + " missing from the native load");
                else if(std::fabs(it->second - kv.second) > 1e-4f)
                    say(kv.first + " native " + std::to_string(it->second) + ", lines " + std::to_string(kv.second));
            }
            for(const auto& kv : pa)
                if(!pb.count(kv.first))
                    say(kv.first + " missing from the lines");
            if(host.macroNames(native) != host.macroNames(lines))
                say("macro names differ");
            if(a.settings != b.settings)
                say("settings (macro names, formulas) differ");
            if(a.chain.inserts.size() != b.chain.inserts.size())
                say("insert count " + std::to_string(a.chain.inserts.size()) + " vs " +
                    std::to_string(b.chain.inserts.size()));
            for(size_t k = 0; k < a.chain.inserts.size() && k < b.chain.inserts.size(); ++k)
            {
                const auto& ia = a.chain.inserts[k];
                const auto& ib = b.chain.inserts[k];
                if(ia.type != ib.type || ia.bypass != ib.bypass || ia.settings != ib.settings)
                    say("insert " + std::to_string(k) + " " + ia.type + " vs " + ib.type);
                std::map<std::string, float> xa(ia.params.begin(), ia.params.end());
                for(const auto& kv : ib.params)
                    if(!xa.count(kv.first) || std::fabs(xa[kv.first] - kv.second) > 1e-4f)
                        say("insert " + std::to_string(k) + " " + kv.first);
            }
            if(a.chain.hasSends != b.chain.hasSends || std::fabs(a.chain.sendReverb - b.chain.sendReverb) > 1e-4f ||
               std::fabs(a.chain.sendDelay - b.chain.sendDelay) > 1e-4f)
                say("sends differ");
            std::printf("  %s: %zu params compared\n", name, pb.size());
            check(loaded && replayed && !a.machine.empty() && pb.size() > 10 && differ == 0,
                  std::string(name) + ": /preset_load matches its engine lines");
            cmd("/unmount", {Value::i(native)});
            host.unmountMachine(lines);
            c->drain();
            blocks(4);
        }
    }

    // The app's effect presets: /fx_preset_load into part 6's own slot
    // against the preset's engine/1.txt replayed onto rack 7's other own
    // slot. Every Delay param matches within 1e-4.
    {
        auto&      host   = acidulous::EngineHost::instance();
        const int  native = 6, lines = 7;
        const auto blocks = [&](int n) {
            const int64_t until = host.framesRendered() + 64 * n;
            for(int k = 0; k < 2000 && host.framesRendered() < until; ++k)
                std::this_thread::sleep_for(200us);
        };
        int at = 0;
        for(const char* name : {"delay", "delay2"})
        {
            const int            mine = at == 0 ? 2 : 3, theirs = at == 0 ? 3 : 2;
            const std::string    file = fixtures + "/" + name + ".acpreset";
            std::ifstream        f(file, std::ios::binary);
            std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
            std::vector<std::pair<std::string, std::vector<uint8_t>>> entries;
            std::string                                               why, text;
            acidulous::presetfile::unzip(bytes.data(), bytes.size(), entries, why);
            for(const auto& e : entries)
                if(e.first == "engine/1.txt")
                    text.assign(e.second.begin(), e.second.end());
            ++at;

            log.clear();
            cmd("/fx_preset_load", {Value::i(native), Value::i(mine), Value::s(file), Value::i(0), Value::s("")});
            const bool loaded = log.wait("/fx_preset_loaded", r) && r.args[4].str.empty() && r.args[2].str == "Delay";
            c->drain();

            static JNIEnv            env;
            acidulous::lines::Replay replay;
            std::vector<std::string> problems;
            replay.env        = &env;
            replay.root       = scratch + "/replay/" + name + "/";
            replay.targetRack = lines;
            replay.targetSlot = theirs;
            replay.log        = [&](const std::string& p) { problems.push_back(p); };
            replay.afterLine  = [&](int) { blocks(2); };
            const bool replayed = !text.empty() && acidulous::lines::replay(text, replay);
            jniweb::arena().clear();
            for(const auto& p : problems)
                std::printf("  %s lines: %s\n", name, p.c_str());
            blocks(4);

            const std::string ua = "effect" + std::to_string(mine + 1), ub = "effect" + std::to_string(theirs + 1);
            int32_t           count = 0, differ = 0;
            const auto*       defs  = acidulous::EffectRegistry::paramDefs("Delay", count);
            for(int32_t i = 0; i < count; ++i)
            {
                const float a = host.paramNormalized(native, ua, defs[i].name);
                const float b = host.paramNormalized(lines, ub, defs[i].name);
                if(a < 0.f || b < 0.f || std::fabs(a - b) > 1e-4f)
                {
                    std::printf("  %s: %s native %f, lines %f\n", name, defs[i].name, a, b);
                    ++differ;
                }
            }
            const bool mix = std::fabs(host.paramNormalized(native, ua, "mix") - 0.5f) < 1e-4f;
            std::printf("  %s: %d params compared\n", name, int(count));
            check(loaded && replayed && mix && host.mountedEffect(lines, theirs) == std::string("Delay") &&
                      count > 5 && differ == 0,
                  std::string(name) + ": /fx_preset_load matches its engine lines");
            cmd("/insert", {Value::i(native), Value::i(mine), Value::s("")});
            host.mountEffect(lines, theirs, "");
            c->drain();
            blocks(4);
        }
    }

    // Effect presets: load, the mod kept, another type switching the slot,
    // an old pingpong, refusals, a save round trip, the deep scan.
    {
        const std::string fxdir = scratch + "/fx/";
        const auto        fx_load = [&](int part, int slot, const std::string& path, int index, const std::string& name,
                                 Reply& out) {
            log.clear();
            cmd("/fx_preset_load", {Value::i(part), Value::i(slot), Value::s(path), Value::i(index), Value::s(name)});
            return log.wait("/fx_preset_loaded", out) && out.args.size() >= 5;
        };
        const auto fx_save = [&](int part, int slot, const std::string& path, const std::string& name) {
            log.clear();
            cmd("/fx_preset_save", {Value::i(part), Value::i(slot), Value::s(path), Value::s(name)});
            Reply out;
            return log.wait("/fx_preset_saved", out) && out.args.size() > 3 ? out.args[3].str : std::string("no reply");
        };
        const auto value_of = [](const Reply& out, const std::string& name) {
            for(size_t k = 5; k + 2 < out.args.size(); k += 3)
                if(out.args[k].str == name)
                    return float(out.args[k + 1].num);
            return -1.f;
        };
        cmd("/patch", {Value::i(4), Value::s("Trinity"), Value::s("Saw Lead")});
        cmd("/insert", {Value::i(4), Value::i(2), Value::s("Delay")});
        cmd("/fx", {Value::i(4), Value::i(2), Value::s("mod_source"), Value::f(0.5f)});
        cmd("/fx", {Value::i(4), Value::i(2), Value::s("feedback"), Value::f(0.9f)});
        c->drain();
        log.clear();
        Reply ins;
        check(fx_load(4, 2, fixtures + "/delay.acpreset", 0, "D", r) && r.args[4].str.empty() &&
                  r.args[3].str == "D" && std::fabs(value_of(r, "mix") - 0.5f) < 1e-4f,
              "an effect preset loads into fx 3");
        const auto slots = log.wait("/inserts", ins) ? inserts_of(ins) : std::map<int, Slot>{};
        const auto slot2 = slots.count(2) ? slots.at(2) : Slot{};
        check(slot2.type == "Delay" && slot2.values.count("mod_source") && slot2.values.at("mod_source") > 0.f,
              "its /inserts, the slot's mod kept");
        int32_t     count = 0;
        const auto* defs  = acidulous::EffectRegistry::paramDefs("Delay", count);
        float       fb    = -1.f;
        for(int32_t i = 0; i < count; ++i)
            if(!std::strcmp(defs[i].name, "feedback"))
                fb = defs[i].unmap(defs[i].def);
        check(std::fabs(value_of(r, "feedback") - fb) < 1e-4f, "a param it doesn't name back at its default");
        log.clear();
        cmd("/fx_preset_load", {Value::i(4), Value::i(1), Value::s(fixtures + "/delay.acpreset"), Value::i(0), Value::s("")});
        check(log.wait("/error", r) && log.count("/fx_preset_loaded") == 0, "a patch's slot is refused");
        check(fx_load(4, 3, fixtures + "/reflux.acpreset", 0, "", r) && r.args[4].str == "not an effect preset" &&
                  r.args[2].str.empty(),
              "a machine preset is refused, the slot left empty");
        check(fx_load(4, 3, scratch + "/nowhere.acpreset", 0, "", r) && !r.args[4].str.empty(), "a missing file");

        // An old Delay's pingpong as mode, and a Chorus preset switching the slot's effect.
        {
            acidulous::Preset v;
            v.kind = "effect";
            v.unit = "Delay";
            acidulous::PresetPatch pp;
            pp.machine = "Delay";
            pp.name    = "Old";
            pp.version = 5;
            pp.params  = {{"pingpong", 1.f}, {"feedback", 0.7f}};
            v.patches.push_back(pp);
            pp.name    = "Older";
            pp.version = 2;
            v.patches.push_back(pp);
            const auto bytes = acidulous::presetfile::write(v);
            std::system(("mkdir -p '" + fxdir + "Delay'").c_str());
            std::ofstream(fxdir + "Delay/old.acpreset", std::ios::binary)
                .write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()));
        }
        check(fx_load(4, 3, fxdir + "Delay/old.acpreset", 0, "", r) && r.args[4].str.empty() &&
                  std::fabs(value_of(r, "mode") - 1.f / 3.f) < 1e-4f && std::fabs(value_of(r, "feedback") - 0.7f) < 1e-4f,
              "version 5: pingpong moved to mode");
        check(fx_load(4, 3, fxdir + "Delay/old.acpreset", 1, "", r) && !r.args[4].str.empty(), "version 2 refused");
        check(fx_load(4, 3, fxdir + "Delay/old.acpreset", 0, "Older", r) && !r.args[4].str.empty() &&
                  r.args[2].str == "Delay",
              "found by name, and a refusal leaves the slot");
        cmd("/fx", {Value::i(4), Value::i(3), Value::s("feedback"), Value::f(0.25f)});
        check(fx_save(4, 3, fxdir + "Delay/mine.acpreset", "Mine") == "ok", "save fx 4");
        acidulous::Preset back;
        if(read_file(fxdir + "Delay/mine.acpreset", back) && back.patches.size() == 1)
        {
            const auto& pp = back.patches[0];
            check(back.kind == "effect" && back.unit == "Delay" && pp.name == "Mine" &&
                      std::fabs(param(pp, "feedback") - 0.25f) < 1e-4f && param(pp, "mode") >= 0.f &&
                      param(pp, "mod_source") < 0.f && pp.version == acidulous::kPatchVersion,
                  "saved as the app's effect preset, without the mod");
            std::ifstream        f(fxdir + "Delay/mine.acpreset", std::ios::binary);
            std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
            std::vector<std::pair<std::string, std::vector<uint8_t>>> entries;
            std::string                                               why;
            acidulous::presetfile::unzip(bytes.data(), bytes.size(), entries, why);
            std::string json;
            for(const auto& e : entries)
                if(e.first == "patches/1.json")
                    json.assign(e.second.begin(), e.second.end());
            check(json.find("\"fx.Delay\"") != std::string::npos, "its patch keyed fx.Delay as the app keys it");
        }
        else
            check(false, "read the saved effect preset");
        check(fx_save(4, 3, fxdir + "Delay/old.acpreset", "Over") != "ok", "a file of two patches isn't replaced");
        check(fx_save(4, 2, fixtures + "/../nowhere/x.acpreset", "") == "no name", "no name");
        cmd("/insert", {Value::i(4), Value::i(3), Value::s("")});
        c->drain();
        check(fx_save(4, 3, fxdir + "Delay/empty.acpreset", "E") != "ok", "an empty slot isn't saved");

        // A Chorus preset into fx 3, which has the Delay: a new Chorus.
        cmd("/insert", {Value::i(4), Value::i(3), Value::s("Chorus")});
        c->drain();
        check(fx_save(4, 3, fxdir + "Chorus/wide.acpreset", "Wide") == "ok", "save a Chorus");
        check(fx_load(4, 2, fxdir + "Chorus/wide.acpreset", 0, "Wide", r) && r.args[4].str.empty() &&
                  r.args[2].str == "Chorus",
              "a Chorus preset switches fx 3 to a Chorus");
        check(fx_load(4, 3, fxdir + "Delay/mine.acpreset", 0, "Mine", r) && r.args[2].str == "Delay" &&
                  std::fabs(value_of(r, "feedback") - 0.25f) < 1e-4f,
              "the saved Delay back into fx 4");

        // The deep scan lists both folders' effect presets as fx.<type>; a
        // machine folder's scan lists an effect preset in it as fx.Delay.
        log.clear();
        cmd("/preset_scan", {Value::s(fxdir), Value::i(1)});
        std::set<std::string> rows;
        if(log.wait("/preset_scan", r))
            for(size_t k = 1; k + 5 < r.args.size(); k += 6)
                rows.insert(r.args[k].str + "|" + r.args[k + 2].str + "|" + r.args[k + 3].str);
        check(rows.count("Chorus/wide.acpreset|Wide|fx.Chorus") && rows.count("Delay/mine.acpreset|Mine|fx.Delay") &&
                  rows.count("Delay/old.acpreset|Older|fx.Delay") && rows.size() == 4,
              "deep scan: 4 rows with their types");
        log.clear();
        cmd("/preset_scan", {Value::s(fxdir)});
        check(log.wait("/preset_scan", r) && r.args.size() == 1, "a shallow scan doesn't look in folders");
        log.clear();
        cmd("/preset_delete", {Value::s(fxdir + "Chorus/wide.acpreset"), Value::i(0)});
        check(log.wait("/preset_deleted", r) && r.args[2].str == "ok" && !exists(fxdir + "Chorus/wide.acpreset"),
              "delete an effect preset");

        // A Formula's formula: saved as last loaded, back the same, and none
        // for a Formula mounted fresh.
        {
            acidulous::Preset v;
            v.kind = "effect";
            v.unit = "Formula";
            acidulous::PresetPatch pp;
            pp.machine  = "Formula";
            pp.name     = "Crush";
            pp.settings = {{"formula", "x & 224"}};
            v.patches.push_back(pp);
            const auto bytes = acidulous::presetfile::write(v);
            std::system(("mkdir -p '" + fxdir + "Formula'").c_str());
            std::ofstream(fxdir + "Formula/crush.acpreset", std::ios::binary)
                .write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()));
        }
        const auto formula_of = [&](const std::string& file) {
            acidulous::Preset got;
            return read_file(file, got) && got.patches.size() == 1 ? setting(got.patches[0], "formula") : std::string("?");
        };
        check(fx_load(4, 2, fxdir + "Formula/crush.acpreset", 0, "Crush", r) && r.args[4].str.empty() &&
                  r.args[2].str == "Formula",
              "a Formula preset loads");
        check(fx_save(4, 2, fxdir + "Formula/again.acpreset", "Again") == "ok" &&
                  formula_of(fxdir + "Formula/again.acpreset") == "x & 224",
              "its formula saved");
        cmd("/insert", {Value::i(4), Value::i(2), Value::s("Delay")});
        check(fx_load(4, 2, fxdir + "Formula/again.acpreset", 0, "Again", r) && r.args[4].str.empty() &&
                  fx_save(4, 2, fxdir + "Formula/third.acpreset", "Third") == "ok" &&
                  formula_of(fxdir + "Formula/third.acpreset") == "x & 224",
              "saved, loaded and saved again: the same formula");
        cmd("/insert", {Value::i(4), Value::i(2), Value::s("")});
        cmd("/insert", {Value::i(4), Value::i(2), Value::s("Formula")});
        c->drain();
        check(fx_save(4, 2, fxdir + "Formula/fresh.acpreset", "Fresh") == "ok" &&
                  formula_of(fxdir + "Formula/fresh.acpreset").empty(),
              "a fresh Formula saves no formula");
        cmd("/insert", {Value::i(4), Value::i(2), Value::s("")});
        cmd("/insert", {Value::i(4), Value::i(3), Value::s("")});
        c->drain();
    }

    // Modifier presets: the app's Arp and Chord presets into a part's
    // modifier slots, what a load keeps, another type switching the slot,
    // refusals, a save the app can read, the deep scan and a delete.
    {
        const std::string moddir   = scratch + "/mod/";
        const auto        mod_load = [&](int part, int slot, const std::string& path, int index,
                                  const std::string& name, Reply& out) {
            log.clear();
            cmd("/mod_preset_load", {Value::i(part), Value::i(slot), Value::s(path), Value::i(index), Value::s(name)});
            return log.wait("/mod_preset_loaded", out) && out.args.size() >= 5;
        };
        const auto mod_save = [&](int part, int slot, const std::string& path, const std::string& name) {
            log.clear();
            cmd("/mod_preset_save", {Value::i(part), Value::i(slot), Value::s(path), Value::s(name)});
            Reply out;
            return log.wait("/mod_preset_saved", out) && out.args.size() > 3 ? out.args[3].str : std::string("no reply");
        };
        const auto value_of = [](const Reply& out, const std::string& name) {
            for(size_t k = 5; k + 1 < out.args.size(); k += 2)
                if(out.args[k].str == name)
                    return float(out.args[k + 1].num);
            return -1.f;
        };
        // The JSON of each entry of the zip at [file].
        const auto entries_of = [](const std::string& file) {
            std::ifstream        f(file, std::ios::binary);
            std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
            std::vector<std::pair<std::string, std::vector<uint8_t>>> entries;
            std::string                                               why;
            std::map<std::string, std::string>                        out;
            if(!bytes.empty() && acidulous::presetfile::unzip(bytes.data(), bytes.size(), entries, why))
                for(const auto& e : entries)
                    out[e.first].assign(e.second.begin(), e.second.end());
            return out;
        };
        const auto def01 = [](const char* type, const char* name) {
            int32_t     count = 0;
            const auto* defs  = acidulous::InputModRegistry::paramDefs(type, count);
            for(int32_t i = 0; defs && i < count; ++i)
                if(!std::strcmp(defs[i].name, name))
                    return defs[i].unmap(defs[i].def);
            return -1.f;
        };
        const int part = 2;
        check(mod_load(part, 2, fixtures + "/arp.acpreset", 0, "Up 16ths", r) && r.args[4].str.empty() &&
                  r.args[2].str == "Arp" && r.args[3].str == "Up 16ths" &&
                  std::fabs(value_of(r, "rate") - 0.7777778f) < 1e-4f &&
                  std::fabs(value_of(r, "gate") - 0.23076923f) < 1e-4f,
              "an Arp preset into an empty mod 3");
        cmd("/mod_param", {Value::i(part), Value::i(2), Value::s("latch"), Value::f(1.f)});
        check(mod_load(part, 2, fixtures + "/arp.acpreset", 0, "Slow", r) && r.args[3].str == "Slow" &&
                  value_of(r, "rate") == 0.f && std::fabs(value_of(r, "gate") - def01("Arp", "gate")) < 1e-4f &&
                  value_of(r, "latch") == 1.f,
              "found by name, a param it doesn't name at its default, latch kept");
        log.clear();
        cmd("/mods", {Value::i(part)});
        check(log.wait("/mods", r) && r.args.size() > 1 && r.args[1].num == 0 && r.args[2].str.empty(),
              "/mods: slot 1 still empty");
        check(mod_load(part, 2, fixtures + "/chord.acpreset", 0, "", r) && r.args[4].str.empty() &&
                  r.args[2].str == "Chord" && std::fabs(value_of(r, "type") - 0.5f) < 1e-4f &&
                  std::fabs(value_of(r, "key") - def01("Chord", "key")) < 1e-4f &&
                  std::fabs(value_of(r, "scale") - def01("Chord", "scale")) < 1e-4f && value_of(r, "latch") == 0.f,
              "a Chord preset switches the slot, its key, scale and latch not taken");
        check(mod_load(part, 2, fixtures + "/scale.acpreset", 0, "", r) && !r.args[4].str.empty() &&
                  r.args[2].str == "Chord",
              "a Scale preset refused, the slot left");
        check(mod_load(part, 2, fixtures + "/delay.acpreset", 0, "", r) && r.args[4].str == "not a modifier preset",
              "an effect preset refused");
        check(mod_load(part, 0, fixtures + "/reflux.acpreset", 0, "", r) && r.args[4].str == "not a modifier preset" &&
                  r.args[2].str.empty(),
              "a machine preset refused, the slot left empty");
        log.clear();
        cmd("/fx_preset_load", {Value::i(part), Value::i(2), Value::s(fixtures + "/arp.acpreset"), Value::i(0), Value::s("")});
        check(log.wait("/fx_preset_loaded", r) && r.args[4].str == "not an effect preset", "nor an effect slot");

        cmd("/mod_param", {Value::i(part), Value::i(2), Value::s("strum"), Value::f(0.4f)});
        cmd("/mod_param", {Value::i(part), Value::i(2), Value::s("key"), Value::f(0.25f)});
        check(mod_save(part, 2, moddir + "Chord/mine.acpreset", "Mine") == "ok", "save mod 3's Chord");
        {
            auto        e   = entries_of(moddir + "Chord/mine.acpreset");
            std::string man = e["preset.json"], pj = e["patches/1.json"];
            check(man.find("\"kind\": \"modifier\"") != std::string::npos &&
                      man.find("\"unit\": \"Chord\"") != std::string::npos &&
                      pj.find("\"machine\": \"mod.Chord\"") != std::string::npos &&
                      pj.find("\"strum\"") != std::string::npos && pj.find("\"key\"") == std::string::npos &&
                      pj.find("\"latch\"") == std::string::npos && e.size() == 2,
                  "saved as the app's modifier preset, keyed mod.Chord, without key and latch");
        }
        check(mod_load(part, 1, moddir + "Chord/mine.acpreset", 0, "Mine", r) && r.args[4].str.empty() &&
                  r.args[2].str == "Chord" && std::fabs(value_of(r, "strum") - 0.4f) < 1e-4f &&
                  std::fabs(value_of(r, "type") - 0.5f) < 1e-4f,
              "the saved Chord back into mod 2");
        cmd("/mod", {Value::i(part), Value::i(0), Value::s("Scale")});
        check(mod_save(part, 0, moddir + "Scale/s.acpreset", "S") == "no scale presets" && !exists(moddir + "Scale"),
              "a Scale isn't saved");
        cmd("/mod", {Value::i(part), Value::i(0), Value::s("")});
        check(mod_save(part, 0, moddir + "Arp/e.acpreset", "E") != "ok", "an empty slot isn't saved");
        check(mod_save(part, 2, moddir + "Chord/x.acpreset", "") == "no name", "no name");

        // The deep scan lists them as mod.<type>; deleting one patch of the
        // app's two keeps it a modifier preset.
        std::system(("mkdir -p '" + moddir + "Arp' && cp '" + fixtures + "/arp.acpreset' '" + moddir + "Arp/'").c_str());
        const auto scan = [&] {
            std::set<std::string> rows;
            log.clear();
            cmd("/preset_scan", {Value::s(moddir), Value::i(1)});
            if(log.wait("/preset_scan", r))
                for(size_t k = 1; k + 5 < r.args.size(); k += 6)
                    rows.insert(r.args[k].str + "|" + r.args[k + 2].str + "|" + r.args[k + 3].str);
            return rows;
        };
        auto rows = scan();
        check(rows.size() == 3 && rows.count("Arp/arp.acpreset|Up 16ths|mod.Arp") &&
                  rows.count("Arp/arp.acpreset|Slow|mod.Arp") && rows.count("Chord/mine.acpreset|Mine|mod.Chord"),
              "deep scan: 3 rows as mod.<type>");
        log.clear();
        cmd("/preset_delete", {Value::s(moddir + "Arp/arp.acpreset"), Value::i(0)});
        check(log.wait("/preset_deleted", r) && r.args[2].str == "ok", "delete one patch of the app's Arp");
        rows = scan();
        check(rows.size() == 2 && rows.count("Arp/arp.acpreset|Slow|mod.Arp") &&
                  entries_of(moddir + "Arp/arp.acpreset")["preset.json"].find("\"modifier\"") != std::string::npos,
              "delete one patch of two: still a modifier preset");
        for(int slot = 0; slot < 3; ++slot)
            cmd("/mod", {Value::i(part), Value::i(slot), Value::s("")});
        c->drain();
    }

    // Listing and deleting.
    std::system(("cp '" + fixtures + "/app-bank.acpreset' '" + scratch + "/Trinity/'").c_str());
    log.clear();
    cmd("/preset_scan", {Value::s(scratch + "/Trinity")});
    check(log.wait("/preset_scan", r) && (r.args.size() - 1) / 6 == 5, "scan: 5 rows");
    log.clear();
    cmd("/preset_delete", {Value::s(scratch + "/Trinity/app-bank.acpreset"), Value::i(0)});
    check(log.wait("/preset_deleted", r) && r.args[2].str == "ok" && read_file(scratch + "/Trinity/app-bank.acpreset", p) &&
              p.patches.size() == 1 && p.patches[0].name == "Fixture Init",
          "delete one patch of two");
    log.clear();
    cmd("/preset_delete", {Value::s(scratch + "/Trinity/app-bank.acpreset"), Value::i(0)});
    check(log.wait("/preset_deleted", r) && r.args[2].str == "ok" && !exists(scratch + "/Trinity/app-bank.acpreset"),
          "delete the last one");

    // A file with the app's samples in it isn't rewritten or replaced.
    std::system(("cp '" + fixtures + "/packed-bank.acpreset' '" + scratch + "/Trinity/'").c_str());
    log.clear();
    cmd("/preset_delete", {Value::s(scratch + "/Trinity/packed-bank.acpreset"), Value::i(0)});
    check(log.wait("/preset_deleted", r) && r.args[2].str != "ok" && exists(scratch + "/Trinity/packed-bank.acpreset"),
          "a packed file isn't rewritten");
    cmd("/patch", {Value::i(0), Value::s("Trinity"), Value::s("Saw Lead")});
    check(save(0, scratch + "/Trinity/packed-bank.acpreset", "Over", no_roots) != "ok", "nor saved over");
    check(save(0, lead, "Over", no_roots) == "ok", "a one-patch file is saved over");
    log.clear();
    cmd("/preset_delete", {Value::s(scratch + "/Trinity/packed-bank.acpreset"), Value::i(-1)});
    check(log.wait("/preset_deleted", r) && r.args[2].str == "ok" && !exists(scratch + "/Trinity/packed-bank.acpreset"),
          "the whole file deleted");

    // An empty part can't be saved.
    check(save(5, scratch + "/none.acpreset", "None", no_roots) == "no machine", "an empty part");

    c->drain();
    go = false;
    audio.join();
    delete c;
    std::printf("%s\n", failures ? "FAILED" : "all ok");
    return failures ? 1 : 0;
}
