#pragma once

#include <windows.h>

// DLL-wide COM object/lock accounting and module handle, defined in Dll.cpp.
void ModuleAddRef();
void ModuleRelease();

extern HMODULE g_kwietModule;
