#pragma once

#include <QtGlobal>

#include <cstdarg>
#include <cstdio>

namespace ClipboardLog {

inline bool debugEnabled()
{
    return qEnvironmentVariableIntValue("MOONLIGHT_CLIPBOARD_HELPER_DEBUG") != 0;
}

inline void write(const char* level, const char* format, va_list args)
{
    if (level[0] == 'D' && !debugEnabled()) {
        return;
    }

    fprintf(stderr, "ClipboardSync %s: ", level);
    vfprintf(stderr, format, args);
    fputc('\n', stderr);
    fflush(stderr);
}

inline void debug(const char* format, ...)
{
    va_list args;
    va_start(args, format);
    write("DEBUG", format, args);
    va_end(args);
}

inline void info(const char* format, ...)
{
    va_list args;
    va_start(args, format);
    write("INFO", format, args);
    va_end(args);
}

inline void warn(const char* format, ...)
{
    va_list args;
    va_start(args, format);
    write("WARN", format, args);
    va_end(args);
}

}
