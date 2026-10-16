// Checks the sample budget and names with | in them: a folder whose WAVs
// decode past the budget is refused before anything is read, loads fit again
// once a part is cleared or remounted, a file named with | in a folder is left
// out, and a path with | is refused, all without the plugin going down.
// Usage: acidulous-limit-test BANK_DIR SCRATCH_DIR [MATERIAL_DIR]
// With MATERIAL_DIR (host/tests/make_material.py), it also checks the
// estimate read from WAV, AIFF and FLAC headers against a real decode.
// SCRATCH_DIR is emptied of the test's own files and filled with WAVs.
#include "core.h"

#include <engine/format/AudioDecoder.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <sys/stat.h>
#include <vector>

namespace
{

struct Log : core::Replier
{
    std::mutex               lock;
    std::vector<std::string> lines;
    void                     reply(const char* path, const core::Args& args) override
    {
        if(std::strcmp(path, "/loaded") && std::strcmp(path, "/error"))
            return;
        std::string line = path;
        for(const auto& a : args)
            line += " " + (a.type == 's' ? a.str : std::to_string(int(a.num)));
        std::lock_guard<std::mutex> l(lock);
        lines.push_back(line);
    }
    std::vector<std::string> take()
    {
        std::lock_guard<std::mutex> l(lock);
        auto                        out = lines;
        lines.clear();
        return out;
    }
};

// A 16-bit stereo 48 kHz sine.
void write_wav(const std::string& path, double seconds)
{
    const uint32_t frames = uint32_t(seconds * 48000), data = frames * 4;
    FILE*          f      = std::fopen(path.c_str(), "wb");
    if(!f)
    {
        std::perror(path.c_str());
        std::exit(2);
    }
    const auto u32 = [&](uint32_t v) { std::fwrite(&v, 4, 1, f); };
    const auto u16 = [&](uint16_t v) { std::fwrite(&v, 2, 1, f); };
    std::fwrite("RIFF", 1, 4, f), u32(36 + data), std::fwrite("WAVEfmt ", 1, 8, f);
    u32(16), u16(1), u16(2), u32(48000), u32(48000 * 4), u16(4), u16(16);
    std::fwrite("data", 1, 4, f), u32(data);
    for(uint32_t i = 0; i < frames; ++i)
    {
        const int16_t v = int16_t(8000 * std::sin(i * 0.05));
        std::fwrite(&v, 2, 1, f), std::fwrite(&v, 2, 1, f);
    }
    std::fclose(f);
}

int failures = 0;

void expect(bool ok, const char* what, const std::vector<std::string>& got)
{
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    if(!ok)
    {
        ++failures;
        for(const auto& l : got)
            std::printf("    %s\n", l.c_str());
    }
}

bool has(const std::vector<std::string>& lines, const std::string& start, const char* text = "")
{
    for(const auto& l : lines)
        if(l.rfind(start, 0) == 0 && l.find(text) != std::string::npos)
            return true;
    return false;
}

} // namespace

int main(int argc, char** argv)
{
    if(argc < 3)
    {
        std::fprintf(stderr, "usage: acidulous-limit-test BANK_DIR SCRATCH_DIR\n");
        return 2;
    }
    const std::string dir = argv[2];
    const std::string big = dir + "/big", small = dir + "/small", bar = dir + "/bar";
    for(const auto& d : {dir, big, small, bar})
        mkdir(d.c_str(), 0755);
    // 6 x 3 s stereo is 6.6 MB decoded, past a 4 MB budget; 2 x 0.5 s is 0.4 MB
    for(int k = 0; k < 6; ++k)
        write_wav(big + "/tone C" + std::to_string(k + 2) + ".wav", 3);
    write_wav(small + "/a C3.wav", 0.5);
    write_wav(small + "/b C5.wav", 0.5);
    write_wav(bar + "/x D4.wav", 0.5);
    write_wav(bar + "/a|b C4.wav", 0.5);
    write_wav(dir + "/pad.wav", 4); // 1.5 MB decoded
    write_wav(dir + "/p|d.wav", 0.5);

    setenv("ACIDULOUS_SAMPLE_LIMIT_MB", "4", 1);
    Log  log;
    auto c = new core::Core(argv[1], &log, false);
    using core::Value;
    const auto run = [&](const char* path, core::Args a) {
        c->command(path, a);
        c->drain();
        return log.take();
    };
    const auto load = [&](int part, const char* kind, const std::string& path, int arg = 0) {
        return run("/load", {Value::i(part), Value::s(kind), Value::s(path), Value::i(arg)});
    };

    run("/patch", {Value::i(0), Value::s("Mosaic"), Value::s("Init")});
    auto got = load(0, "folder", big);
    expect(has(got, "/error", "needs 6.6 MB") && !has(got, "/loaded"), "a folder past the budget is refused", got);
    got = load(0, "folder", small);
    expect(has(got, "/loaded 0 folder") && has(got, "/loaded", "Mosaic"), "a small folder loads, tagged Mosaic", got);
    got = load(0, "folder", bar);
    expect(has(got, "/loaded 0 folder"), "a folder with | in a file name loads the rest", got);
    got = load(0, "sample", dir + "/p|d.wav");
    expect(has(got, "/error", "names with |"), "a path with | is refused", got);

    // Forage pads fill the budget: 0.4 MB on part 0, then 1.5 MB each.
    run("/patch", {Value::i(1), Value::s("Forage"), Value::s("Init")});
    got = load(1, "pad", dir + "/pad.wav", 0);
    auto more = load(1, "pad", dir + "/pad.wav", 1);
    got.insert(got.end(), more.begin(), more.end());
    expect(has(got, "/loaded 1 pad") && !has(got, "/error"), "two pads fit", got);
    got = load(1, "pad", dir + "/pad.wav", 2);
    expect(has(got, "/error", "for samples is in use"), "a third pad is over the budget", got);
    got = load(1, "pad", dir + "/pad.wav", 1);
    expect(has(got, "/error"), "reloading a pad counts the old one until it's swapped", got);
    load(1, "clear", "");
    got = load(1, "pad", dir + "/pad.wav", 2);
    expect(has(got, "/loaded 1 pad") && !has(got, "/error"), "after a clear it fits again", got);
    load(1, "pad", dir + "/pad.wav", 3);
    run("/patch", {Value::i(1), Value::s("Reflux"), Value::s("Init")});
    run("/patch", {Value::i(1), Value::s("Forage"), Value::s("Init")});
    got = load(1, "pad", dir + "/pad.wav", 4);
    auto again = load(1, "pad", dir + "/pad.wav", 5);
    got.insert(got.end(), again.begin(), again.end());
    expect(!has(got, "/error"), "a remount frees the part's share", got);

    delete c;

    if(argc > 3)
    {
        const std::string mat = argv[3];
        for(const char* f : {"/loop.wav", "/voice.flac", "/kit/02-rim.aif", "/kit/12-cowbell.flac", "/keys/Keys C4.wav"})
        {
            const std::string path = mat + f;
            std::string       why;
            auto              data = acidulous::decodeAudio(path, 48000, why, 30);
            const int64_t     real = data ? int64_t(data->frames) * (data->stereo ? 2 : 1) * 4 : -1;
            const int64_t     est  = core::decoded_bytes(path, 48000, 30, false);
            const bool        ok   = real > 0 && est >= real && est <= real + real / 20 + 64;
            std::printf("%s estimate %lld for %lld bytes decoded, %s\n", ok ? "ok  " : "FAIL", (long long)est,
                        (long long)real, f);
            failures += ok ? 0 : 1;
        }
    }
    std::printf("%s\n", failures ? "FAIL" : "ok");
    return failures ? 1 : 0;
}
