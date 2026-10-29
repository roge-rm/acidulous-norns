// The frozen-clip chooser (song_frozen.cpp) on synthetic engine lines: the
// three modes, a machine that is missing or can't run, a scene over 60%, and
// the sample budget swap. No engine needed.
// Usage: acidulous-frozen-test
#include "song_frozen.h"

#include <cstdio>
#include <cstdlib>
#include <string>

using namespace core;

namespace
{

int failures = 0;

void check(bool ok, const char* what)
{
    if(!ok)
    {
        ++failures;
        std::fprintf(stderr, "FAIL: %s\n", what);
    }
}

bool has(const std::string& text, const std::string& line)
{
    return text.find(line + "\n") != std::string::npos;
}

// A song: [machines] is MountMachine lines, [scenes] the scene ids, [clips]
// "rack scene" pairs with notes, [frozen] "rack sceneId" pairs.
struct Song
{
    std::string text = "acidulous-engine 4\nClearFrozen\n";

    void machine(int rack, const char* name) { text += "MountMachine " + std::to_string(rack) + " \"" + name + "\"\n"; }
    void param(int rack, const char* unit)
    {
        text += "SetParam " + std::to_string(rack) + " \"" + unit + "\" \"p1\" 0.5 false 0\n";
    }
    void sample(int rack, int bytes)
    {
        text += "LoadSample " + std::to_string(rack) + " 0 \"zip:samples/" + std::to_string(bytes) + ".wav\" 30\n";
    }
    void frozen(int rack, const char* scene)
    {
        text += "MountFrozen " + std::to_string(rack) + " " + scene + " \"zip:freeze/r" + std::to_string(rack) +
                "__" + scene + ".wav\" 120.0 1920 192000 64 0.1\n";
    }
    void scenes(std::initializer_list<const char*> ids)
    {
        text += "SnapshotBegin\n";
        for(const char* id : ids)
            text += std::string("SnapshotAddScene @0 ") + id + " 960 1 0.0 0.0 0 false false false\n";
    }
    void clip(int rack, int scene, int notes = 12, bool mute = false)
    {
        text += "SnapshotSetClipCached @0 " + std::to_string(rack) + " " + std::to_string(scene) + " 1\n";
        text += "SnapshotSetClip @0 " + std::to_string(rack) + " " + std::to_string(scene) + " 1 2 0 " +
                (mute ? "true" : "false") + " 0 i[" + std::to_string(notes) + ": 0 120 36 120 0 100] f[0:] null\n";
    }
    void commit() { text += "SnapshotCommit @0\n"; }
};

FrozenHost host_for()
{
    FrozenHost h;
    h.can_run = [](const std::string& m) { return m != "Bias" && m != "Zork"; };
    return h;
}

// Racks 0-2 Trinity and 3 Ratio all play in scene "10"; 3 and 0 have frozen
// clips there. Scene "20" has racks 0 and 1 only.
Song busy()
{
    Song s;
    s.machine(0, "Trinity");
    s.machine(1, "Trinity");
    s.machine(2, "Trinity");
    s.machine(3, "Ratio");
    s.frozen(0, "10");
    s.frozen(3, "10");
    s.scenes({"10", "20"});
    for(int r = 0; r < 4; ++r)
        s.clip(r, 0);
    s.clip(0, 1);
    s.clip(1, 1);
    s.commit();
    return s;
}

} // namespace

int main()
{
    // frozen keeps every MountFrozen, notes drops them
    {
        const Song s = busy();
        auto       f = choose_frozen(s.text, FrozenMode::Frozen, host_for());
        check(f.text == s.text, "frozen mode leaves the lines as they are");
        check(f.frozen == std::vector<int>({0, 3}), "frozen mode freezes racks 0 and 3");
        check(f.no_machine.empty(), "frozen mode keeps the machines");
        auto n = choose_frozen(s.text, FrozenMode::Notes, host_for());
        check(!has(n.text, "MountFrozen 0 10 \"zip:freeze/r0__10.wav\" 120.0 1920 192000 64 0.1") &&
                  n.text.find("MountFrozen") == std::string::npos,
              "notes mode drops the MountFrozen lines");
        check(n.frozen.empty(), "notes mode freezes nothing");
        check(has(n.text, "MountMachine 3 \"Ratio\""), "notes mode keeps the machines");
        check(n.text.find("SnapshotSetClip @0 3 0") != std::string::npos, "notes mode keeps the note lines");
    }

    // auto: scene 10 is 3 x 18.4 + 27.4 = 82.6, over 60. Ratio has a frozen
    // clip there and is the costliest, so it goes; 55.2 is under.
    {
        const Song s = busy();
        auto       a = choose_frozen(s.text, FrozenMode::Auto, host_for());
        check(a.frozen == std::vector<int>({3}), "auto freezes only the Ratio");
        check(has(a.text, "MountFrozen 3 10 \"zip:freeze/r3__10.wav\" 120.0 1920 192000 64 0.1"), "auto keeps rack 3's clip");
        check(a.text.find("MountFrozen 0 ") == std::string::npos, "auto drops rack 0's clip");
        check(a.no_machine.empty() && has(a.text, "MountMachine 3 \"Ratio\""), "auto keeps rack 3's machine");
    }

    // a scene over 60 with no frozen clip to take is left alone
    {
        Song s;
        s.machine(0, "Nexus");
        s.machine(1, "Trinity");
        s.frozen(1, "20");
        s.scenes({"10", "20"});
        s.clip(0, 0);
        s.clip(1, 0);
        s.clip(1, 1);
        s.commit();
        auto a = choose_frozen(s.text, FrozenMode::Auto, host_for());
        check(a.frozen.empty(), "no frozen clip in the heavy scene, nothing frozen");
    }

    // a rack frozen in a scene counts 0 there, so a second rack stays notes:
    // two Trinity (36.8) and a Ratio with frozen clip (27.4) and a Nexus
    // (75.4, no clip), where Nexus alone is over 60 and can't be helped
    {
        Song s;
        s.machine(0, "Ratio");
        s.machine(1, "Ratio");
        s.machine(2, "Trinity");
        s.frozen(0, "10");
        s.frozen(1, "10");
        s.scenes({"10"});
        for(int r = 0; r < 3; ++r)
            s.clip(r, 0);
        s.commit();
        // 27.4 + 27.4 + 18.4 = 73.2: freezing one Ratio leaves 45.8
        auto a = choose_frozen(s.text, FrozenMode::Auto, host_for());
        check(a.frozen.size() == 1 && a.frozen[0] == 0, "one of two equal racks is enough");
    }

    // muted clips and empty ones cost nothing
    {
        Song s;
        s.machine(0, "Nexus");
        s.machine(1, "Ratio");
        s.frozen(1, "10");
        s.scenes({"10"});
        s.clip(0, 0, 12, true);
        s.clip(1, 0, 0);
        s.commit();
        check(choose_frozen(s.text, FrozenMode::Auto, host_for()).frozen.empty(), "muted and empty clips cost nothing");
    }

    // a missing machine, one the plugin can't run and an unknown one are
    // frozen in every mode, with no machine or loads
    {
        Song s;
        s.machine(0, "Bias");
        s.machine(1, "Zork");
        s.machine(2, "Reflux");
        s.sample(0, 100);
        s.sample(1, 100);
        s.param(0, "machine");
        s.param(0, "channel");
        s.frozen(0, "10");
        s.frozen(1, "10");
        s.frozen(2, "10");
        s.frozen(4, "10"); // no MountMachine at all
        s.scenes({"10"});
        s.clip(0, 0);
        s.commit();
        for(FrozenMode m : {FrozenMode::Auto, FrozenMode::Notes, FrozenMode::Frozen})
        {
            auto f = choose_frozen(s.text, m, host_for());
            check(f.no_machine == std::vector<int>({0, 1, 4}), "can't-run racks lose their machine");
            check(has(f.text, "MountMachine 0 \"Reflux\"") && has(f.text, "MountMachine 1 \"Reflux\"") &&
                      has(f.text, "MountMachine 4 \"Reflux\"") && f.text.find("Bias") == std::string::npos,
                  "they hold the stand-in, also the rack with no machine line");
            check(f.text.find("LoadSample 1") == std::string::npos && f.text.find("SetParam 0 \"machine\"") == std::string::npos,
                  "their Load lines and machine values are gone");
            check(has(f.text, "SetParam 0 \"channel\" \"p1\" 0.5 false 0"), "their channel strip stays");
            check(has(f.text, "MountMachine 2 \"Reflux\""), "a machine that runs stays");
            check(f.text.find("MountFrozen 0 ") != std::string::npos && f.text.find("MountFrozen 4 ") != std::string::npos,
                  "their clips stay");
            check((m == FrozenMode::Frozen) == (f.text.find("MountFrozen 2 ") != std::string::npos),
                  "the Reflux's clip is kept only in frozen mode");
        }
    }

    // the sample budget: the audio is the samples of racks that have a
    // machine plus the clips kept. Rack 0 (4 MB of samples) has a frozen clip
    // in its only scene, rack 1 (3 MB) has notes in a second scene too.
    {
        Song s;
        s.machine(0, "Forage");
        s.sample(0, 4000);
        s.machine(1, "Forage");
        s.sample(1, 3000);
        s.frozen(0, "10");
        s.frozen(1, "10");
        s.scenes({"10", "20"});
        s.clip(0, 0);
        s.clip(1, 0);
        s.clip(1, 1);
        s.commit();
        FrozenHost h = host_for();
        // each frozen clip counts 500
        h.audio_bytes = [](const std::string& t) {
            int64_t sum = 0;
            for(size_t at = 0; at < t.size();)
            {
                size_t end = t.find('\n', at);
                if(end == std::string::npos)
                    end = t.size();
                const std::string line = t.substr(at, end - at);
                at                     = end + 1;
                if(line.compare(0, 10, "LoadSample") == 0)
                    sum += std::atoi(line.c_str() + line.find("samples/") + 8);
                else if(line.compare(0, 11, "MountFrozen") == 0)
                    sum += 500;
            }
            return sum;
        };
        h.budget = 100000;
        auto fits = choose_frozen(s.text, FrozenMode::Frozen, h);
        check(fits.text == s.text && fits.no_machine.empty(), "under the budget nothing is swapped");
        h.budget = 8000; // 7000 + 1000 of clips = 8000: fits exactly
        check(choose_frozen(s.text, FrozenMode::Frozen, h).no_machine.empty(), "exactly at the budget is fine");
        h.budget = 5000;
        auto f = choose_frozen(s.text, FrozenMode::Frozen, h);
        check(f.no_machine == std::vector<int>({0}), "rack 0 is covered and drops its samples");
        check(f.text.find("LoadSample 0") == std::string::npos && has(f.text, "MountMachine 0 \"Reflux\"") &&
                  !has(f.text, "MountMachine 0 \"Forage\""),
              "it holds the stand-in and its LoadSample is gone");
        check(has(f.text, "MountMachine 1 \"Forage\""), "rack 1 has notes in a scene without a clip and stays");
        h.budget = 100;
        f = choose_frozen(s.text, FrozenMode::Frozen, h);
        check(f.no_machine == std::vector<int>({0}), "still over: only what's covered is swapped");
        check(h.audio_bytes(f.text) > h.budget, "and the caller sees it's over");
        // notes mode drops the clips, and nothing is covered
        h.budget = 5000;
        f = choose_frozen(s.text, FrozenMode::Notes, h);
        check(f.no_machine.empty(), "notes mode swaps nothing");
        // auto with the heavy racks gets the same swap for racks it froze
        h.budget = 5000;
        f = choose_frozen(s.text, FrozenMode::Auto, h);
        check(f.no_machine.empty() && f.frozen.empty(), "auto with a light song freezes nothing and swaps nothing");
    }

    // no snapshot, and an empty song
    {
        auto f = choose_frozen("", FrozenMode::Auto, host_for());
        check(f.text.empty() && f.frozen.empty(), "an empty song");
        Song s;
        s.machine(0, "Trinity");
        s.frozen(0, "10");
        auto a = choose_frozen(s.text, FrozenMode::Auto, host_for());
        check(a.frozen.empty(), "no arrangement, no need to freeze");
    }

    // mode names
    {
        FrozenMode m = FrozenMode::Frozen;
        check(parse_frozen_mode("auto", m) && m == FrozenMode::Auto, "auto");
        check(parse_frozen_mode("notes", m) && m == FrozenMode::Notes, "notes");
        check(parse_frozen_mode("frozen", m) && m == FrozenMode::Frozen, "frozen");
        check(!parse_frozen_mode("Frozen", m) && !parse_frozen_mode("", m), "anything else is refused");
    }

    if(failures)
    {
        std::fprintf(stderr, "%d failed\n", failures);
        return 1;
    }
    std::printf("frozen chooser: ok\n");
    return 0;
}
