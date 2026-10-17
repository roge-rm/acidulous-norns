# Machines on norns

What each machine costs on a Pi 4 norns (shieldXL, 32-bit Raspberry Pi OS), as a share of one 64-frame block at 48 kHz, which is 1333 µs. The figures come from the app's `tools/cpu_test.cpp`, built on the norns with the plugin's flags (`-O3 -ffast-math`, Cortex-A53). Mean is the average block and worst is the 99th percentile block, which is the one that causes dropouts.

The test plays a note every 43 ms, about three times faster than sixteenths at 124 bpm, so patches with long releases hold more voices than they would in a song. The second pair of columns is at 8 notes a second.

Parts is how many of that machine look safe at once: the worst block times the parts stays under 60%, leaving room for scsynth, the rest of norns and chords, which cost more than single notes. 8 is the most the plugin has.

## The first five

Timed with real patches. The figure is the heaviest patch tried.

| Machine | Patch | Mean % | Worst % | Mean % at 8/s | Worst % at 8/s | Parts |
|---|---|---|---|---|---|---|
| Reflux | Squelch | 0.9 | 1.0 | 0.8 | 0.9 | 8 |
| Hexbeat | Hall | 1.8 | 2.5 | 1.7 | 2.4 | 8 |
| Trinity | Wide Saws | 17.2 | 20.2 | 6.8 | 8.7 | 2 |
| Trinity | Glass Pad | 15.2 | 18.6 | 11.8 | 18.4 | 3 |
| Ratio | Morph Sweep | 25.2 | 28.7 | 20.1 | 27.4 | 2 |
| Ratio | Tine | 17.9 | 20.8 | 7.2 | 9.6 | 2 |
| Formulate | Pulse Lead | 0.7 | 1.1 | 0.7 | 1.1 | 8 |

Trinity's other patches tried (Drift Strings, Bell Keys) and Ratio's Bell land between these.

## Playing on the norns

The plugin's own load readout (`/load`) while `acidulous.lua` plays at 90 bpm, with the scsynth process's CPU from `top`. Load is smoothed and worst is the slowest block in each quarter second, the highest seen over about 10 seconds.

| What plays | Load % | Worst % | scsynth CPU % |
|---|---|---|---|
| Reflux, a 16th-note line | 3 | 10 | 5 |
| Hexbeat, a beat | 3 | 10 | 5 |
| Trinity Glass Pad, 4-note chords | 20 | 36 | 20 |
| Ratio Tine, 4-note chords | 11 to 23 | 29 | 15 |
| Formulate Pulse Lead, an arpeggio | 3 | 8 | 5 |
| Reflux, Hexbeat, Trinity and Ratio together | 31 to 35 | 55 | 32 |

No xruns in JACK's log while any of these played.

## Cumulus, Nexus and Diction

Played on the norns as above: 4-note chords on each beat at 90 bpm, three beats on and one off, for 12 seconds. Load is the mean and highest of the plugin's smoothed readout, and worst the slowest block seen. With one part doing nothing, load reads about 3% and worst about 10%.

| Machine | Patch | Load % | Highest % | Worst % | scsynth CPU % |
|---|---|---|---|---|---|
| Cumulus | Drone Field | 11.3 | 12.8 | 31.7 | 15 |
| Cumulus | Nimbus | 9.9 | 12.5 | 24.7 | 12 |
| Cumulus | Deep Wash | 9.5 | 13.3 | 22.4 | 13 |
| Cumulus | Fifth Up | 8.9 | 10.6 | 18.0 | 12 |
| Cumulus | Bell Cloud | 7.4 | 8.7 | 19.0 | 10 |
| Nexus | Wall Pad | 38.3 | 50.2 | 75.4 | 44 |
| Nexus | Morphing | 28.2 | 31.4 | 49.3 | 33 |
| Nexus | Rotor String | 26.7 | 28.6 | 55.6 | 31 |
| Nexus | Plucked | 24.8 | 27.9 | 45.2 | 29 |
| Nexus | Free Reeds | 24.2 | 34.4 | 52.7 | 27 |
| Nexus | Horn in a Hall | 23.8 | 27.1 | 46.3 | 28 |
| Nexus | One Knob, Four Things | 23.0 | 34.9 | 48.3 | 27 |
| Nexus | Bytebeat | 20.7 | 31.2 | 60.4 | 24 |
| Nexus | Feeding Back | 18.6 | 27.4 | 53.2 | 22 |
| Nexus | Wavetable Drift | 18.2 | 26.7 | 51.0 | 21 |
| Nexus | Sitar Hall | 17.3 | 20.3 | 30.4 | 21 |
| Nexus | Two Operators | 17.2 | 26.6 | 36.5 | 21 |
| Nexus | Talking Harp | 16.3 | 18.9 | 29.4 | 20 |
| Nexus | Talking | 16.1 | 22.5 | 44.3 | 19 |
| Nexus | Tonewheels | 15.9 | 25.3 | 42.1 | 20 |
| Nexus | Macro Sweep | 15.9 | 23.4 | 46.3 | 19 |
| Nexus | Init | 15.2 | 23.9 | 44.5 | 18 |
| Nexus | Subtractive | 15.2 | 23.6 | 37.9 | 19 |
| Nexus | Talk Box Guitar | 13.5 | 18.3 | 35.2 | 16 |
| Nexus | Sample and Hold | 11.9 | 20.6 | 31.1 | 14 |
| Nexus | Birds by the Brook | 10.9 | 15.2 | 28.7 | 14 |
| Nexus | Runs Itself | 10.0 | 14.5 | 23.8 | 13 |
| Nexus | Drum Echoes | 7.2 | 8.7 | 17.2 | 10 |
| Nexus | Sing to the Piano | 4.2 | 5.6 | 12.2 | 7 |
| Nexus | Listening | 3.7 | 4.1 | 11.4 | 6 |
| Diction | Chorale | 4.4 | 6.6 | 13.3 | 7 |
| Diction | Choir | 3.8 | 4.9 | 12.7 | 6 |
| Diction | Harmony | 3.7 | 4.8 | 11.7 | 6 |
| Diction | Crowd | 3.6 | 4.5 | 10.9 | 6 |
| Diction | Ghosts | 3.6 | 4.3 | 11.8 | 6 |
| Diction | Init | 2.8 | 3.8 | 10.2 | 5 |
| Diction | Opera | 2.7 | 3.4 | 9.8 | 5 |
| Diction | Whisper | 2.7 | 3.7 | 10.2 | 5 |

Listening, Talking and Sing to the Piano play the audio input, and there was none, so they cost more with something coming in.

Nexus is the heavy one: two parts of most patches, one of Wall Pad. Cumulus's cloud is rebuilt on its own thread 50 ms after a spectrum param stops changing, which takes about 260 ms on the Pi 4 (Nimbus). Notes and other messages don't wait for it: while builds ran, replies from the plugin's worker came back in 2 to 6 ms. Diction without words sings the vowel param's vowel.

## The sample machines

Played on the norns the same way, with the test files from `host/tests/make_material.py` loaded (a 13-pad kit, test.sf2, short WAV and FLAC files). Bias isn't offered yet, as it follows song scenes and plays nothing from notes.

| Machine | Patch | Loaded | Load % | Highest % | Worst % | scsynth CPU % |
|---|---|---|---|---|---|---|
| Forage | Init | kit | 5.5 | 6.1 | 13.0 | 7 |
| Forage | Body | kit | 5.5 | 6.8 | 12.6 | 7 |
| Mosaic | Init | test.sf2 | 6.7 | 9.0 | 18.0 | 7 |
| Mosaic | Soft Pad | test.sf2 | 13.0 | 15.9 | 23.2 | 7 |
| Dice | Init | loop.wav | 6.5 | 8.1 | 34.0 | 7 |
| Dice | Stutter | loop.wav | 5.6 | 8.1 | 30.7 | 7 |
| Pollen | Init | voice.wav | 13.5 | 16.8 | 25.6 | 7 |
| Pollen | Boulders | voice.wav | 10.4 | 11.8 | 21.3 | 7 |
| Pollen | Chord Dust | voice.flac | 21.9 | 24.9 | 35.8 | 10 |
| Molt | Init | voice.wav | 5.6 | 6.8 | 15.1 | 7 |
| Molt | Choir | voice.flac | 6.1 | 7.0 | 14.4 | 7 |

Dice's worst blocks are when it swaps slices. Pollen's grains are the heavy part, Chord Dust most of all.

Memory: the Pi 4 has 1851 MB and no swap, and with norns and acidulous running about 1165 MB is free, with scsynth at about 145 MB. A 62 MB SoundFont (65 samples) took scsynth to 268 MB, and it went back to 146 MB once a small one replaced it.

Two limits keep loads from running it out of memory:

- All the decoded samples every part holds together can't pass 320 MB. Each load works out its size from the files' headers before reading anything (floats, both channels for a stereo file, and the 30 seconds a pad or Dice, Pollen and Molt keep), and counts what the part held before as well, since that's freed only once the new samples are in. A Mosaic folder of WAVs is read whole, so a folder of long multisamples is the one most likely to hit it. 320 MB leaves the Pi 4 about 800 MB, and a Pi 3 with 1 GB would have about 150 MB left, which hasn't been tried.
- SoundFonts over 64 MB are refused, as reading one holds the whole file and then its preset's samples as floats, about three times its size at the peak. A SoundFont counts as three times its size against the 320 MB while it loads and twice once it's in.

`ACIDULOUS_SAMPLE_LIMIT_MB` and `ACIDULOUS_SF2_LIMIT_MB` in scsynth's environment change them.

## The other pure-DSP machines

At their default settings, which aren't patches anyone plays, so a real patch can cost twice this (Trinity's defaults measure 8% against 15 to 17% for its patches). Time a machine's heavy patches before adding it.

| Machine | Mean % | Worst % | Parts |
|---|---|---|---|
| Cipher | 9.6 | 11.0 | 5 |
| Hammer | 9.3 | 11.9 | 5 |
| Manual | 9.3 | 10.7 | 5 |
| Filament | 8.8 | 10.2 | 5 |
| Sympath | 7.6 | 8.9 | 6 |
| Brazen | 5.3 | 6.3 | 8 |
| Fathom | 5.1 | 7.4 | 8 |
| Resonance | 5.1 | 5.8 | 8 |
| Draw | 4.8 | 5.6 | 8 |
| Chanter | 3.0 | 3.7 | 8 |
| Fret | 2.0 | 2.8 | 8 |
| Tine | 1.6 | 2.3 | 8 |
| Genesis | 1.6 | 2.2 | 8 |
| Palm | 1.3 | 1.9 | 8 |
| Tongue | 1.1 | 1.8 | 8 |
| Timber | 1.1 | 1.7 | 8 |
| Aviary | 0.9 | 1.7 | 8 |

All of them run in real time on the Pi 4. The cheap ones (Genesis, Tine, Palm, Tongue, Timber, Aviary, Fret, Chanter) are the easiest to add next.

## Insert effects

One Trinity part holding a 4-note chord of Glass Pad without its chorus or reverb, then the same with one insert at its default settings. The scsynth figures are norns' `cpu_avg` and `cpu_peak` polls, averaged and the highest over 6 seconds, and load is the plugin's own readout. Reflux and Hexbeat were mounted on two other parts, doing nothing. With no insert, scsynth reads 12.1% avg and 12.6% peak and load 6.4%; the same run at the end read 12.2% and 14.3%.

| Effect | scsynth avg % | scsynth peak % | Load % | Added avg % |
|---|---|---|---|---|
| Amp | 22.9 | 23.7 | 17.1 | +10.8 |
| Spectral | 20.9 | 21.3 | 15.4 | +8.8 |
| Reverb | 17.7 | 20.4 | 11.8 | +5.6 |
| Resonator | 17.0 | 17.7 | 11.2 | +4.9 |
| Swell | 15.6 | 15.9 | 9.7 | +3.5 |
| Magneto | 15.5 | 15.9 | 9.7 | +3.4 |
| Smash | 13.9 | 14.3 | 8.1 | +1.8 |
| Grain | 13.8 | 14.2 | 8.0 | +1.7 |
| Tape | 13.8 | 14.4 | 8.1 | +1.7 |
| Harmonizer | 13.7 | 14.3 | 7.9 | +1.6 |
| Chorus | 13.6 | 14.0 | 7.8 | +1.5 |
| Horn | 13.5 | 14.1 | 7.6 | +1.4 |
| Rotary | 13.3 | 13.8 | 7.5 | +1.2 |
| Acid | 13.3 | 13.9 | 7.5 | +1.2 |
| Mouth | 13.3 | 13.8 | 7.6 | +1.2 |
| Shifter | 12.8 | 14.5 | 7.1 | +0.7 |
| Eq | 12.7 | 13.0 | 6.8 | +0.6 |
| Distortion | 12.7 | 13.3 | 6.9 | +0.6 |
| Tremolo | 12.7 | 13.3 | 6.9 | +0.6 |
| Slicer | 12.7 | 13.5 | 7.0 | +0.6 |
| Compressor | 12.6 | 13.0 | 6.9 | +0.5 |
| Delay | 12.5 | 13.0 | 6.8 | +0.4 |
| Phaser | 12.5 | 12.9 | 6.8 | +0.4 |
| Flanger | 12.5 | 13.1 | 6.8 | +0.4 |
| Gate | 12.5 | 12.9 | 6.7 | +0.4 |
| Bitcrusher | 12.4 | 12.8 | 6.6 | +0.3 |
| Filter | 12.3 | 13.1 | 6.7 | +0.2 |
| Width | 12.3 | 12.8 | 6.5 | +0.2 |
| Formula | 12.1 | 12.6 | 6.4 | +0.0 |

Amp is the only one that adds more than 10% to a part, and Spectral, Reverb and Resonator about 5 to 9%. The rest add under 4%. Formula's default formula does nothing, so it costs nothing here; a real formula will cost more.

## Running the test

Copy `tools/cpu_test.cpp` and `tools/audition_material.h` from the app to the norns, and build them against the plugin's engine library:

```
A=~/dust/code/acidulous-norns
g++ -O3 -ffast-math -fno-finite-math-only -mcpu=cortex-a53 -mfpu=neon-fp-armv8 -mfloat-abi=hard \
    -std=c++17 -w -I $A/third_party/acidulous -I $A/third_party/acidulous/tools \
    cpu_test.cpp $A/host/build/libacidulous-engine.a -lpthread -o cpu_test
./cpu_test              # every machine and effect
./cpu_test --rate 8.3   # at 8 notes a second
```

`--patch` only works with `paired`, unless you move its parsing up to where `--rate` is read, as was done for the table above. It reads `tools/banks` under `$ACIDULOUS_ROOT`.

There is no Pi 3 to test on yet.
