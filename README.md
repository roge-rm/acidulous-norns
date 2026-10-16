# acidulous

acidulous puts the machines from my [Acidulous](https://github.com/roge-rm/Acidulous) app on [norns](https://monome.org/docs/norns/) as a multitimbral patch player. It runs the app's own engine, so the patches sound the same as they do there. The app is on [my F-Droid repo](https://hunke.ws/fdroid/repo).

There are 8 parts. Each one plays a machine and patch on its own MIDI channel, so you can play a bassline, a beat and a pad from one keyboard or sequencer. One part at a time can take MPE.

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
- [Saving](#saving)
- [Credits](#credits)

## What you need

- A [norns](https://monome.org/docs/norns/) or [norns shield](https://github.com/okyeron/shieldXL).
- A MIDI keyboard or sequencer, by USB.
- Optional: an [OMX-27](https://github.com/okyeron/OMX-27) with [Quixotic7's firmware](https://github.com/Quixotic7/OMX-27) 1.15.4 or newer, which has REMOTE mode.
- Optional, for MPE: an Exquis or a QuNexus, or any MPE controller.

## Installing

1. In maiden, type `;install https://github.com/roge-rm/acidulous-norns` into the REPL.
2. Load acidulous from **SELECT**.
3. The first time, it installs its SuperCollider plugin and restarts SuperCollider, then loads by itself. This takes about 20 seconds.

If SuperCollider won't start after you've been logged in over ssh, restart the norns.

## Updating

1. In maiden, open the project list, find acidulous-norns and press **update**.
2. Load acidulous again. If the plugin has changed it installs it and restarts SuperCollider, then loads by itself.

## On the norns

The main page lists the parts, each with its channel, machine, patch and a level meter.

| | |
|---|---|
| **E1** | pick a part |
| **E2** | patch |
| **K1** held + **E2** | machine |
| **E3** | the part's volume |
| **K2** | the macro page |
| **K3** | play a note on the part |

The macro page has five of the machine's main controls, like cutoff, resonance, envmod, decay and accent on Reflux. **E2** picks one and **E3** sets it, with **K1** held for fine steps. **K2** goes back.

Every param of every part is in **PARAMETERS > EDIT**, one group per part.

## Parts and MIDI

Part 1 listens on channel 1, part 2 on channel 2 and so on. To change it, set **channel** in the part's group. Two parts on the same channel play together, and **off** leaves a part silent.

**midi from** picks the MIDI device, or **all** to listen to every one.

## MPE

1. Put your controller in MPE mode, on the lower zone.
2. Set **mpe** to **on** in one part's group. That part gets every channel of the MPE controller, with per-note bend, pressure and timbre.
3. **mpe bend** is the bend range, 48 semitones by default. Set it to match your controller.

**mpe from** is **auto** by default, which finds an Exquis or QuNexus by itself. For another controller, pick its port, or the MPE part stays silent. While a part is MPE, the other parts still play from **midi from**.

## OMX-27

Plug it in by USB and acidulous puts it in REMOTE mode.

- The keys play the selected part.
- **AUX** + the two top left keys: octave down and up.
- **AUX** + the first 8 bottom keys: pick parts 1 to 8.
- The five pots are the part's macros. A pot takes over once you sweep it past where the macro is set.
- The encoder picks the patch. Hold it and turn for the machine, and tap it to switch the norns page.
- The little screen shows the part, the patch and the macro you're turning.

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
| Diction | 8 | | |

The measurements are in [docs/machines.md](docs/machines.md). Diction sings a vowel with its built-in voice; the vowel macro picks which. Nexus's Listening, Talking and Sing to the Piano patches play the norns input, so they're silent without one. Turning Cumulus's stretch or width rebuilds its sound, which cuts the notes that are held. The machines that need samples or SoundFonts aren't here yet.

## Saving

Machines, patches and any changed params save with norns PSETs, in **PARAMETERS > PSET**.

## Credits

acidulous is GPL-3.0, see [LICENSE](LICENSE). The engine in `third_party/acidulous` is copied from the Acidulous app, see [its README](third_party/acidulous/README.md).

The OMX-27 is by Denki Oto, with firmware by okyeron and Quixotic7.

Building the plugin is in [docs/building.md](docs/building.md).
