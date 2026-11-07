#include "core.h"
#include "json.h"

#include <EngineHost.h>
#include <drivers/AudioDriver.h>
#include <engine/core/Constants.h>
#include <engine/core/Handover.h>
#include <engine/core/Messages.h>
#include <engine/core/Reel.h>
#include <engine/core/Sample.h>
#include <engine/core/SampleMap.h>
#include <engine/core/Take.h>
#include <engine/core/Utterance.h>
#include <engine/dsp/Denormals.h>
#include <engine/format/AudioDecoder.h>
#include <engine/format/Sf2Reader.h>
#include <engine/effect/EffectRegistry.h>
#include <engine/effect/SlotMod.h>
#include <engine/inputmod/InputModRegistry.h>
#include <engine/machine/MachineRegistry.h>
#include <engine/machine/TableCounts.h>
#include <engine/machine/cumulus/Cloud.h>
#include <engine/machine/cumulus/Cumulus.h>
#include <engine/machine/diction/RecordedVoice.h>
#include <engine/machine/filament/SymLevel.h>
#include <engine/machine/formulate/Program.h>
#include <engine/machine/nexus/Graph.h>
#include <engine/machine/nexus/Modules.h>
#include <tools/patchbank.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cerrno>
#include <cstring>
#include <dirent.h>
#include <map>
#include <set>
#include <sys/stat.h>

namespace core
{

using namespace acidulous;
namespace ab = acidulous::audition;

static_assert(kBlockFrames == 64, "the FIFO is sized for 64-frame blocks");
static_assert(kParts <= kRackCount, "a part is a rack");

// The channel strip params /channel takes by index, from Rack::ChannelParam.
// Any other channel param can be set by name.
static const char* const kChannelNames[] = {"gain", "pan", "mute", "solo"};
// The engine units of the part's modifier slots.
static const char* const kModUnits[] = {"mod1", "mod2", "mod3"};
static_assert(sizeof(kModUnits) / sizeof(kModUnits[0]) == kModSlots, "a unit name per modifier slot");
// The engine units of the part's insert slots.
static const char* const kInsertUnits[] = {"effect1", "effect2", "effect3", "effect4"};
static_assert(sizeof(kInsertUnits) / sizeof(kInsertUnits[0]) == kEffectSlots, "a unit name per insert slot");

// Forage's 13 pads and the shared sample above them.
constexpr int kForagePads  = 13;
constexpr int kForageSlots = kForagePads + 1;
static_assert(kForageSlots <= Core::kHeldSlots, "a held entry per Forage slot");

// The level a file is heard at from the picker, under the machines.
constexpr float kHearGain = 0.5f;

// The biggest SoundFont /load takes, in MB. Reading one holds the whole file
// and then its preset's samples as floats, about three times the file's size
// at the peak, and the Pi has no swap.
// ACIDULOUS_SF2_LIMIT_MB in the environment overrides it, for tests.
constexpr long long kMaxSoundFontMB = 64;

static long long max_soundfont_bytes()
{
    const char* env = std::getenv("ACIDULOUS_SF2_LIMIT_MB");
    const long long mb = env && *env ? std::atoll(env) : kMaxSoundFontMB;
    return mb * 1024 * 1024;
}

// All the decoded samples every part holds together, in MB, counting a load's
// new samples before its old ones are freed. A Pi 4 norns has about 1.1 GB
// free with scsynth running and a Pi 3 about half that, with no swap.
// ACIDULOUS_SAMPLE_LIMIT_MB in the environment overrides it, for tests.
constexpr long long kSampleBudgetMB = 320;

long long sample_budget_bytes()
{
    const char* env = std::getenv("ACIDULOUS_SAMPLE_LIMIT_MB");
    const long long mb = env && *env ? std::atoll(env) : kSampleBudgetMB;
    return mb * 1024 * 1024;
}

// Cumulus's params before this one shape its spectrum, so changing one
// rebuilds the cloud (Cumulus::P, and CLOUD_PARAMS in the app's EngineSync).
constexpr int32_t kCloudParams = machine::Cumulus::Morph;

struct Core::BankCache
{
    ab::Bank bank;
    bool     ok = false;
};

namespace
{

template <typename T> void delete_as(void* p)
{
    delete static_cast<T*>(p);
}

int to_int(const Value& v)
{
    return v.type == 's' ? std::atoi(v.str.c_str()) : int(std::lround(v.num));
}

float to_float(const Value& v)
{
    return v.type == 's' ? float(std::atof(v.str.c_str())) : float(v.num);
}

std::string to_str(const Value& v)
{
    if(v.type == 's')
        return v.str;
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%g", v.num);
    return buf;
}

uint8_t clamp7(int v)
{
    return uint8_t(v < 0 ? 0 : v > 127 ? 127 : v);
}

bool lower_equal(const std::string& a, const std::string& b)
{
    if(a.size() != b.size())
        return false;
    for(size_t i = 0; i < a.size(); ++i)
        if(std::tolower((unsigned char)a[i]) != std::tolower((unsigned char)b[i]))
            return false;
    return true;
}

// Params a patch leaves alone, by machine.
bool patch_keeps(const std::string& machine, const char* param)
{
    if(machine != "Bias")
        return false;
    const std::string p = param;
    return p.size() == 5 && (p.compare(0, 4, "lane") == 0 || p.compare(0, 4, "mute") == 0) && p[4] >= '1' &&
           p[4] <= '4';
}

std::string lower(std::string s)
{
    for(char& c : s)
        c = char(std::tolower((unsigned char)c));
    return s;
}

std::string base_name(const std::string& path)
{
    std::string p = path;
    while(p.size() > 1 && p.back() == '/')
        p.pop_back();
    const size_t slash = p.find_last_of('/');
    return slash == std::string::npos ? p : p.substr(slash + 1);
}

std::string stem(const std::string& path)
{
    const std::string b   = base_name(path);
    const size_t      dot = b.find_last_of('.');
    return dot == std::string::npos || dot == 0 ? b : b.substr(0, dot);
}

bool has_ext(const std::string& name, std::initializer_list<const char*> exts)
{
    const std::string l = lower(name);
    for(const char* e : exts)
    {
        const size_t n = std::strlen(e);
        if(l.size() > n && l.compare(l.size() - n, n, e) == 0)
            return true;
    }
    return false;
}

// The audio files in a folder, not its subfolders, sorted by name without
// case. Hidden files (macOS leaves ._ files on sticks) are skipped.
std::vector<std::string> audio_files(const std::string& folder, bool wav_only)
{
    std::vector<std::string> out;
    DIR*                     d = opendir(folder.c_str());
    if(!d)
        return out;
    std::string dir = folder;
    if(!dir.empty() && dir.back() != '/')
        dir += '/';
    while(dirent* e = readdir(d))
    {
        const std::string name = e->d_name;
        if(name.empty() || name[0] == '.')
            continue;
        // the zone spec and the script's saved file list use | and lines
        if(name.find_first_of("|\n") != std::string::npos)
        {
            std::fprintf(stderr, "acidulous: %s: | in the name, left out\n", name.c_str());
            continue;
        }
        const bool ok = wav_only ? has_ext(name, {".wav"}) : has_ext(name, {".wav", ".aif", ".aiff", ".flac"});
        struct stat st{};
        if(ok && stat((dir + name).c_str(), &st) == 0 && S_ISREG(st.st_mode))
            out.push_back(dir + name);
    }
    closedir(d);
    std::sort(out.begin(), out.end(), [](const std::string& a, const std::string& b) {
        const std::string la = lower(a), lb = lower(b);
        return la != lb ? la < lb : a < b;
    });
    return out;
}

uint32_t le32(const unsigned char* b) { return b[0] | b[1] << 8 | b[2] << 16 | uint32_t(b[3]) << 24; }
uint16_t le16(const unsigned char* b) { return uint16_t(b[0] | b[1] << 8); }
uint32_t be32(const unsigned char* b) { return uint32_t(b[0]) << 24 | b[1] << 16 | b[2] << 8 | b[3]; }
uint16_t be16(const unsigned char* b) { return uint16_t(b[0] << 8 | b[1]); }

// A file's frames, rate and channels from its header, for WAV, AIFF and
// FLAC. False if the header isn't one of those or doesn't say.
bool probe_audio(const std::string& path, int64_t& frames, int& rate, int& channels)
{
    FILE* f = std::fopen(path.c_str(), "rb");
    if(!f)
        return false;
    unsigned char h[12];
    bool          ok = false;
    frames = 0, rate = 0, channels = 0;
    if(std::fread(h, 1, 12, f) == 12)
    {
        if(!std::memcmp(h, "RIFF", 4) && !std::memcmp(h + 8, "WAVE", 4))
        {
            int           bits = 0;
            int64_t       data = -1;
            unsigned char c[8];
            while(std::fread(c, 1, 8, f) == 8)
            {
                const uint32_t size = le32(c + 4);
                if(!std::memcmp(c, "fmt ", 4) && size >= 16)
                {
                    unsigned char fmt[16];
                    if(std::fread(fmt, 1, 16, f) != 16)
                        break;
                    channels = le16(fmt + 2), rate = int(le32(fmt + 4)), bits = le16(fmt + 14);
                    std::fseek(f, long(size - 16 + (size & 1)), SEEK_CUR);
                }
                else if(!std::memcmp(c, "data", 4))
                {
                    data = size;
                    break;
                }
                else if(std::fseek(f, long(size + (size & 1)), SEEK_CUR) != 0)
                    break;
            }
            if(data >= 0 && channels > 0 && bits >= 8)
                frames = data / (channels * (bits / 8)), ok = true;
        }
        else if(!std::memcmp(h, "FORM", 4) && (!std::memcmp(h + 8, "AIFF", 4) || !std::memcmp(h + 8, "AIFC", 4)))
        {
            unsigned char c[8];
            while(std::fread(c, 1, 8, f) == 8)
            {
                const uint32_t size = be32(c + 4);
                if(!std::memcmp(c, "COMM", 4) && size >= 18)
                {
                    unsigned char m[18];
                    if(std::fread(m, 1, 18, f) != 18)
                        break;
                    channels = be16(m), frames = be32(m + 2);
                    // the rate is an 80-bit extended float
                    const int exp  = ((m[8] & 0x7f) << 8 | m[9]) - 16383 - 31;
                    const uint32_t hi = be32(m + 10);
                    rate = int(std::ldexp(double(hi), exp));
                    ok   = channels > 0;
                    break;
                }
                if(std::fseek(f, long(size + (size & 1)), SEEK_CUR) != 0)
                    break;
            }
        }
        else if(!std::memcmp(h, "fLaC", 4))
        {
            // h[4..7] is the STREAMINFO block's header, then 34 bytes of it
            unsigned char si[34];
            if((h[4] & 0x7f) == 0 && std::fseek(f, 8, SEEK_SET) == 0 && std::fread(si, 1, 34, f) == 34)
            {
                rate     = si[10] << 12 | si[11] << 4 | si[12] >> 4;
                channels = ((si[12] >> 1) & 7) + 1;
                frames   = int64_t(si[13] & 15) << 32 | be32(si + 14);
                ok       = frames > 0 && rate > 0;
            }
        }
    }
    std::fclose(f);
    return ok && rate > 0 && channels > 0;
}

std::string mb(int64_t bytes)
{
    char text[32];
    const double m = double(bytes) / (1 << 20);
    std::snprintf(text, sizeof text, m < 10 ? "%.1f MB" : "%.0f MB", m);
    return text;
}

// The last note name in a file name, as a MIDI note with C4 = 60, or -1:
// a letter A-G not after another letter, an optional # or b, and an octave
// -1 to 9 not followed by another digit. "Piano C#4 ff" is 61.
int note_in_name(const std::string& name)
{
    static const int semis[] = {9, 11, 0, 2, 4, 5, 7}; // A B C D E F G
    int              found   = -1;
    for(size_t i = 0; i < name.size(); ++i)
    {
        const char c = char(std::toupper((unsigned char)name[i]));
        if(c < 'A' || c > 'G' || (i > 0 && std::isalpha((unsigned char)name[i - 1])))
            continue;
        size_t j    = i + 1;
        int    note = semis[c - 'A'];
        if(j < name.size() && (name[j] == '#' || name[j] == 'b'))
            note += name[j++] == '#' ? 1 : -1;
        bool neg = false;
        if(j < name.size() && name[j] == '-')
            neg = true, ++j;
        if(j >= name.size() || !std::isdigit((unsigned char)name[j]))
            continue;
        const int oct = (name[j] - '0') * (neg ? -1 : 1);
        if(j + 1 < name.size() && std::isdigit((unsigned char)name[j + 1]))
            continue;
        if(neg && oct != -1)
            continue;
        const int midi = (oct + 1) * 12 + note;
        if(midi >= 0 && midi <= 127)
            found = midi;
    }
    return found;
}

// The prompts a voice is recorded from, as the app's VoicePrompts lists them:
// each vowel held, then each consonant between two of each carrier vowel.
const char* const kVoiceVowels[] = {"IY", "IH", "EH", "AE", "AA", "AO", "AH", "UH",
                                    "UW", "ER", "EY", "AY", "AW", "OY", "OW"};
const char* const kVoiceConsonants[] = {"P", "B",  "T",  "D",  "K",  "G", "CH", "JH", "F", "V", "TH", "DH",
                                        "S", "Z",  "SH", "ZH", "HH", "M", "N",  "NG", "L", "R", "W",  "Y"};
const char* const kVoiceCarriers[] = {"AA", "IY", "UW"};

// The folder [path] is in, with its slash, or "".
std::string folder_of(const std::string& path)
{
    const size_t slash = path.find_last_of('/');
    return slash == std::string::npos ? std::string() : path.substr(0, slash + 1);
}

} // namespace

// What Diction sings a recorded voice from, as the app's VoiceBank.engineSpec
// builds it: a line per take that's cut and fine, from the voice's bank.json
// at [bank], with its takes beside it. [files] gets each such take's file
// name, there or not; takes not there are left out and counted in [missing].
// "" with [why] set if the bank can't be read.
std::string voice_spec(const std::string& bank, std::vector<std::string>& files, int& missing, std::string& why)
{
    files.clear();
    missing = 0;
    std::string text;
    {
        FILE* f = std::fopen(bank.c_str(), "rb");
        if(!f)
        {
            why = "not found";
            return "";
        }
        char   buf[4096];
        size_t n;
        while((n = std::fread(buf, 1, sizeof buf, f)) > 0 && text.size() < (size_t(1) << 20))
            text.append(buf, n);
        std::fclose(f);
    }
    JsonReader r{text};
    const Json j = r.value();
    const Json* takes = j.get("takes");
    const Json* cuts  = j.get("cuts");
    if(!r.ok || j.kind != Json::Object || !takes || takes->kind != Json::Object)
    {
        why = "not a voice";
        return "";
    }
    const std::string folder = folder_of(bank);
    std::string       spec;
    // Adds [head]'s line for prompt [id] if its take is cut and fine, with
    // [fields] of its cut after the path.
    const auto add = [&](const std::string& id, const std::string& head, std::initializer_list<const char*> fields) {
        const std::string file = takes->text(id.c_str());
        const Json*       cut  = cuts ? cuts->get(id.c_str()) : nullptr;
        if(file.empty() || !cut || cut->kind != Json::Object || !cut->text("problem").empty())
            return;
        if(file.find_first_of("/|\n") != std::string::npos || file == "." || file == "..")
            return;
        files.push_back(file);
        struct stat st{};
        if(stat((folder + file).c_str(), &st) != 0)
        {
            ++missing;
            return;
        }
        std::string line = head + "|" + folder + file;
        for(const char* k : fields)
        {
            const Json* v = cut->get(k);
            line += "|" + std::to_string(v && v->kind == Json::Number ? (long long)v->num : 0LL);
        }
        spec += (spec.empty() ? "" : "\n") + line;
    };
    for(const char* v : kVoiceVowels)
    {
        const std::string vs = v;
        const bool glides = vs == "EY" || vs == "AY" || vs == "AW" || vs == "OY" || vs == "OW";
        if(glides)
            add("v-" + lower(vs), "D|" + vs, {"holdFrom", "holdTo", "glideFrom", "glideTo"});
        else
            add("v-" + lower(vs), "V|" + vs, {"holdFrom", "holdTo"});
    }
    for(const char* c : kVoiceCarriers)
        for(const char* k : kVoiceConsonants)
            add(lower(c) + "-" + lower(k), std::string("C|") + k + "|" + c, {"consonantFrom", "consonantTo"});
    return spec;
}

// The bytes a file takes decoded to floats: at `to_rate` (0 keeps its own),
// the first `max_seconds` (0 all of it), mono kept as one channel unless
// `both` (a Take keeps two). A file whose header can't be read counts as
// eight times its size, more than any 16-bit or compressed file decodes to.
int64_t decoded_bytes(const std::string& path, int to_rate, int max_seconds, bool both)
{
    int64_t frames = 0;
    int     rate = 0, channels = 0;
    if(!probe_audio(path, frames, rate, channels))
    {
        struct stat st{};
        return stat(path.c_str(), &st) == 0 ? int64_t(st.st_size) * 8 : 0;
    }
    if(max_seconds > 0)
        frames = std::min<int64_t>(frames, int64_t(max_seconds) * rate);
    if(to_rate > 0)
        frames = frames * to_rate / rate + 1;
    const int kept = both || channels > 1 ? 2 : 1;
    return frames * kept * int64_t(sizeof(float));
}


Core::Core(const std::string& bank_dir_, Replier* replier_, bool inline_jobs_)
: host(EngineHost::instance()), bank_dir(bank_dir_), replier(replier_), inline_jobs(inline_jobs_)
{
    // Every track renders on the audio thread, since scsynth wants its core.
    // ACIDULOUS_WORKERS in the environment overrides it.
    setenv("ACIDULOUS_WORKERS", "0", 0);
    host.start();
    // The master fader at its top, 1.5, which is 5.5 dB over the app's
    // default of 0.8, so one Reflux part peaks around -9 dBFS. The limiter
    // (on, ceiling 0.95) keeps four loud parts from clipping.
    host.setParam(0, "master", "volume", 1.f, false);
    if(!inline_jobs)
    {
        worker       = std::thread([this] { worker_loop(); });
        cloud_thread = std::thread([this] { cloud_loop(); });
    }
}

Core::~Core()
{
    stopping = true;
    if(cloud_thread.joinable())
    {
        {
            std::lock_guard<std::mutex> l(cloud_lock);
            cloud_quit = true;
        }
        cloud_cv.notify_all();
        cloud_thread.join();
    }
    if(worker.joinable())
    {
        {
            std::lock_guard<std::mutex> l(jobs_lock);
            quitting = true;
        }
        jobs_cv.notify_all();
        worker.join();
    }
    // Empties the racks and sends so the next Core starts clean. Mounts
    // apply at the top of a block, at most kMaxMountsPerBlock (8) a block,
    // and the engine's queue holds 64, so blocks are pulled as they're
    // queued since the UGen no longer runs this core. A part queues at most
    // 8 (machine, 3 modifiers, 4 inserts), so a block after each keeps the
    // queue from filling. A retired machine frees what was mounted into it.
    // The UGen has stopped calling process(), but a pull may still be
    // finishing, so a pull that fails is tried again, for up to 100 ms.
    float      in[kBlockFrames * 2] = {}, out[kBlockFrames * 2];
    const auto pull                 = [&](int blocks_wanted) {
        for(int tries = 0, pulled = 0; tries < 100 && pulled < blocks_wanted; ++tries)
        {
            if(AudioDriver::pull(in, out))
                ++pulled;
            else
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    };
    drop_song();
    pull(2);
    for(int p = 0; p < kParts; ++p)
    {
        host.unmountMachine(p);
        clear_mods(p);
        for(int s = 0; s < kEffectSlots; ++s)
        {
            const char* fx = host.mountedEffect(p, s);
            if(fx && *fx)
            {
                if(std::strcmp(fx, "Venue") == 0)
                    host.loadImpulse(p, kInsertUnits[s], "", 1.f, 1.f, 1.f);
                host.mountEffect(p, s, "");
            }
            fx_formula[p][s].clear();
            impulse_on[p][s] = false;
        }
        pull(1);
    }
    for(int s = 0; s < kSends; ++s)
        if(send_mounted[s])
            host.mountSend(s, "");
    host.setMpeZone(0, 15, 48);
    // What's left in the queue: 64 entries at most, 8 a block.
    pull(10);
    host.stop();
    delete hear_buf.exchange(nullptr);
}

// --- Audio thread ------------------------------------------------------------

void Core::reset_fifo()
{
    std::memset(fifo_in, 0, sizeof(fifo_in));
    std::memset(fifo_out, 0, sizeof(fifo_out));
    fifo_pos = 0;
}

void Core::process(const float* in_l, const float* in_r, float* out_l, float* out_r, int n)
{
    dsp::flushDenormalsOnce();
    for(int i = 0; i < n; ++i)
    {
        fifo_in[fifo_pos * 2]     = in_l ? in_l[i] : 0.f;
        fifo_in[fifo_pos * 2 + 1] = in_r ? in_r[i] : 0.f;
        out_l[i]                  = fifo_out[fifo_pos * 2];
        out_r[i]                  = fifo_out[fifo_pos * 2 + 1];
        if(++fifo_pos == kBlockFrames)
        {
            // Silence while EngineHost has the driver stopped (an offline
            // render or a freeze). Blocks settle() rendered early go out
            // first; that only happens inline, in the render tool.
            if(early.empty())
            {
                if(!AudioDriver::pull(fifo_in, fifo_out))
                    std::memset(fifo_out, 0, sizeof(fifo_out));
            }
            else
            {
                Block b;
                if(!AudioDriver::pull(fifo_in, b.data()))
                    b.fill(0.f);
                early.push_back(b);
                std::memcpy(fifo_out, early.front().data(), sizeof(fifo_out));
                early.pop_front();
            }
            mix_hear(fifo_out);
            mounts_queued = false;
            blocks.fetch_add(1, std::memory_order_release);
            fifo_pos = 0;
        }
    }
}

// Adds a block of the file being heard. Audio thread; seq_cst on hear_busy
// and hear_buf, so hear() sees busy set or this sees the new buffer.
void Core::mix_hear(float* out)
{
    hear_busy.store(true);
    if(const std::vector<float>* b = hear_buf.load())
    {
        const int64_t total = int64_t(b->size() / 2);
        int64_t       at    = hear_pos.load(std::memory_order_relaxed);
        for(int i = 0; i < kBlockFrames && at < total; ++i, ++at)
        {
            out[i * 2] += kHearGain * (*b)[size_t(at) * 2];
            out[i * 2 + 1] += kHearGain * (*b)[size_t(at) * 2 + 1];
        }
        hear_pos.store(at, std::memory_order_relaxed);
    }
    hear_busy.store(false);
}

std::string Core::macro_names(int part) const
{
    return host.macroNames(part);
}

// The part's macro names as /macro_names, 8 strings, all "" to clear.
void Core::send_macro_names(int part)
{
    std::string names = host.macroNames(part);
    Args args = {Value::i(part)};
    size_t at = 0;
    for(int k = 0; k < 8; ++k)
    {
        size_t end = names.find('\n', at);
        if(end == std::string::npos)
            end = names.size();
        args.push_back(Value::s(at < names.size() ? names.substr(at, end - at) : ""));
        at = end + 1;
    }
    reply("/macro_names", args);
}

float Core::load_percent() const
{
    return host.loadPercent();
}

float Core::worst_percent()
{
    const float block_us = 1e6f * kBlockFrames / kSampleRate;
    return 100.f * float(host.worstBlockUs()) / block_us;
}

// --- Control thread ----------------------------------------------------------

void Core::reply(const char* path, const Args& args)
{
    if(replier)
        replier->reply(path, args);
}

void Core::error(const std::string& text)
{
    std::fprintf(stderr, "acidulous: %s\n", text.c_str());
    reply("/error", {Value::s(text)});
}

void Core::post(std::function<void()> job)
{
    if(inline_jobs)
    {
        job();
        return;
    }
    {
        std::lock_guard<std::mutex> l(jobs_lock);
        jobs.push_back(std::move(job));
    }
    jobs_cv.notify_one();
}

void Core::post_part(int part, std::function<void()> job)
{
    pending[part].fetch_add(1, std::memory_order_acq_rel);
    post([this, part, job = std::move(job)] {
        // counted down even if the job throws
        struct Done
        {
            std::atomic<int>& n;
            ~Done() { n.fetch_sub(1, std::memory_order_acq_rel); }
        } done{pending[part]};
        job();
    });
}

// A job for every part: messages for any part wait behind it.
void Core::post_all(std::function<void()> job)
{
    for(auto& n : pending)
        n.fetch_add(1, std::memory_order_acq_rel);
    post([this, job = std::move(job)] {
        struct Done
        {
            std::atomic<int>* n;
            ~Done()
            {
                for(int p = 0; p < kParts; ++p)
                    n[p].fetch_sub(1, std::memory_order_acq_rel);
            }
        } done{pending};
        job();
    });
}

// A queued mount is applied at the start of the next block that starts after
// it, so two finished blocks are enough. If nothing is rendering this gives up
// after half a second. The render tool runs jobs inline between blocks, where
// the order is already right.
void Core::wait_applied()
{
    if(inline_jobs)
        return;
    const uint64_t want = blocks.load(std::memory_order_acquire) + 2;
    for(int tries = 0; tries < 250 && !stopping && blocks.load(std::memory_order_acquire) < want; ++tries)
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
}

void Core::settle()
{
    if(!inline_jobs)
    {
        wait_applied();
        return;
    }
    if(!mounts_queued)
        return;
    float in[kBlockFrames * 2] = {};
    Block b;
    if(!AudioDriver::pull(in, b.data()))
        b.fill(0.f);
    early.push_back(b);
    mounts_queued = false;
    blocks.fetch_add(1, std::memory_order_release);
}

void Core::reset()
{
    host.panic();
    mpe_kind.store(0);
    mpe_members.store(15);
    host.setMpeZone(0, 15, mpe_bend.load());
    post_all([this] {
        if(!song_folder.empty())
            clear_song(true);
    });
    for(int p = 0; p < kParts; ++p)
        post_part(p, [this, p] {
            unmount(p);
            clear_mods(p);
        });
    drain();
}

void Core::drain()
{
    if(inline_jobs)
        return;
    std::unique_lock<std::mutex> l(jobs_lock);
    idle_cv.wait(l, [this] { return jobs.empty() && !busy; });
}

void Core::worker_loop()
{
    std::unique_lock<std::mutex> l(jobs_lock);
    for(;;)
    {
        // A teardown left unfinished is tried again every 250 ms, and
        // before each job.
        if(teardown_dirty)
            jobs_cv.wait_for(l, std::chrono::milliseconds(250), [this] { return quitting || !jobs.empty(); });
        else
            jobs_cv.wait(l, [this] { return quitting || !jobs.empty(); });
        if(quitting)
            return;
        if(teardown_dirty)
        {
            busy = true;
            l.unlock();
            resync_teardown();
            l.lock();
            busy = false;
        }
        if(jobs.empty())
        {
            idle_cv.notify_all();
            continue;
        }
        auto job = std::move(jobs.front());
        jobs.pop_front();
        busy = true;
        l.unlock();
        try
        {
            job();
        }
        catch(const std::exception& e)
        {
            error(std::string("worker: ") + e.what());
        }
        catch(...)
        {
            error("worker: unknown exception");
        }
        l.lock();
        busy = false;
        if(jobs.empty())
            idle_cv.notify_all();
    }
}

bool Core::command(const std::string& path, const Args& a)
{
    const auto part_ok = [&](int p) {
        if(p >= 0 && p < kParts)
            return true;
        error("no part " + std::to_string(p));
        return false;
    };
    // Channels are 1-16 as norns numbers them; the engine wants 0-15.
    const auto chan = [&](const Value& v) { return uint8_t(std::max(0, std::min(15, to_int(v) - 1))); };
    const auto member = [&](uint8_t ch) {
        const int kind = mpe_kind.load(), members = mpe_members.load();
        if(kind == 1)
            return ch >= 1 && ch <= members;
        if(kind == 2)
            return ch <= 14 && ch >= 15 - members;
        return false;
    };
    // Messages for a part with a mount or patch still on the worker wait behind
    // it, in order. Otherwise they go straight to the engine.
    const auto live = [&](int part, std::function<void()> fn) {
        if(inline_jobs || pending[part].load(std::memory_order_acquire) == 0)
            fn();
        else
            post_part(part, std::move(fn));
    };
    // The mod wheel, pedals and channel pressure go as performance params,
    // as the app does, so the machine sees them the same way.
    // EngineHost sends CC 1, 64, 66, 67 and pressure that way.
    const auto perf_cc = [&](int part, uint8_t cc, uint8_t value) {
        live(part, [this, part, cc, value] { host.controlChange(part, cc, value, false); });
    };
    const auto perf_pressure = [&](int part, uint8_t value) {
        live(part, [this, part, value] { host.channelPressure(part, value, false); });
    };
    // Anything else goes in as MIDI on its channel.
    const auto midi = [&](int part, const MidiMessage& m) {
        live(part, [this, part, m] { host.midiEvent(part, m.status, m.data1, m.data2, m.channel); });
    };

    if(path == "/note" && a.size() >= 4)
    {
        const int part = to_int(a[0]);
        if(!part_ok(part))
            return true;
        const uint8_t ch = chan(a[1]), note = clamp7(to_int(a[2])), vel = clamp7(to_int(a[3]));
        const uint8_t status = uint8_t((vel ? 0x90 : 0x80) | part);
        const MidiMessage m{status, note, vel, ch};
        if(member(ch))
        {
            if(vel)
                fingers[ch] = Finger{note, -1, -1, -1};
            else if(fingers[ch].note == note)
                fingers[ch].note = -1;
        }
        midi(part, m);
        return true;
    }
    if(path == "/cc" && a.size() >= 4)
    {
        const int part = to_int(a[0]);
        if(!part_ok(part))
            return true;
        const uint8_t ch = chan(a[1]), num = clamp7(to_int(a[2])), val = clamp7(to_int(a[3]));
        if(!member(ch) && (num == 1 || num == 64 || num == 66 || num == 67))
            perf_cc(part, num, val);
        else
        {
            const MidiMessage m{uint8_t(0xb0 | part), num, val, ch};
            if(num == 74 && member(ch))
                fingers[ch].timbre = val;
            midi(part, m);
        }
        return true;
    }
    if(path == "/bend" && a.size() >= 3)
    {
        const int part = to_int(a[0]);
        if(!part_ok(part))
            return true;
        int v = to_int(a[2]);
        v     = v < 0 ? 0 : v > 16383 ? 16383 : v;
        const MidiMessage m{uint8_t(0xe0 | part), uint8_t(v & 0x7f), uint8_t(v >> 7), chan(a[1])};
        if(member(m.channel))
            fingers[m.channel].bend = v;
        midi(part, m);
        return true;
    }
    if(path == "/pressure" && a.size() >= 3)
    {
        const int part = to_int(a[0]);
        if(!part_ok(part))
            return true;
        const uint8_t ch = chan(a[1]);
        if(member(ch))
        {
            const MidiMessage m{uint8_t(0xd0 | part), clamp7(to_int(a[2])), 0, ch};
            fingers[ch].pressure = m.data1;
            midi(part, m);
        }
        else
            perf_pressure(part, clamp7(to_int(a[2])));
        return true;
    }
    if(path == "/mount" && a.size() >= 2)
    {
        const int part = to_int(a[0]);
        if(part_ok(part))
            post_part(part, [this, part, m = to_str(a[1])] {
                if(mount(part, m) && parts[part].machine == "Cumulus")
                    request_cloud(part);
            });
        return true;
    }
    if(path == "/unmount" && a.size() >= 1)
    {
        const int part = to_int(a[0]);
        if(part_ok(part))
            post_part(part, [this, part] { unmount(part); });
        return true;
    }
    if(path == "/param" && a.size() >= 3)
    {
        const int part = to_int(a[0]);
        if(part_ok(part))
            post_part(part, [this, part, which = a[1], v = to_float(a[2])] { set_param(part, which, v); });
        return true;
    }
    if(path == "/channel" && a.size() >= 3)
    {
        const int part = to_int(a[0]);
        if(part_ok(part))
            post_part(part, [this, part, which = a[1], v = to_float(a[2])] { set_channel(part, which, v); });
        return true;
    }
    if(path == "/patch" && a.size() >= 3)
    {
        const int part = to_int(a[0]);
        if(part_ok(part))
            post_part(part, [this, part, m = to_str(a[1]), n = to_str(a[2])] { load_patch(part, m, n); });
        return true;
    }
    if(path == "/song_load" && a.size() >= 1)
    {
        // s:path [s:songs_dir [s:frozen mode]]. Every part waits behind it.
        // load_song checks the folder and fills in the default.
        std::string dir = a.size() > 1 ? to_str(a[1]) : std::string();
        FrozenMode  mode = FrozenMode::Auto;
        if(a.size() > 2 && !parse_frozen_mode(to_str(a[2]), mode))
            error("song: frozen clips are auto, notes or frozen, not " + to_str(a[2]));
        post_all([this, p = to_str(a[0]), dir, mode] { load_song(p, dir, "", mode); });
        return true;
    }
    if(path == "/song_unload")
    {
        post_all([this] { unload_song(); });
        return true;
    }
    if(path == "/song_play")
    {
        // [i:scene], else from the scene the transport is on. On the worker,
        // so it waits for a /song_load sent before it.
        const int scene = a.empty() ? -1 : to_int(a[0]);
        post([this, scene] {
            if(song_folder.empty())
                return error("song: no song loaded");
            host.transportPlay(scene < 0 || size_t(scene) >= song_scenes.size() ? -1 : scene);
        });
        return true;
    }
    if(path == "/song_stop")
    {
        post([this] {
            host.queueScene(-1);
            host.transportStop();
        });
        return true;
    }
    if(path == "/song_queue" && a.size() >= 1)
    {
        post([this, which = a[0]] { queue_scene(which); });
        return true;
    }
    if(path == "/song_position")
    {
        // On the worker, after what was sent before it.
        post([this] {
            Args out;
            if(song_position(out))
                reply("/song_position", out);
        });
        return true;
    }
    if(path == "/state" && a.size() >= 1)
    {
        const int part = to_int(a[0]);
        if(part_ok(part))
            post_part(part, [this, part] { send_state(part); });
        return true;
    }
    if(path == "/defs" && a.size() >= 1)
    {
        post([this, m = to_str(a[0])] { send_defs(m); });
        return true;
    }
    if(path == "/patches" && a.size() >= 1)
    {
        post([this, m = to_str(a[0])] { send_patches(m); });
        return true;
    }
    if(path == "/machines")
    {
        Args out;
        for(int32_t i = 0; i < MachineRegistry::count(); ++i)
            out.push_back(Value::s(MachineRegistry::name(i)));
        reply("/machines", out);
        return true;
    }
    if(path == "/load" && a.size() >= 3)
    {
        const int part = to_int(a[0]);
        if(part_ok(part))
        {
            std::vector<std::string> roots;
            for(size_t k = 4; k < a.size(); ++k)
                roots.push_back(to_str(a[k]));
            post_part(part, [this, part, kind = to_str(a[1]), p = to_str(a[2]), arg = a.size() > 3 ? to_int(a[3]) : 0,
                             roots] { load(part, kind, p, arg, roots); });
        }
        return true;
    }
    if(path == "/preset_save" && a.size() >= 4)
    {
        // i:part s:path s:name i:nroots s:root... (s:key s:value)...
        const int part = to_int(a[0]);
        if(!part_ok(part))
            return true;
        const size_t nroots = size_t(std::max(0, to_int(a[3])));
        std::vector<std::string> roots;
        for(size_t k = 4; k < a.size() && k < 4 + nroots; ++k)
            roots.push_back(to_str(a[k]));
        std::vector<std::pair<std::string, std::string>> settings;
        for(size_t k = 4 + nroots; k + 1 < a.size(); k += 2)
            settings.emplace_back(to_str(a[k]), to_str(a[k + 1]));
        post_part(part, [this, part, p = to_str(a[1]), n = to_str(a[2]), settings, roots] {
            save_preset(part, p, n, settings, roots);
        });
        return true;
    }
    if(path == "/preset_load" && a.size() >= 7)
    {
        // i:part s:path i:index s:tag i:files s:machine s:name s:root...
        const int part = to_int(a[0]);
        if(!part_ok(part))
            return true;
        std::vector<std::string> roots;
        for(size_t k = 7; k < a.size(); ++k)
            roots.push_back(to_str(a[k]));
        post_part(part, [this, part, p = to_str(a[1]), index = to_int(a[2]), tag = to_str(a[3]),
                         files = to_int(a[4]) != 0, m = to_str(a[5]), n = to_str(a[6]), roots] {
            load_preset(part, p, index, tag, files, m, n, roots);
        });
        return true;
    }
    if(path == "/preset_scan" && a.size() >= 1)
    {
        post([this, f = to_str(a[0]), deep = a.size() > 1 && to_int(a[1]) != 0] { scan_presets(f, deep); });
        return true;
    }
    if(path == "/fx_preset_load" && a.size() >= 5)
    {
        // i:part i:slot s:path i:index s:name s:root...
        const int part = to_int(a[0]), slot = to_int(a[1]);
        if(part_ok(part) && slot >= kPatchInsertSlots && slot < kEffectSlots)
        {
            std::vector<std::string> roots;
            for(size_t k = 5; k < a.size(); ++k)
                roots.push_back(to_str(a[k]));
            post_part(part, [this, part, slot, p = to_str(a[2]), index = to_int(a[3]), n = to_str(a[4]), roots] {
                load_fx_preset(part, slot, p, index, n, roots);
            });
        }
        else if(part_ok(part))
            error("insert slot " + std::to_string(slot) + " is a patch's");
        return true;
    }
    if(path == "/fx_preset_save" && a.size() >= 4)
    {
        // i:part i:slot s:path s:name s:root...
        const int part = to_int(a[0]), slot = to_int(a[1]);
        if(part_ok(part) && slot >= kPatchInsertSlots && slot < kEffectSlots)
        {
            std::vector<std::string> roots;
            for(size_t k = 4; k < a.size(); ++k)
                roots.push_back(to_str(a[k]));
            post_part(part, [this, part, slot, p = to_str(a[2]), n = to_str(a[3]), roots] {
                save_fx_preset(part, slot, p, n, roots);
            });
        }
        else if(part_ok(part))
            error("insert slot " + std::to_string(slot) + " is a patch's");
        return true;
    }
    if(path == "/mod_preset_load" && a.size() >= 5)
    {
        // i:part i:slot s:path i:index s:name
        const int part = to_int(a[0]), slot = to_int(a[1]);
        if(part_ok(part) && slot >= 0 && slot < kModSlots)
            post_part(part, [this, part, slot, p = to_str(a[2]), index = to_int(a[3]), n = to_str(a[4])] {
                load_mod_preset(part, slot, p, index, n);
            });
        return true;
    }
    if(path == "/mod_preset_save" && a.size() >= 4)
    {
        // i:part i:slot s:path s:name
        const int part = to_int(a[0]), slot = to_int(a[1]);
        if(part_ok(part) && slot >= 0 && slot < kModSlots)
            post_part(part, [this, part, slot, p = to_str(a[2]), n = to_str(a[3])] { save_mod_preset(part, slot, p, n); });
        return true;
    }
    if(path == "/preset_delete" && a.size() >= 2)
    {
        post([this, p = to_str(a[0]), index = to_int(a[1])] { delete_preset(p, index); });
        return true;
    }
    if(path == "/shape" && a.size() >= 1)
    {
        post([this, p = to_str(a[0])] { send_shape(p); });
        return true;
    }
    if(path == "/hear")
    {
        post([this, p = a.empty() ? std::string() : to_str(a[0])] { hear(p); });
        return true;
    }
    if(path == "/presets" && a.size() >= 1)
    {
        post([this, p = to_str(a[0])] { send_presets(p); });
        return true;
    }
    if(path == "/mpe" && a.size() >= 3)
    {
        // members 0 turns MPE off. The zone is the lower one: master channel
        // 1, members from 2 up. The part only matters to Lua's routing.
        const int members = to_int(a[1]);
        mpe_kind.store(members > 0 ? 1 : 0);
        mpe_members.store(members > 0 ? std::min(members, 15) : 15);
        mpe_bend.store(to_float(a[2]));
        // Here rather than on the audio thread, as the app does, so the routing
        // below and EngineHost's agree from the next message on.
        host.setMpeZone(mpe_kind.load(), mpe_members.load(), mpe_bend.load());
        for(Finger& f : fingers)
            f = Finger{};
        return true;
    }
    if(path == "/mpe_state")
    {
        // After everything queued so far, so the engine has seen it. The held
        // mask is read off the audio thread, which is fine for a test readout.
        post([this, f = std::vector<Finger>(fingers, fingers + 16)] {
            wait_applied();
            Args out{Value::i(mpe_kind.load()), Value::i(mpe_members.load()), Value::f(mpe_bend.load()),
                     Value::i(host.mpeHeldMask())};
            for(int c = 0; c < 16; ++c)
                if(f[size_t(c)].note >= 0)
                    for(int v : {c + 1, f[size_t(c)].note, f[size_t(c)].bend, f[size_t(c)].pressure, f[size_t(c)].timbre})
                        out.push_back(Value::i(v));
            reply("/mpe_state", out);
        });
        return true;
    }
    if(path == "/send" && a.size() >= 3)
    {
        const int slot = to_int(a[0]);
        if(slot >= 0 && slot < kSends)
            post([this, slot, n = to_str(a[1]), v = to_float(a[2])] { set_send(slot, n, v); });
        return true;
    }
    if(path == "/fx" && a.size() >= 4)
    {
        const int part = to_int(a[0]), slot = to_int(a[1]);
        if(part_ok(part) && slot >= 0 && slot < kEffectSlots)
            post_part(part, [this, part, slot, n = to_str(a[2]), v = to_float(a[3])] { set_fx(part, slot, n, v); });
        return true;
    }
    if(path == "/impulse" && a.size() >= 3)
    {
        // i:part i:slot s:impulse s:root...
        const int part = to_int(a[0]), slot = to_int(a[1]);
        if(part_ok(part) && slot >= 0 && slot < kEffectSlots)
        {
            std::vector<std::string> roots;
            for(size_t k = 3; k < a.size(); ++k)
                roots.push_back(to_str(a[k]));
            post_part(part, [this, part, slot, i = to_str(a[2]), roots] { set_impulse(part, slot, i, roots); });
        }
        return true;
    }
    if(path == "/insert" && a.size() >= 3)
    {
        const int part = to_int(a[0]), slot = to_int(a[1]);
        if(part_ok(part) && slot >= kPatchInsertSlots && slot < kEffectSlots)
            post_part(part, [this, part, slot, t = to_str(a[2])] { set_insert(part, slot, t); });
        else if(part_ok(part))
            error("insert slot " + std::to_string(slot) + " is a patch's");
        return true;
    }
    if(path == "/effects")
    {
        Args out;
        for(int32_t i = 0; i < EffectRegistry::count(); ++i)
            out.push_back(Value::s(EffectRegistry::name(i)));
        reply("/effects", out);
        return true;
    }
    if(path == "/mod" && a.size() >= 3)
    {
        const int part = to_int(a[0]), slot = to_int(a[1]);
        if(part_ok(part) && slot >= 0 && slot < kModSlots)
            post_part(part, [this, part, slot, t = to_str(a[2])] { set_mod(part, slot, t); });
        return true;
    }
    if(path == "/mod_bypass" && a.size() >= 3)
    {
        const int part = to_int(a[0]), slot = to_int(a[1]);
        if(part_ok(part) && slot >= 0 && slot < kModSlots)
            post_part(part, [this, part, slot, on = to_int(a[2]) != 0] {
                if(mod_types[part][slot].empty())
                    return;
                // note offs sent before land first, so none goes to the
                // modifier coming back on
                if(!on)
                    settle();
                mod_bypass[part][slot] = on;
                push_param(part, kModUnits[slot], "bypass", on ? 1.f : 0.f);
            });
        return true;
    }
    if(path == "/mods_off" && a.size() >= 1)
    {
        const int part = to_int(a[0]);
        if(part_ok(part))
            post_part(part, [this, part] { mods_off(part); });
        return true;
    }
    if(path == "/mod_param" && a.size() >= 4)
    {
        const int part = to_int(a[0]), slot = to_int(a[1]);
        if(part_ok(part) && slot >= 0 && slot < kModSlots)
            post_part(part, [this, part, slot, n = to_str(a[2]), v = to_float(a[3])] { set_mod_param(part, slot, n, v); });
        return true;
    }
    if(path == "/mods" && a.size() >= 1)
    {
        const int part = to_int(a[0]);
        if(part_ok(part))
            post_part(part, [this, part] { send_mods(part); });
        return true;
    }
    if(path == "/mod_defs")
    {
        post([this] { send_mod_defs(); });
        return true;
    }
    if(path == "/master" && a.size() >= 2)
    {
        post([this, n = to_str(a[0]), v = to_float(a[1])] {
            if(host.paramIndex("", "master", n) < 0)
                error("no master param " + n);
            else
            {
                // With a song loaded it moves the song's master.
                if(song_folder.empty())
                    own_master[n] = v;
                push_param(0, "master", n, v);
            }
        });
        return true;
    }
    if(path == "/tempo")
    {
        // The engine's tempo, which its clock runs at with the transport
        // stopped too (arps, synced delays and LFOs). Any thread may set it.
        // A loaded song keeps its own tempo.
        if(!a.empty())
        {
            if(!song_on.load())
                host.setTempo(to_float(a[0]));
        }
        else
            post([this] {
                wait_applied();
                reply("/tempo", {Value::f(host.tempo())});
            });
        return true;
    }
    if(path == "/beat" && a.size() >= 2)
    {
        // norns' clock beat and tempo. The frame is read as it arrives,
        // which is a few ms after the script read the beat; alignBeat can't
        // be called on the audio thread, so this is as close as it gets.
        // One thread only: the OSC thread.
        // Not while a song is loaded: it keeps its own tempo and scene
        // tempos, which following a beat would override.
        std::lock_guard<std::mutex> l(beat_lock);
        if(!song_on.load())
            host.alignBeat(to_float(a[0]), to_float(a[1]), host.framesRendered());
        return true;
    }
    if(path == "/beat_release")
    {
        host.releaseBeat();
        return true;
    }
    if(path == "/meters" && a.size() >= 1)
    {
        metering.store(to_int(a[0]) != 0);
        return true;
    }
    if(path == "/graph" && a.size() >= 1)
    {
        const int part = to_int(a[0]);
        if(part_ok(part))
            post_part(part, [this, part] { send_graph(part); });
        return true;
    }
    if(path == "/scope" && a.size() >= 2)
    {
        const int part = to_int(a[0]);
        if(part_ok(part))
            post_part(part, [this, part, slot = to_int(a[1])] { set_scope(part, slot); });
        return true;
    }
    return false;
}

// --- Worker thread -----------------------------------------------------------

bool Core::mount(int part, const std::string& name)
{
    // The registry's own spelling of the name, which the replies use.
    int32_t i = 0;
    while(i < MachineRegistry::count() && !lower_equal(MachineRegistry::name(i), name))
        ++i;
    if(i == MachineRegistry::count())
    {
        error("no machine " + name);
        return false;
    }
    const std::string type = MachineRegistry::name(i);
    mounts_queued = true;
    bool queued;
    {
        // A cloud build in flight finishes first, and one not yet started is
        // dropped, so no cloud is handed to the new machine.
        std::lock_guard<std::mutex> l(mount_locks[part]);
        forget_cloud(part);
        clear_held(part);
        queued = host.mountMachine(part, type);
    }
    if(!queued)
    {
        error("mount queue full");
        return false;
    }
    wait_applied();
    host.setMacroNames(part, "");
    parts[part] = Part{};
    parts[part].machine = type;
    if(scope_part.load() == part)
        scope_part.store(-1);
    int32_t         count = 0;
    const ParamDef* defs  = MachineRegistry::paramDefs(type.c_str(), count);
    for(int32_t k = 0; k < count; ++k)
        parts[part].values.push_back(defs[k].unmap(defs[k].def));
    reply("/mounted", {Value::i(part), Value::s(parts[part].machine)});
    send_macro_names(part);
    return true;
}

void Core::unmount(int part)
{
    mounts_queued = true;
    {
        std::lock_guard<std::mutex> l(mount_locks[part]);
        forget_cloud(part);
        clear_held(part);
        host.unmountMachine(part);
    }
    // An empty part keeps no patch effects: the inserts its patches put in go
    // and the sends they hold go back to the player's.
    apply_chain(part, acidulous::PatchChain{}, "unmount");
    wait_applied();
    host.setMacroNames(part, "");
    parts[part] = Part{};
    if(scope_part.load() == part)
        scope_part.store(-1);
    reply("/mounted", {Value::i(part), Value::s("")});
    send_macro_names(part);
}

void Core::set_param(int part, const Value& which, float v01)
{
    const Part& p = parts[part];
    if(p.machine.empty())
        return;
    int32_t         count = 0;
    const ParamDef* defs  = MachineRegistry::paramDefs(p.machine.c_str(), count);
    int32_t         index = -1;
    if(which.type != 's')
        index = to_int(which);
    else
        for(int32_t i = 0; i < count; ++i)
            if(which.str == defs[i].name)
                index = i;
    if(index < 0 || index >= count)
    {
        error(p.machine + " has no param " + to_str(which));
        return;
    }
    if(!push_param(part, "machine", defs[index].name, v01))
        return;
    if(size_t(index) < p.values.size())
        parts[part].values[size_t(index)] = v01 < 0.f ? 0.f : v01 > 1.f ? 1.f : v01;
    if(p.machine == "Cumulus" && index < kCloudParams)
        request_cloud(part);
}

// How long a cloud request waits for more changes before it builds, so a
// patch followed by its edits (a PSET) builds once.
constexpr int kCloudSettleMs = 50;

// Cumulus is silent until it has a cloud, built from its spectrum params and
// mounted. Worker thread. Inline (the render tool) it builds now.
void Core::request_cloud(int part)
{
    const Part&        p = parts[part];
    std::vector<float> values(p.values.begin(), p.values.begin() + std::min<size_t>(p.values.size(), kCloudParams));
    if(inline_jobs)
    {
        settle();
        if(values != clouds[part].built)
            build_cloud(part, values, clouds[part].epoch);
        return;
    }
    {
        std::lock_guard<std::mutex> l(cloud_lock);
        CloudRequest& c = clouds[part];
        c.want   = true;
        c.values = std::move(values);
        c.due    = std::chrono::steady_clock::now() + std::chrono::milliseconds(kCloudSettleMs);
    }
    cloud_cv.notify_one();
}

// A /load clear: takes the machine's cloud, graph, program or samples out and
// the engine frees them. Worker thread, under the part's mount lock.
void Core::clear_objects(int part)
{
    const std::string& m = parts[part].machine;
    void (*deleter)(void*) = nullptr;
    int slots              = 1;
    if(m == "Cumulus")
        deleter = delete_as<machine::cumulus::CloudSet>;
    else if(m == "Nexus")
        deleter = delete_as<machine::nexus::Graph>;
    else if(m == "Formulate")
        deleter = delete_as<machine::formulate::Program>;
    else if(m == "Forage")
        deleter = delete_as<SampleData>, slots = kForageSlots;
    else if(m == "Mosaic")
        deleter = delete_as<SampleMap>;
    else if(m == "Dice" || m == "Pollen")
        deleter = delete_as<audio::Take>;
    else if(m == "Molt")
        deleter = delete_as<audio::Utterance>;
    else if(m == "Bias")
        deleter = delete_as<audio::Reel>;
    else if(m == "Diction")
        deleter = delete_as<machine::diction::RecordedVoice>, slots = 2;
    else
        return;
    clear_held(part);
    for(auto& v : parts[part].voice)
        v.clear();
    parts[part].voice_lsf[0] = parts[part].voice_lsf[1] = false;
    for(int slot = 0; slot < slots; ++slot)
    {
        Mount mt;
        mt.kind    = Mount::Kind::Object;
        mt.rack    = part;
        mt.slot    = slot;
        mt.object  = nullptr;
        mt.deleter = deleter;
        if(!host.mountObjectWithRetry(mt))
            error("mount queue full");
    }
}

// Before a mount or unmount, under the part's mount lock: the new machine has
// no cloud, and a waiting request was for the old one. Worker thread.
void Core::forget_cloud(int part)
{
    std::lock_guard<std::mutex> l(cloud_lock);
    CloudRequest& c = clouds[part];
    c.want = false;
    c.built.clear();
    ++c.epoch;
}

// Cloud thread, or the render tool's main loop. buildCloud checks the rack
// holds a Cumulus and then mounts the cloud into whatever is there, so the
// part's mount lock is held across it: a remount waits for it, and a request
// from before a remount is dropped.
void Core::build_cloud(int part, const std::vector<float>& values, uint64_t epoch)
{
    std::string why;
    {
        std::lock_guard<std::mutex> m(mount_locks[part]);
        {
            std::lock_guard<std::mutex> l(cloud_lock);
            if(stopping || clouds[part].epoch != epoch)
                return;
        }
        why = host.buildCloud(part, values.data(), int32_t(values.size()));
    }
    {
        std::lock_guard<std::mutex> l(cloud_lock);
        if(clouds[part].epoch != epoch)
            return; // remounted while it built: a request for the new one follows
        if(why.empty())
            clouds[part].built = values;
    }
    if(!why.empty())
        error("Cumulus: " + why);
}

void Core::cloud_loop()
{
    std::unique_lock<std::mutex> l(cloud_lock);
    for(;;)
    {
        if(cloud_quit || stopping)
            return;
        // The earliest request, a cloud's or an impulse's.
        int  next = -1, ip = -1, is = -1;
        auto now  = std::chrono::steady_clock::now();
        for(int p = 0; p < kParts; ++p)
            if(clouds[p].want && (next < 0 || clouds[p].due < clouds[next].due))
                next = p;
        for(int p = 0; p < kParts; ++p)
            for(int s = 0; s < kEffectSlots; ++s)
                if(impulse_req[p][s].want && (ip < 0 || impulse_req[p][s].due < impulse_req[ip][is].due))
                    ip = p, is = s;
        if(next < 0 && ip < 0)
        {
            cloud_cv.wait(l);
            continue;
        }
        const bool cloud = ip < 0 || (next >= 0 && clouds[next].due <= impulse_req[ip][is].due);
        const auto due   = cloud ? clouds[next].due : impulse_req[ip][is].due;
        if(due > now)
        {
            cloud_cv.wait_until(l, due);
            continue;
        }
        if(!cloud)
        {
            ImpulseRequest r = impulse_req[ip][is];
            impulse_req[ip][is].want = false;
            l.unlock();
            build_impulse(ip, is, r.impulse, r.shape);
            l.lock();
            continue;
        }
        CloudRequest&      c      = clouds[next];
        c.want                    = false;
        std::vector<float> values = c.values;
        const uint64_t     epoch  = c.epoch;
        if(values == c.built)
            continue;
        l.unlock();
        build_cloud(next, values, epoch);
        l.lock();
    }
}

// A Nexus patch's graph, a line per module and cable, built and mounted by
// EngineHost.
bool Core::load_nexus(int part, const std::string& text)
{
    settle();
    const std::string why = host.loadNexusPatch(part, text);
    if(!why.empty())
        error("Nexus: " + why);
    return why.empty();
}

// The track's channel strip, in Rack::ChannelParam's order. Applies with or
// without a machine.
void Core::set_channel(int part, const Value& which, float v01)
{
    std::string name;
    if(which.type == 's')
        name = which.str;
    else if(to_int(which) >= 0 && to_int(which) < 4)
        name = kChannelNames[to_int(which)];
    if(name.empty() || host.paramIndex("", "channel", name) < 0)
    {
        error("no channel param " + to_str(which));
        return;
    }
    // A send's effect is mounted the first time a part sends to it, so the
    // reverb and delay cost nothing until then.
    if(name == "sendreverb" || name == "senddelay")
    {
        const int slot        = name == "sendreverb" ? 0 : 1;
        part_sends[part][slot] = v01 < 0.f ? 0.f : v01 > 1.f ? 1.f : v01;
        if(v01 > 0.f)
            ensure_send(slot);
    }
    push_param(part, "channel", name, v01);
}

static int64_t now_ms()
{
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

// The two send buses, as the app starts them.
static const char* const kSendTypes[] = {"Reverb", "Delay"};
static const char* const kSendUnits[] = {"send1", "send2"};

static bool send_has(int slot, const std::string& name)
{
    int32_t         n    = 0;
    const ParamDef* defs = EffectRegistry::paramDefs(kSendTypes[slot], n);
    for(int32_t i = 0; i < n; ++i)
        if(name == defs[i].name)
            return true;
    return false;
}

// Mounts the send's effect, fully wet, with the values set so far.
void Core::ensure_send(int slot)
{
    if(send_mounted[slot])
        return;
    if(!host.mountSend(slot, kSendTypes[slot]))
    {
        error("mount queue full");
        return;
    }
    send_mounted[slot] = true;
    for(const auto& kv : send_values[slot])
        push_param(0, kSendUnits[slot], kv.first, kv.second);
}

// Puts a patch's inserts and sends on the part's track, by EngineHost's rule
// (engine/core/PatchChain.h): it empties the slots the last patch filled,
// mounts a new effect for each insert in a free slot and sets the sends, or
// gives the player's back. The effects go through the mount queue and are the
// engine's from then on. An empty chain takes the last patch's effects off.
// Sends it changes go to the script as "/sends i:part f:reverb f:delay".
void Core::apply_chain(int part, const acidulous::PatchChain& chain, const std::string& what)
{
    if(!chain.inserts.empty() || patch_fx[part].owned != 0)
        mounts_queued = true;
    // Which insert each slot gets, worked out as applyPatchChain does: the
    // inserts this build has, placed by planPatchInserts on the slots as they
    // are now. A slot whose mount fails isn't owned after, so it shows none.
    std::vector<const acidulous::PatchInsert*> known;
    for(const auto& fx : chain.inserts)
    {
        int32_t n = 0;
        if(EffectRegistry::paramDefs(fx.type.c_str(), n) && n > 0)
            known.push_back(&fx);
    }
    bool occupied[kEffectSlots];
    for(int s = 0; s < kEffectSlots; ++s)
        occupied[s] = !std::string(host.mountedEffect(part, s)).empty();
    const acidulous::InsertPlan slots = acidulous::planPatchInserts(occupied, patch_fx[part].owned, int32_t(known.size()));
    const acidulous::PatchInsert* at[kEffectSlots] = {};
    for(size_t i = 0; i < known.size() && i < size_t(kPatchInsertSlots); ++i)
        if(slots.slotOf[i] >= 0)
            at[slots.slotOf[i]] = known[i];

    // The engine can't resolve the app's "factory:<id>", so a Venue's room goes in as its file.
    acidulous::PatchChain mapped = chain;
    for(auto& fx : mapped.inserts)
        if(fx.type == "Venue")
            for(auto& kv : fx.settings)
                if(kv.first == "impulse")
                    kv.second = room_to_file(kv.second);
    int32_t                   skipped = 0, unknown = 0;
    const acidulous::SendPlan plan    = host.applyPatchChain(part, mapped, patch_fx[part], part_sends[part][0],
                                                             part_sends[part][1], &skipped, &unknown);
    if(skipped > 0)
        std::fprintf(stderr, "acidulous: %s: %d insert(s) found no free slot\n", what.c_str(), int(skipped));
    if(unknown > 0)
        std::fprintf(stderr, "acidulous: %s: %d insert(s) name no effect this build has\n", what.c_str(), int(unknown));
    sync_impulses(part, at);
    send_inserts(part, at);
    if(!plan.set)
        return;
    part_sends[part][0] = plan.reverb;
    part_sends[part][1] = plan.delay;
    for(int s = 0; s < kSends; ++s)
        if(part_sends[part][s] > 0.f)
            ensure_send(s);
    reply("/sends", {Value::i(part), Value::f(plan.reverb), Value::f(plan.delay)});
}

// "/inserts i:part" and per mounted insert slot "i:slot s:type i:count" and
// count params of "s:name f:min f:max f:default s:unit i:steps s:curve
// f:value01", as /def has them, then "i:mcount" and the slot's mod params
// ("~mod") the same way. The values are the ones the patch set, or the
// defaults (with [live], what the engine holds). [at] is the insert the patch
// put in each slot, or null.
void Core::send_inserts(int part, const acidulous::PatchInsert* const at[kEffectSlots], bool live)
{
    if(quiet_inserts)
        return;
    int32_t         mod_count = 0;
    const ParamDef* mod_defs  = EffectRegistry::paramDefs(kSlotModType, mod_count);
    Args            out{Value::i(part)};
    for(int slot = 0; slot < kEffectSlots; ++slot)
    {
        const std::string type = host.mountedEffect(part, slot);
        int32_t           count = 0;
        const ParamDef*   defs  = type.empty() ? nullptr : EffectRegistry::paramDefs(type.c_str(), count);
        if(!defs || count <= 0)
            continue;
        const acidulous::PatchInsert* fx = nullptr;
        if(((patch_fx[part].owned >> slot) & 1u) && at[slot] && at[slot]->type == type)
            fx = at[slot];
        const auto param = [&](const ParamDef& d) {
            float v = d.unmap(d.def);
            if(live && !fx)
            {
                const float now = host.paramNormalized(part, kInsertUnits[slot], d.name);
                if(now >= 0.f)
                    v = now;
            }
            if(fx)
                for(const auto& kv : fx->params)
                    if(kv.first == d.name)
                        v = kv.second;
            out.push_back(Value::s(d.name));
            out.push_back(Value::f(d.min));
            out.push_back(Value::f(d.max));
            out.push_back(Value::f(d.def));
            out.push_back(Value::s(d.unit ? d.unit : ""));
            out.push_back(Value::i(d.curve == Curve::Stepped ? d.steps : 0));
            out.push_back(Value::s(d.curve == Curve::Exponential ? "exp" : d.curve == Curve::Stepped ? "step" : "lin"));
            out.push_back(Value::f(v < 0.f ? 0.f : v > 1.f ? 1.f : v));
        };
        out.push_back(Value::i(slot));
        out.push_back(Value::s(type));
        out.push_back(Value::i(count));
        for(int32_t i = 0; i < count; ++i)
            param(defs[i]);
        out.push_back(Value::i(mod_defs ? mod_count : 0));
        for(int32_t i = 0; mod_defs && i < mod_count; ++i)
            param(mod_defs[i]);
    }
    reply("/inserts", out);
}

// One of the part's insert's params, or its mod slot's ("mod_source" ...), by
// name.
void Core::set_fx(int part, int slot, const std::string& name, float v01)
{
    const std::string type = host.mountedEffect(part, slot);
    if(type.empty())
    {
        error("no insert in slot " + std::to_string(slot));
        return;
    }
    int32_t         count = 0;
    const ParamDef* defs  = EffectRegistry::paramDefs(type.c_str(), count);
    bool            found = slotModIndexOf(name.c_str()) != -1;
    for(int32_t i = 0; i < count && !found; ++i)
        found = name == defs[i].name;
    if(!found)
    {
        error("no " + type + " param " + name);
        return;
    }
    if(!push_param(part, kInsertUnits[slot], name, v01))
        return;
    // Venue's shape knobs are built into its impulse, so they build it again.
    if(type == "Venue" && impulse_on[part][slot])
    {
        const int k = name == "length" ? 0 : name == "decay" ? 1 : name == "stretch" ? 2 : -1;
        if(k >= 0)
        {
            fx_shape[part][slot][k] = v01 < 0.f ? 0.f : v01 > 1.f ? 1.f : v01;
            request_impulse(part, slot, true);
        }
    }
}

// Mounts a new effect with its defaults in one of the part's own insert slots
// (2-3; patches fill 0-1), or empties it with "", then sends /inserts with
// what the engine holds.
void Core::set_insert(int part, int slot, const std::string& type)
{
    int32_t count = 0;
    if(!type.empty() && !EffectRegistry::paramDefs(type.c_str(), count))
    {
        error("no effect " + type);
        return;
    }
    if(type.empty() && !*host.mountedEffect(part, slot))
        return;
    settle();
    mounts_queued = true;
    fx_formula[part][slot].clear();
    impulse_on[part][slot] = false;
    if(!host.mountEffect(part, slot, type))
    {
        error("can't mount effect " + type);
        host.mountEffect(part, slot, "");
    }
    settle();
    sync_impulses(part);
    const acidulous::PatchInsert* none[kEffectSlots] = {};
    send_inserts(part, none, true);
}

// Mounts a modifier (Scale, Chord, Arp) in the part's slot 0 to 2, or empties
// it with "". It starts on, with its defaults. Modifiers belong to the part,
// so machine and patch changes leave them.
void Core::set_mod(int part, int slot, const std::string& type)
{
    int32_t count = 0;
    if(!type.empty() && !InputModRegistry::paramDefs(type.c_str(), count))
    {
        error("no modifier " + type);
        return;
    }
    if(type.empty() && mod_types[part][slot].empty())
        return;
    // Note offs sent before land first, so none goes to the new modifier,
    // and params sent after land on it, not the old one.
    settle();
    mounts_queued = true;
    if(!host.mountInputMod(part, slot, type))
    {
        error("can't mount modifier " + type);
        return;
    }
    settle();
    mod_types[part][slot] = type;
    mod_bypass[part][slot] = false;
}

// Ends what the part's modifiers are playing and forgets their held and
// latched notes, keeping their params: each one that's on is bypassed and
// put back, as bypassing calls its allNotesOff.
void Core::mods_off(int part)
{
    for(int slot = 0; slot < kModSlots; ++slot)
        if(!mod_types[part][slot].empty() && !mod_bypass[part][slot])
        {
            push_param(part, kModUnits[slot], "bypass", 1.f);
            push_param(part, kModUnits[slot], "bypass", 0.f);
        }
}

void Core::set_mod_param(int part, int slot, const std::string& name, float v01)
{
    const std::string& type  = mod_types[part][slot];
    int32_t            count = 0;
    const ParamDef*    defs  = type.empty() ? nullptr : InputModRegistry::paramDefs(type.c_str(), count);
    bool               found = false;
    for(int32_t i = 0; i < count && !found; ++i)
        found = name == defs[i].name;
    if(!found)
    {
        error(type.empty() ? "no modifier in slot " + std::to_string(slot) : "no " + type + " param " + name);
        return;
    }
    push_param(part, kModUnits[slot], name, v01);
}

// "/mods i:part" and per slot "i:slot s:type i:bypass i:count" and count
// "s:name f:value01", read back from the engine once what was sent before has
// landed. An empty slot has type "" and no params.
void Core::send_mods(int part)
{
    wait_applied();
    Args out{Value::i(part)};
    for(int slot = 0; slot < kModSlots; ++slot)
    {
        const std::string& type  = mod_types[part][slot];
        int32_t            count = 0;
        const ParamDef*    defs  = type.empty() ? nullptr : InputModRegistry::paramDefs(type.c_str(), count);
        out.push_back(Value::i(slot));
        out.push_back(Value::s(type));
        out.push_back(Value::i(defs ? host.paramNormalized(part, kModUnits[slot], "bypass") >= 0.5f : 0));
        out.push_back(Value::i(defs ? count : 0));
        for(int32_t i = 0; defs && i < count; ++i)
        {
            out.push_back(Value::s(defs[i].name));
            out.push_back(Value::f(host.paramNormalized(part, kModUnits[slot], defs[i].name)));
        }
    }
    reply("/mods", out);
}

// "/mod_defs s:type i:count" and count "s:name f:min f:max f:default s:unit
// i:steps s:curve", as /def has them, for each modifier type.
void Core::send_mod_defs()
{
    for(int32_t t = 0; t < InputModRegistry::count(); ++t)
    {
        const char*     type  = InputModRegistry::name(t);
        int32_t         count = 0;
        const ParamDef* defs  = InputModRegistry::paramDefs(type, count);
        if(!defs)
            continue;
        Args out{Value::s(type), Value::i(count)};
        for(int32_t i = 0; i < count; ++i)
        {
            const ParamDef& d = defs[i];
            out.push_back(Value::s(d.name));
            out.push_back(Value::f(d.min));
            out.push_back(Value::f(d.max));
            out.push_back(Value::f(d.def));
            out.push_back(Value::s(d.unit ? d.unit : ""));
            out.push_back(Value::i(d.curve == Curve::Stepped ? d.steps : 0));
            out.push_back(Value::s(d.curve == Curve::Exponential ? "exp" : d.curve == Curve::Stepped ? "step" : "lin"));
        }
        reply("/mod_defs", out);
    }
}

// Empties the part's modifier slots.
void Core::clear_mods(int part)
{
    for(int slot = 0; slot < kModSlots; ++slot)
        if(!mod_types[part][slot].empty())
        {
            host.mountInputMod(part, slot, "");
            mod_types[part][slot].clear();
            mod_bypass[part][slot] = false;
        }
}

// A send effect's param, kept for when it's mounted.
void Core::set_send(int slot, const std::string& name, float v01)
{
    if(name == "mix" || !send_has(slot, name))
    {
        error("no send param " + name);
        return;
    }
    // With a song loaded it moves the song's send, and the script's values
    // stay for when the song goes.
    if(!song_folder.empty())
    {
        if(send_mounted[slot] && std::string(host.mountedSend(slot)) == kSendTypes[slot])
            push_param(0, kSendUnits[slot], name, v01);
        return;
    }
    send_values[slot][name] = v01;
    if(send_mounted[slot])
        push_param(0, kSendUnits[slot], name, v01);
}

namespace
{
// A Nexus graph's module lines as {slot, type}, in slot order.
std::vector<std::pair<int, std::string>> graph_modules(const std::string& text)
{
    std::vector<std::pair<int, std::string>> out;
    size_t at = 0;
    while(at < text.size())
    {
        size_t end = text.find('\n', at);
        if(end == std::string::npos)
            end = text.size();
        const std::string line = text.substr(at, end - at);
        at = end + 1;
        if(line.size() < 4 || line[0] != 'm' || line[1] != '|')
            continue;
        const size_t bar = line.find('|', 2);
        if(bar == std::string::npos)
            continue;
        const size_t bar2 = line.find('|', bar + 1);
        out.emplace_back(std::atoi(line.c_str() + 2), line.substr(bar + 1, bar2 == std::string::npos ? std::string::npos : bar2 - bar - 1));
    }
    std::sort(out.begin(), out.end());
    return out;
}

int graph_cables(const std::string& text)
{
    int n = 0;
    for(size_t at = 0; at < text.size();)
    {
        if(text[at] == 'c' && at + 1 < text.size() && text[at + 1] == '|')
            ++n;
        const size_t end = text.find('\n', at);
        if(end == std::string::npos)
            break;
        at = end + 1;
    }
    return n;
}

// Modules that aren't in the signal chain, left off the scope's boxes.
bool chain_module(const std::string& type)
{
    return type != "voice" && type != "perf" && type != "macro" && type != "out" && type != "scope" && type != "blank";
}
} // namespace

// "/graph i:part s:patch (i:slot s:type)..." for the part's Nexus patch, or
// just the part when it has none.
void Core::send_graph(int part)
{
    Args out{Value::i(part), Value::s(parts[part].patch)};
    for(const auto& m : graph_modules(parts[part].nexus))
        if(chain_module(m.second))
        {
            out.push_back(Value::i(m.first));
            out.push_back(Value::s(m.second));
        }
    reply("/graph", out);
}

// Points the scope at a module's first output: the patch is mounted again
// with a scope module in a free slot, cabled from it. Its output goes
// nowhere, so the sound is the same, but the remount dips the sound for a
// block. slot -1 stops the trace and leaves the scope in until the next patch.
void Core::set_scope(int part, int slot)
{
    Part& p = parts[part];
    if(slot < 0 || p.machine != "Nexus" || p.nexus.empty())
    {
        if(scope_part.load() == part)
            scope_part.store(-1);
        return;
    }
    namespace nx = machine::nexus;
    const auto mods = graph_modules(p.nexus);
    bool found = false;
    std::vector<bool> used(nx::kSlots, false);
    for(const auto& m : mods)
    {
        if(m.first >= 0 && m.first < nx::kSlots)
            used[size_t(m.first)] = true;
        found = found || (m.first == slot && chain_module(m.second));
    }
    int free = -1;
    for(int s = nx::kSlots - 1; s >= 0 && free < 0; --s)
        if(!used[size_t(s)])
            free = s;
    if(!found || free < 0)
    {
        error("Nexus: no module in slot " + std::to_string(slot) + " for the scope");
        return;
    }
    if(slot == p.scope_slot)
    {
        scope_slot_live.store(slot);
        scope_part.store(part);
        return;
    }
    // The scope's cable takes the next cable depth param, so there must be one.
    const int cable = graph_cables(p.nexus);
    if(cable >= nx::kCables)
    {
        error("Nexus: the patch has every cable in use, no room for the scope");
        return;
    }
    // The scope's module line goes first, since the graph reads the first
    // scope in the patch, and its cable last, so the patch's cables keep
    // their depth params.
    char head[48], tail[48];
    std::snprintf(head, sizeof(head), "m|%d|scope|mono\n", free);
    std::snprintf(tail, sizeof(tail), "\nc|%d.0|%d.0|1.0", slot, free);
    // Zoom 3 of 64: a point every 4 samples, 512 points, about 43 ms.
    char name[16];
    std::snprintf(name, sizeof(name), "s%02d_p1", free);
    push_param(part, "machine", name, 3.5f / 64.f);
    for(const char* end : {"a", "b"})
    {
        std::snprintf(name, sizeof(name), "c%02d_%s", cable, end);
        push_param(part, "machine", name, (1.f + 4.f) / 8.f);
    }
    scope_part.store(-1);
    p.scope_slot = -1;
    if(!load_nexus(part, head + p.nexus + tail))
        return;
    p.scope_slot = slot;
    // The remounted scope keeps its ring, so the old module's trace is in it
    // until 2048 samples have gone through: about 43 ms, and a little more.
    scope_from.store(now_ms() + 60);
    scope_slot_live.store(slot);
    scope_part.store(part);
}

int Core::scope_trace(float* out, int max, int& part, int& slot)
{
    part = scope_part.load();
    slot = scope_slot_live.load();
    if(part < 0 || now_ms() < scope_from.load())
        return 0;
    float     ring[512];
    const int n = host.nexusScope(part, ring, 512);
    if(n < 512)
        return 0;
    // From the first rising zero crossing in the first quarter, so a steady
    // note stands still.
    int from = 0;
    for(int i = 1; i < 128; ++i)
        if(ring[i - 1] <= 0.f && ring[i] > 0.f)
        {
            from = i;
            break;
        }
    const int count = std::min(max, (512 - 128) / 3);
    for(int i = 0; i < count; ++i)
        out[i] = ring[from + i * 3];
    return count;
}

void Core::master_meters(float& peak, float& lufs)
{
    peak = host.peakLevel();
    float l[4];
    host.loudness(l);
    lufs = l[1];
}

// The engine's param queue holds 512 and the audio thread empties it every
// block, so when it's full this waits for blocks to render and tries again.
// It gives up only if no block renders for a second. Worker thread; inline
// jobs (the render tool) run between blocks, so they can't wait.
// The name must be one the unit has, so a false from setParam means a full queue.
bool Core::push_param(int part, const char* unit, const std::string& name, float v01)
{
    const float v = v01 < 0.f ? 0.f : v01 > 1.f ? 1.f : v01;
    const auto  push = [&] { return host.setParam(part, unit, name, v, false); };
    if(push())
        return true;
    if(!inline_jobs)
    {
        uint64_t seen = blocks.load(std::memory_order_acquire);
        int      idle = 0;
        while(!stopping && idle < 1000)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            if(push())
                return true;
            const uint64_t now = blocks.load(std::memory_order_acquire);
            if(now != seen)
                seen = now, idle = 0;
            else
                ++idle;
        }
    }
    error("param queue full");
    return false;
}

void Core::levels(float* out)
{
    for(int p = 0; p < kParts; ++p)
        out[p] = host.rackPeak(p);
}

std::shared_ptr<Core::BankCache> Core::bank(const std::string& machine)
{
    auto it = banks.find(machine);
    if(it != banks.end())
        return it->second;
    auto        cache = std::make_shared<BankCache>();
    std::string why;
    cache->ok = ab::readBank(bank_dir + "/" + machine + ".bank", cache->bank, why);
    if(!cache->ok)
    {
        error(why);
        return nullptr;
    }
    banks[machine] = cache;
    return cache;
}

// A /patch or /preset_load that fails takes the last patch's inserts off and
// gives back the sends it held, so every one ends in one /inserts.
void Core::patch_failed(int part, const std::string& what)
{
    host.setMacroNames(part, "");
    apply_chain(part, acidulous::PatchChain{}, what);
    send_macro_names(part);
}

// The same, for a patch that couldn't be read at all: the part still gets
// [machine], at its defaults if it's new, so it isn't silent, and its values
// go to the script as /values named [tag].
void Core::patch_failed_on(int part, const std::string& machine, const std::string& tag, const std::string& what)
{
    int32_t i = 0;
    while(i < MachineRegistry::count() && !lower_equal(MachineRegistry::name(i), machine))
        ++i;
    if(i < MachineRegistry::count() && parts[part].machine != MachineRegistry::name(i))
        mount(part, MachineRegistry::name(i));
    patch_failed(part, what);
    if(parts[part].machine.empty() || !lower_equal(parts[part].machine, machine))
        return;
    reply("/patch_failed", {Value::i(part), Value::s(parts[part].machine), Value::s(tag)});
    Args values{Value::i(part), Value::s(parts[part].machine), Value::s(tag)};
    for(const float v : parts[part].values)
        values.push_back(Value::f(v));
    reply("/values", values);
}

void Core::load_patch(int part, const std::string& machine, const std::string& name)
{
    const auto fail = [&] { patch_failed_on(part, machine, name, machine + " " + name); };
    const auto b = bank(machine);
    if(!b)
        return fail();
    const ab::BankPatch* patch = nullptr;
    for(const auto& p : b->bank.patches)
        if(p.name == name)
            patch = &p;
    if(!patch)
        for(const auto& p : b->bank.patches)
            if(lower_equal(p.name, name))
                patch = &p;
    if(!patch)
    {
        error("no " + machine + " patch " + name);
        return fail();
    }
    if(parts[part].machine != machine && !mount(part, machine))
        return fail();

    int32_t         count = 0;
    const ParamDef* defs  = MachineRegistry::paramDefs(machine.c_str(), count);
    ab::Resolved    r     = ab::resolve(*patch, defs, count);
    for(const auto& p : r.problems)
        std::fprintf(stderr, "acidulous: %s %s: %s\n", machine.c_str(), patch->name.c_str(), p.c_str());

    PatchSource src;
    src.name     = patch->name;
    src.norm     = std::move(r.norm);
    src.settings = std::move(r.settings);
    for(const auto& v : patch->values)
        src.named.push_back(v.name);
    std::vector<std::string> fx_problems;
    src.chain = ab::resolveChain(*patch, fx_problems);
    for(const auto& p : fx_problems)
        std::fprintf(stderr, "acidulous: %s %s: %s\n", machine.c_str(), patch->name.c_str(), p.c_str());
    src.low  = patch->low;
    src.high = patch->high;
    apply_patch(part, machine, src, patch->name);
}

// Puts a patch on the part, which already has its machine: every param, its
// settings, macro names and effects, then /macros, /macro_names, /values and
// /patch with [tag] as the patch's name. False if it failed part way, which
// has taken the last patch's effects off.
bool Core::apply_patch(int part, const std::string& machine, PatchSource& src, const std::string& tag)
{
    const auto fail = [&] {
        patch_failed(part, machine + " " + src.name);
        return false;
    };
    int32_t         count = 0;
    const ParamDef* defs  = MachineRegistry::paramDefs(machine.c_str(), count);
    if(src.norm.size() != size_t(count))
        return fail();

    // A Nexus patch names only the knobs it changes; the rest take their
    // module's default from the graph, since every slot knob defaults to zero
    // and an oscillator at level zero is silent. As the app's audition does.
    std::string nexus;
    Args        macros;
    if(machine == "Nexus")
    {
        for(const auto& kv : src.settings)
            if(kv.first == "nexus")
                nexus = kv.second;
        std::string why;
        std::unique_ptr<machine::nexus::Graph> g;
        if(!nexus.empty())
            g.reset(machine::nexus::Graph::parse(nexus, float(kSampleRate), why));
        if(g)
        {
            namespace nx = machine::nexus;
            std::vector<float> knobs(size_t(nx::kSlots) * nx::kKnobs, 0.f);
            g->defaultKnobs(knobs.data());
            std::set<std::string> named(src.named.begin(), src.named.end());
            for(int32_t i = 0; i < count; ++i)
            {
                const std::string n = defs[i].name;
                if(n.size() < 6 || n[0] != 's' || n[3] != '_' || n[4] != 'p' || named.count(n))
                    continue;
                const int slot = (n[1] - '0') * 10 + (n[2] - '0'), knob = n[5] - '1';
                if(slot >= 0 && slot < nx::kSlots && knob >= 0 && knob < nx::kKnobs)
                    src.norm[size_t(i)] = defs[i].unmap(knobs[size_t(slot) * nx::kKnobs + size_t(knob)]);
            }
            macros = nexus_macros(*g, src.named);
        }
        else
        {
            error(machine + " " + src.name + ": " + (nexus.empty() ? std::string("no graph") : why));
            nexus.clear();
        }
    }

    // Every param, so nothing is left over from the last patch, except the
    // ones the machine keeps (Bias's lane levels and mutes, which are the mix
    // of its takes, as the app's MachineUi.patchKeeps). The new machine is in
    // place. Loaded samples stay too, since the machine isn't remounted.
    for(int32_t i = 0; i < count; ++i)
    {
        if(patch_keeps(machine, defs[i].name) && size_t(i) < parts[part].values.size())
        {
            src.norm[size_t(i)] = parts[part].values[size_t(i)];
            continue;
        }
        if(!push_param(part, "machine", defs[i].name, src.norm[size_t(i)]))
        {
            // The params pushed so far are the engine's now and the rest keep
            // the last patch's: the part's values follow that (and
            // fail() sends them), and fail() takes the last patch's effects off.
            auto& held = parts[part].values;
            if(held.size() == size_t(count))
            {
                std::copy(src.norm.begin(), src.norm.begin() + i, held.begin());
                if(machine == "Cumulus")
                    request_cloud(part);
            }
            return fail();
        }
    }
    parts[part].values = src.norm;

    // Formulate's formula and tables are compiled here and swapped in. A
    // patch without them clears the last one, which plays the plain oscillator.
    if(machine == "Formulate")
    {
        std::string formula, arp, duty, vol;
        for(const auto& kv : src.settings)
        {
            if(kv.first == "formula")
                formula = kv.second;
            else if(kv.first == "arp")
                arp = kv.second;
            else if(kv.first == "duty")
                duty = kv.second;
            else if(kv.first == "vol")
                vol = kv.second;
        }
        parts[part].program_settings.clear();
        const std::pair<const char*, const std::string*> program_keys[] = {
            {"formula", &formula}, {"arp", &arp}, {"duty", &duty}, {"vol", &vol}};
        for(const auto& kv : program_keys)
            if(!kv.second->empty())
                parts[part].program_settings.emplace_back(kv.first, *kv.second);
        std::unique_ptr<machine::formulate::Program> program;
        if(!(formula.empty() && arp.empty() && duty.empty() && vol.empty()))
        {
            std::string why;
            program = machine::formulate::compile(formula, arp, duty, vol, why);
            if(!program)
                error(machine + " " + src.name + ": " + why);
        }
        if(program || parts[part].program)
        {
            Mount mt;
            mt.kind    = Mount::Kind::Object;
            mt.rack    = part;
            mt.slot    = 0;
            mt.object  = program.get();
            mt.deleter = delete_as<machine::formulate::Program>;
            // Frees the program itself if the queue stays full.
            const bool had = program != nullptr;
            program.release();
            mounts_queued = true;
            if(host.mountObjectWithRetry(mt))
                parts[part].program = had;
            else
                error("mount queue full");
        }
    }
    else if(machine == "Nexus")
    {
        parts[part].nexus      = nexus;
        parts[part].scope_slot = -1;
        if(scope_part.load() == part)
            scope_part.store(-1);
        if(!nexus.empty())
            load_nexus(part, nexus);
        else
        {
            // No graph: clear the last patch's, so the part is silent.
            Mount mt;
            mt.kind    = Mount::Kind::Object;
            mt.rack    = part;
            mt.slot    = 0;
            mt.object  = nullptr;
            mt.deleter = delete_as<machine::nexus::Graph>;
            settle();
            if(!host.mountObjectWithRetry(mt))
                error("mount queue full");
        }
    }
    else if(machine == "Cumulus")
        request_cloud(part);

    // The patch's macro names, macro1..macro8 joined by newlines, or "" when
    // it names none.
    {
        std::string names;
        bool        any = false;
        for(int k = 1; k <= 8; ++k)
        {
            const std::string key = "macro" + std::to_string(k);
            std::string       v;
            for(const auto& kv : src.settings)
                if(kv.first == key)
                    v = kv.second;
            any = any || !v.empty();
            names += (k > 1 ? "\n" : "") + v;
        }
        if(!any)
            names.clear();
        host.setMacroNames(part, names);
    }

    // The patch's inserts and sends. One without them takes the last patch's
    // inserts off and gives back the sends it held.
    apply_chain(part, src.chain, machine + " " + src.name);

    parts[part].patch = tag;
    parts[part].low   = src.low;
    parts[part].high  = src.high;
    if(!macros.empty())
    {
        Args out{Value::i(part), Value::s(machine), Value::s(tag)};
        out.insert(out.end(), macros.begin(), macros.end());
        reply("/macros", out);
    }
    send_macro_names(part);
    Args values{Value::i(part), Value::s(machine), Value::s(tag)};
    for(int32_t i = 0; i < count; ++i)
        values.push_back(Value::f(src.norm[size_t(i)]));
    reply("/values", values);
    reply("/patch", {Value::i(part), Value::s(machine), Value::s(tag)});
    if(machine == "Nexus")
        send_graph(part);
    return true;
}

// A Nexus patch's macros: the knobs the patch sets, in its order, then the
// knobs of the modules in its graph, as param and label pairs, up to five.
Args Core::nexus_macros(const machine::nexus::Graph& g, const std::vector<std::string>& named)
{
    namespace nx = machine::nexus;
    const char* knob_name[nx::kSlots][nx::kKnobs] = {};
    for(int32_t i = 0; i < g.nodeCount(); ++i)
    {
        const auto& n = g.nodeAt(i);
        if(n.slot < 0 || n.slot >= nx::kSlots || n.unknown)
            continue;
        const auto& info = nx::infoFor(n.type);
        for(int k = 0; k < nx::kKnobs; ++k)
            if(info.knob[k] && *info.knob[k])
                knob_name[n.slot][k] = info.knob[k];
    }
    std::vector<std::pair<int, int>> picks;
    const auto add = [&](int slot, int knob) {
        if(picks.size() < 5 && knob_name[slot][knob] &&
           std::find(picks.begin(), picks.end(), std::make_pair(slot, knob)) == picks.end())
            picks.push_back({slot, knob});
    };
    for(const auto& n : named)
    {
        if(n.size() != 6 || n[0] != 's' || n[3] != '_' || n[4] != 'p' || !std::isdigit((unsigned char)n[1]) ||
           !std::isdigit((unsigned char)n[2]))
            continue;
        const int slot = (n[1] - '0') * 10 + (n[2] - '0'), knob = n[5] - '1';
        if(slot < nx::kSlots && knob >= 0 && knob < nx::kKnobs)
            add(slot, knob);
    }
    // Then the graph's own modules, leaving out its voice and output.
    for(int32_t i = 0; i < g.nodeCount(); ++i)
    {
        const auto& n = g.nodeAt(i);
        if(n.slot < 0 || n.slot >= nx::kSlots || n.unknown)
            continue;
        const char* type = nx::infoFor(n.type).name;
        if(type && (!std::strcmp(type, "voice") || !std::strcmp(type, "out")))
            continue;
        for(int k = 0; k < nx::kKnobs; ++k)
            add(n.slot, k);
    }
    Args                  out;
    std::set<std::string> labels;
    for(const auto& pk : picks)
    {
        char param[32];
        std::snprintf(param, sizeof(param), "s%02d_p%d", pk.first, pk.second + 1);
        std::string label = std::string(knob_name[pk.first][pk.second]).substr(0, 6);
        if(labels.count(label))
            label = (std::to_string(pk.first) + knob_name[pk.first][pk.second]).substr(0, 6);
        labels.insert(label);
        out.push_back(Value::s(param));
        out.push_back(Value::s(label));
    }
    return out;
}

// What the engine holds for the part once everything queued before has been
// applied, for tests: the gain and the machine's params, 0-1.
void Core::send_state(int part)
{
    wait_applied();
    Args out{Value::i(part), Value::s(parts[part].machine), Value::s(parts[part].patch),
             Value::f(host.paramNormalized(part, "channel", "gain"))};
    int32_t count = 0;
    if(const ParamDef* defs = MachineRegistry::paramDefs(host.mountedMachine(part), count))
        for(int32_t i = 0; i < count; ++i)
            out.push_back(Value::f(host.paramNormalized(part, "machine", defs[i].name)));
    reply("/state", out);
}

void Core::send_defs(const std::string& machine)
{
    int32_t         count = 0;
    const ParamDef* defs  = MachineRegistry::paramDefs(machine.c_str(), count);
    if(!defs)
    {
        error("no machine " + machine);
        count = 0;
    }
    for(int32_t i = 0; i < count; ++i)
    {
        const ParamDef& d = defs[i];
        reply("/def", {Value::s(machine), Value::i(i), Value::s(d.name), Value::f(d.min), Value::f(d.max),
                       Value::f(d.def), Value::s(d.unit ? d.unit : ""),
                       Value::i(d.curve == Curve::Stepped ? d.steps : 0),
                       Value::s(d.curve == Curve::Exponential ? "exp" : d.curve == Curve::Stepped ? "step" : "lin")});
    }
    reply("/defs_end", {Value::s(machine), Value::i(count)});
}

void Core::send_patches(const std::string& machine)
{
    const auto b = bank(machine);
    Args       out{Value::s(machine)};
    if(b)
        for(const auto& p : b->bank.patches)
            out.push_back(Value::s(p.name));
    reply("/patches", out);
}

// --- Samples -----------------------------------------------------------------

void Core::loaded(int part, const std::string& kind, const std::string& path, int arg, const std::string& name)
{
    reply("/loaded", {Value::i(part), Value::s(kind), Value::s(path), Value::i(arg), Value::s(name),
                      Value::s(part >= 0 ? parts[part].machine : "")});
}

void Core::load_error(int part, const std::string& kind, const std::string& path, int arg, const std::string& why)
{
    const std::string text = (path.empty() ? kind : base_name(path)) + ": " + why;
    std::fprintf(stderr, "acidulous: part %d %s\n", part, text.c_str());
    reply("/error", {Value::s(text), Value::i(part), Value::s(kind), Value::s(path), Value::i(arg),
                     Value::s(part >= 0 ? parts[part].machine : "")});
}

// --- The sample budget: what each part's slots hold decoded ------------------

int64_t Core::held_total()
{
    std::lock_guard<std::mutex> l(held_lock);
    int64_t                     sum = song_held;
    for(const auto& p : held)
        for(int64_t b : p)
            sum += b;
    return sum;
}

// Whether `need` more bytes fit, with everything held now still counted, as
// the old samples live until the new ones are swapped in.
bool Core::room_for(int64_t need, std::string& why)
{
    const int64_t now = held_total(), limit = sample_budget_bytes();
    if(now + need <= limit)
        return true;
    why = "needs " + mb(need) + ", " + mb(now) + " of the " + mb(limit) + " for samples is in use";
    return false;
}

void Core::set_held(int part, int slot, int64_t bytes)
{
    std::lock_guard<std::mutex> l(held_lock);
    held[part][slot] = bytes;
}

void Core::clear_held(int part)
{
    std::lock_guard<std::mutex> l(held_lock);
    for(int64_t& b : held[part])
        b = 0;
}

// /load: decodes on this worker and mounts into the part's machine, under the
// part's mount lock so nothing remounts the part meanwhile. An empty path
// clears what the kind loads. Anything the engine's readers throw becomes an
// /error.
void Core::load(int part, const std::string& kind, const std::string& path, int arg,
                const std::vector<std::string>& roots)
{
    try
    {
        load_now(part, kind, path, arg, roots);
    }
    catch(const std::exception& e)
    {
        load_error(part, kind, path, arg, std::string("couldn't load it: ") + e.what());
    }
    catch(...)
    {
        load_error(part, kind, path, arg, "couldn't load it");
    }
}

void Core::load_now(int part, const std::string& kind, const std::string& path, int arg,
                    const std::vector<std::string>& roots)
{
    const std::string& m    = parts[part].machine;
    const auto         want = [&](std::initializer_list<const char*> machines) {
        for(const char* x : machines)
            if(m == x)
                return true;
        load_error(part, kind, path, arg, "part " + std::to_string(part + 1) + " has " + (m.empty() ? "no machine" : m));
        return false;
    };
    // The machine has to be in place before anything goes into it.
    settle();
    std::lock_guard<std::mutex> lock(mount_locks[part]);
    std::string                 why;
    if(kind == "clear")
    {
        parts[part].zones.clear();
        clear_objects(part);
        loaded(part, kind, "", 0, "");
        return;
    }
    if(path.find_first_of("|\n") != std::string::npos)
        return load_error(part, kind, path, arg, "names with | can't be loaded");
    struct stat found{};
    if(!path.empty() && stat(path.c_str(), &found) != 0)
        return load_error(part, kind, path, arg, "not found");
    if(kind == "pad")
    {
        if(!want({"Forage"}))
            return;
        if(arg < 0 || arg >= kForagePads)
            return load_error(part, kind, path, arg, "no pad " + std::to_string(arg + 1));
        const int64_t need = path.empty() ? 0 : decoded_bytes(path, kSampleRate, kMaxDecodeSeconds, false);
        if(!room_for(need, why))
            return load_error(part, kind, path, arg, why);
        if(!host.loadSample(part, arg, path, why))
            return load_error(part, kind, path, arg, why);
        set_held(part, arg, need);
        return loaded(part, kind, path, arg, path.empty() ? "" : stem(path));
    }
    if(kind == "kit")
    {
        if(!want({"Forage"}))
            return;
        int count = 0;
        why       = load_kit(part, path, count);
        if(!why.empty())
            return load_error(part, kind, path, arg, why);
        return loaded(part, kind, path, count, base_name(path));
    }
    if(kind == "sf2")
    {
        if(!want({"Mosaic"}))
            return;
        if(found.st_size > max_soundfont_bytes())
            return load_error(part, kind, path, arg,
                              "over " + mb(max_soundfont_bytes()) + ", too big for the norns");
        // the file is read whole, then its preset's 16-bit samples become floats
        if(!room_for(int64_t(found.st_size) * 3, why))
            return load_error(part, kind, path, arg, why);
        std::vector<Sf2Reader::PresetInfo> presets;
        if(!Sf2Reader::listPresets(path, presets, why))
            return load_error(part, kind, path, arg, why);
        if(arg < 0 || size_t(arg) >= presets.size())
            return load_error(part, kind, path, arg, "no preset " + std::to_string(arg));
        if(!host.loadSoundFont(part, path, arg, why))
            return load_error(part, kind, path, arg, why);
        parts[part].zones.clear();
        set_held(part, 0, int64_t(found.st_size) * 2);
        return loaded(part, kind, path, arg, presets[size_t(arg)].name);
    }
    if(kind == "folder")
    {
        if(!want({"Mosaic"}))
            return;
        why = load_folder_map(part, path);
        if(!why.empty())
            return load_error(part, kind, path, arg, why);
        return loaded(part, kind, path, arg, base_name(path));
    }
    if(kind == "zones")
    {
        if(!want({"Mosaic"}))
            return;
        int missing = 0;
        why         = load_zones(part, path, arg, roots, missing);
        if(!why.empty())
            return load_error(part, kind, path, arg, why);
        loaded(part, kind, path, arg, stem(path));
        if(missing > 0)
            load_error(part, kind, path, arg,
                       std::to_string(missing) + (missing == 1 ? " file" : " files") + " not found");
        return;
    }
    if(kind == "sample")
    {
        if(!want({"Dice", "Pollen", "Molt"}))
            return;
        // a Take keeps both channels; Molt's mono copy and analysis fit in that too
        const int64_t need = path.empty() ? 0 : decoded_bytes(path, kSampleRate, kMaxDecodeSeconds, true);
        if(!room_for(need, why))
            return load_error(part, kind, path, arg, why);
        why = load_take(part, path, m == "Molt");
        if(!why.empty())
            return load_error(part, kind, path, arg, why);
        set_held(part, 0, need);
        return loaded(part, kind, path, arg, path.empty() ? "" : stem(path));
    }
    if(kind == "slices")
    {
        if(!want({"Forage"}))
            return;
        // the one file every pad can slice, in the slot above them, up to ten minutes
        const int64_t need = path.empty() ? 0 : decoded_bytes(path, kSampleRate, kMaxSliceSeconds, false);
        if(!room_for(need, why))
            return load_error(part, kind, path, arg, why);
        if(!host.loadSample(part, kForagePads, path, why, kMaxSliceSeconds))
            return load_error(part, kind, path, arg, why);
        set_held(part, kForagePads, need);
        return loaded(part, kind, path, arg, path.empty() ? "" : stem(path));
    }
    if(kind == "voice")
    {
        if(!want({"Diction"}))
            return;
        if(arg < 0 || arg > 1)
            return load_error(part, kind, path, arg, "no voice " + std::to_string(arg));
        int missing = 0;
        why         = load_voice(part, arg, path, missing);
        if(!why.empty())
            return load_error(part, kind, path, arg, why);
        // A voice to morph to needs the one sung in analysed for it too.
        const Part& p = parts[part];
        if(arg == 1 && !path.empty() && !p.voice[0].empty() && !p.voice_lsf[0])
        {
            int         also = 0;
            const std::string first = p.voice[0];
            why                     = load_voice(part, 0, first, also);
            if(!why.empty())
                load_error(part, kind, first, 0, why);
        }
        loaded(part, kind, path, arg, path.empty() ? "" : base_name(folder_of(path)));
        if(missing > 0)
            load_error(part, kind, path, arg,
                       std::to_string(missing) + (missing == 1 ? " take" : " takes") + " not found");
        return;
    }
    load_error(part, kind, path, arg, "no such kind of load");
}

// Diction: the recorded voice whose bank.json is at [path] into voice [slot]
// (0 sung in, 1 morphed to), "" for the built-in one. Its takes are counted
// against the sample budget at twice their decoded size, the voice keeping a
// cut of each and its source. Returns "" or why not. Under the mount lock.
std::string Core::load_voice(int part, int slot, const std::string& path, int& missing)
{
    Part&       p = parts[part];
    std::string spec, why;
    int64_t     need = 0;
    missing          = 0;
    if(!path.empty())
    {
        std::vector<std::string> files;
        spec = voice_spec(path, files, missing, why);
        if(spec.empty())
            return why.empty() ? "no takes in that voice" : why;
        const std::string folder = folder_of(path);
        for(const auto& f : files)
            need += 2 * decoded_bytes(folder + f, kSampleRate, 0, false);
        if(!room_for(need, why))
            return why;
    }
    const bool lsf = !spec.empty() && (slot == 1 || !p.voice[1].empty());
    why            = host.loadVoice(part, slot, spec.empty() ? "" : spec + (lsf ? "\nLSF" : ""));
    if(!why.empty())
        return why;
    p.voice[slot]     = path;
    p.voice_lsf[slot] = lsf;
    set_held(part, slot, need);
    return "";
}

// Forage: the folder's first 13 audio files, sorted, on pads 1-13, and the
// pads past the last file emptied. A /loaded for each pad, so the script
// knows what's where.
std::string Core::load_kit(int part, const std::string& folder, int& count)
{
    const auto files = audio_files(folder, false);
    if(files.empty())
        return "no audio files in it";
    int64_t need = 0;
    for(size_t k = 0; k < files.size() && k < size_t(kForagePads); ++k)
        need += decoded_bytes(files[k], kSampleRate, kMaxDecodeSeconds, false);
    std::string why;
    if(!room_for(need, why))
        return why;
    count = 0;
    for(int pad = 0; pad < kForagePads; ++pad)
    {
        const std::string path = size_t(pad) < files.size() ? files[size_t(pad)] : "";
        if(!host.loadSample(part, pad, path, why))
        {
            // the engine keeps the pad's old sample, so its held entry stays
            load_error(part, "pad", path, pad, why);
            continue;
        }
        set_held(part, pad, path.empty() ? 0 : decoded_bytes(path, kSampleRate, kMaxDecodeSeconds, false));
        if(!path.empty())
            ++count;
        loaded(part, "pad", path, pad, path.empty() ? "" : stem(path));
    }
    return count ? "" : "none of its files would load";
}

// Mosaic: the folder's WAVs as a map. Files with a note name (C4 = 60) play
// from that note, each reaching halfway to its neighbours; files on the same
// note are velocity layers, in name order. With no note names the files sit
// side by side, up to an octave each, around middle C, with the end ones
// stretched to the ends of the keyboard. The WAVs are read whole at their own
// rate, so the folder's total is checked against the budget first.
std::string Core::load_folder_map(int part, const std::string& folder)
{
    auto files = audio_files(folder, true);
    if(files.empty())
        return "no WAV files in it";
    std::map<int, std::vector<std::string>> by_note;
    for(const auto& f : files)
    {
        const int n = note_in_name(stem(f));
        if(n >= 0)
            by_note[n].push_back(f);
    }
    std::vector<std::string> used;
    if(!by_note.empty())
        for(const auto& kv : by_note)
            used.insert(used.end(), kv.second.begin(), kv.second.end());
    else
        used.assign(files.begin(), files.begin() + long(std::min<size_t>(files.size(), 128)));
    int64_t need = 0;
    for(const auto& f : used)
        need += decoded_bytes(f, 0, 0, false);
    std::string why;
    if(!room_for(need, why))
        return why;
    std::string spec;
    const auto  zone = [&](const std::string& path, int lo, int hi, int root, int lo_vel, int hi_vel) {
        spec += path + "|" + std::to_string(lo) + "|" + std::to_string(hi) + "|" + std::to_string(root) + "|" +
                std::to_string(lo_vel) + "|" + std::to_string(hi_vel) + "|0|1|0|0\n";
    };
    if(!by_note.empty())
    {
        std::vector<int> roots;
        for(const auto& kv : by_note)
            roots.push_back(kv.first);
        for(size_t k = 0; k < roots.size(); ++k)
        {
            const int lo = k == 0 ? 0 : (roots[k - 1] + roots[k]) / 2 + 1;
            const int hi = k + 1 == roots.size() ? 127 : (roots[k] + roots[k + 1]) / 2;
            const auto& layers = by_note[roots[k]];
            const int   n      = int(layers.size());
            for(int j = 0; j < n; ++j)
                zone(layers[size_t(j)], lo, hi, roots[k], j * 128 / n, (j + 1) * 128 / n - 1);
        }
        if(used.size() < files.size())
            std::fprintf(stderr, "acidulous: %s: %zu files have no note name and are left out\n", folder.c_str(),
                         files.size() - used.size());
    }
    else
    {
        const int n     = int(std::min<size_t>(files.size(), 128));
        const int w     = std::max(1, std::min(12, 128 / n));
        const int start = std::max(0, std::min(128 - n * w, 60 - (n * w) / 2));
        for(int k = 0; k < n; ++k)
        {
            const int lo = start + k * w, hi = lo + w - 1;
            zone(files[size_t(k)], k == 0 ? 0 : lo, k + 1 == n ? 127 : hi, lo + w / 2, 0, 127);
        }
    }
    if(!host.loadZoneMap(part, spec, base_name(folder), why))
        return why;
    parts[part].zones = spec;
    set_held(part, 0, need);
    return "";
}

// Dice and Pollen take a file with its transients found, Molt one summed to
// mono with its pitch marks found, as EngineHost's loadTake and
// loadUtterance do, but through the decoder that reads every format, and
// only the first kMaxDecodeSeconds.
std::string Core::load_take(int part, const std::string& path, bool sung)
{
    Mount mt;
    mt.kind    = Mount::Kind::Object;
    mt.rack    = part;
    mt.slot    = 0;
    mt.object  = nullptr;
    mt.deleter = sung ? delete_as<audio::Utterance> : delete_as<audio::Take>;
    if(path.empty())
        return host.mountObjectWithRetry(mt) ? "" : "mount queue full";
    std::string why;
    auto        data = decodeAudio(path, kSampleRate, why, kMaxDecodeSeconds);
    if(!data || data->frames <= 1)
        return why.empty() ? "that file couldn't be read" : why;
    if(sung)
    {
        auto u  = std::make_unique<audio::Utterance>();
        u->name = data->name;
        u->mono.resize(size_t(data->frames));
        for(int32_t i = 0; i < data->frames; ++i)
            u->mono[size_t(i)] = data->stereo ? 0.5f * (data->left[size_t(i)] + data->right[size_t(i)])
                                              : data->left[size_t(i)];
        u->analyse(float(kSampleRate));
        // The engine owns it from here and frees it if the queue stays full.
        mt.object = u.release();
        return host.mountObjectWithRetry(mt) ? "" : "mount queue full";
    }
    auto t    = std::make_unique<audio::Take>();
    t->name   = data->name;
    t->frames = data->frames;
    t->left   = std::move(data->left);
    t->right  = data->stereo ? std::move(data->right) : t->left;
    t->detect(float(kSampleRate));
    mt.object = t.release();
    return host.mountObjectWithRetry(mt) ? "" : "mount queue full";
}

// A SoundFont's presets, in file order: /load sf2's index.
void Core::send_presets(const std::string& path)
{
    std::vector<Sf2Reader::PresetInfo> presets;
    std::string                        why;
    Args                               out{Value::s(path)};
    if(!Sf2Reader::listPresets(path, presets, why))
    {
        load_error(-1, "presets", path, 0, why);
        reply("/presets", out);
        return;
    }
    for(const auto& p : presets)
        out.push_back(Value::s(p.name));
    reply("/presets", out);
}

// The shape of a file for the picker: its length in seconds, its own rate,
// whether it's longer than a pad keeps, and 128 columns of its loudest
// point, 0-1 against its peak. Only the first kMaxDecodeSeconds are read.
void Core::send_shape(const std::string& path)
{
    constexpr int kColumns = 128;
    std::string   why;
    auto          data = decodeAudio(path, 0, why, kMaxDecodeSeconds);
    Args          out{Value::s(path)};
    if(!data || data->frames <= 0 || data->rate <= 0)
    {
        out.push_back(Value::f(0));
        out.push_back(Value::i(0));
        out.push_back(Value::i(0));
        reply("/shape", out);
        return;
    }
    out.push_back(Value::f(double(data->frames) / data->rate));
    out.push_back(Value::i(data->rate));
    out.push_back(Value::i(data->truncated ? 1 : 0));
    std::vector<float> cols(kColumns, 0.f);
    float              peak = 0.f;
    for(int c = 0; c < kColumns; ++c)
    {
        const int64_t from = int64_t(data->frames) * c / kColumns;
        const int64_t to   = std::max(from + 1, int64_t(data->frames) * (c + 1) / kColumns);
        float         m    = 0.f;
        for(int64_t i = from; i < to && i < data->frames; ++i)
        {
            m = std::max(m, std::fabs(data->left[size_t(i)]));
            if(data->stereo)
                m = std::max(m, std::fabs(data->right[size_t(i)]));
        }
        cols[size_t(c)] = m;
        peak            = std::max(peak, m);
    }
    for(float v : cols)
        out.push_back(Value::f(peak > 0.f ? v / peak : 0.f));
    reply("/shape", out);
}

// Plays a file once over everything, for the picker, up to
// kMaxDecodeSeconds of it. An empty path stops it.
void Core::hear(const std::string& path)
{
    std::vector<float>* next = nullptr;
    double              secs = 0;
    if(!path.empty())
    {
        std::string why;
        auto        data = decodeAudio(path, kSampleRate, why, kMaxDecodeSeconds);
        if(!data || data->frames <= 0)
            return load_error(-1, "hear", path, 0, why.empty() ? "that file couldn't be read" : why);
        next = new std::vector<float>(size_t(data->frames) * 2);
        for(int32_t i = 0; i < data->frames; ++i)
        {
            (*next)[size_t(i) * 2]     = data->left[size_t(i)];
            (*next)[size_t(i) * 2 + 1] = data->stereo ? data->right[size_t(i)] : data->left[size_t(i)];
        }
        secs = double(data->frames) / kSampleRate;
    }
    // Out first, then wait for a block that may still be reading it.
    std::vector<float>* old = hear_buf.exchange(nullptr);
    while(hear_busy.load())
        std::this_thread::yield();
    delete old;
    hear_pos.store(0);
    hear_buf.store(next);
    reply("/hear", {Value::s(path), Value::f(secs)});
}

// --- Preset files (.acpreset) ------------------------------------------------

namespace
{

// The app's settings that hold paths to the user's files (PresetFile.isMediaKey).
bool media_key(const std::string& k)
{
    return k == "sample" || k == "slice_sample" || k == "sf2" || k == "zones" || k == "voice" || k == "morphvoice" ||
           k == "impulse" ||
           (k.size() > 7 && k.compare(k.size() - 7, 7, "_sample") == 0);
}

// Each path [value] holds, put through [fn]: the first field of each line for
// zones, the whole value otherwise.
template <typename Fn> std::string map_paths(const std::string& key, const std::string& value, Fn fn)
{
    if(value.empty() || !media_key(key))
        return value;
    if(key != "zones")
        return fn(value);
    std::string out;
    size_t      at = 0;
    while(at < value.size())
    {
        size_t end = value.find('\n', at);
        if(end == std::string::npos)
            end = value.size();
        const std::string line = value.substr(at, end - at);
        const size_t      bar  = line.find('|');
        const std::string path = line.substr(0, bar);
        if(!out.empty())
            out += '\n';
        out += path.empty() ? line : fn(path) + (bar == std::string::npos ? "" : line.substr(bar));
        at = end + 1;
    }
    return out;
}

std::string dir_of(const std::string& path)
{
    const size_t slash = path.find_last_of('/');
    return slash == std::string::npos ? std::string() : path.substr(0, slash + 1);
}

bool exists(const std::string& path)
{
    struct stat st{};
    return !path.empty() && stat(path.c_str(), &st) == 0;
}

// [path] with each run of slashes made one, as norns's script path can end in "//".
std::string squash(const std::string& path)
{
    std::string out;
    for(const char ch : path)
        if(ch != '/' || out.empty() || out.back() != '/')
            out += ch;
    return out;
}

// A root as the script sends it: "/dir/" for one whose files are written
// relative to it, as the app's are to its user folder, or "label=/dir/" for
// one whose files are written "label:path", so a path is never read against
// the wrong folder.
struct Root
{
    std::string label, dir; // dir ends in '/'
};

Root parse_root(const std::string& root)
{
    Root         r;
    const size_t eq = root.find('=');
    if(eq != std::string::npos && eq > 0 && root.find('/') > eq)
    {
        r.label = root.substr(0, eq);
        r.dir   = root.substr(eq + 1);
    }
    else
        r.dir = root;
    r.dir = squash(r.dir);
    if(!r.dir.empty() && r.dir.back() != '/')
        r.dir += '/';
    return r;
}

// [path] under the first of [roots] it's in, written for that root, or as it is.
std::string relative_to(const std::string& path, const std::vector<std::string>& roots)
{
    const std::string p = squash(path);
    for(const auto& root : roots)
    {
        const Root r = parse_root(root);
        if(r.dir.size() > 1 && p.size() > r.dir.size() && p.compare(0, r.dir.size(), r.dir) == 0)
            return (r.label.empty() ? "" : r.label + ":") + p.substr(r.dir.size());
    }
    return p;
}

// Where a path from a preset is: as it is if absolute, "label:path" under
// that root, else under the first unlabelled root that has it, then the
// preset's own folder, else under the first unlabelled root (so a "not
// found" names it).
std::string resolve(const std::string& path, const std::vector<std::string>& roots, const std::string& own)
{
    if(path.empty() || path[0] == '/')
        return path;
    const size_t colon = path.find(':');
    if(colon != std::string::npos && colon > 0 && path.find('/') > colon)
        for(const auto& root : roots)
        {
            const Root r = parse_root(root);
            if(!r.label.empty() && path.compare(0, colon, r.label) == 0)
                return squash(r.dir + path.substr(colon + 1));
        }
    std::string first;
    for(const auto& root : roots)
    {
        const Root r = parse_root(root);
        if(!r.label.empty() || r.dir.empty())
            continue;
        if(first.empty())
            first = r.dir;
        if(exists(r.dir + path))
            return squash(r.dir + path);
    }
    if(!own.empty() && exists(own + path))
        return own + path;
    return squash((first.empty() ? own : first) + path);
}

// Where a file in a zip is, from its central directory.
struct ZipEntry
{
    uint32_t flags = 0, method = 0, crc = 0, packed = 0, size = 0;
    uint32_t local = 0; // its local header's offset
};

void put16(std::vector<uint8_t>& o, uint32_t v)
{
    o.push_back(uint8_t(v));
    o.push_back(uint8_t(v >> 8));
}
void put32(std::vector<uint8_t>& o, uint32_t v)
{
    put16(o, v & 0xffff);
    put16(o, v >> 16);
}

// Each entry of the zip [f] by name, from its central directory, reading only
// the end of the file and the directory. False if that can't be read.
bool zip_index(FILE* f, std::map<std::string, ZipEntry>& out)
{
    if(std::fseek(f, 0, SEEK_END) != 0)
        return false;
    const long size = std::ftell(f);
    if(size < 22)
        return false;
    const long                 tail = std::min<long>(size, 22 + 0xffff);
    std::vector<unsigned char> end(size_t(tail), 0);
    if(std::fseek(f, size - tail, SEEK_SET) != 0 || std::fread(end.data(), 1, end.size(), f) != end.size())
        return false;
    for(long at = tail - 22; at >= 0; --at)
    {
        const unsigned char* e = end.data() + at;
        if(le32(e) != 0x06054b50u)
            continue;
        const uint32_t count = le16(e + 10), dsize = le32(e + 12), dat = le32(e + 16);
        if(uint64_t(dat) + dsize > uint64_t(size - tail + at))
            return false;
        std::vector<unsigned char> dir(dsize);
        if(std::fseek(f, long(dat), SEEK_SET) != 0 || std::fread(dir.data(), 1, dir.size(), f) != dir.size())
            return false;
        size_t p = 0;
        for(uint32_t k = 0; k < count; ++k)
        {
            if(p + 46 > dir.size() || le32(dir.data() + p) != 0x02014b50u)
                return false;
            const unsigned char* c = dir.data() + p;
            const size_t n = le16(c + 28), skip = size_t(le16(c + 30)) + le16(c + 32);
            if(p + 46 + n > dir.size())
                return false;
            ZipEntry z;
            z.flags  = le16(c + 8);
            z.method = le16(c + 10);
            z.crc    = le32(c + 16);
            z.packed = le32(c + 20);
            z.size   = le32(c + 24);
            z.local  = le32(c + 42);
            out.emplace(std::string(reinterpret_cast<const char*>(c + 46), n), z);
            p += 46 + n + skip;
        }
        return true;
    }
    return false;
}

// The entries [names] of the zip [f], still packed, copied into a zip of
// their own for presetfile to read. False if one can't be read or they come
// to over [max] bytes packed.
bool zip_subset(FILE* f, const std::map<std::string, ZipEntry>& index, const std::vector<std::string>& names,
                size_t max, std::vector<uint8_t>& out)
{
    out.clear();
    if(std::fseek(f, 0, SEEK_END) != 0)
        return false;
    const long size = std::ftell(f);
    size_t     packed = 0;
    for(const auto& n : names)
    {
        const auto it = index.find(n);
        if(it == index.end() || it->second.packed > max - packed)
            return false;
        packed += it->second.packed;
    }
    std::vector<uint8_t> dir;
    for(const auto& n : names)
    {
        const ZipEntry& z = index.at(n);
        unsigned char   lh[30];
        if(std::fseek(f, long(z.local), SEEK_SET) != 0 || std::fread(lh, 1, 30, f) != 30 || le32(lh) != 0x04034b50u)
            return false;
        const uint64_t data = uint64_t(z.local) + 30 + le16(lh + 26) + le16(lh + 28);
        if(data + z.packed > uint64_t(size))
            return false;
        const uint32_t at = uint32_t(out.size());
        // the local header, then its central directory record
        const auto common = [&](std::vector<uint8_t>& o) {
            put16(o, 20);
            put16(o, z.flags & ~8u); // sizes are in the header, no descriptor after
            put16(o, z.method);
            put16(o, 0);
            put16(o, 0x21);
            put32(o, z.crc);
            put32(o, z.packed);
            put32(o, z.size);
            put16(o, uint32_t(n.size()));
            put16(o, 0);
        };
        put32(out, 0x04034b50u);
        common(out);
        out.insert(out.end(), n.begin(), n.end());
        put32(dir, 0x02014b50u);
        put16(dir, 20);
        common(dir);
        put16(dir, 0);
        put16(dir, 0);
        put16(dir, 0);
        put32(dir, 0);
        put32(dir, at);
        dir.insert(dir.end(), n.begin(), n.end());
        const size_t from = out.size();
        out.resize(from + z.packed);
        if(z.packed > 0 &&
           (std::fseek(f, long(data), SEEK_SET) != 0 || std::fread(out.data() + from, 1, z.packed, f) != z.packed))
            return false;
    }
    const uint32_t dir_at = uint32_t(out.size());
    out.insert(out.end(), dir.begin(), dir.end());
    put32(out, 0x06054b50u);
    put16(out, 0);
    put16(out, 0);
    put16(out, uint32_t(names.size()));
    put16(out, uint32_t(names.size()));
    put32(out, uint32_t(dir.size()));
    put32(out, dir_at);
    put16(out, 0);
    return true;
}

// Whether a zip has entries under media/ or engine/, which the app writes
// and presetfile::write doesn't. Reads only the central directory. True when
// it can't tell, so nothing is rewritten by mistake.
bool has_packed_entries(const std::string& path)
{
    FILE* f = std::fopen(path.c_str(), "rb");
    if(!f)
        return true;
    std::map<std::string, ZipEntry> index;
    bool                            packed = !zip_index(f, index);
    std::fclose(f);
    for(const auto& kv : index)
        packed = packed || kv.first.rfind("media/", 0) == 0 || kv.first.rfind("engine/", 0) == 0;
    return packed;
}

// A preset's manifest and patches are read from the first 64 MB of a file
// whose zip directory can't be read.
constexpr long kMaxPresetFile = 64L << 20;
// The most a preset's manifest and patches may take, packed.
constexpr size_t kMaxPatchBytes = size_t(16) << 20;

// Reads the manifest and patches of the preset at [path], only those
// entries, so the app's packed files can be any size. A modifier preset
// has the kind "modifier".
bool read_preset(const std::string& path, Preset& out, std::string& why)
{
    FILE* f = std::fopen(path.c_str(), "rb");
    if(!f)
    {
        why = "can't be read";
        return false;
    }
    std::vector<uint8_t>            data;
    std::map<std::string, ZipEntry> index;
    bool                            ok = false;
    if(zip_index(f, index))
    {
        std::vector<std::string> want;
        for(const auto& kv : index)
        {
            const std::string& n = kv.first;
            if(n == "preset.json" ||
               (n.rfind("patches/", 0) == 0 && n.size() > 13 && n.compare(n.size() - 5, 5, ".json") == 0))
                want.push_back(n);
        }
        ok = zip_subset(f, index, want, kMaxPatchBytes, data);
    }
    if(!ok)
    {
        // read whole, for presetfile to say what's wrong with it
        std::fseek(f, 0, SEEK_END);
        const long size = std::ftell(f);
        std::fseek(f, 0, SEEK_SET);
        if(size <= 0 || size > kMaxPresetFile)
        {
            std::fclose(f);
            why = size <= 0 ? "empty" : "too big";
            return false;
        }
        data.assign(static_cast<size_t>(size), 0);
        ok = std::fread(data.data(), 1, data.size(), f) == data.size();
    }
    std::fclose(f);
    if(!ok)
    {
        why = "can't be read";
        return false;
    }
    return EngineHost::readPreset(data.data(), data.size(), out, why);
}

// A patch from before version 6 has the Delay's old pingpong switch, which
// the app moves to mode on load: on is ping-pong (1/3), off is normal (0).
// Its other params stay.
void upgrade_delay_mode(PatchChain& chain)
{
    for(auto& fx : chain.inserts)
    {
        if(fx.type != "Delay")
            continue;
        for(auto& kv : fx.params)
            if(kv.first == "pingpong")
                kv = {"mode", kv.second >= 0.5f ? 1.f / 3.f : 0.f};
    }
}

// A Filament param [name] at [v] from a patch before version 5, on today's
// louder sympathetic bank: symlevel and the depth of each matrix slot aimed
// at the bank are scaled, as the app does (FilamentMigration.kt).
float filament_level(const std::vector<std::pair<std::string, float>>& params, const std::string& name, float v)
{
    namespace fl = filament_levels;
    if(name == "symlevel")
        return fl::level(v);
    const int slot = fl::matrixSlot(name, "amt");
    if(slot == 0)
        return v;
    const std::string dst = "m" + std::to_string(slot) + "_dst";
    for(const auto& kv : params)
        if(kv.first == dst)
            return fl::aimsAtBank(kv.second) ? fl::depth(v) : v;
    return v;
}

// Makes [dir] and the folders above it.
bool make_dirs(const std::string& dir)
{
    for(size_t at = 1; at <= dir.size(); ++at)
        if(at == dir.size() || dir[at] == '/')
        {
            const std::string sub = dir.substr(0, at);
            if(mkdir(sub.c_str(), 0755) != 0 && errno != EEXIST)
                return false;
        }
    return true;
}

// Writes [bytes] beside [path] and moves them over it, so a failed write
// leaves the old file.
bool write_file(const std::string& path, const std::vector<uint8_t>& bytes, std::string& why)
{
    if(!make_dirs(dir_of(path)))
    {
        why = std::string("can't make its folder: ") + std::strerror(errno);
        return false;
    }
    const std::string tmp = path + ".tmp";
    FILE*             f   = std::fopen(tmp.c_str(), "wb");
    if(!f)
    {
        why = std::string("can't write it: ") + std::strerror(errno);
        return false;
    }
    bool ok = std::fwrite(bytes.data(), 1, bytes.size(), f) == bytes.size();
    ok      = std::fclose(f) == 0 && ok;
    if(!ok || std::rename(tmp.c_str(), path.c_str()) != 0)
    {
        why = std::string("can't write it: ") + std::strerror(errno);
        std::remove(tmp.c_str());
        return false;
    }
    return true;
}

// The most a preset's packed files can unpack to at once, inflated.
constexpr size_t kMaxMediaBytes = size_t(64) << 20;

// A zip entry's name that can be written under a folder: relative, no "..",
// nothing odd.
bool safe_name(const std::string& name)
{
    if(name.empty() || name[0] == '/' || name.back() == '/' || name.find('\\') != std::string::npos ||
       name.find('\0') != std::string::npos)
        return false;
    size_t at = 0;
    while(at <= name.size())
    {
        size_t end = name.find('/', at);
        if(end == std::string::npos)
            end = name.size();
        const std::string part = name.substr(at, end - at);
        if(part.empty() || part == "." || part == "..")
            return false;
        at = end + 1;
    }
    return true;
}

// Unpacks the entries [names] of the preset zip at [path] into
// <first unlabelled root>acidulous/presets/<unit>/<file stem>/, "media/x" as
// x there and any other entry by its own name. Returns that folder, or "" if
// there's no root to put it under. A file already there with the entry's size
// is kept. Entries that can't be unpacked are in [failed] with why; the rest
// are still written. Only the named entries are read, one at a time, packed
// and inflated, so a preset's other files can be any size. [used] counts what
// was unpacked against kMaxMediaBytes over calls for one patch. Worker only.
std::string unpack_media(const std::string& path, const std::string& unit, const std::set<std::string>& names,
                         const std::vector<std::string>& roots, std::map<std::string, std::string>& failed,
                         uint64_t& used)
{
    std::string root;
    for(const auto& r : roots)
    {
        const Root pr = parse_root(r);
        if(pr.label.empty() && !pr.dir.empty())
        {
            root = pr.dir;
            break;
        }
    }
    const std::string name = stem(path);
    const auto fail_all = [&](const std::string& why) {
        for(const auto& e : names)
            failed[e] = why;
    };
    if(root.empty() || !safe_name(unit) || unit.find('/') != std::string::npos || !safe_name(name))
    {
        fail_all("nowhere to unpack it");
        return "";
    }
    const std::string dir = root + "acidulous/presets/" + unit + "/" + name + "/";
    const auto dest = [&](const std::string& e) { return dir + (e.rfind("media/", 0) == 0 ? e.substr(6) : e); };

    FILE* f = std::fopen(path.c_str(), "rb");
    if(!f)
    {
        fail_all("can't read the preset");
        return dir;
    }
    std::map<std::string, ZipEntry> index;
    if(!zip_index(f, index))
    {
        std::fclose(f);
        fail_all("not a zip");
        return dir;
    }
    std::vector<std::string> need;
    uint64_t                 total = 0;
    for(const auto& e : names)
    {
        const auto it = index.find(e);
        if(!safe_name(e))
            failed[e] = "a bad name";
        else if(it == index.end())
            failed[e] = "not in the preset";
        else
        {
            struct stat st{};
            if(stat(dest(e).c_str(), &st) == 0 && S_ISREG(st.st_mode) && uint64_t(st.st_size) == it->second.size)
                continue;
            need.push_back(e);
            total += it->second.size;
        }
    }
    if(used + total > kMaxMediaBytes)
    {
        for(const auto& e : need)
            failed[e] = "too big";
        need.clear();
    }
    used += total;
    for(const auto& e : need)
    {
        std::vector<uint8_t>                                      one;
        std::vector<std::pair<std::string, std::vector<uint8_t>>> entries;
        std::string                                               why;
        // packed can be a little over inflated, never by a megabyte
        if(!zip_subset(f, index, {e}, kMaxMediaBytes + (size_t(1) << 20), one))
            failed[e] = "a broken zip";
        else if(!presetfile::unzip(one.data(), one.size(), entries, why, kMaxMediaBytes) || entries.size() != 1)
            failed[e] = why == "too big to be a preset file" ? "too big" : why.empty() ? "a broken zip" : why;
        else
        {
            one = {};
            if(!write_file(dest(e), entries[0].second, why))
                failed[e] = why;
        }
    }
    std::fclose(f);
    return dir;
}

// Whether a path in a preset names one of the files packed in it.
bool packed_path(const std::string& p) { return p.rfind("media/", 0) == 0; }

// The file at [path] into [out], if it's a regular file of at most [max] bytes.
bool read_bytes(const std::string& path, size_t max, std::vector<uint8_t>& out)
{
    struct stat st{};
    if(stat(path.c_str(), &st) != 0 || !S_ISREG(st.st_mode) || uint64_t(st.st_size) > max)
        return false;
    FILE* f = std::fopen(path.c_str(), "rb");
    if(!f)
        return false;
    out.assign(size_t(st.st_size), 0);
    const bool ok = std::fread(out.data(), 1, out.size(), f) == out.size();
    std::fclose(f);
    return ok;
}

// Size of the regular file at [path], or -1.
int64_t file_size(const std::string& path)
{
    struct stat st{};
    return stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode) ? int64_t(st.st_size) : -1;
}

// CRC-32 of [n] bytes at [data] carried on from [c] (0 to start).
uint32_t crc32_add(uint32_t c, const uint8_t* data, size_t n)
{
    static uint32_t table[256];
    static bool     made = [] {
        for(uint32_t i = 0; i < 256; ++i)
        {
            uint32_t x = i;
            for(int k = 0; k < 8; ++k)
                x = (x & 1u) ? 0xedb88320u ^ (x >> 1) : x >> 1;
            table[i] = x;
        }
        return true;
    }();
    (void)made;
    c ^= 0xffffffffu;
    for(size_t i = 0; i < n; ++i)
        c = table[(c ^ data[i]) & 0xffu] ^ (c >> 8);
    return c ^ 0xffffffffu;
}

// A file going into a saved preset as [entry]: [data] if [source] is empty,
// else read from [source] while writing, [size] bytes.
struct Packed
{
    std::string          entry, source;
    std::vector<uint8_t> data;
    uint64_t             size = 0;
};

// Diction's voice and morphvoice in [settings], absolute paths to a voice's
// bank.json, packed as the app's PresetFile packs them: its bank.json and
// takes as media/voices/<name>/ in [files], the setting made
// media/voices/<name>/bank.json. A voice that can't be read, or would take
// [total] over kMaxMediaBytes, keeps its path. Entries go in [taken].
void pack_voices(std::vector<std::pair<std::string, std::string>>& settings, std::vector<Packed>& files,
                 std::set<std::string>& taken, uint64_t& total)
{
    std::map<std::string, std::string> done; // bank path -> its setting
    std::set<std::string>              names;
    for(auto& kv : settings)
    {
        if((kv.first != "voice" && kv.first != "morphvoice") || kv.second.empty() || packed_path(kv.second))
            continue;
        const std::string bank = squash(kv.second);
        if(done.count(bank))
        {
            kv.second = done[bank];
            continue;
        }
        std::string folder = dir_of(bank);
        if(base_name(bank) != "bank.json" || folder.size() < 2)
            continue;
        folder.pop_back();
        std::vector<uint8_t> index;
        if(!read_bytes(bank, size_t(1) << 20, index))
            continue;
        const std::string text(index.begin(), index.end());
        JsonReader        r{text};
        const Json  j     = r.value();
        const Json* takes = j.get("takes");
        if(!r.ok || j.kind != Json::Object || !takes || takes->kind != Json::Object)
            continue;
        // the folder's name, made free if another voice here has it
        const std::string want = base_name(folder);
        std::string       name = want;
        for(int n = 2; names.count(name); ++n)
            name = want + " " + std::to_string(n);
        const std::string   prefix = "media/voices/" + name + "/";
        std::vector<Packed> own;
        uint64_t            size = index.size();
        own.push_back({prefix + "bank.json", "", std::move(index), size});
        std::set<std::string> seen;
        for(const auto& t : takes->fields)
        {
            const std::string& file = t.second.str;
            if(t.second.kind != Json::String || file.empty() || file == "." || file == ".." ||
               file.find_first_of("/\\") != std::string::npos || !seen.insert(file).second)
                continue;
            const int64_t n = file_size(folder + "/" + file);
            if(n < 0)
                continue;
            size += uint64_t(n);
            own.push_back({prefix + file, folder + "/" + file, {}, uint64_t(n)});
        }
        if(total + size > kMaxMediaBytes)
        {
            std::fprintf(stderr, "acidulous: voice %s too big to pack, kept as a path\n", bank.c_str());
            continue;
        }
        total += size;
        names.insert(name);
        for(auto& f : own)
        {
            taken.insert(f.entry);
            files.push_back(std::move(f));
        }
        kv.second  = prefix + "bank.json";
        done[bank] = kv.second;
    }
}

// The other file settings in [settings] (pads, sample, slice_sample, sf2, the
// files of Mosaic's zones), absolute paths, packed as the app's PresetFile
// packs them: the file as media/<its path under the first unlabelled root> in
// [files], the setting pointing there. A file unpacked from a preset,
// <root>acidulous/presets/<unit>/<preset>/x, goes back in as media/x, and one
// under no root as media/<its name>. A name another file has gets " (2)" as
// the app's import does. Files under a labelled root (the script's own),
// missing ones and ones that would take [total] over kMaxMediaBytes keep their
// path.
void pack_files(std::vector<std::pair<std::string, std::string>>& settings, const std::vector<std::string>& roots,
                std::vector<Packed>& files, std::set<std::string>& taken, uint64_t& total)
{
    std::map<std::string, std::string> done; // file -> its entry
    const auto pack = [&](const std::string& path) -> std::string {
        if(path.empty() || packed_path(path) || path.rfind("factory:", 0) == 0)
            return path;
        const std::string p  = squash(path);
        const auto        at = done.find(p);
        if(at != done.end())
            return at->second;
        std::string rel;
        for(const auto& root : roots)
        {
            const Root r = parse_root(root);
            if(r.dir.size() > 1 && p.size() > r.dir.size() && p.compare(0, r.dir.size(), r.dir) == 0)
            {
                if(!r.label.empty())
                    return path;
                rel = p.substr(r.dir.size());
                break;
            }
        }
        if(rel.empty())
            rel = base_name(p);
        // acidulous/presets/<unit>/<preset>/x back to x
        if(rel.rfind("acidulous/presets/", 0) == 0)
        {
            const size_t unit = rel.find('/', 18);
            const size_t name = unit == std::string::npos ? unit : rel.find('/', unit + 1);
            if(name != std::string::npos && name + 1 < rel.size())
                rel = rel.substr(name + 1);
        }
        if(!safe_name(rel))
            rel = base_name(p);
        const int64_t size = file_size(p);
        if(size < 0 || !safe_name(rel))
            return path;
        if(total + uint64_t(size) > kMaxMediaBytes)
        {
            std::fprintf(stderr, "acidulous: %s too big to pack, kept as a path\n", p.c_str());
            return path;
        }
        std::string  entry = "media/" + rel;
        const size_t dot   = rel.find_last_of('.');
        const size_t slash = rel.find_last_of('/');
        const bool   ext   = dot != std::string::npos && (slash == std::string::npos || dot > slash + 1);
        for(int n = 2; taken.count(entry); ++n)
            entry = "media/" + (ext ? rel.substr(0, dot) : rel) + " (" + std::to_string(n) + ")" +
                    (ext ? rel.substr(dot) : std::string());
        total += uint64_t(size);
        taken.insert(entry);
        files.push_back({entry, p, {}, uint64_t(size)});
        done[p] = entry;
        return entry;
    };
    for(auto& kv : settings)
        if(kv.first != "voice" && kv.first != "morphvoice")
            kv.second = map_paths(kv.first, kv.second, pack);
}

// Writes the stored zip [zip] (as presetfile::write makes it, no comment) to
// [path] with [files] added, stored and dated like its own entries: a temp
// file, then renamed over [path]. False with [why] if it can't.
bool write_with_files(const std::string& path, const std::vector<uint8_t>& zip, const std::vector<Packed>& files,
                      std::string& why)
{
    if(files.empty())
        return write_file(path, zip, why);
    if(zip.size() < 22 || le32(zip.data() + zip.size() - 22) != 0x06054b50u)
    {
        why = "couldn't pack its files";
        return false;
    }
    const unsigned char* e     = zip.data() + zip.size() - 22;
    const uint32_t       count = le16(e + 10), dsize = le32(e + 12), dat = le32(e + 16);
    if(uint64_t(dat) + dsize != zip.size() - 22 || count + files.size() > 0xffff)
    {
        why = "couldn't pack its files";
        return false;
    }
    if(!make_dirs(dir_of(path)))
    {
        why = std::string("can't make its folder: ") + std::strerror(errno);
        return false;
    }
    const std::string tmp = path + ".tmp";
    FILE*             out = std::fopen(tmp.c_str(), "wb");
    if(!out)
    {
        why = std::string("can't write it: ") + std::strerror(errno);
        return false;
    }
    std::vector<uint8_t> dir(zip.begin() + dat, zip.begin() + dat + dsize);
    std::vector<uint8_t> buf(size_t(1) << 16);
    uint64_t             at = dat;
    bool                 ok = std::fwrite(zip.data(), 1, dat, out) == dat;
    // Streams [f]'s source through [each], false if it isn't [f.size] bytes.
    const auto stream = [&](const Packed& f, auto each) {
        FILE* in = std::fopen(f.source.c_str(), "rb");
        if(!in)
            return false;
        uint64_t got = 0;
        size_t   n;
        while((n = std::fread(buf.data(), 1, buf.size(), in)) > 0 && got + n <= f.size)
        {
            got += n;
            each(buf.data(), n);
        }
        std::fclose(in);
        return n == 0 && got == f.size;
    };
    for(size_t k = 0; ok && k < files.size(); ++k)
    {
        const Packed& f   = files[k];
        uint32_t      crc = 0;
        if(f.source.empty())
            crc = crc32_add(0, f.data.data(), f.data.size());
        else if(!stream(f, [&](const uint8_t* d, size_t n) { crc = crc32_add(crc, d, n); }))
        {
            why = base_name(f.source) + " changed while saving";
            ok  = false;
            break;
        }
        const std::string& n      = f.entry;
        const uint32_t     size   = uint32_t(f.size);
        const auto         common = [&](std::vector<uint8_t>& o) {
            put16(o, 0x0800); // names are UTF-8
            put16(o, 0);      // stored
            put16(o, 0);
            put16(o, 0x21);
            put32(o, crc);
            put32(o, size);
            put32(o, size);
            put16(o, uint32_t(n.size()));
            put16(o, 0);
        };
        std::vector<uint8_t> head;
        put32(head, 0x04034b50u);
        put16(head, 20);
        common(head);
        head.insert(head.end(), n.begin(), n.end());
        put32(dir, 0x02014b50u);
        put16(dir, 20);
        put16(dir, 20);
        common(dir);
        put16(dir, 0);
        put16(dir, 0);
        put16(dir, 0);
        put32(dir, 0);
        put32(dir, uint32_t(at));
        dir.insert(dir.end(), n.begin(), n.end());
        ok = std::fwrite(head.data(), 1, head.size(), out) == head.size();
        if(ok && f.source.empty())
            ok = std::fwrite(f.data.data(), 1, f.data.size(), out) == f.data.size();
        else if(ok)
        {
            uint32_t   again = 0;
            const bool same  = stream(f, [&](const uint8_t* d, size_t m) {
                again = crc32_add(again, d, m);
                ok    = std::fwrite(d, 1, m, out) == m && ok;
            }) && again == crc;
            if(!same)
                why = base_name(f.source) + " changed while saving";
            ok = ok && same;
        }
        at += head.size() + f.size;
    }
    if(ok)
    {
        const uint32_t total = count + uint32_t(files.size());
        std::vector<uint8_t> end;
        put32(end, 0x06054b50u);
        put16(end, 0);
        put16(end, 0);
        put16(end, total);
        put16(end, total);
        put32(end, uint32_t(dir.size()));
        put32(end, uint32_t(at));
        put16(end, 0);
        ok = std::fwrite(dir.data(), 1, dir.size(), out) == dir.size() &&
             std::fwrite(end.data(), 1, end.size(), out) == end.size();
    }
    ok = std::fclose(out) == 0 && ok;
    if(!ok || std::rename(tmp.c_str(), path.c_str()) != 0)
    {
        if(why.empty())
            why = std::string("can't write it: ") + std::strerror(errno);
        std::remove(tmp.c_str());
        return false;
    }
    return true;
}

// Whether the preset at [path] has the app's engine lines, which a rewrite
// would lose, or can't be read as a zip.
bool has_engine_lines(const std::string& path)
{
    FILE* f = std::fopen(path.c_str(), "rb");
    if(!f)
        return true;
    std::map<std::string, ZipEntry> index;
    bool                            app = !zip_index(f, index);
    std::fclose(f);
    for(const auto& kv : index)
        app = app || kv.first.rfind("engine/", 0) == 0;
    return app;
}

} // namespace

// Saves the part's sound as a preset at [path], with [settings] (the files
// the script knows, as absolute paths) and what the core holds: a Nexus
// graph, a Formulate program, Mosaic's zones. The files are packed in it
// under media/ (pack_voices, pack_files); paths left out are written relative
// to the first of [roots] they're under. Replies /preset_saved.
void Core::save_preset(int part, const std::string& path, const std::string& name,
                       std::vector<std::pair<std::string, std::string>> settings,
                       const std::vector<std::string>& roots)
{
    const auto done = [&](const std::string& why) {
        if(!why.empty())
            std::fprintf(stderr, "acidulous: saving %s: %s\n", path.c_str(), why.c_str());
        reply("/preset_saved", {Value::i(part), Value::s(path), Value::s(why.empty() ? "ok" : why)});
    };
    // A song's track holds what the song set, which the script can't name.
    if(song_on.load())
        return done("not while a song is loaded");
    const Part& p = parts[part];
    if(p.machine.empty())
        return done("no machine");
    if(name.empty())
        return done("no name");
    const auto set = [&](const std::string& key, const std::string& value) {
        for(auto& kv : settings)
            if(kv.first == key)
            {
                kv.second = value;
                return;
            }
        settings.emplace_back(key, value);
    };
    if(p.machine == "Nexus" && !p.nexus.empty())
        set("nexus", p.nexus);
    if(p.machine == "Formulate")
        for(const auto& kv : p.program_settings)
            set(kv.first, kv.second);
    if(p.machine == "Mosaic" && !p.zones.empty())
        set("zones", p.zones);
    // The files go in the zip under media/, up to kMaxMediaBytes in all.
    std::vector<Packed>   media;
    std::set<std::string> taken;
    uint64_t              packed = 0;
    if(p.machine == "Diction")
        pack_voices(settings, media, taken, packed);
    pack_files(settings, roots, media, taken, packed);
    for(auto& kv : settings)
        kv.second = map_paths(kv.first, kv.second, [&](const std::string& f) { return relative_to(f, roots); });

    // The values come from the engine, so it has to have run since the last
    // change, and a mount still queued makes it fail: wait a few blocks.
    std::vector<uint8_t> bytes;
    std::string          why;
    for(int tries = 0; tries < 10 && bytes.empty() && !stopping; ++tries)
    {
        mounts_queued = true;
        settle();
        PresetPatch patch = host.rackPatch(part, name, why, true, &patch_fx[part]);
        if(patch.machine.empty())
            continue;
        for(const auto& kv : settings)
        {
            bool replaced = false;
            for(auto& have : patch.settings)
                if(have.first == kv.first)
                {
                    have.second = kv.second;
                    replaced    = true;
                }
            if(!replaced)
                patch.settings.push_back(kv);
        }
        // A Venue insert's room goes back to the app's id, its own file is packed.
        std::vector<std::pair<std::string, std::string>> impulses;
        std::vector<std::string*>                        where;
        for(auto& fx : patch.chain.inserts)
            if(fx.type == "Venue")
                for(auto& kv : fx.settings)
                    if(kv.first == "impulse")
                    {
                        kv.second = file_to_room(kv.second);
                        impulses.emplace_back("impulse", kv.second);
                        where.push_back(&kv.second);
                    }
        pack_files(impulses, roots, media, taken, packed);
        for(size_t i = 0; i < impulses.size(); ++i)
            *where[i] = map_paths("impulse", impulses[i].second, [&](const std::string& f) { return relative_to(f, roots); });
        patch.low  = p.low;
        patch.high = p.high;
        Preset preset;
        preset.unit = patch.machine;
        preset.patches.push_back(std::move(patch));
        bytes = presetfile::write(preset);
    }
    if(bytes.empty())
        return done(why.empty() ? "busy" : why);
    // Only a file of one patch is replaced, so a bank's other patches stay.
    {
        Preset      old;
        std::string ignore;
        if(exists(path) && (!read_preset(path, old, ignore) || old.patches.size() > 1 || has_engine_lines(path)))
            return done("would replace more than one patch");
    }
    if(!write_with_files(path, bytes, media, why))
        return done(why);
    done("");
}

// Puts patch [index] of the preset at [path] on the part, as /patch does, its
// replies naming it [tag]. With [files] its samples are loaded too, found by
// resolve(). Ends with /preset_loaded i:part s:tag s:name s:error.
void Core::load_preset(int part, const std::string& path, int index, const std::string& tag, bool files,
                       const std::string& want_machine, const std::string& want_name,
                       const std::vector<std::string>& roots)
{
    const auto done = [&](const std::string& name, const std::string& why) {
        if(!why.empty())
            std::fprintf(stderr, "acidulous: %s: %s\n", path.c_str(), why.c_str());
        reply("/preset_loaded", {Value::i(part), Value::s(tag), Value::s(name), Value::s(why), Value::i(index)});
    };
    // The part still gets the machine the script has for it.
    const auto fail = [&](const std::string& why) {
        patch_failed_on(part, want_machine, tag, path);
        done("", why);
    };
    Preset      preset;
    std::string why;
    if(!read_preset(path, preset, why))
        return fail(why);
    if(preset.kind != "machine")
        return fail(preset.kind == "effect" ? "an effect preset" : "a modifier preset");
    // By name first, as a patch deleted from the file moves the ones after it.
    if(!want_name.empty() && !(index >= 0 && size_t(index) < preset.patches.size() &&
                               preset.patches[size_t(index)].name == want_name))
        for(size_t k = 0; k < preset.patches.size(); ++k)
            if(preset.patches[k].name == want_name)
            {
                index = int(k);
                break;
            }
    if(index < 0 || size_t(index) >= preset.patches.size())
        return fail("no patch " + std::to_string(index + 1));
    PresetPatch& pp = preset.patches[size_t(index)];
    // Version 3 differs only in Trinity's destinations, moved below.
    if(pp.version < 3)
        return fail("from an older app, save it again there");
    int32_t m = 0;
    while(m < MachineRegistry::count() && !lower_equal(MachineRegistry::name(m), pp.machine))
        ++m;
    if(m == MachineRegistry::count())
        return fail("no machine " + pp.machine);
    const std::string machine = MachineRegistry::name(m);
    if(parts[part].machine != machine && !mount(part, machine))
        return fail("can't mount " + machine);

    int32_t         count = 0;
    const ParamDef* defs  = MachineRegistry::paramDefs(machine.c_str(), count);
    PatchSource     src;
    src.name = pp.name;
    for(int32_t i = 0; i < count; ++i)
        src.norm.push_back(defs[i].unmap(defs[i].def));
    int unknown = 0;
    for(const auto& kv : pp.params)
    {
        int32_t i = 0;
        while(i < count && kv.first != defs[i].name)
            ++i;
        if(i == count)
        {
            ++unknown;
            continue;
        }
        float v = kv.second < 0.f ? 0.f : kv.second > 1.f ? 1.f : kv.second;
        if(pp.version < 4 && machine == "Trinity" && tables::isTrinityDest(kv.first))
            v = tables::trinityDest(v);
        if(pp.version < 5 && machine == "Filament")
            v = filament_level(pp.params, kv.first, v);
        src.norm[size_t(i)] = v;
        src.named.push_back(kv.first);
    }
    // Before version 5 a Filament patch that never set symlevel had the old
    // default, which is moved like a saved one.
    if(pp.version < 5 && machine == "Filament")
    {
        bool has = false;
        for(const auto& kv : pp.params)
            has = has || kv.first == "symlevel";
        for(int32_t i = 0; i < count && !has; ++i)
            if(std::strcmp(defs[i].name, "symlevel") == 0)
            {
                src.norm[size_t(i)] = filament_levels::level(filament_levels::kLevelDefault);
                src.named.push_back("symlevel");
                has = true;
            }
    }
    if(unknown > 0)
        std::fprintf(stderr, "acidulous: %s: %d param(s) %s hasn't got\n", path.c_str(), unknown, machine.c_str());
    src.settings = pp.settings;
    src.chain    = pp.chain;
    if(pp.version < 6)
        upgrade_delay_mode(src.chain);
    // A Venue in the patch's inserts: its impulse file is unpacked or found.
    for(auto& fx : src.chain.inserts)
    {
        if(fx.type != "Venue")
            continue;
        for(auto& kv : fx.settings)
        {
            std::string& impulse = kv.second;
            if(kv.first != "impulse" || impulse.empty() || impulse.rfind("factory:", 0) == 0)
                continue;
            if(packed_path(impulse))
            {
                std::map<std::string, std::string> failed;
                uint64_t                           used = 0;
                const std::string dir = unpack_media(path, "Venue", {impulse}, roots, failed, used);
                if(failed.count(impulse))
                    std::fprintf(stderr, "acidulous: %s: %s: %s\n", path.c_str(), impulse.c_str(), failed[impulse].c_str());
                if(!dir.empty())
                    impulse = dir + impulse.substr(6);
            }
            else
                impulse = resolve(impulse, roots, dir_of(path));
        }
    }
    src.low      = pp.low;
    src.high     = pp.high;
    if(!apply_patch(part, machine, src, tag))
        return done("", "couldn't apply it");
    if(files)
        load_preset_files(part, path, index, src.settings, roots);
    done(pp.name, "");
}

// Loads the files a preset's settings name into the part's machine, each
// with a /loaded or an /error. Files it doesn't name stay.
void Core::load_preset_files(int part, const std::string& path, int index,
                             const std::vector<std::pair<std::string, std::string>>& settings,
                             const std::vector<std::string>& roots)
{
    const std::string  own = dir_of(path);
    const std::string& m   = parts[part].machine;
    const auto is_pad = [&](const std::string& k) {
        return m == "Forage" && k.size() == 10 && k[0] == 'p' && std::isdigit((unsigned char)k[1]) &&
               std::isdigit((unsigned char)k[2]) && k.compare(3, 7, "_sample") == 0;
    };
    const auto is_sample = [&](const std::string& k) {
        return k == "sample" && (m == "Dice" || m == "Pollen" || m == "Molt");
    };
    const auto is_slices = [&](const std::string& k) { return k == "slice_sample" && m == "Forage"; };
    const auto is_voice  = [&](const std::string& k) { return (k == "voice" || k == "morphvoice") && m == "Diction"; };
    // The app's packed files these settings name are unpacked first.
    std::set<std::string> packed;
    for(const auto& kv : settings)
        if(packed_path(kv.second) && (is_pad(kv.first) || is_sample(kv.first) || is_slices(kv.first) ||
                                      is_voice(kv.first) || (kv.first == "sf2" && m == "Mosaic")))
            packed.insert(kv.second);
    std::map<std::string, std::string> failed;
    std::string                        media;
    uint64_t                           used = 0;
    if(!packed.empty() && has_packed_entries(path))
        media = unpack_media(path, m, packed, roots, failed, used);
    else
        packed.clear();
    // A packed voice's takes, named in its bank.json, come out after it.
    for(const auto& kv : settings)
    {
        if(!is_voice(kv.first) || !packed.count(kv.second) || failed.count(kv.second))
            continue;
        std::vector<std::string> files;
        int                      missing = 0;
        std::string              why;
        voice_spec(media + kv.second.substr(6), files, missing, why);
        std::set<std::string> takes;
        for(const auto& t : files)
            takes.insert(dir_of(kv.second) + t);
        std::map<std::string, std::string> bad;
        if(!takes.empty())
            unpack_media(path, m, takes, roots, bad, used);
        if(!bad.empty())
            load_error(part, "voice", kv.second, kv.first == "voice" ? 0 : 1,
                       base_name(bad.begin()->first) + ": " + bad.begin()->second +
                           (bad.size() > 1 ? " (and " + std::to_string(bad.size() - 1) + " more)" : ""));
    }
    // Loads [file] as [kind], or says why its packed copy couldn't be unpacked.
    const auto load_file = [&](const std::string& kind, const std::string& file, int arg) {
        if(!packed.count(file))
            return load(part, kind, resolve(file, roots, own), arg, roots);
        const auto it = failed.find(file);
        if(it != failed.end())
            return load_error(part, kind, file, arg, it->second);
        load(part, kind, media + file.substr(6), arg, roots);
    };
    std::string sf2, voice, morph;
    int         sf2preset = 0;
    bool        zones     = false;
    for(const auto& kv : settings)
    {
        const std::string& k = kv.first;
        if(kv.second.empty())
            continue;
        if(is_pad(k))
        {
            const int pad = (k[1] - '0') * 10 + (k[2] - '0');
            if(pad < kForagePads)
                load_file("pad", kv.second, pad);
        }
        else if(is_slices(k))
            load_file("slices", kv.second, 0);
        else if(is_sample(k))
            load_file("sample", kv.second, 0);
        else if(k == "sf2" && m == "Mosaic")
            sf2 = kv.second;
        else if(k == "sf2preset")
            sf2preset = std::max(0, std::atoi(kv.second.c_str()));
        else if(k == "zones" && m == "Mosaic")
            zones = true;
        else if(is_voice(k))
            (k == "voice" ? voice : morph) = kv.second;
    }
    if(!sf2.empty())
        load_file("sf2", sf2, sf2preset);
    else if(zones)
        load(part, "zones", path, index, roots);
    // the voice to morph to first, so the one sung in is analysed for it once
    if(!morph.empty())
        load_file("voice", morph, 1);
    if(!voice.empty())
        load_file("voice", voice, 0);
}

// Mosaic: the zones of patch [index] of the preset at [path], their files
// found by resolve(). Zones whose file is missing are left out and counted in
// [missing]. Worker, under the part's mount lock.
std::string Core::load_zones(int part, const std::string& path, int index, const std::vector<std::string>& roots,
                             int& missing)
{
    Preset      preset;
    std::string why;
    if(!read_preset(path, preset, why))
        return why;
    if(index < 0 || size_t(index) >= preset.patches.size())
        return "no patch " + std::to_string(index + 1);
    std::string text;
    for(const auto& kv : preset.patches[size_t(index)].settings)
        if(kv.first == "zones")
            text = kv.second;
    const std::string own = dir_of(path);
    std::string       spec;
    int64_t           need = 0;
    missing                = 0;
    // The app's packed files the zones name are unpacked first; one that
    // can't be is left out, as a missing one is, with an /error.
    std::set<std::string> packed;
    map_paths("zones", text, [&](const std::string& f) {
        if(packed_path(f))
            packed.insert(f);
        return f;
    });
    std::map<std::string, std::string> failed;
    std::string                        media;
    if(!packed.empty() && has_packed_entries(path))
    {
        uint64_t used = 0;
        media         = unpack_media(path, "Mosaic", packed, roots, failed, used);
    }
    else
        packed.clear();
    if(!failed.empty())
        load_error(part, "zones", path, index,
                   base_name(failed.begin()->first) + ": " + failed.begin()->second +
                       (failed.size() > 1 ? " (and " + std::to_string(failed.size() - 1) + " more)" : ""));
    // the zones whose file is there, one per line
    const std::string all = map_paths("zones", text, [&](const std::string& f) {
        if(!packed.count(f))
            return resolve(f, roots, own);
        return failed.count(f) ? std::string() : media + f.substr(6);
    });
    size_t            at  = 0;
    while(at < all.size())
    {
        size_t end = all.find('\n', at);
        if(end == std::string::npos)
            end = all.size();
        const std::string line = all.substr(at, end - at);
        const size_t      bar  = line.find('|');
        at                     = end + 1;
        if(bar == 0 || bar == std::string::npos)
            continue;
        const std::string file = line.substr(0, bar);
        if(!exists(file))
        {
            ++missing;
            continue;
        }
        need += decoded_bytes(file, 0, 0, false);
        spec += line + "\n";
    }
    if(spec.empty())
        return text.empty() ? "no zones in it" : "none of its files found";
    if(!room_for(need, why))
        return why;
    if(!host.loadZoneMap(part, spec, stem(path), why))
        return why;
    parts[part].zones = spec;
    set_held(part, 0, need);
    return "";
}

// --- Venue impulses -------------------------------------------------------

namespace
{
constexpr const char* kFactory     = "factory:";
constexpr const char* kDefaultRoom = "factory:wooden-hall";
// How long a shape knob rests before the impulse is built again.
constexpr int kImpulseSettleMs = 150;

bool starts_with(const std::string& s, const char* prefix) { return s.rfind(prefix, 0) == 0; }
} // namespace

// Where the app's rooms are: data/rooms beside the banks.
std::string Core::rooms_dir() const
{
    std::string d = bank_dir;
    while(d.size() > 1 && d.back() == '/')
        d.pop_back();
    const size_t slash = d.find_last_of('/');
    return (slash == std::string::npos ? std::string() : d.substr(0, slash + 1)) + "rooms";
}

// The file an impulse setting names: a room's as "factory:<id>", else the path
// as it is. "" with [why] set if it isn't there.
std::string Core::impulse_file(const std::string& impulse, std::string& why) const
{
    std::string file = impulse;
    if(starts_with(impulse, kFactory))
    {
        const std::string id = impulse.substr(8);
        if(id.empty() || !safe_name(id) || id.find('/') != std::string::npos)
        {
            why = "no room " + id;
            return "";
        }
        file = rooms_dir() + "/impulse-" + id + ".wav";
    }
    if(!exists(file))
    {
        why = starts_with(impulse, kFactory) ? "no room " + impulse.substr(8) : "not found";
        return "";
    }
    return file;
}

// A "factory:<id>" setting as its room file, anything else as it is.
std::string Core::room_to_file(const std::string& impulse) const
{
    std::string why;
    const std::string file = starts_with(impulse, kFactory) ? impulse_file(impulse, why) : std::string();
    return file.empty() ? impulse : file;
}

// A room's file as "factory:<id>", anything else as it is.
std::string Core::file_to_room(const std::string& file) const
{
    const std::string dir = rooms_dir() + "/impulse-";
    if(file.size() > dir.size() + 4 && file.compare(0, dir.size(), dir) == 0 && file.compare(file.size() - 4, 4, ".wav") == 0)
        return kFactory + file.substr(dir.size(), file.size() - dir.size() - 4);
    return file;
}

// Asks for the slot's impulse to be built from fx_impulse and fx_shape, now
// or once the knobs have rested. Worker thread. Inline (the render tool) it
// builds now.
void Core::request_impulse(int part, int slot, bool settle_first)
{
    if(inline_jobs)
    {
        settle();
        build_impulse(part, slot, fx_impulse[part][slot], fx_shape[part][slot]);
        return;
    }
    {
        std::lock_guard<std::mutex> l(cloud_lock);
        ImpulseRequest& r = impulse_req[part][slot];
        r.want            = true;
        r.impulse         = fx_impulse[part][slot];
        std::copy(fx_shape[part][slot], fx_shape[part][slot] + 3, r.shape);
        r.due = std::chrono::steady_clock::now() + std::chrono::milliseconds(settle_first ? kImpulseSettleMs : 0);
    }
    cloud_cv.notify_one();
}

// Cloud thread, or the render tool's main loop: reads, shapes and builds the
// impulse and hands it to the Venue. The engine refuses it if the slot has
// another effect by then. Replies "/impulse_loaded i:part i:slot s:impulse
// s:error".
void Core::build_impulse(int part, int slot, const std::string& impulse, const float* shape)
{
    std::string why;
    {
        std::lock_guard<std::mutex> m(mount_locks[part]);
        if(stopping)
            return;
        std::string file;
        if(!impulse.empty())
            file = impulse_file(impulse, why);
        if(why.empty())
            why = host.loadImpulse(part, kInsertUnits[slot], file, shape[0], shape[1], shape[2]);
    }
    if(!why.empty())
        error("Venue: " + (impulse.empty() ? std::string("no impulse") : base_name(impulse)) + ": " + why);
    reply("/impulse_loaded", {Value::i(part), Value::i(slot), Value::s(impulse), Value::s(why)});
}

// Worker thread. /impulse: names the slot's Venue impulse, "factory:<room>",
// a room's bare id, a file (read against [roots] if relative) or "" for none.
void Core::set_impulse(int part, int slot, const std::string& impulse, const std::vector<std::string>& roots)
{
    if(!impulse_on[part][slot])
    {
        error("no Venue in slot " + std::to_string(slot));
        return;
    }
    std::string setting = impulse;
    if(!impulse.empty() && !starts_with(impulse, kFactory))
    {
        std::string ignore;
        if(impulse.find('/') == std::string::npos && !impulse_file(kFactory + impulse, ignore).empty())
            setting = kFactory + impulse;
        else
            setting = resolve(impulse, roots, "");
    }
    fx_impulse[part][slot] = setting;
    request_impulse(part, slot, false);
}

// Takes the slot's impulse out of the host and forgets it. Worker thread.
void Core::drop_impulse(int part, int slot)
{
    {
        std::lock_guard<std::mutex> l(cloud_lock);
        impulse_req[part][slot].want = false;
    }
    if(std::strcmp(host.mountedEffect(part, slot), "Venue") == 0)
    {
        std::lock_guard<std::mutex> m(mount_locks[part]);
        host.loadImpulse(part, kInsertUnits[slot], "", 1.f, 1.f, 1.f);
    }
    impulse_on[part][slot]  = false;
    fx_impulse[part][slot].clear();
}

// After mounts: a Venue in a slot that had none starts with the default room
// and its knobs at their defaults, or the patch's [at] insert's; a slot that
// stopped being a Venue lets its impulse go. With [request] false the caller
// asks for the build itself. Worker thread.
void Core::sync_impulses(int part, const acidulous::PatchInsert* const at[acidulous::kEffectSlots], bool request)
{
    for(int slot = 0; slot < kEffectSlots; ++slot)
    {
        const bool venue = std::strcmp(host.mountedEffect(part, slot), "Venue") == 0;
        if(!venue)
        {
            if(impulse_on[part][slot])
                drop_impulse(part, slot);
            continue;
        }
        const acidulous::PatchInsert* fx = at && at[slot] && at[slot]->type == "Venue" ? at[slot] : nullptr;
        if(impulse_on[part][slot] && !fx)
            continue;
        impulse_on[part][slot]  = true;
        fx_impulse[part][slot]  = kDefaultRoom;
        int32_t         count   = 0;
        const ParamDef* defs    = EffectRegistry::paramDefs("Venue", count);
        static const char* const names[3] = {"length", "decay", "stretch"};
        for(int k = 0; k < 3; ++k)
        {
            fx_shape[part][slot][k] = 1.f;
            for(int32_t i = 0; defs && i < count; ++i)
                if(std::strcmp(defs[i].name, names[k]) == 0)
                    fx_shape[part][slot][k] = defs[i].unmap(defs[i].def);
            if(fx)
                for(const auto& kv : fx->params)
                    if(kv.first == names[k])
                        fx_shape[part][slot][k] = kv.second < 0.f ? 0.f : kv.second > 1.f ? 1.f : kv.second;
        }
        // applyPatchChain loads the patch's own impulse, so only a patch without one asks.
        bool loaded = false;
        if(fx)
            for(const auto& kv : fx->settings)
                if(kv.first == "impulse" && !kv.second.empty())
                {
                    fx_impulse[part][slot] = file_to_room(kv.second);
                    loaded                 = true;
                }
        if(loaded)
        {
            // A rebuild queued for the old Venue must not shape into this one.
            {
                std::lock_guard<std::mutex> l(cloud_lock);
                impulse_req[part][slot].want = false;
            }
            reply("/impulse_loaded", {Value::i(part), Value::i(slot), Value::s(fx_impulse[part][slot]), Value::s("")});
        }
        else if(request)
            request_impulse(part, slot, false);
    }
}

// Puts patch [index] (or the one called [want_name]) of the effect preset at
// [path] in the part's own insert slot 2 or 3, as the app's slot picker does:
// a new effect when the slot has another type, the preset's params and the
// rest at their defaults. The slot's mod stays, as an effect preset has none.
// Replies "/fx_preset_loaded i:part i:slot s:type s:name s:error (s:param
// f:value01 f:default01)..." with the slot's type and values, then /inserts
// as /insert does.
void Core::load_fx_preset(int part, int slot, const std::string& path, int index, const std::string& want_name,
                          const std::vector<std::string>& roots)
{
    const char* unit = kInsertUnits[slot];
    const auto  done = [&](const std::string& name, const std::string& why) {
        if(!why.empty())
            std::fprintf(stderr, "acidulous: %s: %s\n", path.c_str(), why.c_str());
        const std::string type = host.mountedEffect(part, slot);
        Args              out{Value::i(part), Value::i(slot), Value::s(type), Value::s(name), Value::s(why)};
        int32_t           count = 0;
        const ParamDef*   defs  = type.empty() ? nullptr : EffectRegistry::paramDefs(type.c_str(), count);
        for(int32_t i = 0; defs && i < count; ++i)
        {
            const float v = host.paramNormalized(part, unit, defs[i].name);
            out.push_back(Value::s(defs[i].name));
            out.push_back(Value::f(v < 0.f ? defs[i].unmap(defs[i].def) : v));
            out.push_back(Value::f(defs[i].unmap(defs[i].def)));
        }
        reply("/fx_preset_loaded", out);
        const acidulous::PatchInsert* none[kEffectSlots] = {};
        send_inserts(part, none, true);
    };
    Preset      preset;
    std::string why;
    if(!read_preset(path, preset, why))
        return done("", why);
    if(preset.kind != "effect")
        return done("", "not an effect preset");
    if(!want_name.empty() && !(index >= 0 && size_t(index) < preset.patches.size() &&
                               preset.patches[size_t(index)].name == want_name))
        for(size_t k = 0; k < preset.patches.size(); ++k)
            if(preset.patches[k].name == want_name)
            {
                index = int(k);
                break;
            }
    if(index < 0 || size_t(index) >= preset.patches.size())
        return done("", "no patch " + std::to_string(index + 1));
    const PresetPatch& pp = preset.patches[size_t(index)];
    if(pp.version < 3)
        return done("", "from an older app, save it again there");
    int32_t e = 0;
    while(e < EffectRegistry::count() && !lower_equal(EffectRegistry::name(e), preset.unit))
        ++e;
    if(e == EffectRegistry::count())
        return done("", "no effect " + preset.unit);
    const std::string type  = EffectRegistry::name(e);
    int32_t           count = 0;
    const ParamDef*   defs  = EffectRegistry::paramDefs(type.c_str(), count);
    if(!defs || count <= 0)
        return done("", "no effect " + preset.unit);
    // Below version 6 a Delay has the old pingpong switch.
    acidulous::PatchChain chain;
    chain.inserts.push_back({type, pp.params, {}, false});
    if(pp.version < 6)
        upgrade_delay_mode(chain);
    const auto& params = chain.inserts[0].params;

    settle();
    if(type != host.mountedEffect(part, slot))
    {
        mounts_queued = true;
        fx_formula[part][slot].clear();
        if(!host.mountEffect(part, slot, type))
        {
            host.mountEffect(part, slot, "");
            settle();
            sync_impulses(part);
            return done("", "can't mount effect " + type);
        }
        settle();
        sync_impulses(part, nullptr, false);
    }
    int unknown = 0;
    for(const auto& kv : params)
    {
        int32_t i = 0;
        while(i < count && kv.first != defs[i].name)
            ++i;
        // the slot's mod isn't a preset's to set
        if(i == count && slotModIndexOf(kv.first.c_str()) == -1)
            ++unknown;
    }
    if(unknown > 0)
        std::fprintf(stderr, "acidulous: %s: %d param(s) %s hasn't got\n", path.c_str(), unknown, type.c_str());
    for(int32_t i = 0; i < count; ++i)
    {
        float v = defs[i].unmap(defs[i].def);
        for(const auto& kv : params)
            if(kv.first == defs[i].name)
                v = kv.second;
        push_param(part, unit, defs[i].name, v);
    }
    // A Formula's formula comes with its preset, none for its default.
    if(type == "Formula")
    {
        std::string formula;
        for(const auto& kv : pp.settings)
            if(kv.first == "formula")
                formula = kv.second;
        const std::string error = host.loadEffectFormula(part, unit, formula);
        if(!error.empty())
            std::fprintf(stderr, "acidulous: %s: formula: %s\n", path.c_str(), error.c_str());
        fx_formula[part][slot] = error.empty() ? formula : "";
    }
    // A Venue's impulse comes with its preset, the default room without one.
    if(type == "Venue" && impulse_on[part][slot])
    {
        static const char* const names[3] = {"length", "decay", "stretch"};
        for(int k = 0; k < 3; ++k)
            for(int32_t i = 0; i < count; ++i)
                if(std::strcmp(defs[i].name, names[k]) == 0)
                {
                    float v = defs[i].unmap(defs[i].def);
                    for(const auto& kv : params)
                        if(kv.first == names[k])
                            v = kv.second;
                    fx_shape[part][slot][k] = v < 0.f ? 0.f : v > 1.f ? 1.f : v;
                }
        std::string impulse = kDefaultRoom;
        for(const auto& kv : pp.settings)
            if(kv.first == "impulse")
                impulse = kv.second;
        if(!impulse.empty() && !starts_with(impulse, kFactory))
        {
            if(packed_path(impulse))
            {
                std::map<std::string, std::string> failed;
                uint64_t                           used = 0;
                const std::string                  dir  = unpack_media(path, "Venue", {impulse}, roots, failed, used);
                if(failed.count(impulse))
                    std::fprintf(stderr, "acidulous: %s: %s: %s\n", path.c_str(), impulse.c_str(),
                                 failed[impulse].c_str());
                impulse = dir.empty() ? impulse : dir + impulse.substr(6);
            }
            else
                impulse = resolve(impulse, roots, dir_of(path));
        }
        fx_impulse[part][slot] = impulse;
        request_impulse(part, slot, false);
    }
    mounts_queued = true;
    settle();
    done(pp.name, "");
}

// Saves the effect in the part's own insert slot 2 or 3 as an effect preset
// at [path] called [name], as the app's slot picker saves one: its params
// without the slot's mod, and a Formula's formula as last loaded. Replies
// "/fx_preset_saved i:part i:slot s:path s:ok|error".
void Core::save_fx_preset(int part, int slot, const std::string& path, const std::string& name,
                          const std::vector<std::string>& roots)
{
    const auto done = [&](const std::string& why) {
        if(!why.empty())
            std::fprintf(stderr, "acidulous: saving %s: %s\n", path.c_str(), why.c_str());
        reply("/fx_preset_saved", {Value::i(part), Value::i(slot), Value::s(path), Value::s(why.empty() ? "ok" : why)});
    };
    if(song_on.load())
        return done("not while a song is loaded");
    if(name.empty())
        return done("no name");
    const std::string type = host.mountedEffect(part, slot);
    if(type.empty())
        return done("no effect in fx " + std::to_string(slot + 1));
    int32_t         count = 0;
    const ParamDef* defs  = EffectRegistry::paramDefs(type.c_str(), count);
    PresetPatch     pp;
    pp.machine = type;
    pp.name    = name;
    // The values come from the engine, once a queued mount has landed.
    for(int tries = 0; tries < 10 && !stopping && int32_t(pp.params.size()) < count; ++tries)
    {
        mounts_queued = true;
        settle();
        pp.params.clear();
        for(int32_t i = 0; i < count; ++i)
        {
            const float v = host.paramNormalized(part, kInsertUnits[slot], defs[i].name);
            if(v < 0.f)
                break;
            pp.params.emplace_back(defs[i].name, v);
        }
    }
    if(count <= 0 || int32_t(pp.params.size()) < count)
        return done("busy");
    if(type == "Formula" && !fx_formula[part][slot].empty())
        pp.settings.emplace_back("formula", fx_formula[part][slot]);
    // A Venue's room, or its file packed in the preset.
    std::vector<Packed>   media;
    std::set<std::string> taken;
    uint64_t              packed = 0;
    if(type == "Venue")
    {
        pp.settings.emplace_back("impulse", impulse_on[part][slot] ? fx_impulse[part][slot] : std::string(kDefaultRoom));
        pack_files(pp.settings, roots, media, taken, packed);
        for(auto& kv : pp.settings)
            kv.second = map_paths(kv.first, kv.second, [&](const std::string& f) { return relative_to(f, roots); });
    }
    Preset preset;
    preset.kind = "effect";
    preset.unit = type;
    preset.patches.push_back(pp);
    std::string why;
    // Only a file of one patch is replaced, so a bank's other patches stay.
    {
        Preset      old;
        std::string ignore;
        if(exists(path) && (!read_preset(path, old, ignore) || old.patches.size() > 1 || has_engine_lines(path)))
            return done("would replace more than one patch");
    }
    if(!write_with_files(path, presetfile::write(preset), media, why))
        return done(why);
    done("");
}

// The modifiers the app has presets for (PatchStore.MODIFIERS_WITH_PRESETS).
bool mod_has_presets(const std::string& type) { return type == "Arp" || type == "Chord"; }

// A modifier param a preset neither saves nor loads, as the app's
// PatchStore.modifierKeeps: latch, the chord's key and scale, and its memory.
bool mod_preset_keeps(const std::string& name)
{
    if(name == "latch" || name == "key" || name == "scale" || name == "memory")
        return true;
    return name.size() == 4 && name[0] == 'm' && name[1] >= '1' && name[1] <= '8' && name[2] == '_' && name[3] >= '1' &&
           name[3] <= '6';
}

// Puts patch [index] (or the one called [want_name]) of the modifier preset
// at [path] in the part's modifier slot 0 to 2, as the app's slot picker
// does: its type mounted, on, if the slot has another, the preset's params
// and the rest at their defaults, apart from those mod_preset_keeps names.
// Replies "/mod_preset_loaded i:part i:slot s:type s:name s:error (s:param
// f:value01)..." with the slot's type and values.
void Core::load_mod_preset(int part, int slot, const std::string& path, int index, const std::string& want_name)
{
    const char* unit = kModUnits[slot];
    const auto  done = [&](const std::string& name, const std::string& why) {
        if(!why.empty())
            std::fprintf(stderr, "acidulous: %s: %s\n", path.c_str(), why.c_str());
        wait_applied();
        const std::string& type = mod_types[part][slot];
        Args               out{Value::i(part), Value::i(slot), Value::s(type), Value::s(name), Value::s(why)};
        int32_t            count = 0;
        const ParamDef*    defs  = type.empty() ? nullptr : InputModRegistry::paramDefs(type.c_str(), count);
        for(int32_t i = 0; defs && i < count; ++i)
        {
            const float v = host.paramNormalized(part, unit, defs[i].name);
            out.push_back(Value::s(defs[i].name));
            out.push_back(Value::f(v < 0.f ? defs[i].unmap(defs[i].def) : v));
        }
        reply("/mod_preset_loaded", out);
    };
    Preset      preset;
    std::string why;
    if(!read_preset(path, preset, why))
        return done("", why);
    if(preset.kind != "modifier")
        return done("", "not a modifier preset");
    if(!want_name.empty() && !(index >= 0 && size_t(index) < preset.patches.size() &&
                               preset.patches[size_t(index)].name == want_name))
        for(size_t k = 0; k < preset.patches.size(); ++k)
            if(preset.patches[k].name == want_name)
            {
                index = int(k);
                break;
            }
    if(index < 0 || size_t(index) >= preset.patches.size())
        return done("", "no patch " + std::to_string(index + 1));
    const PresetPatch& pp = preset.patches[size_t(index)];
    int32_t            t  = 0;
    while(t < InputModRegistry::count() && !lower_equal(InputModRegistry::name(t), preset.unit))
        ++t;
    if(t == InputModRegistry::count() || !mod_has_presets(InputModRegistry::name(t)))
        return done("", "no modifier " + preset.unit);
    const std::string type  = InputModRegistry::name(t);
    int32_t           count = 0;
    const ParamDef*   defs  = InputModRegistry::paramDefs(type.c_str(), count);
    if(!defs || count <= 0)
        return done("", "no modifier " + preset.unit);
    if(type != mod_types[part][slot])
    {
        set_mod(part, slot, type);
        if(mod_types[part][slot] != type)
            return done("", "can't mount modifier " + type);
    }
    int unknown = 0;
    for(const auto& kv : pp.params)
    {
        int32_t i = 0;
        while(i < count && kv.first != defs[i].name)
            ++i;
        if(i == count)
            ++unknown;
    }
    if(unknown > 0)
        std::fprintf(stderr, "acidulous: %s: %d param(s) %s hasn't got\n", path.c_str(), unknown, type.c_str());
    for(int32_t i = 0; i < count; ++i)
    {
        if(mod_preset_keeps(defs[i].name))
            continue;
        float v = defs[i].unmap(defs[i].def);
        for(const auto& kv : pp.params)
            if(kv.first == defs[i].name)
                v = kv.second;
        push_param(part, unit, defs[i].name, v);
    }
    done(pp.name, "");
}

// Saves the modifier in the part's slot 0 to 2 as a modifier preset at
// [path] called [name], as the app's slot picker saves one: its params
// without those mod_preset_keeps names. Only an Arp or a Chord, as the app
// has presets for those alone. Replies "/mod_preset_saved i:part i:slot
// s:path s:ok|error".
void Core::save_mod_preset(int part, int slot, const std::string& path, const std::string& name)
{
    const auto done = [&](const std::string& why) {
        if(!why.empty())
            std::fprintf(stderr, "acidulous: saving %s: %s\n", path.c_str(), why.c_str());
        reply("/mod_preset_saved", {Value::i(part), Value::i(slot), Value::s(path), Value::s(why.empty() ? "ok" : why)});
    };
    if(song_on.load())
        return done("not while a song is loaded");
    if(name.empty())
        return done("no name");
    const std::string type = mod_types[part][slot];
    if(type.empty())
        return done("no modifier in mod " + std::to_string(slot + 1));
    if(!mod_has_presets(type))
        return done("no " + lower(type) + " presets");
    int32_t         count = 0;
    const ParamDef* defs  = InputModRegistry::paramDefs(type.c_str(), count);
    if(!defs || count <= 0)
        return done("no modifier " + type);
    wait_applied();
    PresetPatch pp;
    pp.machine = type;
    pp.name    = name;
    for(int32_t i = 0; i < count; ++i)
    {
        if(mod_preset_keeps(defs[i].name))
            continue;
        const float v = host.paramNormalized(part, kModUnits[slot], defs[i].name);
        if(v < 0.f)
            return done("busy");
        pp.params.emplace_back(defs[i].name, v);
    }
    Preset preset;
    preset.kind = "modifier";
    preset.unit = type;
    preset.patches.push_back(pp);
    std::string why;
    // Only a file of one patch is replaced, so a bank's other patches stay.
    {
        Preset      old;
        std::string ignore;
        if(exists(path) && (!read_preset(path, old, ignore) || old.patches.size() > 1 || has_engine_lines(path)))
            return done("would replace more than one patch");
    }
    const std::vector<uint8_t> bytes = presetfile::write(preset);
    if(bytes.empty())
        return done("can't be written");
    if(!write_file(path, bytes, why))
        return done(why);
    done("");
}

// "/preset_scan s:folder (s:file i:index s:name s:unit i:count i:packed)..."
// for every patch of every .acpreset in [folder], by file name; with [deep]
// the ones in its folders too, file as "folder/name". An effect preset's unit
// is "fx.<type>" and a modifier preset's "mod.<type>", as the app keys them.
// Files that can't be read are left out. [folder] can be one .acpreset file
// instead, which gives just its rows, the reply naming that path.
void Core::scan_presets(const std::string& folder, bool deep)
{
    struct stat one{};
    const bool  is_file = !folder.empty() && folder.back() != '/' && stat(folder.c_str(), &one) == 0 && S_ISREG(one.st_mode);
    const size_t cut    = is_file ? folder.rfind('/') : std::string::npos;
    const std::string dir = is_file ? (cut == std::string::npos ? "" : folder.substr(0, cut + 1))
                            : folder.empty() || folder.back() == '/' ? folder : folder + "/";
    std::vector<std::string> names;
    const auto               list = [&](const std::string& sub) {
        DIR* d = opendir((dir + sub).c_str());
        if(!d)
            return;
        while(const dirent* e = readdir(d))
        {
            const std::string n = e->d_name;
            if(n.empty() || n[0] == '.')
                continue;
            if(n.size() > 9 && lower(n.substr(n.size() - 9)) == ".acpreset")
                names.push_back(sub + n);
            else if(deep && sub.empty())
            {
                struct stat st{};
                if(stat((dir + n).c_str(), &st) == 0 && S_ISDIR(st.st_mode))
                    names.push_back(n + "/");
            }
        }
        closedir(d);
    };
    if(is_file)
        names.push_back(folder.substr(cut == std::string::npos ? 0 : cut + 1));
    else
        list("");
    if(deep && !is_file)
    {
        std::vector<std::string> subs;
        for(const auto& n : names)
            if(n.back() == '/')
                subs.push_back(n);
        names.erase(std::remove_if(names.begin(), names.end(), [](const std::string& n) { return n.back() == '/'; }),
                    names.end());
        for(const auto& sub : subs)
            list(sub);
    }
    std::sort(names.begin(), names.end(), [](const std::string& a, const std::string& b) { return lower(a) < lower(b); });
    Args out{Value::s(folder)};
    // one UDP message: a few hundred rows fit
    constexpr size_t kMaxRows = 300;
    for(const auto& n : names)
    {
        Preset      preset;
        std::string why;
        if(!read_preset(dir + n, preset, why))
        {
            std::fprintf(stderr, "acidulous: %s: %s\n", (dir + n).c_str(), why.c_str());
            continue;
        }
        const std::string unit = preset.kind == "effect"     ? "fx." + preset.unit
                                 : preset.kind == "modifier" ? "mod." + preset.unit
                                                             : preset.unit;
        // what a rewrite would lose: the app's engine lines, or another patch's files
        const bool packed = has_engine_lines(dir + n) || (preset.patches.size() > 1 && has_packed_entries(dir + n));
        for(size_t k = 0; k < preset.patches.size() && out.size() < 1 + 6 * kMaxRows; ++k)
        {
            out.push_back(Value::s(n));
            out.push_back(Value::i(int(k)));
            out.push_back(Value::s(preset.patches[k].name));
            out.push_back(Value::s(unit));
            out.push_back(Value::i(int(preset.patches.size())));
            out.push_back(Value::i(packed ? 1 : 0));
        }
    }
    reply("/preset_scan", out);
}

// Takes patch [index] out of the preset at [path], or deletes the file when
// it's the only one. Replies "/preset_deleted s:path i:index s:ok|error".
void Core::delete_preset(const std::string& path, int index)
{
    const auto done = [&](const std::string& why) {
        reply("/preset_deleted", {Value::s(path), Value::i(index), Value::s(why.empty() ? "ok" : why)});
    };
    Preset      preset;
    std::string why;
    // index -1 deletes the whole file, whatever is in it
    if(index < 0)
        return done(std::remove(path.c_str()) == 0 ? "" : std::string("can't delete it: ") + std::strerror(errno));
    if(!read_preset(path, preset, why))
        return done(why);
    if(size_t(index) >= preset.patches.size())
        return done("no patch " + std::to_string(index + 1));
    // The app's samples and engine lines would be lost in a rewrite.
    if(preset.patches.size() > 1 && has_packed_entries(path))
        return done("it has samples, delete the whole file");
    if(preset.patches.size() == 1)
        return done(std::remove(path.c_str()) == 0 ? "" : std::string("can't delete it: ") + std::strerror(errno));
    preset.patches.erase(preset.patches.begin() + index);
    if(!write_file(path, presetfile::write(preset), why))
        return done(why);
    done("");
}

} // namespace core
