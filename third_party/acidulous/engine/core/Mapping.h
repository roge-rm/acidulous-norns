#pragma once
#include <cstdint>
#include <string>
#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

// A file mapped read-only so the audio thread can read it like memory. Used
// for takes too long to hold in RAM, letting the kernel's page cache do the
// streaming instead of a prefetch ring and filler thread.
//
// The first touch of a region can cause a major page fault, so a worker calls
// `willNeed` ahead of a cell that's about to play.
namespace acidulous::audio {

class Mapping {
  public:
    Mapping() = default;
    ~Mapping() { close(); }
    Mapping(const Mapping &) = delete;
    Mapping &operator=(const Mapping &) = delete;

#ifdef _WIN32
    // The Windows version: maps read-only, closes the handles once the view
    // exists, and uses the closest hints Windows has. The path is UTF-8.
    bool open(const std::string &path) {
        close();
        const int wide = MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, nullptr, 0);
        if (wide <= 0) return false;
        std::wstring name(static_cast<size_t>(wide), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, name.data(), wide);
        HANDLE f = CreateFileW(name.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                               FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
        if (f == INVALID_HANDLE_VALUE) return false;
        LARGE_INTEGER size{};
        if (!GetFileSizeEx(f, &size) || size.QuadPart <= 0) {
            CloseHandle(f);
            return false;
        }
        HANDLE m = CreateFileMappingW(f, nullptr, PAGE_READONLY, 0, 0, nullptr);
        CloseHandle(f);
        if (m == nullptr) return false;
        void *p = MapViewOfFile(m, FILE_MAP_READ, 0, 0, 0);
        CloseHandle(m);
        if (p == nullptr) return false;
        base = static_cast<const unsigned char *>(p);
        length = static_cast<size_t>(size.QuadPart);
        return true;
    }

    void close() {
        if (base != nullptr) UnmapViewOfFile(base);
        base = nullptr;
        length = 0;
    }

    void willNeed(size_t offset, size_t bytes) const {
        if (base == nullptr || offset >= length) return;
        WIN32_MEMORY_RANGE_ENTRY range{const_cast<unsigned char *>(base + offset), offset + bytes > length ? length - offset : bytes};
        PrefetchVirtualMemory(GetCurrentProcess(), 1, &range, 0);
    }
#else
    bool open(const std::string &path) {
        close();
        const int f = ::open(path.c_str(), O_RDONLY);
        if (f < 0) return false;
        struct stat st {};
        if (::fstat(f, &st) != 0 || st.st_size <= 0) {
            ::close(f);
            return false;
        }
        void *p = ::mmap(nullptr, static_cast<size_t>(st.st_size), PROT_READ, MAP_PRIVATE, f, 0);
        // The mapping outlives the descriptor, so close it now.
        ::close(f);
        if (p == MAP_FAILED) return false;
        base = static_cast<const unsigned char *>(p);
        length = static_cast<size_t>(st.st_size);
        // A take is read straight through, so pages behind the playhead can
        // be dropped first.
        ::madvise(const_cast<void *>(static_cast<const void *>(base)), length, MADV_SEQUENTIAL);
        return true;
    }

    void close() {
        if (base != nullptr) ::munmap(const_cast<void *>(static_cast<const void *>(base)), length);
        base = nullptr;
        length = 0;
    }

    /**
     * Hints that [bytes] from [offset] should be paged in, without waiting.
     * Called from a worker before a cell plays so the audio thread doesn't
     * take the page fault.
     */
    void willNeed(size_t offset, size_t bytes) const {
        if (base == nullptr || offset >= length) return;
        const size_t n = offset + bytes > length ? length - offset : bytes;
        ::madvise(const_cast<void *>(static_cast<const void *>(base + offset)), n, MADV_WILLNEED);
    }
#endif

    bool valid() const { return base != nullptr; }
    const unsigned char *data() const { return base; }
    size_t size() const { return length; }

  private:
    const unsigned char *base = nullptr;
    size_t length = 0;
};

} // namespace acidulous::audio
