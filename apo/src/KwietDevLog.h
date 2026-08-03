#pragma once

// Dev-only diagnostics. Compiled in when KWIET_DEV_LOG is defined (CMake
// option). Never call from the RT path (APOProcess/CalcFrames): file I/O.
// Output: OutputDebugString + append to C:\ProgramData\Kwiet\apo-log.txt
// (directory must exist and be writable by LOCAL SERVICE / SYSTEM).

#if defined(KWIET_DEV_LOG)

#include <guiddef.h>

void KwietLogf(const char* fmt, ...);
// Formats a GUID into caller-provided buffer (min 40 chars), returns buffer.
const char* KwietGuidToA(const GUID& guid, char* buffer, unsigned bufferLen);

#define KWIET_LOG(...) KwietLogf(__VA_ARGS__)

#else

#define KWIET_LOG(...) ((void)0)

#endif
