// Which racks of a song play their frozen clips and which play their notes.
// The app's engine.txt carries both: the note lines of every clip and a
// MountFrozen line per frozen clip. A rack with a frozen clip in a scene
// renders that audio instead of its machine, so keeping a rack's MountFrozen
// lines freezes it and dropping them plays its notes.
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace core
{

enum class FrozenMode
{
    Auto,   // frozen for machines that can't run or can't be afforded
    Notes,  // notes, except where the machine can't run
    Frozen, // every frozen clip, as the app plays it
};

// "auto", "notes" or "frozen"; false for anything else.
bool parse_frozen_mode(const std::string& name, FrozenMode& out);

// The share of the 64-frame block, in percent, that a song's scene may cost
// before racks are frozen.
constexpr double kFrozenCpuLimit = 60.0;

// The worst block of a machine on the Pi 4 in percent (docs/machines.md), 10
// for one that isn't measured.
double machine_cost(const std::string& machine);

// The machine a frozen rack holds when its own can't run or its loads were
// dropped: the engine plays no rack without one, and this has no samples and
// costs under 1%. It doesn't render while the rack plays its frozen clip.
constexpr const char* kStandInMachine = "Reflux";

struct FrozenHost
{
    // Whether the plugin can run a machine.
    std::function<bool(const std::string&)> can_run;
    // The song's audio, decoded, as the lines load it. Empty: no budget.
    std::function<int64_t(const std::string&)> audio_bytes;
    int64_t                                    budget = 0;
};

struct FrozenChoice
{
    std::string      text;       // engine.txt with the lines left out
    std::vector<int> frozen;     // racks that keep a MountFrozen line
    std::vector<int> no_machine; // racks holding the stand-in, their own machine and loads dropped
};

// Picks per rack, with the rules in the plan: [mode] Frozen keeps every
// MountFrozen and Notes drops them, except for racks whose machine is
// missing or can't run, which are frozen in every mode. Auto also freezes the
// costliest rack with a frozen clip in each scene whose racks add up to more
// than kFrozenCpuLimit, and then, if the song's audio is over the budget,
// drops the machine and loads of frozen racks whose clips cover every scene
// they have notes in, largest first. A song that is still over is left to
// the caller to refuse.
FrozenChoice choose_frozen(const std::string& text, FrozenMode mode, const FrozenHost& host);

} // namespace core
