// Ableton Link for the browser build, always off. Link finds peers by
// multicast, which a web page can't do, so there's never a session and the
// engine keeps its own time. Same class as link/LinkTimebase.h so EngineHost
// builds unchanged.

#include <link/LinkTimebase.h>

namespace acidulous {

struct LinkTimebase::Impl {};

LinkTimebase::LinkTimebase() : impl(new Impl()) {}
LinkTimebase::~LinkTimebase() = default;

void LinkTimebase::setEnabled(bool) {}
bool LinkTimebase::enabled() const { return false; }
int32_t LinkTimebase::peers() const { return 0; }
double LinkTimebase::sessionTempo() const { return 0.0; }
void LinkTimebase::setQuantum(double) {}
void LinkTimebase::setAnchor(int64_t, int64_t, int32_t) {}
void LinkTimebase::setFallbackLatency(int64_t) {}
void LinkTimebase::tempoFromApp(double) {}
Timebase::State LinkTimebase::capture(int64_t) { return {}; }
void LinkTimebase::proposeTempo(double) {}
void LinkTimebase::proposePlaying(bool) {}
void LinkTimebase::setBlockFrames(int32_t, int32_t) {}

} // namespace acidulous
