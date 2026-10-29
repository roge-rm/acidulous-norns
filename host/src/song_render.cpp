// Replays a song zip's engine lines into EngineHost and writes a WAV.
//
//   acidulous-song [--live SECONDS] [--frozen auto|notes|frozen] <song.zip or folder> <out.wav> [seconds] [tail] [bits]
//
// The lines are engine.txt in the zip (unpacked with unzip into a scratch
// folder) or the folder. Each line is the app's own JNI call, made through
// the web build's stand-in JNI (third_party/acidulous/platform/lines), into
// the same EngineHost and audio driver the plugin uses.
//
// By default it renders the way the app's export and tools/engine_replay.sh
// do (EngineHost::renderSong from the top): seconds 0 is to the end of the
// song, tail is 2 seconds and bits 32 (float) unless given. --live plays the
// transport from scene 0 instead and pulls SECONDS of blocks through the
// driver, as the plugin hears it, always float.
//
// --frozen picks which racks play the zip's frozen clips (default frozen, as
// the app plays them; see song_frozen.h). The sample budget isn't applied.
//
// Tracks render on the calling thread, as in the plugin (ACIDULOUS_WORKERS=0
// unless set).
#include "song_frozen.h"

#include <drivers/AudioDriver.h>
#include <EngineHost.h>
#include <engine/machine/MachineRegistry.h>
#include <jni.h>
#include <platform/lines/EngineLines.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <unistd.h>
#include <vector>

extern "C" {
jboolean Java_com_rm_acidulous_engine_EngineNative_nativeStart(JNIEnv*, jobject);
void     Java_com_rm_acidulous_engine_EngineNative_nativeStop(JNIEnv*, jobject);
void     Java_com_rm_acidulous_engine_EngineNative_nativePrewarm(JNIEnv*, jobject);
jstring  Java_com_rm_acidulous_engine_EngineNative_nativeRenderSong(JNIEnv*, jobject, jstring, jfloat, jint, jint,
                                                                   jint, jfloat);
}

namespace
{

// Pulls [n] blocks so the audio thread's side takes what's queued.
void pull_blocks(int n)
{
    float out[acidulous::kBlockFrames * 2];
    for(int i = 0; i < n; ++i)
        AudioDriver::pull(nullptr, out);
}

void put32(std::ofstream& f, uint32_t v) { f.write(reinterpret_cast<const char*>(&v), 4); }
void put16(std::ofstream& f, uint16_t v) { f.write(reinterpret_cast<const char*>(&v), 2); }

// 48 kHz stereo float WAV.
bool write_float_wav(const std::string& path, const std::vector<float>& lr)
{
    std::ofstream f(path, std::ios::binary);
    if(!f)
        return false;
    const uint32_t bytes = uint32_t(lr.size() * sizeof(float));
    f.write("RIFF", 4);
    put32(f, 36 + bytes);
    f.write("WAVEfmt ", 8);
    put32(f, 16);
    put16(f, 3); // IEEE float
    put16(f, 2);
    put32(f, acidulous::kSampleRate);
    put32(f, acidulous::kSampleRate * 8);
    put16(f, 8);
    put16(f, 32);
    f.write("data", 4);
    put32(f, bytes);
    f.write(reinterpret_cast<const char*>(lr.data()), bytes);
    return bool(f);
}

std::string shell_quote(const std::string& s)
{
    std::string out = "'";
    for(char c : s)
        out += c == '\'' ? std::string("'\\''") : std::string(1, c);
    return out + "'";
}

} // namespace

int main(int argc, char** argv)
{
    float live = -1.f;
    core::FrozenMode frozen = core::FrozenMode::Frozen;
    std::vector<std::string> rest;
    for(int k = 1; k < argc; ++k)
    {
        const std::string a = argv[k];
        if(a == "--live" && k + 1 < argc)
            live = std::strtof(argv[++k], nullptr);
        else if(a == "--frozen" && k + 1 < argc)
        {
            if(!core::parse_frozen_mode(argv[++k], frozen))
            {
                std::fprintf(stderr, "acidulous-song: --frozen takes auto, notes or frozen\n");
                return 2;
            }
        }
        else
            rest.push_back(a);
    }
    if(rest.size() < 2)
    {
        std::fprintf(stderr,
                     "usage: acidulous-song [--live SECONDS] [--frozen auto|notes|frozen] <song.zip or folder> <out.wav> [seconds] [tail] [bits]\n");
        return 2;
    }
    const std::string in      = rest[0];
    const std::string out     = rest[1];
    const float       seconds = rest.size() > 2 ? std::strtof(rest[2].c_str(), nullptr) : 0.f;
    const float       tail    = rest.size() > 3 ? std::strtof(rest[3].c_str(), nullptr) : 2.f;
    const int         bits    = rest.size() > 4 ? std::atoi(rest[4].c_str()) : 32;

    // A zip is unpacked into a scratch folder, removed at the end.
    std::string dir = in;
    std::string scratch;
    if(in.size() > 4 && in.compare(in.size() - 4, 4, ".zip") == 0)
    {
        const char* tmp = std::getenv("TMPDIR");
        std::string tmpl = std::string(tmp && *tmp ? tmp : "/tmp") + "/acidulous-song-XXXXXX";
        std::vector<char> buf(tmpl.begin(), tmpl.end());
        buf.push_back(0);
        if(!mkdtemp(buf.data()))
        {
            std::perror("acidulous-song: mkdtemp");
            return 1;
        }
        scratch = dir = buf.data();
        if(std::system(("unzip -q " + shell_quote(in) + " -d " + shell_quote(dir)).c_str()) != 0)
        {
            std::fprintf(stderr, "acidulous-song: couldn't unzip %s\n", in.c_str());
            std::system(("rm -rf " + shell_quote(scratch)).c_str());
            return 1;
        }
    }
    auto cleanup = [&] {
        if(!scratch.empty())
            std::system(("rm -rf " + shell_quote(scratch)).c_str());
    };

    std::ifstream file(dir + "/engine.txt", std::ios::binary);
    if(!file)
    {
        std::fprintf(stderr, "acidulous-song: no engine.txt in %s\n", in.c_str());
        cleanup();
        return 1;
    }
    std::stringstream text;
    text << file.rdbuf();

    setenv("ACIDULOUS_WORKERS", "0", 0);
    static JNIEnv env;
    Java_com_rm_acidulous_engine_EngineNative_nativePrewarm(&env, nullptr);
    if(!Java_com_rm_acidulous_engine_EngineNative_nativeStart(&env, nullptr))
    {
        std::fprintf(stderr, "acidulous-song: the engine didn't start\n");
        cleanup();
        return 1;
    }

    acidulous::lines::Replay replay;
    replay.env  = &env;
    replay.root = dir;
    // A block after every line: mounts apply at the top of a block, and a
    // line after a mount (LoadNexusPatch, BuildCloud) needs the machine in
    // place. It also keeps the 512-entry parameter queue from filling.
    replay.afterLine = [](int) { pull_blocks(1); };
    core::FrozenHost fh;
    fh.can_run = [](const std::string& m) {
        for(int32_t i = 0; i < acidulous::MachineRegistry::count(); ++i)
            if(m == acidulous::MachineRegistry::name(i))
                return true;
        return false;
    };
    const core::FrozenChoice pick = core::choose_frozen(text.str(), frozen, fh);
    if(!pick.frozen.empty())
        std::fprintf(stderr, "acidulous-song: %zu racks play frozen clips, %zu without a machine\n", pick.frozen.size(),
                     pick.no_machine.size());
    if(!acidulous::lines::replay(pick.text, replay))
    {
        jniweb::arena().clear();
        Java_com_rm_acidulous_engine_EngineNative_nativeStop(&env, nullptr);
        cleanup();
        return 1;
    }
    pull_blocks(4);
    std::fprintf(stderr, "acidulous-song: %d calls, %d skipped, %d answered false\n", replay.calls, replay.skipped,
                 replay.refused);

    bool ok = true;
    if(live >= 0.f)
    {
        auto& host = acidulous::EngineHost::instance();
        host.transportPlay(0);
        const int64_t       blocks = int64_t(live * acidulous::kSampleRate) / acidulous::kBlockFrames;
        std::vector<float> lr(size_t(blocks) * acidulous::kBlockFrames * 2);
        for(int64_t b = 0; b < blocks; ++b)
            AudioDriver::pull(nullptr, lr.data() + b * acidulous::kBlockFrames * 2);
        host.transportStop();
        ok = write_float_wav(out, lr);
        if(!ok)
            std::fprintf(stderr, "acidulous-song: couldn't write %s\n", out.c_str());
    }
    else
    {
        jstring path  = env.NewStringUTF(out.c_str());
        jstring error = Java_com_rm_acidulous_engine_EngineNative_nativeRenderSong(&env, nullptr, path, tail, 0, bits,
                                                                                   0, seconds);
        ok = error == nullptr || error->text.empty();
        if(!ok)
            std::fprintf(stderr, "acidulous-song: render failed: %s\n", error->text.c_str());
    }
    jniweb::arena().clear();
    Java_com_rm_acidulous_engine_EngineNative_nativeStop(&env, nullptr);
    cleanup();
    return ok && replay.skipped == 0 ? 0 : 1;
}
