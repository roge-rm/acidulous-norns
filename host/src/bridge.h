// OSC link between the core and the norns script, over liblo.
//
// In (UDP, `port`, default 57150). Parts are 0-15 (the script uses 0-7
// outside songs), channels 1-16, values 0-1:
//   /mount i:part s:machine        /unmount i:part
//   /note i:part i:ch i:note i:vel  velocity 0 is note off
//   /cc i:part i:ch i:num i:val    /bend i:part i:ch i:value14 (8192 centre)
//   /pressure i:part i:ch i:val
//   /param i:part s:name|i:index f:value01
//   /channel i:part s:name|i:index f:value01   the track's strip: "gain"
//        (0-1.5 linear, so 1/1.5 is unity), "pan" (0.5 centre), "mute",
//        "solo", "sendreverb", "senddelay". A send above 0 mounts its effect
//   /send i:slot s:name f:value01      a send effect's param: slot 0 the
//        Reverb (size, damp, tone, predelay ...), 1 the Delay (time, feedback,
//        tone, mode ...). Kept until the effect is mounted. With a song
//        loaded it moves the song's send and the script's values stay
//   /fx i:part i:slot s:name f:value01  a param of the part's insert in slot
//        0 to 3, or of its mod slot ("mod_source" ...), by name, as /inserts
//        lists them. A patch's inserts go in slots 0-1; 2-3 are the script's
//   /insert i:part i:slot s:type       mounts a new effect with its defaults
//        in the part's own slot 2 or 3, or empties it with "", then replies
//        /inserts with what the engine holds. Patch and machine changes keep
//        it; a song load or unload empties it
//   /effects                           the insert effects there are
//   /mod i:part i:slot s:type          mounts a modifier ("Scale", "Chord",
//        "Arp") in the part's slot 0 to 2, on and with its defaults, or
//        empties the slot with "". Notes go through slot 0, then 1, then 2.
//        They belong to the part: machine and patch changes keep them
//   /mod_bypass i:part i:slot i:bypass   1 bypasses the slot's modifier
//   /mod_param i:part i:slot s:name f:value01   a param of the slot's
//        modifier, by name, as /mod_defs lists them
//   /mods_off i:part   ends what the part's modifiers play and forgets
//        their held and latched notes, keeping their params
//   /mods i:part   /mod_defs
//   /master s:name f:value01           the master's params, e.g. "volume"
//        (0-1.5 linear). With a song loaded it moves the song's master;
//        the script's own, from /master sent with no song, come back when
//        the song goes
//   /meters i:on                       sends /meters while on
//   /tempo f:bpm                       the engine's tempo (20-999); with
//        no args it replies /tempo with the tempo the engine runs at
//   /beat f:beat f:bpm                 the norns clock's beat and tempo, a
//        few times a second. The engine follows it (arps, synced LFOs and
//        delays) and play waits for its next bar line; beat 0 mod 4 is a bar
//   /beat_release                      back to the /tempo tempo; a second
//        with no /beat does the same
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
//        "zones" (Mosaic, path a .acpreset, arg its patch index) loads the
//        patch's zones, its files found as /preset_load does with the
//        s:root args after arg.
//   /preset_save i:part s:path s:name i:nroots s:root... (s:key s:value)...
//        writes the part's sound as a .acpreset the app can read, making its
//        folder. The settings are the files the script knows (sample,
//        pNN_sample, sf2, sf2preset) as absolute paths; the plugin adds a
//        Nexus graph, a Formulate program and Mosaic's zones, and writes
//        every file path relative to the first root it's under: a root
//        "/dir/" as "path", one "label=/dir/" as "label:path". Waits for a
//        queued mount; "busy" if it never lands. Won't replace a file of
//        more than one patch or with the app's samples in it, or while a song
//        is loaded
//   /preset_load i:part s:path i:index s:tag i:files s:machine s:name s:root...
//        puts the patch called name (or at index, if none is) on the part as
//        /patch does, mounting its machine, with tag as the patch name in its
//        replies. files 1 loads the files it names too: "label:path" under
//        that root, a plain relative path from the first unlabelled root that
//        has it, then the file's own folder. A patch from before version 3 is
//        refused; a version 3 Trinity patch has its destinations moved, and
//        below version 6 a Delay insert's pingpong becomes mode. When
//        it fails the part still gets machine, as does a /patch that can't be
//        found, with /values of what it has
//   /preset_scan s:folder              the patches of the .acpreset files in it
//   /preset_delete s:path i:index      takes the patch out of the file, or
//        deletes the file when it's the last or index is -1. A file with the
//        app's samples (media/ or engine/) isn't rewritten
//   /presets s:path                    a SoundFont's presets
//   /shape s:path                      a file's length and shape, for the picker
//   /hear [s:path]                     plays a file once over everything, up
//        to 30 seconds of it; no path stops it
//   /song_load s:path [s:songs_dir [s:frozen]]   a song zip from the app, unpacked
//        into songs_dir/<zip name>/ (default ~/dust/data/acidulous/songs,
//        else a folder under ~/dust/data), or a folder holding one already
//        unpacked. Every part is emptied, then its engine.txt replayed
//        (machines, patches, inserts, sends, modifiers, the arrangement,
//        frozen clips) and each part takes what its rack has. Messages for
//        any part wait for it. A zip over 128 MB unpacked, one without
//        engine.txt, with a name outside its folder or inside the folder it
//        would unpack into, and a song whose samples and frozen clips go over
//        the sample budget are refused, and what's loaded and the old folder
//        stay. Its audio counts in the budget after. If unpacking beside
//        what's loaded would go over the budget, the parts are emptied
//        first. A song that fails after the parts were emptied (that, or the
//        audio stopping during the replay) loads the song before it again
//        from its folder, unless the new one replaced it; if that can't be
//        done, /song_unloaded 1 comes before its /song_loaded; the old song's
//        /song_loaded has the path it was first loaded from. frozen is
//        which racks play the zip's frozen clips instead of their notes:
//        auto (default), notes or frozen. frozen plays every frozen clip
//        as the app does, notes drops them, and auto freezes the racks
//        whose machine is missing or can't run, the costliest ones in a
//        scene over 60% (docs/machines.md) and, when the song is over the
//        sample budget, racks whose clips cover all their notes, which then
//        drop their samples and machine. A frozen rack that loses its
//        machine holds a Reflux instead (the engine plays no rack without
//        one) and is an empty part to the script; its clips have no
//        notes, so it plays only where it has a frozen clip. A rack whose machine
//        can't run is frozen in every mode. A teardown
//        while the audio is stopped is finished once it's back. The
//        transport is left stopped
//   /song_unload                       empties every part and what the song
//        put around them (arrangement, frozen clips, master and group
//        inserts, tunings, channel strips, master and perform params) and
//        puts the sends, tempo, swing unit, loop-song and the script's
//        master back to the plugin's; does nothing but reply when no song is loaded
//   /song_play [i:scene]               plays the loaded song from the top of
//        the scene (0 is the first), or from the scene it's on
//   /song_stop                         stops it and drops a queued scene
//   /song_queue i:scene|s:id           the scene to play when the current
//        one has finished its repeats, by index, or by song.json id or
//        engine id (as /song_scenes lists them); -1 cancels
//   /song_position                     replies /song_position now
//        While a song is loaded or loading, its own tempo rules: /tempo with
//        a tempo and /beat are ignored, and a followed beat is let go. After
//        /song_unload they work again
//   /state i:part                      for tests
//   /mpe_state                         for tests
// Out (to `reply` on localhost, matron's 10111):
//   /mounted i:part s:machine          "" when emptied
//   /hello                             each time AcidulousStart runs, with an
//        empty engine, so the script sends its parts again
//   /values i:part s:machine s:patch f:value01...   every param, sent just before
//   /patch i:part s:machine s:patch    once it's applied
//   /macro_names i:part s:name x8      a patch's macro names, all "" for none
//        by "|", or "" when it names none; just before /values, and after
//        each /mounted and a failed /patch with ""
//   /macros i:part s:machine s:patch (s:param s:label)...   a Nexus patch's
//        macros, just before /values
//   /state i:part s:machine s:patch f:gain01 f:value01...   what the engine
//        holds once everything sent before has been applied
//   /mpe_state i:kind i:members f:bend i:held_mask (i:ch i:note i:bend14
//        i:pressure i:cc74)...   the zone, the member channels the engine
//        holds a note on (bit 0 is channel 1), and what each member channel
//        holding a note last sent, -1 for nothing
//   /inserts i:part (i:slot s:type i:count (s:name f:min f:max f:default
//        s:unit i:steps s:curve f:value01)*count i:mcount (the same)*mcount)*
//        the part's inserts, slots 0-3, one after each /patch and /unmount (a
//        /patch that fails takes the last patch's inserts off); just the part
//        when it has none. Params as /def has them, then the slot's mod
//        params ("~mod": mod_source, mod_shape ... mod_step16); values are
//        the patch's, or the defaults
//   /mods i:part (i:slot s:type i:bypass i:count (s:name f:value01)*count)*
//        both slots as the engine holds them once everything sent before
//        has landed; an empty slot has type "" and count 0
//   /tempo f:bpm                       once everything sent before has landed
//   /mod_defs s:type i:count (s:name f:min f:max f:default s:unit i:steps
//        s:curve)*count   one per modifier type, params as /def has them
//   /def s:machine i:index s:name f:min f:max f:default s:unit i:steps s:curve
//        steps is 0 unless stepped; curve is "lin", "exp" or "step"
//   /defs_end s:machine i:count
//   /patches s:machine s:name...       /machines s:name...
//   /effects s:name...
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
//   /song_loaded s:path s:folder s:error i:skipped i:count (i:part s:machine
//        s:patch s:track)*count   after each part's /mounted, /macro_names,
//        /macros and /graph (Nexus), /values, /patch, /sends, /inserts (with the
//        song's values), /strip and /mods. error "" when it loaded; skipped is the
//        engine lines that failed, each also an /error "song: line N: ...".
//        patch and track are the names in song.json, "" if it has none
//   /strip i:part f:gain f:pan i:mute i:solo   a song part's channel strip
//        as /channel takes it (gain 0-1.5 linear, pan 0.5 centre), read back
//        after a song loads
//   /song_frozen i:count (i:part i:machine s:track)*count   the parts that
//        play frozen clips, just before /song_loaded; machine is 0 for a
//        part with no machine of its own (it plays only its clips, and isn't
//        in /song_loaded), track is its name in song.json
//   /song_scenes i:count (s:name s:id s:engine_id i:repeat)*count   the
//        song's scenes in play order, just before /song_loaded: names and
//        ids from song.json ("scene N" for one without a name), the engine
//        id as a decimal string
//   /song_position i:playing i:scene i:repeat i:tick i:queued f:bpm   5
//        times a second while a song plays and once a second stopped: the
//        scene's index, its pass (0 first), the tick in that pass, the
//        queued scene (-1 none) and the tempo it plays at
//   /song_master f:volume01            the song's master volume, as
//        /master takes it, just before /song_loaded
//   /song_sends f:size01 f:feedback01   the song's Reverb size and Delay
//        feedback as /send takes them, -1 for a send it doesn't have, just
//        before /song_loaded
//   /song_unloaded i:emptied           after the parts' /mounted "" and
//        /inserts; emptied is 1 when a song or a failed load emptied them
//   /preset_saved i:part s:path s:result   "ok" or what went wrong
//   /preset_loaded i:part s:tag s:name s:error i:index   after its /values and /patch
//        and the /loaded for its files; error "" when it loaded. A missing
//        file is an /error of its own and the rest still loads
//   /preset_scan s:folder (s:file i:index s:name s:unit i:count i:packed)...
//        by file name, unit the machine, count the file's patches, packed 1
//        when it has the app's samples; files that can't be read are left out
//   /preset_deleted s:path i:index s:result   "ok" or what went wrong
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
