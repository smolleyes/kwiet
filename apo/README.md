# apo/ — the COM shim loaded by audiodg

A user-mode COM DLL that Windows' audio engine loads into `audiodg.exe`. It owns
the real-time path, the lock-free rings, the worker thread that calls the Rust
DSP, and the shared-memory block the panel reads.

## Files

| File | Role |
|---|---|
| `src/KwietApo.h/.cpp` | The APO class: `IAudioProcessingObject`, `IAudioProcessingObjectRT` (real-time path), `IAudioProcessingObjectConfiguration`, `IAudioSystemEffects3`, notifications |
| `src/DspHost.h/.cpp` | Loads `kwiet_dsp.dll` by absolute path, allocates the rings, primes latency, runs the worker |
| `src/SpscRing.h` | Lock-free single-producer/single-consumer float ring |
| `src/ChannelMix.h` | Channel conversion, allocation-free |
| `src/ControlShm.h/.cpp` | Creates the named section the panel maps |
| `src/KwietControl.h` | **The contract with the panel.** Any change here must be mirrored in `ui/src-tauri/src/control.rs` |
| `src/KwietGuids.h` | CLSID, effect GUID, processing modes — duplicated in the installer, keep in sync |
| `src/Dll.cpp` | Class factory, `DllRegisterServer`, `DllMain` |

Endpoint registration is not done by the DLL: the effect pack under
[`../installer/effectpack/`](../installer/effectpack/) carries that policy.

## Build

Requirements: MSVC (VS 2019+, C++ workload), Windows 10/11 SDK, CMake ≥ 3.21.

```powershell
cmake -S . -B build -A x64
cmake --build build --config Release
# -> build/Release/KwietApo.dll  (x64, static CRT /MT)
```

`-DKWIET_DEV_LOG=ON` adds a file log and a registry override for the attenuation.
Useful while developing, never in a build you hand to anyone: it makes the APO
obey the registry rather than the panel, silently. `build-package.ps1` refuses to
package such a DLL for a release.

## Real-time path rules

`APOProcess()` runs on audiodg's real-time thread: no allocation, no lock, no
system call, no logging, no COM. Fail-open — at the slightest doubt, copy input
to output. A violation is a blocking bug whatever it buys, because an APO that
stalls or crashes `audiodg` takes the whole machine's sound with it.
