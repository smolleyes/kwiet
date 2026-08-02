# apo/ — shim COM passthrough (jalon 1)

DLL COM usermode chargée par `audiodg.exe`. Au jalon 1 : passthrough strict
input→output, zéro DSP. Les ring buffers SPSC, le worker thread et l'appel au
cdylib Rust (`dsp/`) arrivent au jalon 2, dans `LockForProcess`/`APOProcess`
(voir les commentaires `Milestone 2` dans le code).

## Fichiers

| Fichier | Rôle |
|---|---|
| `src/KwietApo.h/.cpp` | La classe APO : `IAudioProcessingObject`, `IAudioProcessingObjectRT` (chemin temps-réel), `IAudioProcessingObjectConfiguration`, `IAudioSystemEffects2` |
| `src/Dll.cpp` | Class factory, `DllRegisterServer` (CLSID sous `HKLM\Software\Classes`), `DllMain` |
| `src/KwietGuids.h` | CLSID + GUID d'effet + modes de traitement (dupliqué côté installeur — garder synchronisé) |
| `src/KwietApo.def` | Exports COM |

L'enregistrement **endpoint** (FxProperties) n'est pas fait par la DLL :
c'est `installer/install.ps1` qui porte cette politique.

## Build

Prérequis : MSVC (VS 2019+, workload C++), Windows 10/11 SDK, CMake ≥ 3.21.

```powershell
cmake -S . -B build -A x64
cmake --build build --config Release
# → build/Release/KwietApo.dll  (x64, CRT statique /MT)
```

Ne jamais enregistrer la DLL sur le poste de dev — installation en VM
uniquement (`docs/procedure-test-vm.md`).

## Règles du chemin RT

`APOProcess()` : pas d'allocation, pas de lock, pas de syscall, pas de
logging, pas de COM. Fail-open : au moindre doute, copie input→output.
Toute violation est un bug bloquant, quel que soit le gain.
