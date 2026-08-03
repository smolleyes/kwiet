cmake_minimum_required(VERSION 3.21)
project(kwiet_apo VERSION 0.1.0 LANGUAGES CXX)

if(NOT WIN32)
    message(FATAL_ERROR "kwiet_apo is a Windows APO and only builds on Windows.")
endif()
if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "x64 only (audiodg.exe is 64-bit). Configure with: cmake -A x64")
endif()

# Dev diagnostics (file log + OutputDebugString in non-RT entry points).
# Never enable in distributed builds.
option(KWIET_DEV_LOG "Enable Kwiet dev diagnostics logging" OFF)

add_library(kwiet_apo SHARED
    src/ControlShm.cpp
    src/ControlShm.h
    src/Dll.cpp
    src/DspHost.cpp
    src/DspHost.h
    src/KwietControl.h
    src/KwietApo.cpp
    src/KwietApo.def
    src/KwietApo.h
    src/KwietDevLog.cpp
    src/KwietDevLog.h
    src/KwietGuids.h
    src/KwietSe3.h
    src/Module.h
    src/SpscRing.h
)

# Stable C ABI shared with the Rust cdylib (dsp/).
target_include_directories(kwiet_apo PRIVATE ../dsp/include)

if(KWIET_DEV_LOG)
    target_compile_definitions(kwiet_apo PRIVATE KWIET_DEV_LOG)
endif()

set_target_properties(kwiet_apo PROPERTIES
    OUTPUT_NAME "KwietApo"
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
    # Static CRT: the DLL loads into audiodg.exe and must not depend on a
    # VC++ redistributable being present system-wide.
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
)

target_compile_definitions(kwiet_apo PRIVATE
    UNICODE
    _UNICODE
    NOMINMAX
    WIN32_LEAN_AND_MEAN
)

if(MSVC)
    target_compile_options(kwiet_apo PRIVATE /W4 /permissive- /EHsc /guard:cf)
    target_link_options(kwiet_apo PRIVATE /guard:cf)
endif()

target_link_libraries(kwiet_apo PRIVATE
    ole32       # CoTaskMemAlloc, StringFromGUID2
    advapi32    # registry (self-registration)
)
