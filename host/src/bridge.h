// OSC link between the core and the norns script, over liblo.
//
// In (UDP, `port`, default 57150). Parts are 0-7, channels 1-16, values 0-1:
//   /mount i:part s:machine        /unmount i:part
//   /note i:part i:ch i:note i:vel  velocity 0 is note off
//   /cc i:part i:ch i:num i:val    /bend i:part i:ch i:value14 (8192 centre)
//   /pressure i:part i:ch i:val
//   /param i:part s:name|i:index f:value01
//   /channel i:part s:name|i:index f:value01   the track's strip: "gain"
//        (0-1.5 linear, so 1/1.5 is unity), "pan", "mute", "solo"
//   /patch i:part s:machine s:patch    mounts the machine if it isn't there
//   /mpe i:part i:members f:bend       members 0 turns MPE off
//   /defs s:machine   /patches s:machine   /machines
//   /state i:part                      for tests
//   /mpe_state                         for tests
// Out (to `reply` on localhost, matron's 10111):
//   /mounted i:part s:machine          "" when emptied
//   /hello                             each time AcidulousStart runs, with an
//        empty engine, so the script sends its parts again
//   /values i:part s:machine s:patch f:value01...   every param, sent just before
//   /patch i:part s:machine s:patch    once it's applied
//   /macros i:part s:machine s:patch (s:param s:label)...   a Nexus patch's
//        macros, just before /values
//   /state i:part s:machine s:patch f:gain01 f:value01...   what the engine
//        holds once everything sent before has been applied
//   /mpe_state i:kind i:members f:bend i:held_mask (i:ch i:note i:bend14
//        i:pressure i:cc74)...   the zone, the member channels the engine
//        holds a note on (bit 0 is channel 1), and what each member channel
//        holding a note last sent, -1 for nothing
//   /def s:machine i:index s:name f:min f:max f:default s:unit i:steps s:curve
//        steps is 0 unless stepped; curve is "lin", "exp" or "step"
//   /defs_end s:machine i:count
//   /patches s:machine s:name...       /machines s:name...
//   /load f:percent f:worst_percent    4 times a second
//   /levels f:peak...                  each part's peak, 20 times a second
//   /error s:text
#pragma once
#include <string>

namespace core
{
class Core;
struct Replier;
}

namespace bridge
{

// Sends replies to the address start() is given. Pass it to the Core.
core::Replier* replier_for_core();
bool start(core::Core* core, const std::string& port, const std::string& reply);
void stop();

} // namespace bridge
