// OSC link between the core and the norns script, over liblo.
//
// In (UDP, `port`, default 57150). Parts are 0-7, channels 1-16, values 0-1:
//   /mount i:part s:machine        /unmount i:part
//   /note i:part i:ch i:note i:vel  velocity 0 is note off
//   /cc i:part i:ch i:num i:val    /bend i:part i:ch i:value14 (8192 centre)
//   /pressure i:part i:ch i:val
//   /param i:part s:name|i:index f:value01
//   /channel i:part s:name|i:index f:value01   the track's strip: "gain"
//        (0-1.5 linear, so 1/1.5 is unity), "pan" (0.5 centre), "mute",
//        "solo", "sendreverb", "senddelay". A send above 0 mounts its effect
//   /send i:slot s:name f:value01      a send effect's param: slot 0 the
//        Reverb (size, damp, tone, predelay), 1 the Delay (time, feedback,
//        tone, pingpong). Kept until the effect is mounted
//   /fx i:part i:slot s:name f:value01  a param of the part's insert in slot
//        0 or 1, by name, as /inserts lists them
//   /master s:name f:value01           the master's params, e.g. "volume"
//        (0-1.5 linear)
//   /meters i:on                       sends /meters while on
//   /graph i:part                      a Nexus part's modules
//   /scope i:part i:slot               the scope on a Nexus module's first
//        output; slot -1 closes it
//   /patch i:part s:machine s:patch    mounts the machine if it isn't there
//   /mpe i:part i:members f:bend       members 0 turns MPE off
//   /defs s:machine   /patches s:machine   /machines
//   /load i:part s:kind s:path [i:arg]  decodes a file on the worker and
//        mounts it into the part's machine. Kinds: "pad" (Forage, arg the
//        pad 0-12), "kit" (Forage, a folder: its first 13 audio files by name
//        on pads 0-12), "sf2" (Mosaic, arg the preset's index in /presets),
//        "folder" (Mosaic, a folder of WAVs mapped by the note names in their
//        names), "sample" (Dice, Pollen, Molt), "clear" (empties the part's
//        machine). An empty path clears a pad or sample. WAV, AIFF and FLAC;
//        SoundFonts over 64 MB are refused, and so is a load that would take
//        the decoded samples of every part past 320 MB, or a path with | in it.
//   /presets s:path                    a SoundFont's presets
//   /shape s:path                      a file's length and shape, for the picker
//   /hear [s:path]                     plays a file once over everything, up
//        to 30 seconds of it; no path stops it
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
//   /inserts i:part (i:slot s:type i:count (s:name f:min f:max f:default
//        s:unit i:steps s:curve f:value01)*count)*   the part's inserts, one
//        after each /patch and /unmount (a /patch that fails takes the last
//        patch's inserts off); just the part when it has none. Params as
//        /def has them; values are the patch's, or the defaults
//   /def s:machine i:index s:name f:min f:max f:default s:unit i:steps s:curve
//        steps is 0 unless stepped; curve is "lin", "exp" or "step"
//   /defs_end s:machine i:count
//   /patches s:machine s:name...       /machines s:name...
//   /load f:percent f:worst_percent    4 times a second
//   /levels f:peak...                  each part's peak, 20 times a second
//   /meters f:peak f:lufs              the master's peak and short-term
//        loudness, 20 times a second while /meters is on
//   /graph i:part s:patch (i:slot s:type)...   the patch's modules in slot
//        order, without voice, perf, macro, out and scope; after each Nexus
//        /patch too
//   /scope i:part i:slot f:point...    126 points of the open scope, -1 to
//        1, 12 samples apart (32 ms), 14 times a second
//   /loaded i:part s:kind s:path i:arg s:name s:machine   once it's mounted,
//        machine being what the part had when it loaded. A kit sends
//        one "pad" per pad (path "" for an emptied one), then "kit" with arg
//        the number of pads filled. name is the file's name without its
//        extension, the preset's name or the folder's name.
//   /presets s:path s:name...          in file order, none if it can't be read
//   /shape s:path f:seconds i:rate i:cut f:column...   128 columns of the
//        loudest point, 0-1 against the file's peak; cut is 1 if it's longer
//        than the 30 seconds read. seconds 0 if it can't be read.
//   /hear s:path f:seconds             once it starts, "" when stopped
//   /error s:text [i:part s:kind s:path i:arg s:machine]   a failed /load has
//        the rest
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
