#pragma once
#include <cmath>
#include <cstdint>
#include <vector>
#include <engine/dsp/Biquad.h>
#include <engine/dsp/Math.h>
#include <engine/machine/hammer/Room.h>

// The soundboard: one, shared by every key, as on the instrument.
//
// What the strings push into the bridge comes out of the board coloured by
// its lowest modes (where a grand's warmth and body are) and, above them, by
// how the board radiates: a gentle rise through the middle, falling away at
// the top. The bass end of the bridge is heard more on the left and the
// treble on the right, as the player hears it.
//
// The hammer's blow reaches the board too, through the strings before they
// have settled into a tone: that is the knock under every note, the board's
// own modes struck at once, a broad thump from 60 Hz to 2.5 kHz that dies in
// a few hundred milliseconds. In the top octaves it is as loud as the note
// (measured from the recordings, below each note's fundamental, where
// nothing else lives). The felt's force drives it, so a hard blow is a
// short, wide thump and a soft one a narrower, duller one.
//
// Each instrument voices it its own way (Voicing): a smaller board barely
// radiates the bass, a closed lid takes the top, and an electric grand's
// pickups hear the strings with hardly any board at all.
namespace acidulous::machine::hammer {
using dsp::clampf;

class Board {
  public:
    static constexpr int kModes = 48;

    /** How an instrument's board and case sound, and where it's heard from. */
    struct Voicing {
        /** Below this the board barely radiates the strings, Hz. */
        float radiateHz = 85.0f;
        /** The body curve, as a share of the grand's; the bass bridge's peak, dB. */
        float body = 1.0f, bassBodyDb = 7.0f;
        /** The presence peak at 2.5 kHz and the top's shelf at 6 kHz, dB. */
        float presenceDb = 2.0f, topDb = -12.0f;
        /** How hard the board's modes ring, against the grand's: the knock and the strings' share. */
        float modes = 1.0f;
        /** How much of the room. */
        float room = 1.0f;
        bool operator==(const Voicing &o) const {
            return radiateHz == o.radiateHz && body == o.body && bassBodyDb == o.bassBodyDb &&
                   presenceDb == o.presenceDb && topDb == o.topDb && modes == o.modes && room == o.room;
        }
    };

    void prepare(float sampleRate) {
        sr = sampleRate;
        // Modes from about 55 Hz to 2.5 kHz, evenly on a log scale with a
        // little scatter, each ringing a little shorter (0.4 s at the bottom,
        // 0.07 s at the top); their place in the stereo alternates.
        uint32_t seed = 0x51ee7u;
        auto rnd = [&]() { seed = seed * 1664525u + 1013904223u; return static_cast<float>(seed >> 8) / 16777216.0f; };
        for (int m = 0; m < kModes; ++m) {
            const float t = static_cast<float>(m) / (kModes - 1);
            const float hz = 30.0f * std::pow(100.0f, t) * (0.96f + 0.08f * rnd());
            const float t60 = 0.4f * std::pow(0.15f / 0.4f, t) * (0.8f + 0.4f * rnd());
            const float r = std::exp(-6.90776f / (t60 * sr));
            const float w = 6.28318530718f * hz / sr;
            modeA1[m] = 2.0f * r * std::cos(w);
            modeA2[m] = -r * r;
            // Rising gently with frequency: driven by the change in force
            // (already a tilt up), rising as fast as the frequency made every
            // blow a mallet on wood, 10 to 30 dB of it over the recordings
            // in the middle.
            modeGain[m] = 0.12f * std::sqrt(w);
            const float side = (m % 2 == 0 ? -1.0f : 1.0f) * (0.3f + 0.5f * rnd());
            modeL[m] = std::cos((side + 1.0f) * 0.785398f);
            modeR[m] = std::sin((side + 1.0f) * 0.785398f);
        }
        // The spread's corner at the same frequency at any rate. The same on
        // both sides: spread differently, left and right summed to a phone's
        // one speaker lost 2.7 dB and combed.
        spreadCoef = -std::exp(-6.28318530718f * kSpreadHz / sr);
        spreadCoefR = spreadCoef;
        bassCoef = -std::exp(-6.28318530718f * kBassSpreadHz / sr);
        bassCoefR = bassCoef;
        static constexpr float kPathMs[kPaths] = {1.31f, 2.87f, 4.13f};
        for (int p = 0; p < kPaths; ++p) {
            pathL[p].prepare(kPathMs[p] * 0.001f * sr, kPathGain);
            pathR[p].prepare(kPathMs[p] * 0.001f * sr, kPathGain);
        }
        room.prepare(sr);
        // The room hears what the board radiates, not the thump under it.
        roomCut.highpass(45.0f, 0.707f, sr);
        roomCutR.highpass(45.0f, 0.707f, sr);
        room.setDecay(kRoomLowT60, kRoomHighT60);
        lowCut.highpass(32.0f, 0.707f, sr);
        lowCutR.highpass(32.0f, 0.707f, sr);
        designVoicing();
        clear();
    }

    /** Voices it for an instrument: the filters are designed again only if it changed. */
    void voice(const Voicing &v) {
        if (v == voicing) return;
        voicing = v;
        designVoicing();
    }

    void clear() {
        for (int m = 0; m < kModes; ++m) modeY1[m] = modeY2[m] = 0.0f;
        lowCut.reset(); lowCutR.reset();
        for (int i = 0; i < 2; ++i) { radiateL[i].reset(); radiateR[i].reset(); } presence.reset(); presenceR.reset(); top.reset(); topR.reset();
        inBefore = 0.0f;
        room.clear();
        bassBodyL.reset();
        bassBodyR.reset();
        for (int i = 0; i < kBodyBands; ++i) { bodyL[i].reset(); bodyR[i].reset(); }
        for (int s = 0; s < kSpread; ++s) spreadL[s] = spreadR[s] = 0.0f;
        for (int s = 0; s < kBassSpread; ++s) bassL[s] = bassR[s] = 0.0f;
        for (int p = 0; p < kPaths; ++p) { pathL[p].clear(); pathR[p].clear(); }
        roomCut.reset();
        roomCutR.reset();
        level = 0.0f;
    }

    /**
     * One sample of what the bridge got, already panned into [left] and
     * [right], and of the hammers' blows, [knock]; the board adds its colour
     * and its thump.
     */
    void step(float left, float right, float knock, float &outL, float &outR) {
        for (int i = 0; i < 2; ++i) {
            left = radiateL[i].process(left);
            right = radiateR[i].process(right);
        }
        // The body: what the board and case make of the strings, measured as
        // the recordings' partials against the model's at the same
        // frequencies, every key at once (tools/hammer_reference, the body
        // curve): far less at 200-300 Hz, far more from 600 Hz to 5 kHz.
        for (int i = 0; i < kBodyBands; ++i) {
            left = bodyL[i].process(left);
            right = bodyR[i].process(right);
        }
        const float sum = left + right;
        // The modes take the change in force, so they hold no DC.
        const float drive = (kStrings * (sum - inBefore) + knock) * voicing.modes;
        inBefore = sum;
        float ml = 0.0f, mr = 0.0f;
        // Lean, every fourth mode, each driven harder to keep the thump's
        // energy: the board is what costs when one voice is playing.
        const float leanDrive = modeStep == 1 ? drive : drive * 2.0f;
        for (int m = 0; m < kModes; m += modeStep) {
            const float y = modeGain[m] * leanDrive + modeA1[m] * modeY1[m] + modeA2[m] * modeY2[m];
            modeY2[m] = modeY1[m];
            modeY1[m] = y;
            ml += y * modeL[m];
            mr += y * modeR[m];
        }
        float l = left + ml, r = right + mr;
        // Through the board: its bending waves carry the lows slower than
        // the highs, and by many paths. A string's pulse, and the knock,
        // reach the ear spread over 10 to 30 ms, a bass note building where
        // the strings alone would start with a spike (the recordings: 10% to
        // 90% in 32 ms in the bass, 14 in the middle, 6 at the top).
        for (int s = 0; s < spreadStages; ++s) {
            const float zl = spreadCoef * l + spreadL[s];
            spreadL[s] = l - spreadCoef * zl;
            l = zl;
            const float zr = spreadCoefR * r + spreadR[s];
            spreadR[s] = r - spreadCoefR * zr;
            r = zr;
        }
        for (int p = 0; p < kPaths; ++p) {
            l = pathL[p].step(l);
            r = pathR[p].step(r);
        }
        l = top.process(presence.process(lowCut.process(l)));
        r = topR.process(presenceR.process(lowCutR.process(r)));
        float wl, wr;
        room.step(roomCut.process(l), roomCutR.process(r), roomLines, wl, wr);
        outL = l + roomMix * wl;
        outR = r + roomMix * wr;
        level += (std::fabs(l) + std::fabs(r) - level) * 0.0005f;
    }

    /**
     * The bass bridge's extra way across the board, for what the low keys
     * bring: their sound builds slower still (32 ms in the recordings).
     * Applied to their own bus before it joins the rest at step().
     */
    void spreadBass(float &l, float &r) {
        // The bass bridge's part of the board is brighter around 2 kHz than
        // the rest: the recordings' bass notes have 6 to 12 dB more there
        // than one curve for every key can give without overbrightening
        // the middle.
        l = bassBodyL.process(l);
        r = bassBodyR.process(r);
        for (int s = 0; s < kBassSpread; ++s) {
            const float zl = bassCoef * l + bassL[s];
            bassL[s] = l - bassCoef * zl;
            l = zl;
            const float zr = bassCoefR * r + bassR[s];
            bassR[s] = r - bassCoefR * zr;
            r = zr;
        }
    }

    /** How much of the room is heard (0 for none), and with its full eight lines or four. */
    void setRoom(float mix, bool full) {
        roomMix = kRoom * mix * voicing.room;
        roomLines = full ? Room::kLines : 4;
        if ((modeStep == 1) != full) {
            // Modes left out lean ring on from where they were: quiet them.
            for (int m = 0; m < kModes; ++m) modeY1[m] = modeY2[m] = 0.0f;
        }
        modeStep = full ? 1 : 4;
        spreadStages = full ? kSpread : kSpread / 2;
    }

    float loudness() const { return level + room.loudness(); }

  private:
    void designVoicing() {
        // A grand's board barely radiates below about 85 Hz: a bass note's
        // fundamental is heard far under its third and fourth partials (A0's
        // by 40 dB in the recordings). The strings only; the knock keeps its
        // thump.
        const float hz = clampf(voicing.radiateHz, 20.0f, 400.0f);
        for (int i = 0; i < 2; ++i) {
            radiateL[i].highpass(hz, i == 0 ? 0.541f : 1.307f, sr);
            radiateR[i].highpass(hz, i == 0 ? 0.541f : 1.307f, sr);
        }
        for (int i = 0; i < kBodyBands; ++i) {
            const BodyBand &b = kBody[i];
            const float db = b.db * voicing.body;
            if (b.kind == 0) { bodyL[i].peak(b.hz, db, b.q, sr); bodyR[i].peak(b.hz, db, b.q, sr); }
            else { bodyL[i].highShelf(b.hz, db, sr); bodyR[i].highShelf(b.hz, db, sr); }
        }
        bassBodyL.peak(kBassBodyHz, voicing.bassBodyDb, 1.3f, sr);
        bassBodyR.peak(kBassBodyHz, voicing.bassBodyDb, 1.3f, sr);
        presence.peak(2500.0f, voicing.presenceDb, 0.8f, sr);
        presenceR.peak(2500.0f, voicing.presenceDb, 0.8f, sr);
        top.highShelf(6000.0f, voicing.topDb, sr);
        topR.highShelf(6000.0f, voicing.topDb, sr);
    }

    Voicing voicing;
    /** One band of the body curve: a peak (kind 0) or a high shelf (kind 1). */
    struct BodyBand { int kind; float hz, db, q; };
    static constexpr int kBodyBands = 4;
    static constexpr BodyBand kBody[kBodyBands] = {{0, 250.0f, -2.1f, 0.9f}, {1, 700.0f, 1.80f, 0.7f}, {0, 1600.0f, 0.60f, 0.8f}, {0, 3200.0f, 0.90f, 1.0f}};
    dsp::Biquad bodyL[kBodyBands], bodyR[kBodyBands];
    static constexpr float kBassBodyHz = 1900.0f;
    dsp::Biquad bassBodyL, bassBodyR;
    /** A Schroeder allpass: one of the board's many paths. */
    struct Path {
        std::vector<float> line;
        int length = 1, at = 0;
        float g = 0.5f;
        void prepare(float samples, float gain) {
            length = samples < 1.0f ? 1 : static_cast<int>(samples);
            line.assign(static_cast<size_t>(length), 0.0f);
            g = gain;
            at = 0;
        }
        void clear() { std::fill(line.begin(), line.end(), 0.0f); at = 0; }
        float step(float x) {
            const float d = line[static_cast<size_t>(at)];
            const float v = x + g * d;
            line[static_cast<size_t>(at)] = v;
            at = at + 1 == length ? 0 : at + 1;
            return d - g * v;
        }
    };
    /** The spread: first-order allpasses with their corner at kSpreadHz, and the paths. */
    static constexpr int kSpread = 6;
    static constexpr float kSpreadHz = 300.0f;
    static constexpr int kPaths = 3;
    static constexpr float kPathGain = 0.4f;
    float spreadCoef = 0.0f, spreadCoefR = 0.0f;
    static constexpr int kBassSpread = 4;
    static constexpr float kBassSpreadHz = 150.0f;
    float bassCoef = 0.0f, bassCoefR = 0.0f;
    float bassL[kBassSpread > 0 ? kBassSpread : 1] = {}, bassR[kBassSpread > 0 ? kBassSpread : 1] = {};
    float spreadL[kSpread > 0 ? kSpread : 1] = {}, spreadR[kSpread > 0 ? kSpread : 1] = {};
    Path pathL[kPaths], pathR[kPaths];
    /** The room's level at mix 1, and its T60s at the bottom and the top. */
    static constexpr float kRoom = 0.2f;
    static constexpr float kRoomLowT60 = 1.5f, kRoomHighT60 = 0.5f;
    Room room;
    dsp::Biquad roomCut, roomCutR;
    float roomMix = 0.0f;
    int roomLines = Room::kLines;
    /** Every mode (1), or every fourth (4) when lean; and the spread's stages. */
    int modeStep = 1;
    int spreadStages = kSpread;
    /** How much the strings' own motion rings the board, against a blow. */
    static constexpr float kStrings = 0.06f;
    float sr = 48000.0f;
    float modeA1[kModes] = {}, modeA2[kModes] = {}, modeGain[kModes] = {}, modeL[kModes] = {}, modeR[kModes] = {};
    float modeY1[kModes] = {}, modeY2[kModes] = {};
    dsp::Biquad lowCut, lowCutR, presence, presenceR, top, topR;
    dsp::Biquad radiateL[2], radiateR[2];
    float inBefore = 0.0f;
    float level = 0.0f;
};

} // namespace acidulous::machine::hammer
