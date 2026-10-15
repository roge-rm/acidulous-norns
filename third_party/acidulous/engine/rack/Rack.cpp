#include "Rack.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <cstdlib> // std::llabs, which the NDK pulls in but host g++ doesn't

namespace acidulous {

namespace {
const ParamDef kChannelDefs[Rack::ChannelCount] = {
    {"gain", 0.0f, 1.5f, 1.0f, Curve::Linear, 0, ""},
    {"pan", -1.0f, 1.0f, 0.0f, Curve::Linear, 0, ""},
    {"mute", 0.0f, 1.0f, 0.0f, Curve::Stepped, 2, ""},
    {"solo", 0.0f, 1.0f, 0.0f, Curve::Stepped, 2, ""},
    {"sendreverb", 0.0f, 1.0f, 0.0f, Curve::Linear, 0, ""},
    {"senddelay", 0.0f, 1.0f, 0.0f, Curve::Linear, 0, ""},
    // Where this track's notes go: the machine, the machine and external
    // MIDI, or only external MIDI. Stepped and read unsmoothed so the switch
    // never ramps through the middle value.
    {"midimode", 0.0f, 2.0f, 0.0f, Curve::Stepped, 3, ""},
    {"midichannel", 0.0f, 15.0f, 0.0f, Curve::Stepped, 16, ""},
    /**
     * How late this track's offbeats are, as a percentage of the pair.
     *
     * 50 is straight and is the default. The song's value is resolved before
     * it gets here, so the engine never deals with "follow the song". It's a
     * channel parameter, like midimode, because it belongs to the track, and
     * that way it can be automated and recorded.
     */
    {"swing", 50.0f, 75.0f, 50.0f, Curve::Linear, 0, "%"},
    // Where this track's sound goes: 0 the master, 1..4 a mixer group. 17
    // steps because saved values are normalised against the step count and
    // it shipped that way. Stepped like midimode so it never ramps.
    {"output", 0.0f, 16.0f, 0.0f, Curve::Stepped, 17, ""},
    // Semitones added to every note going to the machine, from a clip or a
    // finger. 0 for drum machines, whose notes pick sounds.
    {"transpose", -48.0f, 48.0f, 0.0f, Curve::Stepped, 97, "st"},
    // Every note at this velocity, or 0 for as played.
    {"velocity", 0.0f, 127.0f, 0.0f, Curve::Stepped, 128, ""},
};
} // namespace

Rack::Rack() {
    channel.init(kChannelDefs, ChannelCount);
    resetSentTo();
    // Done here because it allocates (17 KB of overlap buffer per rack), and
    // racks are built before the audio thread exists.
    frozenStretch.prepare();
    for (int32_t i = 0; i <= kInputModSlots; ++i) {
        sinks[i].rack = this;
        sinks[i].stage = i;
    }
}

void Rack::Sink::send(uint8_t status, uint8_t d1, uint8_t d2) { rack->deliver(stage + 1, status, d1, d2); }

// Stage 0 is "before modifier 1"; stage kInputModSlots is "at the machine".
void Rack::deliver(int32_t fromStage, uint8_t status, uint8_t d1, uint8_t d2) {
    for (int32_t s = fromStage; s < kInputModSlots; ++s) {
        if (modifiers[s] != nullptr && !modifiers[s]->bypassed()) {
            modifiers[s]->handleMidi(status, d1, d2, sinks[s]);
            return; // the modifier passes it on through its sink
        }
    }
    toMachine(status, d1, d2, true);
}

int32_t Rack::midiOutMode() const {
    // Normalised, not smoothed, so the switch never ramps through "both".
    const float v = channel.normalized(MidiMode);
    return static_cast<int32_t>(v * 2.0f + 0.5f);
}

void Rack::updateMidiOut(int64_t frame) {
    outFrame = frame;
    const int32_t mode = midiOutMode();
    const uint8_t ch = static_cast<uint8_t>(channel.normalized(MidiChannel) * 15.0f + 0.5f);
    if ((mode != lastOutMode || ch != lastOutChannel) && lastOutMode != OutInternal && outQueue != nullptr) {
        // It was sending to MIDI and now isn't, or to a different channel. End
        // whatever it left sounding there.
        sendOut({Outgoing::Midi, static_cast<uint8_t>(0xb0 | lastOutChannel), 123, 0, 0, 0.0f, frame});
    }
    lastOutMode = mode;
    lastOutChannel = ch;
}

void Rack::sendOut(const Outgoing &o) {
    if (!holding) {
        emit(o);
    } else if (outgoingCount < kMaxOutgoing) {
        outgoing[outgoingCount++] = o;
    }
}

void Rack::emit(const Outgoing &o) {
    switch (o.kind) {
    case Outgoing::Midi:
        outQueue->push({o.frame, o.status, o.d1, o.d2, static_cast<uint8_t>(rackIndex)});
        break;
    case Outgoing::Recorded: modifiedSink->onModifiedNote(rackIndex, o.status, o.d1, o.d2); break;
    case Outgoing::Perform: performSink->set(o.index, o.v); break;
    }
}

void Rack::sendHeld() {
    holding = false;
    for (int32_t n = 0; n < outgoingCount; ++n) emit(outgoing[n]);
    outgoingCount = 0;
}

void Rack::toMachine(uint8_t status, uint8_t d1, uint8_t d2, bool live) {
    // Anything that came through the chain came from a finger or a modifier
    // acting on one, so that's what gets recorded (the arpeggio, not the key
    // that started it). A clip's own notes come in the other way and aren't
    // recorded again.
    if (live && modifiedSink != nullptr) sendOut({Outgoing::Recorded, status, d1, d2, 0, 0.0f, 0});

    // Transpose and fixed velocity, after the recording tap so a take keeps
    // what was played, and before MIDI out so external gear hears what the
    // machine would. A note-off goes where its note-on went, even if the
    // transpose has changed since.
    {
        const uint8_t k = status & 0xf0;
        if (k == 0x90 && d2 > 0) {
            const int32_t shift = static_cast<int32_t>(std::lround(channel.target(Transpose)));
            const int32_t to = std::clamp(static_cast<int32_t>(d1) + shift, 0, 127);
            sentTo[d1 & 0x7f] = static_cast<uint8_t>(to);
            d1 = static_cast<uint8_t>(to);
            const int32_t fixed = static_cast<int32_t>(std::lround(channel.target(Velocity)));
            if (fixed > 0) d2 = static_cast<uint8_t>(std::min(fixed, 127));
        } else if (k == 0x80 || k == 0x90) {
            const uint8_t played = d1 & 0x7f;
            d1 = sentTo[played];
            sentTo[played] = played;
        } else if (k == 0xa0) {
            d1 = sentTo[d1 & 0x7f];
        }
    }

    // After the modifiers so MIDI out sends what you hear (arpeggiated,
    // scale-corrected). Before the voice limiter, which is only for the
    // machine.
    const int32_t mode = lastOutMode;
    if (mode != OutInternal && outQueue != nullptr) {
        sendOut({Outgoing::Midi, static_cast<uint8_t>((status & 0xf0) | lastOutChannel), d1, d2, 0, 0.0f, outFrame});
    }
    if (mode == OutMidi) return;
    if (machine == nullptr) return;
    const uint8_t kind = status & 0xf0;
    const bool on = kind == 0x90 && d2 > 0;
    const bool off = kind == 0x80 || (kind == 0x90 && d2 == 0);
    // Pedals. A key released while a pedal holds it isn't released yet: its
    // note-off waits in pedalHeld until the pedal comes up.
    const bool takes = machine->takesPedals();
    if (on) {
        keyDown[d1 & 0x7f] = true;
        pedalHeld[d1 & 0x7f] = false;
        if (softDown && !takes) d2 = static_cast<uint8_t>(std::max(1, (d2 * 5) / 8));
    } else if (off) {
        keyDown[d1 & 0x7f] = false;
        if ((sustainDown && !takes) || sostenutoSet[d1 & 0x7f]) {
            pedalHeld[d1 & 0x7f] = true;
            return;
        }
    }
    if (on) {
        forgetHeld(d1); // a retrigger isn't a second note
        // The voice limit lives in the rack because only the rack knows how
        // many notes are asked for, including ones an arpeggiator makes. Over
        // the limit, the oldest held note is released before the new one starts.
        int32_t limit = EngineSettings::get().voiceLimit.load(std::memory_order_relaxed);
        if (limit > 0) {
            if (limit > kMaxHeld) limit = kMaxHeld;
            while (heldCount >= limit) {
                const uint8_t oldest = held[0];
                machine->handleMidi(0x80, oldest, 0);
                forgetHeld(oldest);
                pedalHeld[oldest & 0x7f] = false;
            }
        }
        if (heldCount < kMaxHeld) held[heldCount++] = d1;
    } else if (off) {
        forgetHeld(d1);
    }
    machine->handleMidi(status, d1, d2);
}

void Rack::setPedal(int32_t which, uint8_t value) {
    const int32_t slot = which == kPerfSustain ? 0 : which == kPerfSostenuto ? 1 : 2;
    if (value == pedalValue[slot]) return;
    pedalValue[slot] = value;
    // Send the pedal itself to MIDI out, as far down as it is. External synths
    // have their own pedal handling, so holding their note-offs here too would
    // hold them twice.
    const uint8_t cc = which == kPerfSustain ? 64 : which == kPerfSostenuto ? 66 : 67;
    if (lastOutMode != OutInternal && outQueue != nullptr) {
        sendOut({Outgoing::Midi, static_cast<uint8_t>(0xb0 | lastOutChannel), cc, value, 0, 0.0f, outFrame});
    }
    const bool takes = machineTakesPedals();
    if (takes) machine->pedal(which, static_cast<float>(value) / 127.0f);
    // 64 and up is down, as in MIDI.
    const bool down = value >= 64;
    if (which == kPerfSoft) {
        softDown = down;
        return;
    }
    if (which == kPerfSustain) {
        if (down == sustainDown) return;
        sustainDown = down;
        if (machine != nullptr && !takes) machine->setDampers(down);
    } else {
        if (down == sostenutoDown) return;
        sostenutoDown = down;
        // Sostenuto catches only the keys held when it goes down.
        for (int32_t n = 0; n < 128; ++n) sostenutoSet[n] = down && keyDown[n];
    }
    if (!down) releasePedalled();
}

void Rack::releasePedalled() {
    if (machine == nullptr) return;
    const bool takes = machine->takesPedals();
    for (int32_t n = 0; n < 128; ++n) {
        if (!pedalHeld[n] || (sustainDown && !takes) || sostenutoSet[n]) continue;
        pedalHeld[n] = false;
        forgetHeld(static_cast<uint8_t>(n));
        machine->handleMidi(0x80, static_cast<uint8_t>(n), 0);
    }
}

void Rack::resetPedals() {
    sustainDown = sostenutoDown = softDown = false;
    for (int32_t n = 0; n < 128; ++n) keyDown[n] = pedalHeld[n] = sostenutoSet[n] = false;
    for (uint8_t &v : pedalValue) v = 0;
    if (machine != nullptr) {
        machine->setDampers(false);
        if (machine->takesPedals()) {
            for (int32_t which : {kPerfSustain, kPerfSostenuto, kPerfSoft}) machine->pedal(which, 0.0f);
        }
    }
}

void Rack::forgetHeld(uint8_t note) {
    for (int32_t i = 0; i < heldCount; ++i) {
        if (held[i] != note) continue;
        for (int32_t j = i; j + 1 < heldCount; ++j) held[j] = held[j + 1];
        --heldCount;
        return;
    }
}

void Rack::handleMidi(uint8_t status, uint8_t d1, uint8_t d2) { deliver(0, status, d1, d2); }

bool Rack::keep(Queued h, const uint8_t *phones, int32_t count) {
    if (!queueing) return false;
    count = phones == nullptr ? 0 : std::max(count, 0);
    if (queuedCount >= kMaxQueued || queuedPhoneCount + count > kMaxQueuedPhones) {
        replayQueued();
        return false;
    }
    h.from = queuedPhoneCount;
    h.count = count;
    if (count > 0) std::memcpy(queuedPhones + queuedPhoneCount, phones, static_cast<size_t>(count));
    queuedPhoneCount += count;
    queued[queuedCount++] = h;
    return true;
}

void Rack::replayQueued() {
    const bool was = queueing;
    queueing = false;
    for (int32_t n = 0; n < queuedCount; ++n) {
        const Queued &h = queued[n];
        const uint8_t *phones = queuedPhones + h.from;
        switch (h.kind) {
        case Queued::Note: playSequenced(h.a, h.b, h.c); break;
        case Queued::Lyric: lyric(phones, h.count); break;
        case Queued::Words: wordsAhead(phones, h.count, h.a, h.b, h.i); break;
        case Queued::Expression: noteExpressionValue(h.i, h.a, h.v); break;
        case Queued::Param: setParam(static_cast<Unit>(h.a), h.i, h.v, h.b != 0); break;
        }
    }
    queuedCount = 0;
    queuedPhoneCount = 0;
    queueing = was;
}

void Rack::playQueued() {
    replayQueued();
    queueing = false;
}

void Rack::playSequenced(uint8_t status, uint8_t d1, uint8_t d2) {
    if (keep({Queued::Note, status, d1, d2, 0, 0.0f, 0, 0})) return;
    toMachine(status, d1, d2, false);
}

void Rack::lyric(const uint8_t *phones, int32_t count) {
    if (keep({Queued::Lyric, 0, 0, 0, 0, 0.0f, 0, 0}, phones, count)) return;
    if (machine != nullptr) machine->lyric(phones, count);
}

void Rack::wordsAhead(const uint8_t *phones, int32_t count, uint8_t note, uint8_t velocity, int32_t inFrames) {
    if (keep({Queued::Words, note, velocity, 0, inFrames, 0.0f, 0, 0}, phones, count)) return;
    if (machine == nullptr || lastOutMode == OutMidi) return;
    // As toMachine will send its note-on.
    const int32_t shift = static_cast<int32_t>(std::lround(channel.target(Transpose)));
    const auto to = static_cast<uint8_t>(std::clamp(static_cast<int32_t>(note) + shift, 0, 127));
    const int32_t fixed = static_cast<int32_t>(std::lround(channel.target(Velocity)));
    const auto vel = fixed > 0 ? static_cast<uint8_t>(std::min(fixed, 127)) : velocity;
    machine->wordsAhead(phones, count, to, vel, inFrames);
}

void Rack::noteExpression(uint8_t kind, uint8_t note, uint8_t d1, uint8_t d2, float bendSemis) {
    if (machine == nullptr) return;
    note = sentTo[note & 0x7f];
    switch (kind) {
    case 0xe0: {
        const float bend14 = static_cast<float>((d2 << 7) | d1) - 8192.0f;
        machine->noteBend(note, bend14 / 8192.0f * bendSemis);
        break;
    }
    case 0xd0: machine->notePressure(note, d1); break;
    case 0xb0: if (d1 == 74) machine->noteTimbre(note, d2); break;
    default: break;
    }
}

void Rack::noteExpressionValue(int32_t kind, uint8_t note, float v01) {
    if (keep({Queued::Expression, note, 0, 0, kind, v01, 0, 0})) return;
    if (machine == nullptr) return;
    note = sentTo[note & 0x7f];
    switch (static_cast<Expr>(kind)) {
    case Expr::Bend: machine->noteBend(note, exprBendFrom01(v01)); break;
    case Expr::Pressure: machine->notePressure(note, expr7From01(v01)); break;
    case Expr::Timbre: machine->noteTimbre(note, expr7From01(v01)); break;
    default: break;
    }
}

void Rack::allNotesOff() {
    for (int32_t s = 0; s < kInputModSlots; ++s) {
        if (modifiers[s] != nullptr) modifiers[s]->allNotesOff(sinks[s]);
    }
    if (machine != nullptr) machine->allNotesOff();
    // Panic comes through here, so the frozen ring-out stops too.
    tailClip = nullptr;
    heldCount = 0;
    resetSentTo();
    resetPedals();
}

void Rack::onBlock(int64_t tickStart, int64_t tickEnd, float bpm) {
    for (int32_t s = 0; s < kInputModSlots; ++s) {
        if (modifiers[s] != nullptr) modifiers[s]->run(tickStart, tickEnd, bpm, sinks[s]);
    }
    for (int32_t s = 0; s < kEffectSlots; ++s) {
        if (effects[s] != nullptr) effects[s]->onBlock(tickStart, tickEnd, bpm);
    }
    if (machine != nullptr) machine->onBlock(tickStart, tickEnd, bpm);
}

/**
 * Whether this rack plays its frozen audio.
 *
 * Only when the playing scene's clip is frozen and the song is at the tempo
 * it was frozen at. Audio played at another tempo would drift off the beat.
 * Otherwise it falls back to the machine and the UI shows the freeze as
 * stale.
 */
void Rack::updateFrozen(int64_t sceneId, float bpm, bool playing, bool ramping) {
    const FrozenClip *want = nullptr;
    // A muted clip is muted whether or not it's frozen. ClipPlayer skips a
    // muted clip's notes, but frozen audio needs its own check.
    //
    // The player's clip is set per rack per scene, so it's the right one in
    // both clip mode and the arranger.
    const seq::Clip *clip = clipPlayer.clip();
    const bool muted = clip != nullptr && clip->mute;
    if (playing && !muted && frozenSet != nullptr) {
        const FrozenClip *c = frozenSet->find(sceneId);
        // The tempo has to match the rendered tempo to a hundredth of a beat,
        // or the loop drifts off the beat.
        //
        // Unless the clock is ramping, in which case the ratio goes to the
        // stretcher. It's bounded, because a rate far from 1 sounds like a
        // warble and the machine does a better job.
        if (c != nullptr && c->frames > 0 && c->bpm > 0.0f) {
            const float rate = bpm / c->bpm;
            if (std::fabs(c->bpm - bpm) < 0.01f) {
                want = c;
                frozenRate = 1.0f;
            } else if (ramping && rate > 0.5f && rate < 2.0f) {
                want = c;
                frozenRate = rate;
            }
        }
    }
    if (want == nullptr) frozenRate = 1.0f;
    if (want == frozenNow) return;
    // A frozen clip is exactly its own length, but the machine would keep
    // ringing after it. The tail handles that: hand it to the second cursor
    // so a scene change sounds like the live track, reverb and releases
    // included.
    //
    // On stop too, since a stop is note-offs and a live track fades out over
    // a second or two. Only panic cuts it.
    if (frozenNow != nullptr && want == nullptr && frozenNow->tail > 0) {
        tailClip = frozenNow;
        tailCursor = frozenNow->frames;
    }
    // Either way, stop whatever the machine was holding or it hangs while the
    // frozen audio plays and after it hands back.
    if (machine != nullptr) machine->allNotesOff();
    frozenNow = want;
    frozenCursor = -1;
    stretching = nullptr; // a new clip starts a new cycle, so the stretcher re-seeks
}

void Rack::updateScene(int64_t sceneId, int64_t cycleTick, bool playing) {
    if (machine == nullptr) return;
    // The clip is set per rack per scene, so the mute is right in both modes
    // (same as updateFrozen).
    const seq::Clip *clip = clipPlayer.clip();
    machine->onScene(sceneId, cycleTick, playing, clip != nullptr && clip->mute);
}

void Rack::syncFrozen(int64_t tickInIteration, float bpm) {
    (void)bpm;
    if (frozenNow == nullptr) return;
    // The clip's tempo, not the clock's. The target is a position in the
    // recorded audio, which only matches the clock until the clock ramps.
    const float at = frozenNow->bpm > 0.0f ? frozenNow->bpm : 120.0f;
    const double perTick = static_cast<double>(kSampleRate) * 60.0 / (static_cast<double>(at) * kPPQN);
    const int64_t ticks = frozenNow->ticks > 0 ? frozenNow->ticks : 1;
    const int64_t target = static_cast<int64_t>((tickInIteration % ticks) * perTick) % frozenNow->frames;
    if (frozenRate == 1.0f) {
        // stretching isn't cleared here. The stretcher has to stay alive until
        // the crossfade out of it finishes, and render() knows when that is.
        //
        // The cursor runs free between blocks and is only pulled back when it
        // drifts audibly. Recomputing it from the tick every block would step it
        // by a sample or two each time, which clicks.
        if (frozenCursor < 0 || std::llabs(target - frozenCursor) > 256) frozenCursor = target;
        frozenSyncTarget = target;
        return;
    }
    // Stretching, and the stretcher owns the read position. Stretcher
    // doesn't allow moving it between hops, so it's only seeded when the
    // clip arrives and again when a new cycle starts (the tick going
    // backwards).
    if (stretching != frozenNow || target < frozenSyncTarget) {
        frozenStretch.seek(target);
        stretching = frozenNow;
    }
    frozenSyncTarget = target;
}

/**
 * The frozen clip read at its rendered rate, a plain memory read. Separate
 * from render() so the crossfade can mix it with the stretched read.
 */
void Rack::readFrozenPlain(int32_t frames) {
    const FrozenClip *f = frozenNow;
    if (f == nullptr) return;
    int64_t at = frozenCursor < 0 ? 0 : frozenCursor;
    for (int32_t i = 0; i < frames; ++i) {
        if (at >= f->frames) {
            at = 0;
            // When the loop comes round, the pass that just ended starts ringing
            // over this one, so looping a frozen clip doesn't cut off its reverb at
            // the bar line. Started here because this is where we know the audio
            // itself looped.
            if (f->tail > 0) {
                tailClip = f;
                tailCursor = f->frames;
            }
        }
        bufL[i] = f->left[static_cast<size_t>(at)];
        bufR[i] = f->right[static_cast<size_t>(at)];
        ++at;
    }
    frozenCursor = at;
}

void Rack::render(int32_t frames) {
    // Frozen audio is read plainly, through the stretcher, or crossfading
    // between the two (see frozenBlend). The blend steps once per block, not
    // per sample. That's inaudible and saves a multiply per sample.
    if (frozenNow != nullptr) {
        const bool wantStretch = frozenRate != 1.0f && stretching == frozenNow;
        const float target = wantStretch ? 1.0f : 0.0f;
        const float step = static_cast<float>(frames) / static_cast<float>(kBlendFrames);
        if (frozenBlend < target) frozenBlend = std::min(target, frozenBlend + step);
        else if (frozenBlend > target) frozenBlend = std::max(target, frozenBlend - step);
        if (frozenBlend <= 0.0f && !wantStretch) stretching = nullptr; // the fade is finished
    } else {
        frozenBlend = 0.0f;
        stretching = nullptr;
    }

    if (frozenNow != nullptr && frozenBlend > 0.0f && stretching == frozenNow) {
        // Following a tempo the audio wasn't rendered at, by stretching it.
        //
        // The source runs to the end of the tail, not the clip, on purpose.
        // Stretcher::fill stops a window short of the end it's given, so ending
        // at the clip would drop the last 30 ms of every pass. The tail follows
        // the clip in the same buffer, so it's the right audio to read into.
        const FrozenClip *f = frozenNow;
        float *dst[2] = {bufL, bufR};
        const float *src[2] = {f->left.data(), f->right.data()};
        const int64_t last = static_cast<int64_t>(f->frames) + f->tail;
        int32_t made = frozenStretch.fill(dst, src, 0, last, frames, frozenRate);
        // Loop when the source passes the clip's end. A loop point inside a ramp
        // is joined by the stretcher's search, not sample-exact. That only
        // happens with a clip shorter than the one-bar ramp.
        while (made < frames) {
            if (f->tail > 0) {
                tailClip = f;
                tailCursor = f->frames;
            }
            frozenStretch.seek(0);
            float *more[2] = {bufL + made, bufR + made};
            const int32_t got = frozenStretch.fill(more, src, 0, last, frames - made, frozenRate);
            if (got <= 0) {
                for (int32_t i = made; i < frames; ++i) bufL[i] = bufR[i] = 0.0f;
                break;
            }
            made += got;
        }
        if (frozenStretch.sourcePosition() >= f->frames) {
            if (f->tail > 0) {
                tailClip = f;
                tailCursor = f->frames;
            }
            frozenStretch.seek(0);
        }
        stereo = true;
        // Don't set frozenCursor = sourcePosition(). That's where the next hop
        // will read, up to a hop ahead of what's been played, so the plain read
        // would skip up to 15 ms. The plain cursor stays where the tick put it
        // and the crossfade covers the join.
        if (frozenBlend < 1.0f) {
            // Mid-fade, so we need the plain read too. It goes in a scratch
            // buffer and gets mixed with the stretched read already in buf.
            float wetL[kBlockFrames], wetR[kBlockFrames];
            for (int32_t i = 0; i < frames; ++i) {
                wetL[i] = bufL[i];
                wetR[i] = bufR[i];
            }
            readFrozenPlain(frames);
            const float wet = frozenBlend, dry = 1.0f - frozenBlend;
            for (int32_t i = 0; i < frames; ++i) {
                bufL[i] = bufL[i] * dry + wetL[i] * wet;
                bufR[i] = bufR[i] * dry + wetR[i] * wet;
            }
        }
    } else if (frozenNow != nullptr) {
        readFrozenPlain(frames);
        stereo = true;
    } else if (machine == nullptr) {
        for (int32_t i = 0; i < frames; ++i) bufL[i] = bufR[i] = 0.0f;
        stereo = false;
        // Nothing to render, but a tail may still be ringing from a clip this
        // rack played before its machine was removed.
        if (tailClip == nullptr) {
            for (int32_t i = 0; i < frames; ++i) keyBuf[i] = 0.0f;
            return;
        }
    } else {
        // The track's tuning, from whichever of the two tables is current.
        const int t = tuningIndex.load(std::memory_order_acquire);
        machine->setTuning(t < 0 ? nullptr : tuningTables[t]);
        stereo = machine->render(bufL, bufR, frames);
        for (int32_t s = 0; s < kEffectSlots; ++s) {
            if (effects[s] != nullptr) stereo = effects[s]->run(bufL, bufR, frames, stereo);
        }
    }
    // The ring-out, on top of whatever else plays: the next pass of a loop,
    // the next scene's live clip, or silence. It's the rack's own sound, so
    // it goes in before the dry tap and the channel strip, so the fader
    // still affects it and freezing still captures it.
    if (tailClip != nullptr) {
        const FrozenClip *t = tailClip;
        const int64_t end = static_cast<int64_t>(t->frames) + t->tail;
        if (!stereo) {
            for (int32_t i = 0; i < frames; ++i) bufR[i] = bufL[i];
            stereo = true;
        }
        int64_t at = tailCursor;
        for (int32_t i = 0; i < frames && at < end; ++i, ++at) {
            bufL[i] += t->left[static_cast<size_t>(at)];
            bufR[i] += t->right[static_cast<size_t>(at)];
        }
        tailCursor = at;
        if (at >= end) tailClip = nullptr; // finished
    }
    // The sidechain tap: after the inserts, before the fader and mute.
    // Pre-fader so turning the kick down doesn't remove the ducking, and
    // pre-mute so a muted kick can still drive it.
    if (stereo) {
        for (int32_t i = 0; i < frames; ++i) keyBuf[i] = (bufL[i] + bufR[i]) * 0.5f;
    } else {
        for (int32_t i = 0; i < frames; ++i) keyBuf[i] = bufL[i];
    }
    if (tapDry) {
        for (int32_t i = 0; i < frames; ++i) {
            dryL[i] = bufL[i];
            dryR[i] = stereo ? bufR[i] : bufL[i];
        }
    }
    // Channel strip: smoothed gain and equal-power pan. A mono source pans
    // from L and becomes stereo here.
    channel.tick();
    const float gain = channel.get(Mute) >= 0.5f ? 0.0f : channel.get(Gain);
    const float pan = channel.get(Pan);
    const float angle = (pan + 1.0f) * 0.25f * 3.14159265f; // -1..1 -> 0..pi/2
    const float gl = gain * std::cos(angle);
    const float gr = gain * std::sin(angle);
    float peak = 0.0f;
    if (stereo) {
        for (int32_t i = 0; i < frames; ++i) {
            bufL[i] *= gl * 1.4142f;
            bufR[i] *= gr * 1.4142f;
            const float a = std::fabs(bufL[i]), b = std::fabs(bufR[i]);
            if (a > peak) peak = a;
            if (b > peak) peak = b;
        }
    } else {
        for (int32_t i = 0; i < frames; ++i) {
            const float m = bufL[i];
            bufL[i] = m * gl * 1.4142f;
            bufR[i] = m * gr * 1.4142f;
            const float a = std::fabs(bufL[i]), b = std::fabs(bufR[i]);
            if (a > peak) peak = a;
            if (b > peak) peak = b;
        }
        stereo = true;
    }
    if (peak > peakHold.load(std::memory_order_relaxed)) peakHold.store(peak, std::memory_order_relaxed);
}

Machine *Rack::swapMachine(Machine *next) {
    Machine *old = machine;
    if (old != nullptr) old->allNotesOff();
    heldCount = 0; // the notes went with the machine
    machine = next;
    // A machine that takes the pedals starts where they are.
    if (next != nullptr && next->takesPedals()) {
        next->pedal(kPerfSustain, pedalValue[0] / 127.0f);
        next->pedal(kPerfSostenuto, pedalValue[1] / 127.0f);
        next->pedal(kPerfSoft, pedalValue[2] / 127.0f);
    }
    return old;
}

Effect *Rack::swapEffect(int32_t slot, Effect *next) {
    if (slot < 0 || slot >= kEffectSlots) return next; // caller retires it
    Effect *old = effects[slot];
    effects[slot] = next;
    return old;
}

InputMod *Rack::swapInputMod(int32_t slot, InputMod *next) {
    if (slot < 0 || slot >= kInputModSlots) return next;
    InputMod *old = modifiers[slot];
    if (old != nullptr) old->allNotesOff(sinks[slot]); // end its sounding notes cleanly
    modifiers[slot] = next;
    return old;
}

void Rack::setParam(Unit unit, int32_t index, float v01, bool jump) {
    if (keep({Queued::Param, static_cast<uint8_t>(unit), static_cast<uint8_t>(jump ? 1 : 0), 0, index, v01, 0, 0})) return;
    switch (unit) {
    case Unit::Machine:
        if (machine) {
            if (jump) machine->params().jump(index, v01);
            else machine->params().set(index, v01);
        }
        break;
    case Unit::Effect1:
    case Unit::Effect2: {
        Effect *fx = effects[unit == Unit::Effect1 ? 0 : 1];
        if (fx == nullptr) break;
        if (index == kEffectBypassIndex) fx->setBypass(v01 >= 0.5f);
        else if (jump) fx->params().jump(index, v01);
        else fx->params().set(index, v01);
        break;
    }
    case Unit::Mod1:
    case Unit::Mod2:
    case Unit::Mod3: {
        const int32_t s = unit == Unit::Mod1 ? 0 : (unit == Unit::Mod2 ? 1 : 2);
        InputMod *ev = modifiers[s];
        if (ev == nullptr) break;
        if (index == kInputModBypassIndex) ev->setBypass(v01 >= 0.5f, sinks[s]);
        else if (jump) ev->params().jump(index, v01);
        else ev->params().set(index, v01);
        break;
    }
    case Unit::Channel: channel.set(index, v01); break;
    case Unit::Performance: {
        // Convert back to the MIDI it came in as, so a lane and a finger on the
        // strip reach the machine the same way, through the modifiers as a
        // controller, like a hardware wheel.
        const auto byte = static_cast<uint8_t>(
            v01 <= 0.0f ? 0 : (v01 >= 1.0f ? 127 : static_cast<int32_t>(v01 * 127.0f + 0.5f)));
        if (index == kPerfMod) {
            handleMidi(0xb0, 1, byte);
        } else if (index == kPerfPressure) {
            handleMidi(0xd0, byte, 0);
        } else if (index == kPerfSustain || index == kPerfSostenuto || index == kPerfSoft) {
            setPedal(index, byte);
        }
        break;
    }
    case Unit::Perform: if (performSink != nullptr) sendOut({Outgoing::Perform, 0, 0, 0, index, v01, 0}); break;
    case Unit::Master: break; // never sent to a rack
    }
}

void Rack::setTuning(const float *ratios) {
    if (ratios == nullptr) {
        tuningIndex.store(-1, std::memory_order_release);
        return;
    }
    // Written into the table the audio thread isn't reading, then switched
    // over with one store. Two changes in one block could still write the
    // table being read, but tunings change by hand, not that fast.
    const int next = tuningIndex.load(std::memory_order_acquire) == 0 ? 1 : 0;
    for (int i = 0; i < 128; ++i) tuningTables[next][i] = ratios[i];
    tuningIndex.store(next, std::memory_order_release);
}

} // namespace acidulous
