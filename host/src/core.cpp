#include "core.h"

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
#include <engine/machine/MachineRegistry.h>
#include <engine/machine/cumulus/Cloud.h>
#include <engine/machine/cumulus/Cumulus.h>
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

static long long sample_budget_bytes()
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

} // namespace

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
    // Empties the racks and sends so the next Core starts clean: the
    // unmounts apply at the top of a block, at most kMaxMountsPerBlock (8) a
    // block, so a few blocks are pulled here since the UGen no longer runs
    // this core. A retired machine frees what was mounted into it.
    for(int p = 0; p < kParts; ++p)
    {
        host.unmountMachine(p);
        for(int s = 0; s < kEffectSlots; ++s)
        {
            const char* fx = host.mountedEffect(p, s);
            if(fx && *fx)
                host.mountEffect(p, s, "");
        }
    }
    for(int s = 0; s < kSends; ++s)
        if(send_mounted[s])
            host.mountSend(s, "");
    host.setMpeZone(0, 15, 48);
    // The UGen has stopped calling process(), but a pull may still be
    // finishing, so a pull that fails is tried again, for up to 50 ms. 8
    // machines, 16 inserts and 2 sends take 4 blocks; 6 leaves room.
    float in[kBlockFrames * 2] = {}, out[kBlockFrames * 2];
    for(int tries = 0, pulled = 0; tries < 50 && pulled < 6; ++tries)
    {
        if(AudioDriver::pull(in, out))
            ++pulled;
        else
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
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
    for(int p = 0; p < kParts; ++p)
        post_part(p, [this, p] { unmount(p); });
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
        jobs_cv.wait(l, [this] { return quitting || !jobs.empty(); });
        if(quitting)
            return;
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
            post_part(part, [this, part, kind = to_str(a[1]), p = to_str(a[2]), arg = a.size() > 3 ? to_int(a[3]) : 0] {
                load(part, kind, p, arg);
            });
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
    if(path == "/master" && a.size() >= 2)
    {
        post([this, n = to_str(a[0]), v = to_float(a[1])] {
            if(host.paramIndex("", "master", n) < 0)
                error("no master param " + n);
            else
                push_param(0, "master", n, v);
        });
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
    else
        return;
    clear_held(part);
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
        int  next = -1;
        auto now  = std::chrono::steady_clock::now();
        for(int p = 0; p < kParts; ++p)
            if(clouds[p].want && (next < 0 || clouds[p].due < clouds[next].due))
                next = p;
        if(next < 0)
        {
            cloud_cv.wait(l);
            continue;
        }
        if(clouds[next].due > now)
        {
            cloud_cv.wait_until(l, clouds[next].due);
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
    for(size_t i = 0; i < known.size() && i < size_t(kEffectSlots); ++i)
        if(slots.slotOf[i] >= 0)
            at[slots.slotOf[i]] = known[i];

    int32_t                   skipped = 0, unknown = 0;
    const acidulous::SendPlan plan    = host.applyPatchChain(part, chain, patch_fx[part], part_sends[part][0],
                                                             part_sends[part][1], &skipped, &unknown);
    if(skipped > 0)
        std::fprintf(stderr, "acidulous: %s: %d insert(s) found no free slot\n", what.c_str(), int(skipped));
    if(unknown > 0)
        std::fprintf(stderr, "acidulous: %s: %d insert(s) name no effect this build has\n", what.c_str(), int(unknown));
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
// f:value01", as /def has them. The values are the ones the patch set, or the
// defaults. [at] is the insert the patch put in each slot, or null.
void Core::send_inserts(int part, const acidulous::PatchInsert* const at[kEffectSlots])
{
    Args out{Value::i(part)};
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
        out.push_back(Value::i(slot));
        out.push_back(Value::s(type));
        out.push_back(Value::i(count));
        for(int32_t i = 0; i < count; ++i)
        {
            const ParamDef& d = defs[i];
            float           v = d.unmap(d.def);
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
        }
    }
    reply("/inserts", out);
}

// One of the part's insert's params, by name.
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
    bool            found = false;
    for(int32_t i = 0; i < count && !found; ++i)
        found = name == defs[i].name;
    if(!found)
    {
        error("no " + type + " param " + name);
        return;
    }
    push_param(part, slot == 0 ? "effect1" : "effect2", name, v01);
}

// A send effect's param, kept for when it's mounted.
void Core::set_send(int slot, const std::string& name, float v01)
{
    if(name == "mix" || !send_has(slot, name))
    {
        error("no send param " + name);
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

void Core::load_patch(int part, const std::string& machine, const std::string& name)
{
    // A /patch that fails takes the last patch's inserts off and gives back
    // the sends it held, so every /patch ends in one /inserts.
    const auto fail = [&] { apply_chain(part, acidulous::PatchChain{}, machine + " " + name); };
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

    // A Nexus patch names only the knobs it changes; the rest take their
    // module's default from the graph, since every slot knob defaults to zero
    // and an oscillator at level zero is silent. As the app's audition does.
    std::string nexus;
    Args        macros;
    if(machine == "Nexus")
    {
        for(const auto& kv : r.settings)
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
            std::set<std::string> named;
            for(const auto& v : patch->values)
                named.insert(v.name);
            for(int32_t i = 0; i < count; ++i)
            {
                const std::string n = defs[i].name;
                if(n.size() < 6 || n[0] != 's' || n[3] != '_' || n[4] != 'p' || named.count(n))
                    continue;
                const int slot = (n[1] - '0') * 10 + (n[2] - '0'), knob = n[5] - '1';
                if(slot >= 0 && slot < nx::kSlots && knob >= 0 && knob < nx::kKnobs)
                    r.norm[size_t(i)] = defs[i].unmap(knobs[size_t(slot) * nx::kKnobs + size_t(knob)]);
            }
            macros = nexus_macros(*g, *patch);
        }
        else
        {
            error(machine + " " + patch->name + ": " + (nexus.empty() ? std::string("no graph") : why));
            nexus.clear();
        }
    }

    // Every param, so nothing is left over from the last patch, except the
    // ones the machine keeps (Bias's lane levels and mutes, which are the mix
    // of its takes, as the app's MachineUi.patchKeeps). mount() has waited
    // for the new machine to be in place. Loaded samples stay too, since the
    // machine isn't remounted.
    for(int32_t i = 0; i < count; ++i)
    {
        if(patch_keeps(machine, defs[i].name) && size_t(i) < parts[part].values.size())
        {
            r.norm[size_t(i)] = parts[part].values[size_t(i)];
            continue;
        }
        if(!push_param(part, "machine", defs[i].name, r.norm[size_t(i)]))
            return fail();
    }
    parts[part].values = r.norm;

    // Formulate's formula and tables are compiled here and swapped in. A
    // patch without them clears the last one, which plays the plain oscillator.
    if(machine == "Formulate")
    {
        std::string formula, arp, duty, vol;
        for(const auto& kv : r.settings)
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
        std::unique_ptr<machine::formulate::Program> program;
        if(!(formula.empty() && arp.empty() && duty.empty() && vol.empty()))
        {
            std::string why;
            program = machine::formulate::compile(formula, arp, duty, vol, why);
            if(!program)
                error(machine + " " + patch->name + ": " + why);
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
    else if(machine != "Trinity" && !r.settings.empty())
        std::fprintf(stderr, "acidulous: %s %s: settings are not supported yet\n", machine.c_str(),
                     patch->name.c_str());

    // A Trinity's macro names, macro1..macro8 joined by newlines, or "" when
    // the patch names none. Other machines clear them.
    {
        std::string names;
        if(machine == "Trinity")
        {
            bool any = false;
            for(int k = 1; k <= 8; ++k)
            {
                const std::string key = "macro" + std::to_string(k);
                std::string       v;
                for(const auto& kv : r.settings)
                    if(kv.first == key)
                        v = kv.second;
                any = any || !v.empty();
                names += (k > 1 ? "\n" : "") + v;
            }
            if(!any)
                names.clear();
        }
        host.setMacroNames(part, names);
    }

    // The patch's inserts and sends. One without them takes the last patch's
    // inserts off and gives back the sends it held.
    std::vector<std::string> fx_problems;
    const acidulous::PatchChain chain = ab::resolveChain(*patch, fx_problems);
    for(const auto& p : fx_problems)
        std::fprintf(stderr, "acidulous: %s %s: %s\n", machine.c_str(), patch->name.c_str(), p.c_str());
    apply_chain(part, chain, machine + " " + patch->name);

    parts[part].patch = patch->name;
    if(!macros.empty())
    {
        Args out{Value::i(part), Value::s(machine), Value::s(patch->name)};
        out.insert(out.end(), macros.begin(), macros.end());
        reply("/macros", out);
    }
    Args values{Value::i(part), Value::s(machine), Value::s(patch->name)};
    for(int32_t i = 0; i < count; ++i)
        values.push_back(Value::f(r.norm[size_t(i)]));
    reply("/values", values);
    reply("/patch", {Value::i(part), Value::s(machine), Value::s(patch->name)});
    if(machine == "Nexus")
        send_graph(part);
}

// A Nexus patch's macros: the knobs the patch sets, in its order, then the
// knobs of the modules in its graph, as param and label pairs, up to five.
Args Core::nexus_macros(const machine::nexus::Graph& g, const audition::BankPatch& patch)
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
    for(const auto& v : patch.values)
    {
        const std::string& n = v.name;
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
    int64_t                     sum = 0;
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
void Core::load(int part, const std::string& kind, const std::string& path, int arg)
{
    try
    {
        load_now(part, kind, path, arg);
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

void Core::load_now(int part, const std::string& kind, const std::string& path, int arg)
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
    load_error(part, kind, path, arg, "no such kind of load");
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

} // namespace core
