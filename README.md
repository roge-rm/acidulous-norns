# acidulous

acidulous puts the machines from my [Acidulous](https://github.com/roge-rm/Acidulous) app on [norns](https://monome.org/docs/norns/) as a multitimbral patch player. It runs the app's own engine, so the patches sound the same as they do there. The app is on [my F-Droid repo](https://hunke.ws/fdroid/repo).

There are 8 parts. Each one plays a machine and patch on its own MIDI channel, so you can play a bassline, a beat and a pad from one keyboard or sequencer. One part at a time can take MPE.

![The parts page](docs/images/parts.png) ![A Reflux card](docs/images/card.png) ![The patch list](docs/images/patches.png) ![The mixer](docs/images/mixer.png)

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

1. In maiden, type `;install https://github.com/roge-rm/acidulous-norns` into the REPL.
2. Load acidulous from **SELECT**.
3. The first time, it installs its SuperCollider plugin and restarts SuperCollider, then loads by itself. This takes about 20 seconds.

If SuperCollider won't start after you've been logged in over ssh, restart the norns.

## Updating

1. In maiden, open the project list, find acidulous-norns and press **update**.
2. Load acidulous again. If the plugin has changed it installs it and restarts SuperCollider, then loads by itself.

## On the norns

The parts page lists the parts, each with its machine, patch and a level meter. The bar on the left lights while a part is playing.

| | |
|---|---|
| **K1** held + **E1** | page: parts, card, mixer, sends, a few detents a page |
| **E1** | pick a part |
| **E2** | patch |
| **K1** held + **E2** | machine |
| **E3** | the part's volume |
| **K2** | patch list |
| **K1** held + **K2** | machine picker |
| **K3** | play a note on the part |
| **K1** held + **K3** | load samples, see [Samples](#samples), or the scope on a Nexus part |

The patch list has the machine's patches in families, like classic, motion and short on Reflux, and the machine picker has the machines in the app's groups, each with a short line. In both **E1** changes the tab, **E2** moves and **K3** loads the patch or mounts the machine. **K2** closes.

The card page has the machine's controls in the same cards as the app, like osc, filter, play and out on Reflux. **E1** picks the card, **E2** the control and **E3** sets it, with **K1** held for fine steps. Cards for one pad of a kit start with a pad control. A patch's effects, like Saw Lead's distortion and delay, have cards after the machine's. Effect settings you change are saved in a PSET and go when you pick another patch. **K2** goes back.

Pan and the card controls that go either side of 0 stop at the centre. Turn 3 more detents the same way to go past it.

The mixer page has a strip per part, with its meter, fader and pan, and **m** or **s** under a muted or soloed part. The out strip on the right has the master meter and fader, and **lim** lights while the limiter is at its ceiling. The top right shows the loudness in LUFS over the last 3 seconds.

| Mixer | |
|---|---|
| **E1** | pick a part, or out after part 8 |
| **E2** | pan |
| **E3** | level, or the master level on out |
| **K2** | mute |
| **K3** | solo |

The sends page has a reverb and a delay knob per part. On out they're the reverb's size and the delay's feedback.

| Sends | |
|---|---|
| **E1** | pick a part, or out after part 8 |
| **E2** | reverb send |
| **E3** | delay send |

The reverb and delay start the first time a part sends to them, so they use no CPU until then. Their other settings are in **PARAMETERS > EDIT > sends and master**. The delay's time is in notes at 120 bpm.

The scope shows a Nexus patch's modules in a chain, and the output of the framed one. **E2** picks the module, **K3** plays a note and **K2** closes. Picking a module dips the sound for a moment.

Every param of every part is in **PARAMETERS > EDIT**, one group per part.

## Parts and MIDI

Part 1 listens on channel 1, part 2 on channel 2 and so on. To change it, set **channel** in the part's group. Two parts on the same channel play together, and **off** leaves a part silent.

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
- **AUX** + the first 8 bottom keys: pick parts 1 to 8.
- The five pots are the part's macros. A pot takes over once you sweep it past where the macro is set.
- The encoder picks the patch. Hold it and turn for the machine, and tap it to swap the parts and card pages, or close the patch list or machine picker.
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
| Diction | 8 | Forage | 8 |
| Mosaic | 4 | Dice | 8 |
| Pollen | 2 to 4 | Molt | 8 |

The measurements are in [docs/machines.md](docs/machines.md). Diction sings a vowel with its built-in voice; the vowel macro picks which. Nexus's Listening, Talking and Sing to the Piano patches play the norns input, so they're silent without one. Changing one of Cumulus's spectrum params (partials to bodd in the params menu) rebuilds its sound, which cuts the notes that are held, so none of them are macros. A Nexus patch's macros are the knobs the patch sets, then the knobs of its modules. Bias isn't here yet.

## Samples

Forage, Mosaic, Dice, Pollen and Molt play your samples. Pick one of them on a part and press **K1** + **K3** to open the file list, starting in dust/audio, with a USB stick as a second place when one is in.

| | |
|---|---|
| **E2** | move |
| **K3** | open a folder, or hear the file |
| **K1** held + **K3** | load the file |
| **K2** | back, and close at the top |

- **Forage** is a drum kit with 13 pads. **E3** picks the pad a file goes on, and it moves to the next pad after each one so you can build a kit. **use this folder** puts the folder's first 13 files, by name, on the pads.
- **Mosaic** plays a SoundFont: pick the .sf2, then one of its presets with **K3**. Or **use this folder** on a folder of WAVs. Files named with a note, like `piano C4.wav` or `pad F#3.wav`, go on that note, and files without one are spread across the keys.
- **Dice**, **Pollen** and **Molt** take one file each. Pollen with nothing loaded plays the norns input.

WAV, AIFF and FLAC work, MP3 doesn't. Dice, Pollen, Molt and Forage's pads take the first 30 seconds of a file. All the parts' samples together can take 320 MB of memory once loaded, and SoundFonts over 64 MB are too big for the norns; a load past either shows an error instead. Files on a USB stick are copied into dust/audio/acidulous/usb when you load them, so the part still has them once the stick is out. File names with a | in them can't be loaded.

## Saving

Machines, patches, any changed params and the samples each part has loaded save with norns PSETs, in **PARAMETERS > PSET**. The samples stay where they are, so a PSET needs them in the same place to load them again.

## Credits

acidulous is GPL-3.0, see [LICENSE](LICENSE). The engine in `third_party/acidulous` is copied from the Acidulous app, see [its README](third_party/acidulous/README.md).

The OMX-27 is by Denki Oto, with firmware by okyeron and Quixotic7.

Building the plugin is in [docs/building.md](docs/building.md).
