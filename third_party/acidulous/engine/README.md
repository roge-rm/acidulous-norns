# The engine

Our own, from 2026-09-10; the vendored third-party subset that got the project
to M3 was removed in favour of this. Everything under `cpp/` is ours; the only
dependency is Oboe.

```
core/       the realtime plumbing, and nothing that reads a file:
            Constants · RtQueue (SPSC, wait-free) · Handover (Mount / Retire and the
            retire worker) · Params (ParamDef tables, 0..1 in, smoothed unit-range out) ·
            Messages (MidiMessage, ParamMessage) · Timebase · InputBus · Settings ·
            Frozen · Sample · SampleMap (zones + crossfades) · Take · Expression ·
            Utterance (Molt's analyser) · Capture
format/     reading and writing files, all ours except LAME:
            WavReader · WavWriter · AiffWriter · FlacWriter · Mp3Writer (LAME) ·
            Sf2Reader · AudioSink (the interface the four writers implement)
dsp/        Math · Osc (PolyBLEP saw/pulse) · Filter (TPT SVF) · MultiFilter (12 slopes +
            drive) · Envelope · Adsr (DADSR + repeat) · LfoGen · Lfo (note-value phase) ·
            Wavetable · Biquad · DelayLine · Delay · Limiter · Click · Fft
machine/    Machine interface · MachineRegistry, then one directory each:
            reflux trinity ratio mosaic hexbeat forage genesis resonance cumulus
            pollen dice formulate manual filament brazen timber cipher molt nexus
            diction bias hammer tongue draw fret tine sympath palm chanter aviary fathom
effect/     Effect interface (onBlock for tempo, run() with bypass) · EffectRegistry ·
            Delay Reverb Eq Distortion Compressor Filter Bitcrusher Phaser Flanger
            Chorus Tremolo Width Shifter Harmonizer Amp Gate Swell
            Rotary Grain Resonator (FromMachines.h)
            Smash Acid Mouth Tape Slicer Magneto Horn Spectral Formula
modifier/    InputMod interface + MidiSink · InputModRegistry · Scales.h (33 scales,
            25 chords) · InputMods: Scale Chord Arp
rack/       Rack (clip player -> modifiers -> machine -> effects -> channel strip) ·
            MasterBus (sum, peak, sends, limiter) · Engine (the render loop)
../sequencer/  TickClock · Transport · Clip · ClipPlayer · Song · SceneScheduler ·
               Launcher · RecordQueue · ClockFollower · LinkFollower
../platform/   android/ (the JNI bridge) · drivers/ (the Oboe stream) · link/
../EngineHost  the only thing the app talks to
```

**Which way the arrows point.** `engine/` never includes anything from
`platform/` - that is what makes a headless host on another operating system a
matter of writing a new `platform/`, and it is checked by eye rather than by
the build, so keep it true. `engine/` and `sequencer/` do include each other:
`rack/Engine` owns the transport and the scheduler, so they are one layer in
two directories rather than two layers, and nothing has been gained by
pretending otherwise.


## Rules the audio thread lives by

- It never allocates, locks, logs, or blocks. Objects arrive through `Mount`
  records built elsewhere and leave through `Retire` records to a worker.
- One mount is applied per block, at the block boundary, after rendering.
- MIDI, parameters and recorded events each cross on their own `RtQueue`.
- A block is `kBlockFrames` (64) at `kSampleRate` (48 000); the Oboe driver
  adapts whatever burst the device wants to that with a carry buffer.
- Parameters are addressed by index into a unit's `ParamDef` table; names are
  resolved on the UI thread (`EngineHost::setParam`). Every parameter is
  smoothed per block.
