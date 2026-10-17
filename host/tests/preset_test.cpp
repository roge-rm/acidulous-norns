// Saves parts as .acpreset files and loads them back, with the core's worker
// thread and a fake audio thread, as the plugin runs:
// - a Trinity patch with an edit, its macro names and insert, read back with
//   EngineHost::readPreset and loaded over another patch
// - a save straight after a /patch, while its insert mount is still queued
// - the app's fixture (host/tests/acpreset), and a patch too old to load
// - Dice's sample and Mosaic's folder saved as relative paths and found again,
//   and a missing file that leaves the rest loaded
// - /preset_scan and /preset_delete
// Usage: acidulous-preset-test BANK_DIR SCRATCH_DIR MATERIAL_DIR FIXTURE_DIR
#include "core.h"

#include <EngineHost.h>
#include <engine/core/PresetFile.h>
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
    const auto load = [&](int part, const std::string& path, int index, int files, Reply& r) {
        log.clear();
        cmd("/preset_load", {Value::i(part), Value::s(path), Value::i(index), Value::s("@t"), Value::i(files),
                             Value::s(mat + "/")});
        return log.wait("/preset_loaded", r) && r.args.size() > 3;
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

    // Dice: its sample, relative to the root.
    cmd("/patch", {Value::i(1), Value::s("Dice"), Value::s("Init")});
    cmd("/load", {Value::i(1), Value::s("sample"), Value::s(mat + "/loop.wav")});
    const std::string dice = scratch + "/Dice/loop.acpreset";
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

    // Listing and deleting.
    std::system(("cp '" + fixtures + "/app-bank.acpreset' '" + scratch + "/Trinity/'").c_str());
    log.clear();
    cmd("/preset_scan", {Value::s(scratch + "/Trinity")});
    check(log.wait("/preset_scan", r) && (r.args.size() - 1) / 4 == 5, "scan: 5 rows");
    log.clear();
    cmd("/preset_delete", {Value::s(scratch + "/Trinity/app-bank.acpreset"), Value::i(0)});
    check(log.wait("/preset_deleted", r) && r.args[2].str == "ok" && read_file(scratch + "/Trinity/app-bank.acpreset", p) &&
              p.patches.size() == 1 && p.patches[0].name == "Fixture Init",
          "delete one patch of two");
    log.clear();
    cmd("/preset_delete", {Value::s(scratch + "/Trinity/app-bank.acpreset"), Value::i(0)});
    check(log.wait("/preset_deleted", r) && r.args[2].str == "ok" && !exists(scratch + "/Trinity/app-bank.acpreset"),
          "delete the last one");

    // An empty part can't be saved.
    check(save(5, scratch + "/none.acpreset", "None", no_roots) == "no machine", "an empty part");

    c->drain();
    go = false;
    audio.join();
    delete c;
    std::printf("%s\n", failures ? "FAILED" : "all ok");
    return failures ? 1 : 0;
}
