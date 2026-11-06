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
// - Dice's sample and Mosaic's folder saved as relative paths and found again,
//   and a missing file that leaves the rest loaded
// - the app's packed media/ files unpacked (Forage pad from hit.acpreset,
//   Mosaic zones and SoundFont), kept on a second load, a ../ name refused
//   and over 64 MB refused, the patch still loading
// - /preset_scan and /preset_delete
// Usage: acidulous-preset-test BANK_DIR SCRATCH_DIR MATERIAL_DIR FIXTURE_DIR
#include "core.h"

#include <EngineHost.h>
#include <engine/core/PresetFile.h>
#include <engine/machine/filament/SymLevel.h>
#include <engine/effect/EffectRegistry.h>
#include <engine/machine/MachineRegistry.h>

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
                             Value::s(machine), Value::s(name), Value::s(mat + "/")});
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

    // Dice: its sample, relative to the root.
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
        check(setting(p.patches[0], "sample") == "loop.wav", "Dice's sample is relative");
    cmd("/load", {Value::i(1), Value::s("clear"), Value::s("")});
    check(load(1, dice, 0, 1, r) && r.args[3].str.empty(), "load Dice back");
    {
        bool found = false;
        for(const auto& x : log.all("/loaded"))
            found = found || (x.args[1].str == "sample" && x.args[2].str == mat + "/loop.wav");
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
        check(z.rfind("keys/Keys C3.wav|", 0) == 0 && z.find(mat) == std::string::npos, "Mosaic's zones are relative");
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
            check(setting(p.patches[0], "p00_sample") == "acidulous/presets/Forage/hit/samples/hit.wav",
                  "saved relative to the root");

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
