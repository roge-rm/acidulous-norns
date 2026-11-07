# acidulous

acidulous puts the machines from my [Acidulous](https://github.com/roge-rm/Acidulous) app on [norns](https://monome.org/docs/norns/) as a multitimbral patch player. It runs the app's own engine, so the patches sound the same as they do there. The app is on [my F-Droid repo](https://hunke.ws/fdroid/repo).

There are 8 parts, or 16 if you set **parts** to 16. Each one plays a machine and patch on its own MIDI channel, so you can play a bassline, a beat and a pad from one keyboard or sequencer. One part at a time can take MPE.

![The play page with Reflux, Hexbeat, Tine, Fret, Brazen, Aviary, Mosaic and Diction playing in turn](docs/images/play.gif)

<p>
<img src="docs/images/parts.png" alt="The parts page with 8 parts playing" width="400">
<img src="docs/images/mixer.png" alt="The mixer" width="400">
<img src="docs/images/card.png" alt="Reflux's filter card" width="400">
<img src="docs/images/env.png" alt="Mosaic's amp envelope card" width="400">
<img src="docs/images/mods.png" alt="The mods card with Scale, Chord and Arp" width="400">
<img src="docs/images/patches.png" alt="Tine's patch list with its family tabs" width="400">
<img src="docs/images/machines.png" alt="The machine picker" width="400">
<img src="docs/images/splash.png" alt="The splash screen" width="400">
</p>

This has been tested on my [shieldXL](https://github.com/okyeron/shieldXL) with a Pi 4 inside. It should run on a Pi 3 norns too, with fewer of the heavy machines at once. If you try it on one please let me know how it goes, or open an issue here.

Enjoy!<br>
Dan

---

- [What you need](#what-you-need)
- [Installing](#installing)
- [Updating](#updating)
- [On the norns](#on-the-norns)
- [Parts and MIDI](#parts-and-midi)
- [MPE](#mpe)
- [OMX-27](#omx-27)
- [Machines](#machines)
- [Samples](#samples)
- [Saving](#saving)
- [Credits](#credits)

## What you need

- A [norns](https://monome.org/docs/norns/) or [norns shield](https://github.com/okyeron/shieldXL).
- A MIDI keyboard or sequencer, by USB.
- Optional: an [OMX-27](https://github.com/okyeron/OMX-27) with [Quixotic7's firmware](https://github.com/Quixotic7/OMX-27) 1.15.4 or newer, which has REMOTE mode.
- Optional, for MPE: an Exquis or a QuNexus, or any MPE controller.

## Installing

1. In maiden, open the project list, find acidulous and press **install**. Or over ssh, `git clone https://github.com/roge-rm/acidulous-norns ~/dust/code/acidulous`. The folder needs to be called acidulous for it to show as acidulous in **SELECT**.
2. Load acidulous from **SELECT**.
3. The first time, it installs its SuperCollider plugin and restarts SuperCollider, then loads by itself. This takes about 20 seconds. After that, the acidulous icon shows while it starts.

If SuperCollider won't start after you've been logged in over ssh, restart the norns.

## Updating

1. In maiden, open the project list, find acidulous and press **update**.
2. Load acidulous again. If the plugin has changed it installs it and restarts SuperCollider, then loads by itself.

## On the norns

### Play

The play page shows the selected part's machine, patch and five macros, with a little picture of the machine that moves as it plays. Each machine group has its own look. The bottom line shows the part number, the MPE mark and keys, the part's key and scale when it has a Scale, and the load when it's busy. The macros match the OMX-27's pots.

- E1 changes machine, E2 picks the patch or a macro and E3 changes it. Hold K1 for fine steps.
- K2 opens patches, K1 + K2 opens machines and K3 plays a note.
- Hold K1 and turn E1 to change pages: play, parts, card, mixer, sends and song.

### Parts and patches

The parts page shows each machine, patch and level meter. The bar on the left lights when the part plays.

| | |
|---|---|
| **K1** held + **E1** | page: play, parts, card, mixer, sends, song, a few detents a page |
| **E1** | pick a part |
| **E2** | patch |
| **K1** held + **E2** | machine |
| **E3** | the part's volume |
| **K2** | patch list |
| **K1** held + **K2** | machine picker, or on a card of fx 3 or 4 or on a mods card, that slot's saved presets |
| **K3** | play a note on the part |
| **K1** held + **K3** | load samples, see [Samples](#samples), or the scope on a Nexus part |

Patches are grouped into families and machines into the app's groups. In either picker, E1 changes tab, E2 selects, K3 loads and K2 closes. Loading the current patch again restores its values.

### Cards

Cards group the machine's controls as in the app. E1 picks a card, E2 a control and E3 changes it. Hold K1 for fine steps. Hold K1 and turn E2 right to jump to the amp envelope, or left to return. Kit cards start with a pad selector. K2 returns to play.

Effect cards follow the machine's. A patch's effects fill inserts 1 and 2; on the inserts card, fx 3 and fx 4 pick effects of your own, which stay when you change patches or machines. Each effect has a mod card: pick a source (LFO, steps, follow or another track) and up to two of its knobs to move, each with a depth. Edits and your effects save in PSETs, and edits to a patch's effects reset when you choose a different patch. A Venue has a room knob first: the app's five rooms, or K1+K3 on its card to pick an impulse file.

Each part has three note modifier slots for Scale, Chord or Arp. Set them on the mods card, then use their cards to change settings. Notes pass through slot 1, then 2, then 3. Scale and Chord share key and scale. Modifiers stay when you change patches or machines and save in PSETs. An active Scale shows its key and scale on the play page.

The arp, synced LFOs and delay follow PARAMETERS > CLOCK, from any clock source. Arp sync free follows clock beats; restart begins with the first key.

Pan and other controls that cross zero stop at the centre. Turn three more detents to pass it.

### Mixer and sends

The mixer has a meter, fader and pan for each part, with m for mute and s for solo. The out strip has master level and a lim indicator when the limiter is active. Loudness at the top right covers the last three seconds in LUFS.

| Mixer | |
|---|---|
| **E1** | pick a part, or out after the last part |
| **E2** | pan |
| **E3** | level, or the master level on out |
| **K2** | mute |
| **K3** | solo |

The sends page has reverb and delay sends for each part. On out, E2 and E3 change reverb size and delay feedback, or with K1 held the reverb's model and the delay's mode. The space model has no size, so it shows a dash.

| Sends | |
|---|---|
| **E1** | pick a part, or out after the last part |
| **E2** | reverb send |
| **E3** | delay send |

Reverb and delay start when first used. Other settings are under PARAMETERS > EDIT > sends and master, with the knobs of the reverb's model, and taps and spacing for the delay's multi-tap mode. Delay time is in notes at the norns tempo.

### Songs

The song page plays a song exported from the app as a zip. Put zips in `dust/data/acidulous/songs`, or in `dust/audio` up to three folders down. Loading one unpacks it into `dust/data/acidulous/songs/<name>/` and replaces the parts. The page shows the scene playing, its pass and the tempo, and a row per scene: the bar on the left lights on the one playing and next marks the queued one.

| Song | |
|---|---|
| **K1** held + **K2** | songs, or **K2** when none is loaded. K3 loads, none unloads, K2 closes |
| **K2** | play or stop |
| **E2** | pick a scene |
| **K3** | play from that scene, or queue it to follow the one playing |

A song plays at its own tempo, and norns' clock follows it while its source is internal. While a song is loaded its tracks are the parts: the parts page shows each track's name with its patch, and the play, card, mixer and sends pages, the patch list and the machine picker work on them. Track n plays from MIDI channel n, and a song with more than 8 tracks shows 16 parts. Your edits to a song stay until it's unloaded and aren't saved into the song, and a song's tracks can't be saved as presets. The out strip on the mixer moves the song's master, and the one on the sends page its reverb size and delay feedback. Your own parts, master level, sends and tempo are kept for when it's unloaded, and unloading puts them back. A PSET saved with a song loaded saves your own parts and the song's name, and edits to the song's tracks stay out of it. PSETs keep the loaded song: one saved with the same song leaves it playing, one with another song loads that one, and one without a song unloads it and loads its parts.

### Scope and settings

The Nexus scope shows its modules and the selected module's output. E2 selects, K3 plays a note and K2 closes. Changing modules briefly dips the sound.

PARAMETERS > EDIT has every part's settings. After three minutes without input, both screens show the acid bath screensaver. Norns keys and encoders wake it without acting; notes and OMX-27 controls wake it and play normally. Set screensaver after to another time or off.

## Parts and MIDI

Part 1 listens on channel 1, part 2 on channel 2 and so on. To change it, set **channel** in the part's group. Two parts on the same channel play together, and **off** leaves a part silent.

**parts** in PARAMETERS > EDIT is 8 or 16. With 16, parts 9 to 16 listen on channels 9 to 16, and the parts, mixer and sends pages show 8 at a time: turn E1 past part 8 to get to 9-16, and the header shows which 8. Setting it back to 8 stops parts 9-16 and frees their CPU, and setting 16 again brings them back as they were. PSETs save it, and older ones load as 8. More parts use more CPU, so the overload warning comes sooner with more than 8 machines.

**midi from** picks the MIDI device, or **all** to listen to every one.

## MPE

Plug in your MPE controller, pick a part and play. The selected part gets every channel of the controller, with per-note bend, pressure and timbre, and the parts page marks it with three little bars before its meter. If **mpe part** is set to a part and the controller isn't plugged in, that part shows **mpe: no device**. Notes you're holding stop when you pick another part.

- **mpe part** is **follow** by default, for the selected part. Set it to a part number to keep MPE on that part, or **off**.
- **mpe from** is **auto** by default. It finds an Exquis or QuNexus by name, a controller that sends the MPE configuration message, or one that plays notes on more than one channel with bend or pressure on each. Until then its first note plays as normal MIDI. If yours isn't found, pick its port.
- **mpe bend** in each part's group is the bend range, 48 semitones by default. It's set by itself from a controller that sends its range (RPN 0), and to 2 for a QuNexus. Otherwise set it to match your controller.

The lower and upper zones both work. The MPE part still plays from **midi from** on its own channel, and so do the other parts.

## OMX-27

Plug it in by USB and acidulous puts it in REMOTE mode.

- The keys play the selected part.
- **AUX** + the two top left keys: octave down and up.
- **AUX** + the first 8 bottom keys: pick parts 1 to 8, or 9 to 16 when those are showing.
- **AUX** + the 9th bottom key: with 16 parts, switch between parts 1-8 and 9-16. It's lit while 9-16 show.
- **AUX** + the top keys 3 to 6 turn the part's scale, chord, arp and latch on and off, lit while on. Latch is the arp's, or the chord's when there's no arp. A part has 3 modifier slots; with all of them taken the screen says **slots full**. Off keeps the modifier's settings.
- **AUX** + key 3 held: a key picks the root and the encoder the scale, and tapping the encoder swaps snap and degree. In degree mode the white keys play the scale, starting from the root.
- **AUX** + key 4 held: the encoder picks the chord type.
- With **AUX** held the pots are the arp's rate, gate, mode and octaves and the chord type, picked up the same way as the macros.
- The five pots are the part's macros. A pot takes over once you sweep it past where the macro is set.
- The encoder picks the patch. Hold it and turn for the machine, and tap it to swap the parts and card pages, or close the patch list or machine picker.
- The little screen shows the part, the patch and the macro you're turning, or the scale, chord or arp you've changed.

**omx velocity** in **PARAMETERS > EDIT** sets how hard its keys play.

## Machines

How many parts of each machine can play at once on a Pi 4. When you mix machines it adds up, so a Trinity or Ratio part leaves less room for the rest.

| Machine | Parts | Machine | Parts |
|---|---|---|---|
| Reflux | 8 | Chanter | 8 |
| Hexbeat | 8 | Brazen | 8 |
| Trinity | 2 | Fathom | 8 |
| Ratio | 2 | Resonance | 8 |
| Formulate | 8 | Draw | 8 |
| Genesis | 8 | Sympath | 6 |
| Tine | 8 | Filament | 5 |
| Palm | 8 | Manual | 5 |
| Tongue | 8 | Hammer | 5 |
| Timber | 8 | Cipher | 5 |
| Aviary | 8 | Cumulus | 4 |
| Fret | 8 | Nexus | 1 to 2 |
| Diction | 8 | Forage | 8 |
| Mosaic | 4 | Dice | 8 |
| Pollen | 2 to 4 | Molt | 8 |

The measurements are in [docs/machines.md](docs/machines.md). Diction sings a vowel with its built-in voice; the vowel macro picks which. Nexus's Listening, Talking and Sing to the Piano patches play the norns input, so they're silent without one. Changing one of Cumulus's spectrum params (partials to bodd in the params menu) rebuilds its sound, which cuts the notes that are held, so none of them are macros. A Nexus patch's macros are the knobs the patch sets, then the knobs of its modules. Bias isn't here yet.

## Samples

Forage, Mosaic, Dice, Pollen and Molt play your samples. Pick one of them on a part and press **K1** + **K3** to open the file list, starting in dust/audio, with a USB stick as a second place when one is in.

Picking one on a part loads a sound to start with: the 808 kit from dust/audio/common on Forage, a soft horn made with Trinity on Mosaic, and drumev, hermit_leaves and whirl1 from dust/audio on Dice, Pollen and Molt. If that file isn't there the part shows **empty, k1+k3**. A PSET keeps the files it was saved with, or none.

| | |
|---|---|
| **E2** | move |
| **K3** | open a folder, or hear the file |
| **K1** held + **K3** | load the file |
| **K2** | back, and close at the top |

- **Forage** is a drum kit with 13 pads. **E3** picks the pad a file goes on, and it moves to the next pad after each one so you can build a kit. **use this folder** puts the folder's first 13 files, by name, on the pads.
- **Mosaic** plays a SoundFont: pick the .sf2, then one of its presets with **K3**. Or **use this folder** on a folder of WAVs. Files named with a note, like `piano C4.wav` or `pad F#3.wav`, go on that note, and files without one are spread across the keys.
- **Dice**, **Pollen** and **Molt** take one file each. Pollen with nothing loaded, as in a PSET saved that way, plays the norns input, and **live input** at the top of the file list empties it again.
- The file list also shows the app's .acpreset files, on any part. **K1** + **K3** on one copies it to the saved presets of its machine, or the effect presets of fx 3 and 4 or the mods card for an effect or modifier preset. A name already there gets " 2", " 3" and so on. Files that aren't presets, and Scale presets, are refused with a short message.

WAV, AIFF and FLAC are supported. MP3 files cannot be loaded. Dice, Pollen, Molt and Forage's pads use the first 30 seconds of a file. Samples across all parts can use 320 MB, with a 64 MB limit per SoundFont. Loads past either limit show an error.

USB files are copied to dust/audio/acidulous/usb, so they stay available after the stick is removed. File names containing `|` cannot be loaded.

## Saving

Machines, patches, any changed params and the samples each part has loaded save with norns PSETs, in **PARAMETERS > PSET**. The samples stay where they are, so a PSET needs them in the same place to load them again.

You can save a part's sound as a patch of your own, which the Acidulous app can open too. In the patch list press **K1** + **K3**, type a name and pick **OK**. It's saved with its edits, macros and effects in dust/data/acidulous/presets/ under the machine's name, and shows in a **saved** tab at the start of the list. **K3** loads it like any patch, and **K1** + **K3** on it deletes it after asking. A PSET remembers a saved patch a part was playing.

Presets from the app go in the same folder, or pick the .acpreset in the file picker, see [Samples](#samples). They load with their samples, voices and slices. The files they carry are unpacked to dust/audio/acidulous/presets/, in a folder named after the machine and the preset. Deleting the preset deletes that folder too.

Effect and modifier presets work the same way. On a card of fx 3 or 4 or on the mods card, **K1** + **K2** opens that slot's list, **K3** loads one, **K1** + **K3** saves the slot or deletes the one you're on.

**K1** + **K3** saving a patch packs the files it uses into the preset, up to 64 MB. Past that, the files stay paths relative to dust/audio, so put them in the same place on another norns.

## Credits

acidulous is GPL-3.0, see [LICENSE](LICENSE). The engine in `third_party/acidulous` is copied from the Acidulous app, see [its README](third_party/acidulous/README.md).

The OMX-27 is by Denki Oto, with firmware by okyeron and Quixotic7.

Building the plugin is in [docs/building.md](docs/building.md).
