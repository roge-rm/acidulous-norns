// Runs the core offline from a script and writes a 48 kHz stereo WAV.
//
//   acidulous-render [--banks DIR] [--block N] [--tail S] [--16] script.txt out.wav
//
// Each script line is `seconds command args...`, with the commands the OSC
// bridge takes, without the slash:
//   0    mount 0 Reflux
//   0    patch 0 Reflux "Classic Acid"
//   0.0  note 0 1 36 100       part, channel 1-16, note, velocity
//   0.5  note 0 1 36 0         velocity 0 is note off
// `#` starts a comment. Lines run in time order. Rendering goes on --tail
// seconds (default 1) after the last line. --block sets the size of the
// blocks fed to the FIFO, to test sizes other than the engine's 64.
#include "core.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace
{

constexpr int kRate = 48000;

struct Event
{
    double      time;
    std::string path;
    core::Args  args;
    int         line;
};

struct PrintReplier : core::Replier
{
    void reply(const char* path, const core::Args& args) override
    {
        std::printf("  %s", path);
        for(const auto& a : args)
        {
            if(a.type == 's')
                std::printf(" \"%s\"", a.str.c_str());
            else
                std::printf(" %g", a.num);
        }
        std::printf("\n");
    }
};

// Splits on spaces, keeping "quoted strings" whole.
std::vector<std::string> tokens(const std::string& line)
{
    std::vector<std::string> out;
    size_t                   i = 0;
    while(i < line.size())
    {
        while(i < line.size() && std::isspace((unsigned char)line[i]))
            ++i;
        if(i >= line.size() || line[i] == '#')
            break;
        if(line[i] == '"')
        {
            size_t end = line.find('"', i + 1);
            if(end == std::string::npos)
                end = line.size();
            out.push_back(line.substr(i + 1, end - i - 1));
            i = end + 1;
            continue;
        }
        size_t start = i;
        while(i < line.size() && !std::isspace((unsigned char)line[i]))
            ++i;
        out.push_back(line.substr(start, i - start));
    }
    return out;
}

core::Value value(const std::string& t)
{
    char*        end = nullptr;
    const double d   = std::strtod(t.c_str(), &end);
    if(!t.empty() && end && *end == 0)
        return t.find_first_of(".eE") == std::string::npos ? core::Value::i(int(d)) : core::Value::f(d);
    return core::Value::s(t);
}

void put32(std::FILE* f, uint32_t v)
{
    uint8_t b[4] = {uint8_t(v), uint8_t(v >> 8), uint8_t(v >> 16), uint8_t(v >> 24)};
    std::fwrite(b, 1, 4, f);
}
void put16(std::FILE* f, uint16_t v)
{
    uint8_t b[2] = {uint8_t(v), uint8_t(v >> 8)};
    std::fwrite(b, 1, 2, f);
}

bool write_wav(const char* path, const std::vector<float>& lr, bool pcm16)
{
    std::FILE* f = std::fopen(path, "wb");
    if(!f)
        return false;
    const uint32_t bytes = uint32_t(lr.size() * (pcm16 ? 2 : 4));
    const uint16_t bits  = pcm16 ? 16 : 32;
    std::fwrite("RIFF", 1, 4, f);
    put32(f, 36 + bytes);
    std::fwrite("WAVEfmt ", 1, 8, f);
    put32(f, 16);
    put16(f, pcm16 ? 1 : 3);
    put16(f, 2);
    put32(f, kRate);
    put32(f, kRate * 2 * bits / 8);
    put16(f, 2 * bits / 8);
    put16(f, bits);
    std::fwrite("data", 1, 4, f);
    put32(f, bytes);
    if(pcm16)
    {
        for(float s : lr)
        {
            const float c = std::max(-1.f, std::min(1.f, s));
            put16(f, uint16_t(int16_t(std::lround(c * 32767.f))));
        }
    }
    else
        std::fwrite(lr.data(), sizeof(float), lr.size(), f);
    return std::fclose(f) == 0;
}

} // namespace

int main(int argc, char** argv)
{
    std::string banks = ACIDULOUS_DEFAULT_BANKS;
    int         block = 64;
    double      tail  = 1.0;
    bool        pcm16 = false;
    std::vector<const char*> files;
    for(int i = 1; i < argc; ++i)
    {
        if(!std::strcmp(argv[i], "--banks") && i + 1 < argc)
            banks = argv[++i];
        else if(!std::strcmp(argv[i], "--block") && i + 1 < argc)
            block = std::max(1, std::atoi(argv[++i]));
        else if(!std::strcmp(argv[i], "--tail") && i + 1 < argc)
            tail = std::atof(argv[++i]);
        else if(!std::strcmp(argv[i], "--16"))
            pcm16 = true;
        else
            files.push_back(argv[i]);
    }
    if(files.size() != 2)
    {
        std::fprintf(stderr, "usage: acidulous-render [--banks DIR] [--block N] [--tail S] [--16] script out.wav\n");
        return 2;
    }

    std::ifstream in(files[0]);
    if(!in)
    {
        std::fprintf(stderr, "cannot open %s\n", files[0]);
        return 1;
    }
    std::vector<Event> events;
    std::string        raw;
    int                line_no = 0;
    while(std::getline(in, raw))
    {
        ++line_no;
        auto t = tokens(raw);
        if(t.empty())
            continue;
        if(t.size() < 2)
        {
            std::fprintf(stderr, "%s:%d: wants a time and a command\n", files[0], line_no);
            return 1;
        }
        Event e{std::atof(t[0].c_str()), "/" + t[1], {}, line_no};
        for(size_t k = 2; k < t.size(); ++k)
            e.args.push_back(value(t[k]));
        events.push_back(std::move(e));
    }
    std::stable_sort(events.begin(), events.end(),
                     [](const Event& a, const Event& b) { return a.time < b.time; });

    PrintReplier replier;
    core::Core   c(banks, &replier, true);

    const double  end_s  = (events.empty() ? 0.0 : events.back().time) + tail;
    const int64_t total  = int64_t(end_s * kRate);
    std::vector<float> lr(size_t(total) * 2);
    std::vector<float> l(static_cast<size_t>(block)), r(static_cast<size_t>(block));

    size_t  next   = 0;
    int64_t frame  = 0;
    auto    start  = std::chrono::steady_clock::now();
    float   load_max = 0;
    while(frame < total)
    {
        while(next < events.size() && int64_t(events[next].time * kRate) <= frame)
        {
            const Event& e = events[next++];
            if(!c.command(e.path, e.args))
                std::fprintf(stderr, "%s:%d: unknown command or missing arguments: %s\n", files[0], e.line,
                             e.path.c_str());
        }
        int64_t until = total;
        if(next < events.size())
            until = std::min(until, int64_t(events[next].time * kRate));
        int n = int(std::min<int64_t>(block, std::max<int64_t>(1, until - frame)));
        n     = int(std::min<int64_t>(n, total - frame));
        c.process(nullptr, nullptr, l.data(), r.data(), n);
        for(int i = 0; i < n; ++i)
        {
            lr[size_t(frame + i) * 2]     = l[size_t(i)];
            lr[size_t(frame + i) * 2 + 1] = r[size_t(i)];
        }
        frame += n;
        load_max = std::max(load_max, c.load_percent());
    }
    const double took = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();

    double peak = 0, sum = 0;
    bool   finite = true;
    for(float s : lr)
    {
        if(!std::isfinite(s))
            finite = false;
        peak = std::max(peak, double(std::fabs(s)));
        sum += double(s) * s;
    }
    const double rms = lr.empty() ? 0 : std::sqrt(sum / double(lr.size()));
    const auto   db  = [](double v) { return v > 0 ? 20 * std::log10(v) : -999.0; };
    std::printf("%s: %.2f s, peak %.3f (%.1f dB), rms %.4f (%.1f dB)%s\n", files[1], end_s, peak, db(peak), rms,
                db(rms), finite ? "" : ", NOT FINITE");
    std::printf("load: engine %.1f%% (max %.1f%%), worst block %.1f%%, whole render %.1f%% of real time\n",
                c.load_percent(), load_max, c.worst_percent(), 100.0 * took / end_s);

    for(int p = 0; p < 8; ++p)
    {
        std::string names = c.macro_names(p);
        if(names.empty())
            continue;
        for(char& ch : names)
            if(ch == '\n')
                ch = '|';
        std::printf("macro names, part %d: %s\n", p, names.c_str());
    }

    if(!write_wav(files[1], lr, pcm16))
    {
        std::fprintf(stderr, "cannot write %s\n", files[1]);
        return 1;
    }
    return finite ? 0 : 1;
}
