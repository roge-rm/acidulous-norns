#include "song_frozen.h"

#include <algorithm>
#include <cstdlib>
#include <map>
#include <set>

namespace core
{

bool parse_frozen_mode(const std::string& name, FrozenMode& out)
{
    if(name == "auto")
        out = FrozenMode::Auto;
    else if(name == "notes")
        out = FrozenMode::Notes;
    else if(name == "frozen")
        out = FrozenMode::Frozen;
    else
        return false;
    return true;
}

double machine_cost(const std::string& machine)
{
    static const std::map<std::string, double> cost = {
        {"Reflux", 0.9},    {"Hexbeat", 2.4},   {"Trinity", 18.4}, {"Ratio", 27.4},   {"Formulate", 1.1},
        {"Cumulus", 31.7},  {"Nexus", 75.4},    {"Diction", 13.3}, {"Forage", 13.0},  {"Mosaic", 23.2},
        {"Dice", 34.0},     {"Pollen", 35.8},   {"Molt", 15.1},    {"Cipher", 11.0},  {"Hammer", 11.9},
        {"Manual", 10.7},   {"Filament", 10.2}, {"Sympath", 8.9},  {"Brazen", 6.3},   {"Fathom", 7.4},
        {"Resonance", 5.8}, {"Draw", 5.6},      {"Chanter", 3.7},  {"Fret", 2.8},     {"Tine", 2.3},
        {"Genesis", 2.2},   {"Palm", 1.9},      {"Tongue", 1.8},   {"Timber", 1.7},   {"Aviary", 1.7},
    };
    const auto it = cost.find(machine);
    return it == cost.end() ? 10.0 : it->second;
}

namespace
{

// The first [max] words of an engine line, quoted strings unquoted.
std::vector<std::string> first_words(const std::string& line, size_t max)
{
    std::vector<std::string> out;
    size_t                   i = 0;
    while(i < line.size() && out.size() < max)
    {
        while(i < line.size() && line[i] == ' ')
            ++i;
        if(i >= line.size())
            break;
        std::string w;
        if(line[i] == '"')
        {
            ++i;
            while(i < line.size() && line[i] != '"')
            {
                if(line[i] == '\\' && i + 1 < line.size())
                    ++i;
                w += line[i++];
            }
            ++i;
        }
        else
            while(i < line.size() && line[i] != ' ')
                w += line[i++];
        out.push_back(w);
    }
    return out;
}

enum class Kind
{
    Other,
    Machine,   // MountMachine
    Param,     // SetParam of the machine's own values
    Load,      // a call that loads a rack's samples or patch into its machine
    Frozen,    // MountFrozen
    Begin,
    Scene,
    Clip,      // SnapshotSetClip with notes
    Commit,
};

struct Line
{
    std::string text;
    Kind        kind = Kind::Other;
    int         rack = -1;
    std::string name; // the machine, or a frozen clip's scene id
    int         scene = -1; // a clip's scene index
    bool        dropped = false;
};

// The loads that need the rack's machine. LoadEffectFormula is an insert's.
bool is_machine_load(const std::string& call)
{
    return (call.compare(0, 4, "Load") == 0 && call != "LoadEffectFormula") || call == "BuildCloud";
}

struct Scenes
{
    std::vector<std::string>      ids;   // engine ids, in order
    std::set<std::pair<int, int>> clips; // (rack, scene index) with notes
};

} // namespace

FrozenChoice choose_frozen(const std::string& text, FrozenMode mode, const FrozenHost& host)
{
    std::vector<Line> lines;
    Scenes            building, done;
    std::map<int, std::string> machine_of; // the last MountMachine per rack
    for(size_t at = 0; at < text.size();)
    {
        size_t end = text.find('\n', at);
        if(end == std::string::npos)
            end = text.size();
        Line l;
        l.text = text.substr(at, end - at);
        at     = end + 1;
        const auto w = first_words(l.text, 10);
        if(!w.empty())
        {
            const std::string& call = w[0];
            const auto rack = [&](size_t k) { return k < w.size() ? std::atoi(w[k].c_str()) : -1; };
            if(call == "MountMachine" && w.size() >= 3)
            {
                l.kind = Kind::Machine;
                l.rack = rack(1);
                l.name = w[2];
                machine_of[l.rack] = w[2];
            }
            else if(call == "SetParam" && w.size() >= 3 && w[2] == "machine")
            {
                l.kind = Kind::Param;
                l.rack = rack(1);
            }
            else if(is_machine_load(call) && w.size() >= 2)
            {
                l.kind = Kind::Load;
                l.rack = rack(1);
            }
            else if(call == "MountFrozen" && w.size() >= 3)
            {
                l.kind = Kind::Frozen;
                l.rack = rack(1);
                l.name = w[2];
            }
            else if(call == "SnapshotBegin")
            {
                l.kind   = Kind::Begin;
                building = Scenes{};
            }
            else if(call == "SnapshotAddScene" && w.size() >= 3)
            {
                l.kind = Kind::Scene;
                building.ids.push_back(w[2]);
            }
            else if(call == "SnapshotSetClip" && w.size() >= 10)
            {
                // handle rack scene rev bars playMode mute seed notes
                l.kind = Kind::Clip;
                if(w[7] != "true" && w[9].compare(0, 2, "i[") == 0 && std::atoi(w[9].c_str() + 2) > 0)
                    building.clips.insert({rack(2), rack(3)});
            }
            else if(call == "SnapshotCommit")
            {
                l.kind = Kind::Commit;
                done   = building;
            }
        }
        lines.push_back(std::move(l));
    }

    // The racks with frozen clips, and the scene ids they have one in.
    std::map<int, std::set<std::string>> clips_of;
    for(const Line& l : lines)
        if(l.kind == Kind::Frozen)
            clips_of[l.rack].insert(l.name);

    std::set<int> forced, frozen;
    for(const auto& kv : clips_of)
    {
        const auto m = machine_of.find(kv.first);
        if(m == machine_of.end() || !host.can_run || !host.can_run(m->second))
            forced.insert(kv.first);
    }
    if(mode == FrozenMode::Frozen)
        for(const auto& kv : clips_of)
            frozen.insert(kv.first);
    frozen.insert(forced.begin(), forced.end());

    if(mode == FrozenMode::Auto)
    {
        for(size_t s = 0; s < done.ids.size(); ++s)
        {
            const std::string& id = done.ids[s];
            for(;;)
            {
                double sum   = 0;
                int    worst = -1;
                double worst_cost = 0;
                for(const auto& c : done.clips)
                {
                    if(c.second != int(s) || forced.count(c.first))
                        continue;
                    const auto has = clips_of.find(c.first);
                    const bool here = has != clips_of.end() && has->second.count(id);
                    if(frozen.count(c.first) && here)
                        continue;
                    const double cost = machine_cost(machine_of.count(c.first) ? machine_of[c.first] : "");
                    sum += cost;
                    if(here && !frozen.count(c.first) && cost > worst_cost)
                        worst = c.first, worst_cost = cost;
                }
                if(sum <= kFrozenCpuLimit || worst < 0)
                    break;
                frozen.insert(worst);
            }
        }
    }

    std::set<int> no_machine = forced;
    const auto    build      = [&] {
        std::string   out;
        std::set<int> stood;
        const auto    stand_in = [&](int rack) {
            if(stood.insert(rack).second)
                out += "MountMachine " + std::to_string(rack) + " \"" + kStandInMachine + "\"\n";
        };
        for(const Line& l : lines)
        {
            const bool lost = no_machine.count(l.rack) > 0;
            if(l.kind == Kind::Frozen && !frozen.count(l.rack))
                continue;
            // The engine only plays a rack that has a machine, so a rack
            // without one holds the stand-in, and the machine's own values
            // and loads go.
            if(lost && l.kind == Kind::Machine)
                stand_in(l.rack);
            else if(lost && (l.kind == Kind::Load || l.kind == Kind::Param))
                continue;
            else
            {
                if(lost && l.kind == Kind::Frozen && !machine_of.count(l.rack))
                    stand_in(l.rack);
                out += l.text + '\n';
            }
        }
        return out;
    };

    if(host.audio_bytes && host.budget > 0)
    {
        std::string now   = build();
        int64_t     total = host.audio_bytes(now);
        // A frozen rack covers a scene when it has a frozen clip in it.
        const auto covered = [&](int r) {
            const auto has = clips_of.find(r);
            for(const auto& c : done.clips)
                if(c.first == r && (has == clips_of.end() || c.second < 0 || size_t(c.second) >= done.ids.size() ||
                                    !has->second.count(done.ids[size_t(c.second)])))
                    return false;
            return true;
        };
        while(total > host.budget)
        {
            int     best  = -1;
            int64_t saved = 0;
            for(int r : frozen)
            {
                if(no_machine.count(r) || !covered(r))
                    continue;
                no_machine.insert(r);
                const int64_t gain = total - host.audio_bytes(build());
                no_machine.erase(r);
                if(gain > saved)
                    best = r, saved = gain;
            }
            if(best < 0)
                break;
            no_machine.insert(best);
            total -= saved;
        }
    }

    FrozenChoice out;
    out.text       = build();
    out.frozen     = std::vector<int>(frozen.begin(), frozen.end());
    out.no_machine = std::vector<int>(no_machine.begin(), no_machine.end());
    // Only racks whose frozen lines are really kept.
    out.frozen.erase(std::remove_if(out.frozen.begin(), out.frozen.end(),
                                    [&](int r) { return !clips_of.count(r); }),
                     out.frozen.end());
    return out;
}

} // namespace core
