#include "Decoded.h"
#include <cstdio>

namespace acidulous {

bool slurp(const std::string &path, std::vector<unsigned char> &bytes, std::string &error, size_t ceiling) {
    FILE *f = std::fopen(path.c_str(), "rb");
    if (f == nullptr) { error = "cannot open"; return false; }
    bytes.clear();
    unsigned char buf[65536];
    size_t n = 0;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) {
        bytes.insert(bytes.end(), buf, buf + n);
        if (bytes.size() > ceiling) {
            std::fclose(f);
            error = "file too large";
            return false;
        }
    }
    std::fclose(f);
    if (bytes.empty()) { error = "empty file"; return false; }
    return true;
}

bool slurpHead(const std::string &path, std::vector<unsigned char> &bytes, size_t maxBytes, std::string &error) {
    FILE *f = std::fopen(path.c_str(), "rb");
    if (f == nullptr) { error = "cannot open"; return false; }
    bytes.assign(maxBytes, 0);
    const size_t n = std::fread(bytes.data(), 1, maxBytes, f);
    std::fclose(f);
    bytes.resize(n);
    if (bytes.empty()) { error = "empty file"; return false; }
    return true;
}

} // namespace acidulous
