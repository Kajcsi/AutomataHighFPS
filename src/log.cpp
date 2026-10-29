#include "common.h"
#include <cstdio>
#include <cstdarg>
#include <share.h>

static FILE* g_file;
static CRITICAL_SECTION g_lock;

void LogOpen(const wchar_t* path) {
    InitializeCriticalSection(&g_lock);
    g_file = _wfsopen(path, L"w", _SH_DENYWR);
}

void Log(const char* fmt, ...) {
    if (!g_file) return;
    EnterCriticalSection(&g_lock);
    SYSTEMTIME t;
    GetLocalTime(&t);
    fprintf(g_file, "[%02d:%02d:%02d.%03d] ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(g_file, fmt, ap);
    va_end(ap);
    fputc('\n', g_file);
    fflush(g_file);
    LeaveCriticalSection(&g_lock);
}
