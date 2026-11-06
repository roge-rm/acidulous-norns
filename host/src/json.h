#pragma once
// A small JSON reader, enough for song.json and a recorded voice's bank.json.
#include <cstdint>
#include <cstdlib>
#include <string>
#include <utility>
#include <vector>

namespace core
{

struct Json
{
    enum Kind { Null, Bool, Number, String, Array, Object } kind = Null;
    double                                   num = 0;
    std::string                              str;
    std::vector<Json>                        items;
    std::vector<std::pair<std::string, Json>> fields;

    const Json* get(const char* key) const
    {
        for(const auto& kv : fields)
            if(kv.first == key)
                return &kv.second;
        return nullptr;
    }
    std::string text(const char* key) const
    {
        const Json* j = get(key);
        return j && j->kind == String ? j->str : std::string();
    }
};

struct JsonReader
{
    const std::string& s;
    size_t             i = 0;
    int                depth = 0;
    bool               ok = true;

    void space()
    {
        while(i < s.size() && (s[i] == ' ' || s[i] == '\n' || s[i] == '\r' || s[i] == '\t'))
            ++i;
    }
    void put_utf8(std::string& out, uint32_t c)
    {
        if(c < 0x80)
            out += char(c);
        else if(c < 0x800)
            out += char(0xc0 | (c >> 6)), out += char(0x80 | (c & 0x3f));
        else if(c < 0x10000)
            out += char(0xe0 | (c >> 12)), out += char(0x80 | ((c >> 6) & 0x3f)), out += char(0x80 | (c & 0x3f));
        else
            out += char(0xf0 | (c >> 18)), out += char(0x80 | ((c >> 12) & 0x3f)),
                out += char(0x80 | ((c >> 6) & 0x3f)), out += char(0x80 | (c & 0x3f));
    }
    bool hex4(uint32_t& c)
    {
        if(i + 4 > s.size())
            return false;
        c = 0;
        for(int k = 0; k < 4; ++k)
        {
            const char h = s[i++];
            c <<= 4;
            if(h >= '0' && h <= '9')
                c |= uint32_t(h - '0');
            else if(h >= 'a' && h <= 'f')
                c |= uint32_t(h - 'a' + 10);
            else if(h >= 'A' && h <= 'F')
                c |= uint32_t(h - 'A' + 10);
            else
                return false;
        }
        return true;
    }
    bool string(std::string& out)
    {
        if(i >= s.size() || s[i] != '"')
            return false;
        ++i;
        while(i < s.size() && s[i] != '"')
        {
            char c = s[i++];
            if(c != '\\')
            {
                out += c;
                continue;
            }
            if(i >= s.size())
                return false;
            c = s[i++];
            switch(c)
            {
            case 'n': out += '\n'; break;
            case 't': out += '\t'; break;
            case 'r': out += '\r'; break;
            case 'b': out += '\b'; break;
            case 'f': out += '\f'; break;
            case 'u':
            {
                uint32_t u = 0;
                if(!hex4(u))
                    return false;
                if(u >= 0xd800 && u < 0xdc00 && i + 1 < s.size() && s[i] == '\\' && s[i + 1] == 'u')
                {
                    i += 2;
                    uint32_t lo = 0;
                    if(!hex4(lo))
                        return false;
                    u = 0x10000 + ((u - 0xd800) << 10) + (lo - 0xdc00);
                }
                put_utf8(out, u);
                break;
            }
            default: out += c;
            }
        }
        if(i >= s.size())
            return false;
        ++i;
        return true;
    }
    Json value()
    {
        Json v;
        space();
        if(!ok || i >= s.size() || ++depth > 64)
        {
            ok = false;
            return v;
        }
        const char c = s[i];
        if(c == '{')
        {
            v.kind = Json::Object;
            ++i;
            space();
            if(i < s.size() && s[i] == '}')
                ++i;
            else
                for(;;)
                {
                    space();
                    std::string key;
                    if(!string(key))
                    {
                        ok = false;
                        break;
                    }
                    space();
                    if(i >= s.size() || s[i] != ':')
                    {
                        ok = false;
                        break;
                    }
                    ++i;
                    v.fields.emplace_back(std::move(key), value());
                    space();
                    if(i < s.size() && s[i] == ',')
                    {
                        ++i;
                        continue;
                    }
                    if(i < s.size() && s[i] == '}')
                        ++i;
                    else
                        ok = false;
                    break;
                }
        }
        else if(c == '[')
        {
            v.kind = Json::Array;
            ++i;
            space();
            if(i < s.size() && s[i] == ']')
                ++i;
            else
                for(;;)
                {
                    v.items.push_back(value());
                    space();
                    if(i < s.size() && s[i] == ',')
                    {
                        ++i;
                        continue;
                    }
                    if(i < s.size() && s[i] == ']')
                        ++i;
                    else
                        ok = false;
                    break;
                }
        }
        else if(c == '"')
        {
            v.kind = Json::String;
            ok     = string(v.str) && ok;
        }
        else if(s.compare(i, 4, "true") == 0)
            v.kind = Json::Bool, v.num = 1, i += 4;
        else if(s.compare(i, 5, "false") == 0)
            v.kind = Json::Bool, i += 5;
        else if(s.compare(i, 4, "null") == 0)
            i += 4;
        else
        {
            char*       end   = nullptr;
            const char* start = s.c_str() + i;
            v.kind            = Json::Number;
            v.num             = std::strtod(start, &end);
            if(end == start)
                ok = false;
            i += size_t(end - start);
        }
        --depth;
        return v;
    }
};

} // namespace core
