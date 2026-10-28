#pragma once
#include <cstdint>
#include <engine/core/Frozen.h>
#include <engine/core/Reel.h>
#include <engine/core/Sample.h>
#include <engine/core/SampleMap.h>
#include <engine/core/Take.h>
#include <engine/core/Utterance.h>
#include <unordered_set>
#include <vector>

// The memory a mounted object's audio holds, in bytes: the capacity of its
// sample vectors and their analysis (onsets, pitch marks). Names and the
// objects' own fixed fields are left out. For EngineHost::bytesHeld, read on a
// non-audio thread under Engine::readLive; allocates for the shared lookups.
namespace acidulous::held {

template <class T> inline int64_t bytes(const std::vector<T> &v) {
    return static_cast<int64_t>(v.capacity() * sizeof(T));
}

inline int64_t bytes(const SampleData *s) {
    return s == nullptr ? 0 : bytes(s->left) + bytes(s->right);
}

inline int64_t bytes(const audio::Take *t) {
    return t == nullptr ? 0 : bytes(t->left) + bytes(t->right) + bytes(t->onsets);
}

inline int64_t bytes(const audio::Utterance &u) { return bytes(u.mono) + bytes(u.epochs); }
inline int64_t bytes(const audio::Utterance *u) { return u == nullptr ? 0 : bytes(*u); }

inline int64_t bytes(const SampleMap *m) {
    if (m == nullptr) return 0;
    int64_t n = 0;
    for (const SampleData &s : m->samples) n += bytes(&s);
    return n;
}

/** Resident sources only, each once however many regions share it. A mapped source lives in the file. */
inline int64_t bytes(const audio::Reel *r) {
    if (r == nullptr) return 0;
    std::unordered_set<const audio::Reel::Source *> seen;
    int64_t n = 0;
    for (const audio::Reel::Cell &c : r->cells) {
        for (const audio::Reel::Region &g : c.lanes) {
            const audio::Reel::Source *s = g.source.get();
            if (s != nullptr && seen.insert(s).second) n += bytes(s->own);
        }
    }
    return n;
}

/** Each clip once, though a set can share one with the set before it. */
inline int64_t bytes(const FrozenSet *f) {
    if (f == nullptr) return 0;
    std::unordered_set<const FrozenClip *> seen;
    int64_t n = 0;
    for (const FrozenSet::Entry &e : f->entries) {
        const FrozenClip *c = e.clip.get();
        if (c != nullptr && seen.insert(c).second) n += bytes(c->left) + bytes(c->right);
    }
    return n;
}

} // namespace acidulous::held
