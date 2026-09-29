#include "hyperlink/common.h"

#include <cstdarg>
#include <cstdio>
#include <mutex>

namespace hl {

static std::mutex gLogMutex;
static LogSink gSink;

void setLogSink(LogSink sink) {
    std::lock_guard<std::mutex> lock(gLogMutex);
    gSink = std::move(sink);
}

void log(const char* fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    std::lock_guard<std::mutex> lock(gLogMutex);
    if (gSink) gSink(buf);
    else fprintf(stderr, "%s\n", buf);
}

}  // namespace hl
