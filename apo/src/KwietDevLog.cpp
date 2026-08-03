#if defined(KWIET_DEV_LOG)

#include "KwietDevLog.h"

#include <windows.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>

void KwietLogf(const char* fmt, ...)
{
    char msg[512];
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf_s(msg, _TRUNCATE, fmt, ap);
    va_end(ap);

    char exe[MAX_PATH] = {};
    GetModuleFileNameA(nullptr, exe, MAX_PATH);
    const char* base = strrchr(exe, '\\');
    base = (base != nullptr) ? base + 1 : exe;

    SYSTEMTIME st;
    GetLocalTime(&st);

    char line[768];
    const int n = _snprintf_s(line, _TRUNCATE, "%02u:%02u:%02u.%03u pid=%lu %s | %s\r\n",
                              st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
                              GetCurrentProcessId(), base, msg);
    if (n <= 0) {
        return;
    }

    OutputDebugStringA(line);

    HANDLE h = CreateFileA("C:\\ProgramData\\Kwiet\\apo-log.txt", FILE_APPEND_DATA,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h != INVALID_HANDLE_VALUE) {
        DWORD written = 0;
        WriteFile(h, line, static_cast<DWORD>(strlen(line)), &written, nullptr);
        CloseHandle(h);
    }
}

const char* KwietGuidToA(const GUID& g, char* buffer, unsigned bufferLen)
{
    _snprintf_s(buffer, bufferLen, _TRUNCATE,
                "{%08lX-%04hX-%04hX-%02hhX%02hhX-%02hhX%02hhX%02hhX%02hhX%02hhX%02hhX}",
                g.Data1, g.Data2, g.Data3,
                g.Data4[0], g.Data4[1], g.Data4[2], g.Data4[3],
                g.Data4[4], g.Data4[5], g.Data4[6], g.Data4[7]);
    return buffer;
}

#endif // KWIET_DEV_LOG
