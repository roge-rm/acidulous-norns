// MP3 for norns: none. The app decodes and encodes MP3 with LAME, which isn't
// built here, so reading or writing one fails with an error.
#include <engine/format/Mp3Reader.h>
#include <engine/format/Mp3Writer.h>

namespace acidulous {

size_t Mp3Reader::audioStart(const unsigned char *, size_t) { return 0; }

std::unique_ptr<SampleData> Mp3Reader::read(const std::string &, int32_t, std::string &error, int32_t) {
    error = "MP3 isn't supported on norns";
    return nullptr;
}

Mp3Writer::~Mp3Writer() = default;

bool Mp3Writer::open(const std::string &, int32_t, int32_t, std::string &error) {
    error = "MP3 isn't supported on norns";
    return false;
}

void Mp3Writer::write(const float *, int32_t) {}

bool Mp3Writer::close() { return false; }

} // namespace acidulous
