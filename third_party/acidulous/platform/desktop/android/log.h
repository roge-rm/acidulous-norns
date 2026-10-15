#pragma once
// Android's log call for the desktop build. The engine logs through
// __android_log_print and here it goes to stderr. Only on the include path
// for the desktop build (see desktop/native/CMakeLists.txt).
#include <cstdarg>
#include <cstdio>

enum { ANDROID_LOG_DEBUG = 3, ANDROID_LOG_INFO = 4, ANDROID_LOG_WARN = 5, ANDROID_LOG_ERROR = 6 };

inline int __android_log_print(int priority, const char *tag, const char *fmt, ...) {
    static const char kLevels[] = "??VDIWEF";
    std::fprintf(stderr, "%c/%s: ", priority >= 0 && priority < 8 ? kLevels[priority] : '?', tag);
    va_list args;
    va_start(args, fmt);
    std::vfprintf(stderr, fmt, args);
    va_end(args);
    std::fputc('\n', stderr);
    return 0;
}
