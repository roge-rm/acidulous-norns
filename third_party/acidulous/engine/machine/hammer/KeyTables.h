#pragma once
// What each key of each instrument is, measured.
//
// Generated anchors: tools/hammer_reference/tables.py --emit <name> <set>
// prints a table to paste here. Numbers only, under anonymous names; what the
// recordings were is in the untracked docs/lineage.md. Keys.h reads between
// the anchors.
namespace acidulous::machine::hammer {

/** A value the recordings couldn't give. */
constexpr float kGap = -1e30f;

/** One measured key; kGap where there's nothing. */
struct Anchor {
    int key;
    /** Inharmonicity: partial n at n·f0·sqrt(1 + B·n²). */
    float B;
    /** The tuning against equal temperament, cents: the instrument's stretch. */
    float cents;
    /**
     * T60s in seconds of the prompt sound (the first slope) and the
     * aftersound (the second), for the fundamental, partials 2-4 and 5-10.
     */
    float prompt1, after1, prompt3, after3, prompt7, after7;
    /** How far the prompt sound falls before the aftersound takes over, dB. */
    float kneeDb;
    /** How far the partials' levels wobble around their decay: the beating, dB. */
    float wobbleDb;
    /** How far up the strike reaches at a middle velocity, Hz. */
    float reachHz;
    /** Brightness over the first 150 ms, as a multiple of f0. */
    float brightF0;
};

/**
 * Factors on a key's design values: what the model needs, on top of what the
 * recordings measured, for its renders to measure the same
 * (tools/hammer_reference/calibrate.py).
 */
struct Adjust {
    int key;
    float B, prompt1, after1, prompt3, after3, prompt7, after7, unison, contact, level, knock;
};

/** How the strike changes per 10 velocity steps in a register: brightness and reach, percent. */
struct VelocityShape {
    float brightPct, reachPct;
};

// The measured B levels off near 6e-3 at the top: that's where partials 2
// and 3 are, and they're what's heard.
// GrandA: measured from a reference recording by tools/hammer_reference/tables.py --emit.
// Columns: key, B, cents, t60 1 prompt, t60 1 after, t60 2-4 prompt, t60 2-4 after, t60 5-10 prompt, t60 5-10 after, knee dB, wobble dB, reach Hz, bright x f0.
constexpr Anchor kGrandA[] = {
    {21, 2.716e-04f, -24.65f, 66.02f, 71.96f, 21.22f, 36.42f, 14.86f, 29.91f, 4.535f, 3.321f, 621.8f, 5.566f},
    {24, 1.859e-04f, -20.37f, 56.67f, 71.63f, 20.58f, 37.92f, 15.48f, 32.73f, 8.756f, 3.501f, 644.3f, 4.898f},
    {28, 1.273e-04f, -15.55f, 46.24f, 70.23f, 19.49f, 39.24f, 15.9f, 35.69f, 14.0f, 3.722f, 677.5f, 4.159f},
    {33, 9.540e-05f, -10.77f, 35.86f, 67.01f, 17.83f, 39.68f, 15.72f, 37.7f, 19.95f, 3.97f, 725.0f, 3.428f},
    {36, 8.778e-05f, -8.5f, 30.79f, 64.39f, 16.71f, 39.27f, 15.25f, 37.86f, 23.19f, 4.104f, 757.0f, 3.071f},
    {40, 8.627e-05f, -6.065f, 25.12f, 60.23f, 15.13f, 37.98f, 14.23f, 36.83f, 27.12f, 4.264f, 804.4f, 2.671f},
    {45, 9.643e-05f, -3.83f, 19.49f, 54.19f, 13.08f, 35.28f, 12.48f, 33.73f, 31.43f, 4.436f, 871.9f, 2.268f},
    {48, 1.096e-04f, -2.841f, 16.74f, 50.26f, 11.85f, 33.19f, 11.27f, 31.09f, 33.69f, 4.524f, 917.4f, 2.069f},
    {52, 1.382e-04f, -1.84f, 13.66f, 44.85f, 10.25f, 30.0f, 9.554f, 26.98f, 36.32f, 4.623f, 984.9f, 1.843f},
    {57, 2.001e-04f, -0.951f, 10.6f, 38.04f, 8.375f, 25.61f, 7.434f, 21.42f, 38.99f, 4.719f, 1081.0f, 1.612f},
    {60, 2.583e-04f, -0.5308f, 9.108f, 34.06f, 7.335f, 22.9f, 6.244f, 18.13f, 40.26f, 4.761f, 1147.0f, 1.497f},
    {64, 3.735e-04f, -0.009937f, 7.437f, 28.99f, 6.066f, 19.34f, 4.811f, 14.04f, 41.58f, 4.799f, 1244.0f, 1.366f},
    {69, 6.085e-04f, 0.7272f, 5.773f, 23.19f, 4.684f, 15.17f, 3.321f, 9.662f, 42.62f, 4.819f, 1383.0f, 1.231f},
    {72, 8.198e-04f, 1.295f, 4.96f, 20.04f, 3.966f, 12.89f, 2.596f, 7.505f, 42.91f, 4.815f, 1478.0f, 1.164f},
    {76, 1.214e-03f, 2.291f, 4.051f, 16.27f, 3.135f, 10.17f, 1.818f, 5.183f, 42.92f, 4.793f, 1620.0f, 1.087f},
    {81, 1.934e-03f, 4.07f, 3.146f, 12.27f, 2.287f, 7.332f, 1.113f, 3.094f, 42.32f, 4.736f, 1825.0f, 1.01f},
    {84, 2.499e-03f, 5.502f, 2.703f, 10.24f, 1.872f, 5.922f, 0.8096f, 2.206f, 41.63f, 4.687f, 1966.0f, 0.9722f},
    {88, 3.389e-03f, 7.929f, 2.209f, 7.931f, 1.414f, 4.368f, 0.5151f, 1.359f, 40.33f, 4.603f, 2176.0f, 0.9302f},
    {93, 4.585e-03f, 11.94f, 1.716f, 5.638f, 0.9753f, 2.893f, 0.2798f, 0.7032f, 38.09f, 4.47f, 2484.0f, 0.8902f},
    {96, 5.224e-03f, 14.96f, 1.475f, 4.541f, 0.7716f, 2.221f, 0.1894f, 0.4603f, 36.42f, 4.375f, 2696.0f, 0.8722f},
    {100, 5.793e-03f, 19.77f, 1.205f, 3.356f, 0.5572f, 1.531f, kGap, kGap, 33.81f, 4.23f, 3016.0f, 0.8548f},
    {105, 5.785e-03f, 27.21f, 0.9365f, 2.249f, 0.3632f, 0.9314f, kGap, kGap, 29.93f, 4.021f, 3486.0f, 0.8428f},
    {108, 5.340e-03f, 32.52f, 0.805f, 1.749f, 0.2777f, 0.6797f, kGap, kGap, 27.28f, 3.88f, 3813.0f, 0.8408f},
};
// Per 10 velocity steps, keys 21-47 / 48-71 / 72-108: brightness % and reach %.
constexpr VelocityShape kGrandAVelocity[3] = {
    {4.01f, 7.99f},
    {2.68f, 7.59f},
    {1.86f, 1.81f},
};

// GrandAAdjust: tools/hammer_reference/calibrate.py, --fields B,contact then --fields
// level; contact then x1.3 up to C4 (down to x1 at C6) for the band spectrum; level again
// with the knock in: what the model needs, on top of the measurements, to measure like the
// reference.
// prompt1: how far the fundamental has fallen at 0.5 and 1 s, against the
// recordings (the two-slope fit missed its first fast fall in the middle).
// knock: the board's thump below each note's fundamental in its first 40 ms (the
// first 10 ms in the bass), against the recordings.
// Columns: key, B, prompt1, after1, prompt3, after3, prompt7, after7, unison, contact, level, knock.
constexpr Adjust kGrandAAdjust[] = {
    {21, 0.9151f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 0.5849f, 2.859f, 0.008536f},
    {24, 0.9266f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 0.5684f, 2.957f, 0.008489f},
    {28, 0.9413f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 0.5502f, 2.758f, 0.03334f},
    {33, 0.9583f, 1.321f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 0.5338f, 2.416f, 1.228f},
    {36, 0.9676f, 1.745f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 0.5369f, 2.193f, 1.924f},
    {40, 0.9789f, 0.5599f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 0.5442f, 1.903f, 1.095f},
    {45, 0.9909f, 0.3714f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 0.5584f, 1.578f, 0.06387f},
    {48, 0.9969f, 0.2451f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 0.5698f, 1.413f, 0.172f},
    {52, 1.003f, 0.2815f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 0.5888f, 1.233f, 0.3565f},
    {57, 1.008f, 0.3865f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 0.6189f, 1.068f, 0.2859f},
    {60, 1.01f, 0.3708f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 0.6409f, 1.0f, 0.1016f},
    {64, 1.01f, 0.3752f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 0.6491f, 0.9466f, 0.02092f},
    {69, 1.007f, 0.4598f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 0.6642f, 0.9433f, 0.01315f},
    {72, 1.003f, 0.543f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 0.6756f, 0.9798f, 0.02093f},
    {76, 0.9961f, 0.6481f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 0.6941f, 1.089f, 0.007926f},
    {81, 0.9835f, 0.7354f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 0.7223f, 1.367f, 0.0189f},
    {84, 0.9739f, 0.809f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 0.7421f, 1.448f, 0.07119f},
    {88, 0.9588f, 1.06f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 0.8125f, 1.832f, 0.1497f},
    {93, 0.9363f, 1.296f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 0.9184f, 2.144f, 0.05433f},
    {96, 0.9208f, 0.9864f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 0.9932f, 2.102f, 0.01749f},
    {100, 0.898f, 0.9924f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.109f, 2.153f, 0.05319f},
    {105, 0.8661f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.284f, 1.863f, 0.02197f},
    {108, 0.8453f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.409f, 1.738f, 0.04042f},
};

// The other string instruments, measured the same way. Their recordings are
// shorter and have fewer layers than the grand's, so Keys.h takes from them
// what they measure well (the stiffness, the stretch, how each part rings
// and how bright the strike is, against the grand's) and builds on the
// calibrated grand for the rest.

// UprightA: measured from a reference recording by tools/hammer_reference/tables.py --emit.
// Columns: key, B, cents, t60 1 prompt, t60 1 after, t60 2-4 prompt, t60 2-4 after, t60 5-10 prompt, t60 5-10 after, knee dB, wobble dB, reach Hz, bright x f0.
constexpr Anchor kUprightA[] = {
    {21, 7.305e-04f, -53.79f, kGap, kGap, 6.517f, kGap, 5.524f, kGap, 0.6994f, 0.07334f, 1437.0f, 15.89f},
    {24, 5.264e-04f, -44.25f, kGap, kGap, 6.01f, kGap, 5.382f, kGap, 2.097f, 0.1602f, 1323.0f, 12.83f},
    {28, 3.680e-04f, -33.54f, kGap, kGap, 5.376f, kGap, 5.116f, kGap, 3.904f, 0.2827f, 1202.0f, 9.761f},
    {33, 2.651e-04f, -23.09f, 2.827f, kGap, 4.648f, kGap, 4.678f, kGap, 6.07f, 0.4463f, 1089.0f, 7.057f},
    {36, 2.311e-04f, -18.22f, 2.705f, kGap, 4.246f, kGap, 4.373f, kGap, 7.321f, 0.5502f, 1038.0f, 5.864f},
    {40, 2.054e-04f, -13.15f, 2.544f, kGap, 3.75f, kGap, 3.933f, kGap, 8.931f, 0.6952f, 987.3f, 4.632f},
    {45, 1.954e-04f, -8.804f, 2.348f, kGap, 3.191f, kGap, 3.356f, kGap, 10.85f, 0.8871f, 947.7f, 3.511f},
    {48, 1.989e-04f, -7.092f, 2.233f, kGap, 2.887f, kGap, 3.009f, kGap, 11.95f, 1.008f, 935.4f, 3.002f},
    {52, 2.145e-04f, -5.66f, 2.084f, kGap, 2.517f, kGap, 2.56f, kGap, 13.37f, 1.176f, 931.8f, 2.463f},
    {57, 2.540e-04f, -4.923f, 1.905f, kGap, 2.108f, kGap, 2.038f, kGap, 15.04f, 1.396f, 947.6f, 1.957f},
    {60, 2.915e-04f, -4.876f, 1.802f, kGap, 1.889f, kGap, 1.753f, kGap, 16.0f, 1.533f, 968.4f, 1.721f},
    {64, 3.635e-04f, -5.081f, 1.67f, kGap, 1.626f, kGap, 1.411f, kGap, 17.21f, 1.724f, 1010.0f, 1.467f},
    {69, 5.052e-04f, -5.452f, 1.512f, 7.371f, 1.34f, 4.789f, 1.048f, 2.526f, 18.64f, 1.972f, 1089.0f, 1.222f},
    {72, 6.307e-04f, -5.57f, 1.422f, 6.182f, 1.19f, 4.058f, 0.8649f, 3.735f, 19.45f, 2.127f, 1152.0f, 1.106f},
    {76, 8.681e-04f, -5.412f, 1.307f, 4.892f, 1.011f, 3.215f, 0.6586f, 3.815f, 20.47f, 2.34f, 1258.0f, 0.9784f},
    {81, 1.335e-03f, -4.392f, 1.173f, 3.654f, 0.8204f, 2.358f, 0.4565f, 1.751f, 21.66f, 2.616f, 1436.0f, 0.8547f},
    {84, 1.749e-03f, -3.176f, 1.097f, 3.069f, 0.7213f, 1.938f, 0.3613f, 0.7147f, 22.32f, 2.788f, 1573.0f, 0.7956f},
    {88, 2.535e-03f, -0.6549f, 1.001f, 2.433f, 0.6052f, 1.475f, kGap, kGap, 23.14f, 3.023f, 1800.0f, 0.7312f},
    {93, 4.064e-03f, 4.255f, 0.8892f, 1.821f, 0.4832f, 1.028f, kGap, kGap, 24.08f, 3.328f, 2177.0f, 0.6697f},
    {96, 5.399e-03f, 8.305f, 0.8269f, 1.532f, 0.4208f, 0.8199f, kGap, kGap, 24.59f, 3.517f, 2468.0f, 0.6414f},
    {100, 7.855e-03f, 15.19f, 0.7488f, 1.217f, 0.3486f, 0.599f, kGap, kGap, 25.22f, 3.775f, 2958.0f, 0.6122f},
    {105, 1.238e-02f, 26.49f, 0.6591f, 0.913f, 0.2739f, 0.397f, kGap, kGap, 25.91f, 4.108f, 3790.0f, 0.5878f},
    {108, 1.609e-02f, 34.87f, 0.6094f, 0.7689f, 0.2363f, 0.3071f, kGap, kGap, 26.28f, 4.313f, 4449.0f, 0.5792f},
};
// Per 10 velocity steps, keys 21-47 / 48-71 / 72-108: brightness % and reach %.
constexpr VelocityShape kUprightAVelocity[3] = {
    {3.45f, 4.72f},
    {1.49f, 4.5f},
    {1.5f, -0.00819f},
};

// HonkyA: measured from a reference recording by tools/hammer_reference/tables.py --emit.
// Columns: key, B, cents, t60 1 prompt, t60 1 after, t60 2-4 prompt, t60 2-4 after, t60 5-10 prompt, t60 5-10 after, knee dB, wobble dB, reach Hz, bright x f0.
constexpr Anchor kHonkyA[] = {
    {21, 5.152e-04f, -22.14f, 102.0f, 77.92f, 24.49f, 32.67f, 16.89f, 33.08f, 2.228f, 3.384f, 1549.0f, 12.95f},
    {24, 3.905e-04f, -14.47f, 79.77f, 67.71f, 23.13f, 31.08f, 15.76f, 30.32f, 4.735f, 3.557f, 1491.0f, 10.44f},
    {28, 2.935e-04f, -6.104f, 57.94f, 56.07f, 21.12f, 28.71f, 14.16f, 26.66f, 7.904f, 3.769f, 1429.0f, 7.952f},
    {33, 2.329e-04f, 1.612f, 39.33f, 44.2f, 18.43f, 25.48f, 12.08f, 22.22f, 11.59f, 4.005f, 1371.0f, 5.787f},
    {36, 2.155e-04f, 4.928f, 31.38f, 38.27f, 16.77f, 23.45f, 10.84f, 19.71f, 13.65f, 4.13f, 1347.0f, 4.841f},
    {40, 2.075e-04f, 7.987f, 23.41f, 31.54f, 14.58f, 20.73f, 9.243f, 16.57f, 16.23f, 4.28f, 1325.0f, 3.87f},
    {45, 2.176e-04f, 9.897f, 16.43f, 24.71f, 11.96f, 17.41f, 7.389f, 13.07f, 19.17f, 4.438f, 1316.0f, 2.994f},
    {48, 2.343e-04f, 10.17f, 13.37f, 21.31f, 10.49f, 15.51f, 6.376f, 11.21f, 20.79f, 4.517f, 1319.0f, 2.597f},
    {52, 2.709e-04f, 9.682f, 10.24f, 17.48f, 8.678f, 13.12f, 5.159f, 9.016f, 22.78f, 4.604f, 1334.0f, 2.18f},
    {57, 3.464e-04f, 7.982f, 7.429f, 13.61f, 6.692f, 10.43f, 3.863f, 6.727f, 24.98f, 4.683f, 1370.0f, 1.792f},
    {60, 4.134e-04f, 6.527f, 6.169f, 11.7f, 5.656f, 8.984f, 3.206f, 5.58f, 26.15f, 4.715f, 1401.0f, 1.613f},
    {64, 5.381e-04f, 4.25f, 4.852f, 9.55f, 4.455f, 7.274f, 2.462f, 4.294f, 27.55f, 4.739f, 1456.0f, 1.421f},
    {69, 7.744e-04f, 1.137f, 3.639f, 7.392f, 3.23f, 5.471f, 1.727f, 3.03f, 29.01f, 4.741f, 1546.0f, 1.241f},
    {72, 9.762e-04f, -0.7265f, 3.082f, 6.331f, 2.63f, 4.562f, 1.378f, 2.431f, 29.74f, 4.726f, 1614.0f, 1.159f},
    {76, 1.341e-03f, -3.036f, 2.49f, 5.142f, 1.972f, 3.534f, 1.005f, 1.789f, 30.54f, 4.687f, 1723.0f, 1.072f},
    {81, 2.004e-03f, -5.367f, 1.93f, 3.956f, 1.344f, 2.516f, 0.6602f, 1.195f, 31.26f, 4.61f, 1893.0f, 0.9953f},
    {84, 2.542e-03f, -6.322f, 1.668f, 3.376f, 1.055f, 2.03f, 0.5066f, 0.9272f, 31.55f, 4.549f, 2017.0f, 0.9635f},
    {88, 3.456e-03f, -6.908f, 1.383f, 2.729f, 0.7528f, 1.505f, 0.3504f, 0.6528f, 31.76f, 4.448f, 2212.0f, 0.9359f},
    {93, 4.946e-03f, -6.26f, 1.109f, 2.087f, 0.4825f, 1.014f, 0.2158f, 0.4123f, 31.74f, 4.293f, 2514.0f, 0.9234f},
    {96, 6.016e-03f, -4.989f, 0.9775f, 1.774f, 0.3649f, 0.7914f, 0.1592f, 0.3095f, 31.58f, 4.184f, 2734.0f, 0.9272f},
    {100, 7.591e-03f, -2.095f, 0.8326f, 1.427f, 0.2479f, 0.5614f, 0.1045f, kGap, 31.2f, 4.02f, 3080.0f, 0.9456f},
    {105, 9.599e-03f, 3.727f, 0.6899f, 1.085f, 0.1493f, 0.3581f, kGap, kGap, 30.44f, 3.787f, 3621.0f, 0.9914f},
    {108, 1.067e-02f, 8.542f, 0.6203f, 0.9192f, 0.1088f, 0.2704f, kGap, kGap, 29.84f, 3.631f, 4018.0f, 1.033f},
};
// Per 10 velocity steps, keys 21-47 / 48-71 / 72-108: brightness % and reach %.
constexpr VelocityShape kHonkyAVelocity[3] = {
    {0.0f, 0.0f},
    {0.0f, 0.0f},
    {0.0f, 0.0f},
};

// ElectricGrandA: measured from a reference recording by tools/hammer_reference/tables.py --emit.
// Columns: key, B, cents, t60 1 prompt, t60 1 after, t60 2-4 prompt, t60 2-4 after, t60 5-10 prompt, t60 5-10 after, knee dB, wobble dB, reach Hz, bright x f0.
constexpr Anchor kElectricGrandA[] = {
    {21, kGap, kGap, kGap, kGap, kGap, kGap, kGap, kGap, kGap, kGap, kGap, kGap},
    {24, 2.269e-03f, -28.25f, 14.49f, kGap, 34.26f, kGap, 20.42f, kGap, -1.135f, 0.07405f, 444.1f, 3.843f},
    {28, 1.186e-03f, -20.85f, 15.17f, kGap, 28.63f, kGap, 15.07f, kGap, -0.6858f, 0.1813f, 468.7f, 3.289f},
    {33, 6.346e-04f, -13.73f, 15.64f, kGap, 22.68f, kGap, 10.48f, kGap, 0.006784f, 0.3152f, 506.5f, 2.744f},
    {36, 4.784e-04f, -10.47f, 15.72f, kGap, 19.64f, kGap, 8.5f, kGap, 0.4924f, 0.3956f, 533.6f, 2.478f},
    {40, 3.626e-04f, -7.133f, 15.56f, kGap, 16.13f, kGap, 6.499f, kGap, 1.222f, 0.5026f, 575.7f, 2.182f},
    {45, 2.973e-04f, -4.346f, 14.98f, kGap, 12.51f, kGap, 4.725f, kGap, 2.265f, 0.6364f, 639.6f, 1.885f},
    {48, 2.834e-04f, -3.277f, 14.45f, kGap, 10.69f, kGap, 3.937f, kGap, 2.961f, 0.7167f, 685.0f, 1.739f},
    {52, 2.869e-04f, -2.397f, 13.54f, kGap, 8.628f, kGap, 3.119f, kGap, 3.971f, 0.8237f, 755.5f, 1.574f},
    {57, 3.247e-04f, -1.922f, 12.17f, kGap, 6.547f, kGap, 2.37f, kGap, 5.364f, 0.9573f, 862.8f, 1.408f},
    {60, 3.681e-04f, -1.837f, 11.26f, kGap, 5.524f, kGap, 2.029f, kGap, 6.271f, 1.037f, 939.5f, 1.327f},
    {64, 4.579e-04f, -1.799f, 9.984f, kGap, 4.381f, kGap, 1.665f, kGap, 7.561f, 1.144f, 1059.0f, 1.236f},
    {69, 6.451e-04f, -1.62f, 8.375f, kGap, 3.253f, kGap, 1.323f, kGap, 9.306f, 1.278f, 1243.0f, 1.145f},
    {72, 8.168e-04f, -1.309f, 7.434f, kGap, 2.709f, kGap, 1.163f, kGap, 10.42f, 1.358f, 1377.0f, 1.102f},
    {76, 1.150e-03f, -0.499f, 6.24f, kGap, 2.112f, kGap, 0.9891f, kGap, 11.99f, 1.465f, 1587.0f, 1.055f},
    {81, 1.818e-03f, 1.4f, 4.887f, kGap, 1.534f, kGap, 0.8215f, kGap, 14.09f, 1.598f, 1915.0f, 1.013f},
    {84, 2.415e-03f, 3.147f, 4.162f, 6.095f, 1.261f, 2.443f, 0.7415f, kGap, 15.42f, 1.678f, 2155.0f, 0.9953f},
    {88, 3.540e-03f, 6.342f, 3.307f, 6.611f, 0.9663f, 1.864f, 0.6535f, kGap, 17.27f, 1.784f, 2539.0f, 0.9803f},
    {93, 5.662e-03f, 11.98f, 2.417f, 6.021f, 0.687f, 1.403f, kGap, kGap, 19.71f, 1.918f, 3150.0f, 0.9747f},
    {96, 7.422e-03f, 16.37f, 1.975f, 5.13f, 0.5574f, 1.218f, kGap, kGap, 21.25f, 1.998f, 3604.0f, 0.9781f},
    {100, 1.043e-02f, 23.56f, 1.485f, 3.67f, 0.4197f, 1.044f, kGap, kGap, 23.38f, 2.104f, 4342.0f, 0.9909f},
    {105, 1.521e-02f, 34.95f, 1.013f, 1.986f, 0.292f, 0.9082f, kGap, kGap, 26.18f, 2.237f, 5536.0f, 1.02f},
    {108, kGap, 43.2f, 0.7946f, 1.239f, 0.2339f, 0.8601f, kGap, kGap, 27.93f, 2.317f, 6441.0f, 1.046f},
};
// Per 10 velocity steps, keys 21-47 / 48-71 / 72-108: brightness % and reach %.
constexpr VelocityShape kElectricGrandAVelocity[3] = {
    {5.52f, 16.6f},
    {4.47f, 24.0f},
    {3.67f, 13.5f},
};

// The electric pianos: a tine instrument and a reed one. Their partials
// are the pickups' (their bars are nearly pure), so what's used is how long
// the fundamental rings. The reed recording's notes are cut short; its
// times are what a fit over a few seconds says.

// TineA: measured from a reference recording by tools/hammer_reference/tables.py --emit.
// Columns: key, B, cents, t60 1 prompt, t60 1 after, t60 2-4 prompt, t60 2-4 after, t60 5-10 prompt, t60 5-10 after, knee dB, wobble dB, reach Hz, bright x f0.
constexpr Anchor kTineA[] = {
    {21, kGap, kGap, kGap, kGap, kGap, kGap, kGap, kGap, kGap, kGap, kGap, kGap},
    {24, kGap, kGap, kGap, kGap, kGap, kGap, kGap, kGap, kGap, kGap, kGap, kGap},
    {28, 6.801e-11f, 2.783f, 31.51f, 31.62f, 9.001f, 14.03f, 4.82f, 6.542f, 6.925f, 3.437f, 865.5f, 4.842f},
    {33, 9.448e-09f, 3.268f, 31.33f, 31.38f, 8.869f, 12.85f, 4.315f, 6.139f, 9.196f, 2.871f, 828.7f, 3.674f},
    {36, 8.111e-08f, 3.493f, 30.79f, 30.81f, 8.662f, 12.1f, 3.961f, 5.842f, 10.35f, 2.559f, 813.3f, 3.152f},
    {40, 6.260e-07f, 3.726f, 29.6f, 29.59f, 8.252f, 11.07f, 3.455f, 5.397f, 11.66f, 2.177f, 800.1f, 2.607f},
    {45, 2.608e-06f, 3.921f, 27.46f, 27.42f, 7.555f, 9.765f, 2.809f, 4.785f, 12.92f, 1.751f, 794.8f, 2.104f},
    {48, 3.738e-06f, 3.995f, 25.89f, 25.84f, 7.061f, 8.987f, 2.434f, 4.402f, 13.47f, 1.524f, 797.5f, 1.874f},
    {52, 3.822e-06f, 4.051f, 23.55f, 23.49f, 6.343f, 7.972f, 1.966f, 3.886f, 13.96f, 1.254f, 807.9f, 1.629f},
    {57, 2.293e-06f, 4.067f, 20.38f, 20.33f, 5.397f, 6.766f, 1.452f, 3.256f, 14.2f, 0.9685f, 832.6f, 1.399f},
    {60, 1.406e-06f, 4.056f, 18.42f, 18.39f, 4.827f, 6.084f, 1.187f, 2.895f, 14.14f, 0.8254f, 854.0f, 1.293f},
    {64, 6.683e-07f, 4.024f, 15.85f, 15.83f, 4.089f, 5.234f, 0.888f, 2.442f, 13.82f, 0.6674f, 891.0f, 1.182f},
    {69, 2.771e-07f, 3.973f, 12.8f, 12.79f, 3.233f, 4.275f, 0.5957f, 1.933f, 13.05f, 0.5224f, 952.6f, 1.08f},
    {72, 1.863e-07f, 3.942f, 11.1f, 11.1f, 2.767f, 3.756f, 0.4599f, 1.661f, 12.38f, 0.4636f, 998.9f, 1.036f},
    {76, 1.444e-07f, 3.912f, 9.033f, 9.05f, 2.21f, 3.134f, 0.3184f, 1.34f, 11.25f, 0.4178f, 1073.0f, 0.9947f},
    {81, 1.986e-07f, 3.903f, 6.803f, 6.83f, 1.624f, 2.463f, kGap, kGap, 9.454f, 0.4132f, 1190.0f, 0.9675f},
    {84, 3.752e-07f, 3.921f, 5.659f, 5.691f, 1.33f, 2.115f, kGap, kGap, 8.175f, 0.4385f, 1276.0f, 0.9635f},
    {88, 1.663e-06f, 3.981f, 4.357f, 4.391f, 1.002f, 1.711f, kGap, kGap, 6.232f, 0.5049f, 1412.0f, 0.972f},
    {93, 3.643e-05f, 4.126f, 3.061f, 3.095f, 0.6838f, 1.294f, kGap, kGap, 3.423f, 0.6406f, 1625.0f, 1.006f},
    {96, 4.958e-04f, 4.259f, 2.443f, 2.475f, 0.5359f, 1.086f, kGap, kGap, 1.534f, 0.7501f, 1780.0f, 1.04f},
    {100, kGap, kGap, kGap, kGap, kGap, kGap, kGap, kGap, kGap, kGap, kGap, kGap},
    {105, kGap, kGap, kGap, kGap, kGap, kGap, kGap, kGap, kGap, kGap, kGap, kGap},
    {108, kGap, kGap, kGap, kGap, kGap, kGap, kGap, kGap, kGap, kGap, kGap, kGap},
};
// Per 10 velocity steps, keys 21-47 / 48-71 / 72-108: brightness % and reach %.
constexpr VelocityShape kTineAVelocity[3] = {
    {24.1f, 33.6f},
    {11.3f, 18.6f},
    {0.0213f, 0.0415f},
};

// ReedA: measured from a reference recording by tools/hammer_reference/tables.py --emit.
// Columns: key, B, cents, t60 1 prompt, t60 1 after, t60 2-4 prompt, t60 2-4 after, t60 5-10 prompt, t60 5-10 after, knee dB, wobble dB, reach Hz, bright x f0.
constexpr Anchor kReedA[] = {
    {21, kGap, kGap, kGap, kGap, kGap, kGap, kGap, kGap, kGap, kGap, kGap, kGap},
    {24, kGap, kGap, kGap, kGap, kGap, kGap, kGap, kGap, kGap, kGap, kGap, kGap},
    {28, kGap, kGap, kGap, kGap, kGap, kGap, kGap, kGap, kGap, kGap, kGap, kGap},
    {33, 2.081e-07f, 5.408f, kGap, kGap, 68.01f, kGap, 25.11f, kGap, 0.7507f, 0.07329f, 932.9f, 5.98f},
    {36, 1.389e-07f, 5.931f, kGap, kGap, 45.34f, kGap, 23.27f, kGap, 0.3594f, 0.09958f, 982.5f, 5.073f},
    {40, 8.839e-08f, 6.19f, 81.38f, kGap, 27.23f, kGap, 20.17f, kGap, -0.04259f, 0.1298f, 1047.0f, 4.119f},
    {45, 6.166e-08f, 5.934f, 42.46f, kGap, 15.12f, kGap, 15.8f, kGap, -0.3526f, 0.1598f, 1125.0f, 3.234f},
    {48, 5.736e-08f, 5.536f, 29.66f, kGap, 10.91f, kGap, 13.17f, kGap, -0.4359f, 0.1736f, 1170.0f, 2.825f},
    {52, 6.417e-08f, 4.798f, 19.06f, kGap, 7.277f, kGap, 9.911f, kGap, -0.4273f, 0.1872f, 1226.0f, 2.385f},
    {57, 1.102e-07f, 3.663f, 11.64f, kGap, 4.609f, kGap, 6.504f, kGap, -0.224f, 0.1964f, 1289.0f, 1.966f},
    {60, 1.952e-07f, 2.934f, 8.93f, kGap, 3.598f, kGap, 4.877f, kGap, 0.0006051f, 0.1978f, 1322.0f, 1.767f},
    {64, 5.822e-07f, 1.985f, 6.508f, kGap, 2.666f, kGap, 3.187f, kGap, 0.4198f, 0.1948f, 1361.0f, 1.551f},
    {69, 4.139e-06f, 0.954f, 4.648f, kGap, 1.926f, kGap, kGap, kGap, 1.136f, 0.1833f, 1401.0f, 1.342f},
    {72, 1.909e-05f, 0.485f, 3.919f, kGap, 1.627f, kGap, kGap, kGap, 1.669f, 0.1722f, 1419.0f, 1.243f},
    {76, 2.303e-04f, 0.1102f, 3.239f, kGap, 1.339f, kGap, kGap, kGap, 2.499f, 0.1526f, 1435.0f, 1.134f},
    {81, kGap, 0.1674f, 2.706f, kGap, 1.103f, kGap, kGap, kGap, 3.728f, 0.1203f, 1445.0f, 1.03f},
    {84, kGap, 0.5476f, 2.507f, kGap, 1.008f, kGap, kGap, kGap, 4.569f, 0.09681f, 1444.0f, 0.9818f},
    {88, kGap, 1.535f, 2.349f, kGap, 0.9223f, kGap, kGap, kGap, 5.809f, 0.0606f, 1437.0f, 0.9315f},
    {93, kGap, 3.663f, 2.297f, kGap, kGap, kGap, kGap, kGap, 7.552f, 0.00755f, 1415.0f, 0.8882f},
    {96, kGap, kGap, kGap, kGap, kGap, kGap, kGap, kGap, kGap, kGap, kGap, kGap},
    {100, kGap, kGap, kGap, kGap, kGap, kGap, kGap, kGap, kGap, kGap, kGap, kGap},
    {105, kGap, kGap, kGap, kGap, kGap, kGap, kGap, kGap, kGap, kGap, kGap, kGap},
    {108, kGap, kGap, kGap, kGap, kGap, kGap, kGap, kGap, kGap, kGap, kGap, kGap},
};
// Per 10 velocity steps, keys 21-47 / 48-71 / 72-108: brightness % and reach %.
constexpr VelocityShape kReedAVelocity[3] = {
    {13.2f, 21.5f},
    {11.4f, 14.6f},
    {0.597f, 9.35f},
};

} // namespace acidulous::machine::hammer
