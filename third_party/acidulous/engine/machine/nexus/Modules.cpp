#include "Modules.h"
#include "Instruments.h"

// The module table. The engine publishes it to the UI, so the editor's
// labels, jack names and module defaults all come from here, with no second
// copy in Kotlin.
namespace acidulous::machine::nexus {

namespace {
constexpr const char *kNone = nullptr;

#define KNOBS(...) {__VA_ARGS__}
const ModuleInfo kInfo[TypeCount] = {
    // name        knobs                                                                 defaults                                       inputs                                  outputs                            cap
    {"blank",   {kNone, kNone, kNone, kNone, kNone, kNone, kNone, kNone},                {0, 0, 0, 0, 0, 0, 0, 0},                      {kNone}, {kNone}, CapBoth},
    {"voice",   {"bend", "glide", kNone, kNone, kNone, kNone, kNone, kNone},             {0.08f, 0, 0, 0, 0, 0, 0, 0},                  {kNone},
                                                                                          {"pitch", "gate", "vel", "rnd", "trig"}, CapPoly},
    {"perf",    {kNone, kNone, kNone, kNone, kNone, kNone, kNone, kNone},                {0, 0, 0, 0, 0, 0, 0, 0},                      {kNone},
                                                                                          {"mod", "prs", "bend"}, CapMono},
    {"macro",   {kNone, kNone, kNone, kNone, kNone, kNone, kNone, kNone},                {0, 0, 0, 0, 0, 0, 0, 0},                      {kNone},
                                                                                          {"1", "2", "3", "4", "5", "6", "7", "8"}, CapMono},
    {"out",     {"level", "pan", kNone, kNone, kNone, kNone, kNone, kNone},              {0.5f, 0.5f, 0, 0, 0, 0, 0, 0},                {"in", "pan", "in R"}, {"L", "R"}, CapBoth},
    {"scope",   {"zoom", kNone, kNone, kNone, kNone, kNone, kNone, kNone},               {0.2f, 0, 0, 0, 0, 0, 0, 0},                   {"in", "b"}, {"thru", "b"}, CapMono},

    {"osc",     {"wave", "semis", "fine", "width", "fm", "level", kNone, kNone},         {0, 0.5f, 0.5f, 0.5f, 0.25f, 1.0f, 0, 0},      {"pitch", "fm", "pw"}, {"out"}, CapBoth},
    {"wtosc",   {"table", "frame", "semis", "fm", "level", kNone, kNone, kNone},         {0, 0, 0.5f, 0.25f, 1.0f, 0, 0, 0},            {"pitch", "fm", "frame"}, {"out"}, CapBoth},
    {"noise",   {"colour", "level", kNone, kNone, kNone, kNone, kNone, kNone},           {0, 1.0f, 0, 0, 0, 0, 0, 0},                   {kNone}, {"out"}, CapBoth},
    {"string",  {"semis", "sustain", "tone", "stiff", "tension", "damp at", "damp", "level"},
                                                                                          {0.5f, 0.9f, 0.45f, 0, 0.15f, 0.5f, 0, 0.5f}, {"excite", "pitch", "sustain"}, {"out", "ring"}, CapBoth},
    {"wheels",  {"timbre", "semis", "level", kNone, kNone, kNone, kNone, kNone},         {0.12f, 0.5f, 1.0f, 0, 0, 0, 0, 0},            {"pitch"}, {"out"}, CapBoth},
    {"op",      {"mode", "ratio", "fine", "fb", "level", kNone, kNone, kNone},           {0, 0.033f, 0.5f, 0, 1.0f, 0, 0, 0},           {"phase", "aux", "pitch"}, {"out"}, CapBoth},
    {"grain",   {"pos", "size", "density", "spray", "pitch", "level", kNone, kNone},     {0.1f, 0.4f, 0.4f, 0.2f, 0.5f, 0.5f, 0, 0},    {"in", "pos", "density"}, {"out"}, CapBoth},
    {"audioin", {"gain", kNone, kNone, kNone, kNone, kNone, kNone, kNone},               {0.25f, 0, 0, 0, 0, 0, 0, 0},                  {kNone}, {"L", "R"}, CapMono},

    {"filter",  {"type", "cutoff", "res", "drive", "amount", "key", kNone, kNone},       {0, 0.6f, 0.2f, 0, 0, 0, 0, 0},                {"in", "cutoff", "res"}, {"out"}, CapBoth},
    {"vca",     {"offset", "curve", kNone, kNone, kNone, kNone, kNone, kNone},           {0, 0, 0, 0, 0, 0, 0, 0},                      {"in", "cv"}, {"out"}, CapBoth},
    {"mix",     {"a", "b", "c", "d", "out", kNone, kNone, kNone},                        {0.75f, 0.75f, 0.75f, 0.75f, 0.5f, 0, 0, 0},   {"a", "b", "c", "d"}, {"out"}, CapBoth},
    {"math",    {"mode", "amount", kNone, kNone, kNone, kNone, kNone, kNone},            {0, 0.5f, 0, 0, 0, 0, 0, 0},                   {"a", "b"}, {"out"}, CapBoth},
    {"delay",   {"time", "feedback", "tone", "mix", kNone, kNone, kNone, kNone},         {0.55f, 0.3f, 0.5f, 0.5f, 0, 0, 0, 0},         {"in", "time"}, {"out"}, CapMono},
    {"rotary",  {"horn", "drum", "ramp", "distance", "angle", "spread", kNone, kNone},   {0.75f, 0.7f, 0.5f, 0.35f, 0.8f, 0.75f, 0, 0}, {"in", "speed"}, {"L", "R"}, CapMono},
    {"bands",   {"shift", "follow", "width", "level", kNone, kNone, kNone, kNone},       {0.5f, 0.35f, 0.5f, 0.25f, 0, 0, 0, 0},        {"carrier", "modulator"}, {"out", "loud"}, CapMono},

    {"env",     {"attack", "decay", "sustain", "release", "loop", kNone, kNone, kNone},  {0.1f, 0.4f, 0.7f, 0.3f, 0, 0, 0, 0},          {"gate"}, {"out", "busy"}, CapBoth},
    {"lfo",     {"wave", "rate", "sync", "slew", "depth", kNone, kNone, kNone},          {0, 0.4f, 0, 0, 1.0f, 0, 0, 0},                {"rate"}, {"out", "uni"}, CapBoth},
    {"snh",     {"track", kNone, kNone, kNone, kNone, kNone, kNone, kNone},              {0, 0, 0, 0, 0, 0, 0, 0},                      {"in", "trig"}, {"out"}, CapBoth},
    {"slew",    {"rise", "fall", kNone, kNone, kNone, kNone, kNone, kNone},              {0.3f, 0.3f, 0, 0, 0, 0, 0, 0},                {"in"}, {"out"}, CapBoth},

    {"clock",   {"division", "width", kNone, kNone, kNone, kNone, kNone, kNone},         {0.25f, 0.5f, 0, 0, 0, 0, 0, 0},               {kNone}, {"gate", "ramp", "reset"}, CapMono},
    {"euclid",  {"steps", "pulses", "rotate", kNone, kNone, kNone, kNone, kNone},        {0.45f, 0.25f, 0, 0, 0, 0, 0, 0},              {"clock", "reset"}, {"gate", "step"}, CapBoth},
    {"prob",    {"chance", kNone, kNone, kNone, kNone, kNone, kNone, kNone},             {0.5f, 0, 0, 0, 0, 0, 0, 0},                   {"in", "cv"}, {"pass", "skip"}, CapBoth},
    {"rand",    {"bipolar", "steps", kNone, kNone, kNone, kNone, kNone, kNone},          {0, 0, 0, 0, 0, 0, 0, 0},                      {"trig"}, {"out"}, CapBoth},
    {"quant",   {"scale", "root", kNone, kNone, kNone, kNone, kNone, kNone},             {0, 0, 0, 0, 0, 0, 0, 0},                      {"in"}, {"out", "trig"}, CapBoth},
    {"logic",   {"mode", kNone, kNone, kNone, kNone, kNone, kNone, kNone},               {0, 0, 0, 0, 0, 0, 0, 0},                      {"a", "b"}, {"out", "not"}, CapBoth},
    {"touch",   {kNone, kNone, kNone, kNone, kNone, kNone, kNone, kNone},                {0, 0, 0, 0, 0, 0, 0, 0},                      {kNone}, {"prs", "slide"}, CapPoly},
    {"swell",   {"floor", "ceiling", "amount", "split", "release", "mix", kNone, kNone}, {0.5f, 0.9f, 0.5f, 1.0f, 0.57f, 1.0f, 0, 0},    {"in", "amount"}, {"out"}, CapBoth},
    // The insert effects. Knobs and the second input are named after the
    // effect's own parameters, which is how FxMod finds them.
    {"reverb",  {"size", "damp", "tone", "predelay", "mix", "freeze", "shimmer", "wobble"}, {0.5f, 0.4f, 0.807f, 0.05f, 0.3f, 0, 0, 0}, {"in", "size"}, {"L", "R"}, CapMono},
    {"chorus",  {"rate", "depth", "voices", "spread", "drift", "mix", kNone, kNone},  {0.812f, 0.45f, 0.5f, 0.6f, 0.15f, 0.5f, 0, 0}, {"in", "depth"}, {"L", "R"}, CapMono},
    {"phaser",  {"rate", "depth", "feedback", "stages", "spread", "mix", kNone, kNone}, {0.875f, 0.7f, 0.333f, 0.333f, 0.5f, 0.5f, 0, 0}, {"in", "depth"}, {"L", "R"}, CapBoth},
    {"crush",   {"bits", "rate", "jitter", "tone", "mix", kNone, kNone, kNone},       {0.467f, 0.696f, 0, 1.0f, 1.0f, 0, 0, 0},     {"in", "bits"}, {"out"}, CapBoth},
    {"shift",   {"shift", "fine", "spread", "feedback", "mix", kNone, kNone, kNone},  {0.5f, 0.5f, 0, 0, 0.5f, 0, 0, 0},           {"in", "shift"}, {"L", "R"}, CapBoth},
    {"drive",   {"drive", "tone", "mix", "mode", "bias", kNone, kNone, kNone},        {0.376f, 0.766f, 1.0f, 0, 0, 0, 0, 0},        {"in", "drive"}, {"out"}, CapBoth},
    // The other machines' instruments. See Instruments.h.
    {"bore",    {"semis", "tension", "bell", "size", "brass", "bite", "air", "level"},  {0.5f, 0.611f, 0.556f, 0.6f, 0.45f, 0.4f, 0.15f, 0.5f}, {"breath", "pitch"}, {"out"}, CapBoth},
    {"pipe",    {"kind", "semis", "reed", "lip", "holes", "bell", "air", "level"},     {0, 0.5f, 0.47f, 0.46f, 0.537f, 0.41f, 0.12f, 0.5f}, {"breath", "pitch"}, {"out"}, CapBoth},
    {"reed",    {"kind", "semis", "blow", "air", "level", kNone, kNone, kNone},        {0, 0.5f, 0.558f, 0.333f, 0.5f, 0, 0, 0},     {"breath", "pitch"}, {"out"}, CapBoth},
    {"jaw",     {"kind", "semis", "ring", "overtones", "over ring", "snap", "drive", "level"}, {0, 0.5f, 0.6f, 0.3f, 0.37f, 0.5f, 0, 0.5f}, {"trig", "pitch", "drive"}, {"out"}, CapBoth},
    {"piano",   {"model", "hardness", "tone", "sustain", "unison", "stiffness", "sympathy", "board"}, {0, 0.5f, 0.5f, 0.5f, 0.333f, 0.5f, 0.5f, 0.7f}, {"pitch", "gate", "vel"}, {"L", "R"}, CapMono},
    {"throat",  {"vowel", "size", "nasal", "ring", "level", kNone, kNone, kNone},      {0.5f, 0.5f, 0, 0.5f, 0.5f, 0, 0, 0},         {"in", "vowel", "size"}, {"out"}, CapBoth},
    {"formula", {"semis", "speed", "a", "b", "c", "keyed", "level", kNone},            {0.5f, 0.5f, 0, 0, 0, 1.0f, 0.5f, 0},          {"x", "pitch"}, {"out"}, CapBoth},
    {"follow",  {"sure", "glide", "snap", kNone, kNone, kNone, kNone, kNone},          {0.46f, 0.35f, 0, 0, 0, 0, 0, 0},             {"in"}, {"pitch", "gate", "level"}, CapMono},
    // Whole machines, played by a pitch and a gate like the piano. Knobs are
    // the machine's own parameters, by name; defaults are the machine's.
    {"guitar",  {"model", "pickup", "coil", "tone", "mute", "buzz", "drive", "feedback"}, {0, 1.0f, 0, 0.8f, 0, 0, 0, 0}, {"pitch", "gate", "vel"}, {"L", "R"}, CapMono},
    {"mallets", {"model", "mallet", "position", "decay", "bright", "tube", "motor", "bloom"}, {0, 0.5f, 0.3f, 0.5f, 0.5f, 0.5f, 0, 0.5f}, {"pitch", "gate", "vel"}, {"L", "R"}, CapMono},
    {"sitar",   {"model", "sa", "scale", "bridge", "curve", "pluck", "sustain", "tarbs"}, {0, 0.0909f, 0, 0.6f, 0.3f, 0.5f, 0.5f, 0.5f}, {"pitch", "gate", "vel"}, {"L", "R"}, CapMono},
    {"drum",    {"model", "stroke", "position", "hand", "decay", "damp", "rattle", "body"}, {0.4f, 0, 0.5f, 0.5f, 0.5f, 0, 0, 0.5f}, {"pitch", "gate", "vel"}, {"L", "R"}, CapMono},
    {"pipes",   {"model", "key", "drones", "bag", "reed", "grace", "wheel", "dog"}, {0, 0.818f, 0.7f, 0.188f, 0.5f, 0, 0.5f, 0.5f}, {"pitch", "gate", "vel"}, {"L", "R"}, CapMono},
    {"bird",    {"pattern", "rate", "length", "sweep", "rasp", "two", "flock", "space"}, {0.2f, 0.5f, 0.474f, 0.646f, 0.1f, 0, 0, 0.2f}, {"pitch", "gate", "vel"}, {"L", "R"}, CapMono},
    {"water",   {"model", "density", "size", "rise", "decay", "gust", "whistle", "tone"}, {0, 0.5f, 0.208f, 0.4f, 0.5f, 0.5f, 0, 0.5f}, {"pitch", "gate", "vel"}, {"L", "R"}, CapMono},
};
} // namespace

const ModuleInfo &infoFor(int32_t type) {
    if (type < 0 || type >= TypeCount) return kInfo[TBlank];
    return kInfo[type];
}

int32_t typeFromName(const char *name) {
    if (name == nullptr) return TBlank;
    for (int32_t i = 0; i < TypeCount; ++i) {
        if (std::strcmp(kInfo[i].name, name) == 0) return i;
    }
    return -1; // unknown: the caller puts a blank in the slot and warns
}

void MachineMod::prepare(float sr, int32_t) {
    if (host == nullptr) host = MachineRegistry::create(machine);
    if (host == nullptr) return;
    host->prepare(static_cast<int32_t>(sr));
    const ModuleInfo &info = infoFor(type);
    for (int i = 0; i < kKnobs; ++i) param[i] = info.knob[i] != nullptr ? host->params().indexOf(info.knob[i]) : -1;
    reset();
}

Module *makeModule(int32_t type) {
    switch (type) {
    case TVoice: return new VoiceMod();
    case TPerf: return new PerfMod();
    case TMacro: return new MacroMod();
    case TOut: return new OutMod();
    case TScope: return new ScopeMod();
    case TOsc: return new OscMod();
    case TWtOsc: return new WtOscMod();
    case TNoise: return new NoiseMod();
    case TString: return new StringMod();
    case TWheels: return new WheelsMod();
    case TOp: return new OpMod();
    case TGrain: return new GrainMod();
    case TAudioIn: return new AudioInMod();
    case TFilter: return new FilterMod();
    case TVca: return new VcaMod();
    case TMix: return new MixMod();
    case TMath: return new MathMod();
    case TDelay: return new DelayMod();
    case TRotary: return new RotaryMod();
    case TBands: return new BandsMod();
    case TEnv: return new EnvMod();
    case TLfo: return new LfoMod();
    case TSnh: return new SnhMod();
    case TSlew: return new SlewMod();
    case TClock: return new ClockMod();
    case TEuclid: return new EuclidMod();
    case TProb: return new ProbMod();
    case TRand: return new RandMod();
    case TQuant: return new QuantMod();
    case TLogic: return new LogicMod();
    case TTouch: return new TouchMod();
    case TSwell: return new SwellMod();
    case TReverb: return new FxMod(type, "Reverb");
    case TChorus: return new FxMod(type, "Chorus");
    case TPhaser: return new FxMod(type, "Phaser");
    case TCrush: return new FxMod(type, "Bitcrusher");
    case TShift: return new FxMod(type, "Shifter");
    case TDrive: return new FxMod(type, "Distortion");
    case TBore: return new BoreMod();
    case TPipe: return new PipeMod();
    case TReed: return new ReedMod();
    case TJaw: return new JawMod();
    case TPiano: return new MachineMod(type, "Hammer");
    case TThroat: return new ThroatMod();
    case TFormula: return new FormulaMod();
    case TFollow: return new FollowMod();
    case TGuitar: return new MachineMod(type, "Fret");
    case TMallets: return new MachineMod(type, "Tine");
    case TSitar: return new MachineMod(type, "Sympath");
    case TDrum: return new MachineMod(type, "Palm");
    case TPipes: return new MachineMod(type, "Chanter");
    case TBird: return new MachineMod(type, "Aviary");
    case TWater: return new MachineMod(type, "Fathom");
    default: return new BlankMod();
    }
}

} // namespace acidulous::machine::nexus
