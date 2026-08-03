# Kwiet

Suppression de bruit micro **système-wide** pour Windows 11, open source.

L'utilisateur active un toggle : son micro réel est nettoyé par IA
([DeepFilterNet3](https://github.com/Rikorose/DeepFilterNet)) pour **toutes** les
applications (Google Meet, Discord, …) — sans micro virtuel, sans sélection de
périphérique.

> **Statut : jalon 1 — APO passthrough chargé et actif** dans `audiodg.exe`
> (Windows 11 build 26200, micro USB). Reste le soak 48 h et la matrice de
> robustesse. Aucun DSP encore.
>
> L'installation se fait via un **pack d'effets** (INF composant + extension,
> cf. [`installer/effectpack/`](installer/effectpack/)) — l'édition manuelle
> des `FxProperties` par endpoint ne fonctionne plus sur Windows 11 récent.

## Architecture

**Approche : APO (Audio Processing Object), pas de driver kernel.**
Un APO est une DLL COM usermode chargée par `audiodg.exe`, enregistrée sur un
endpoint de capture via le registre (même mécanisme qu'Equalizer APO). Zéro
signature de driver requise — seulement du code signing usermode (SignPath plus
tard).

**Contrainte centrale : le DSP ne tourne PAS dans le thread temps-réel.**
`APOProcess()` s'exécute dans le thread RT d'audiodg, période 10 ms
(480 frames @ 48 kHz float32 mono/stéréo). Interdictions absolues dans ce
chemin : allocation, locks, syscalls, logging, page faults, appels COM.
DeepFilterNet3 (~2M params, lookahead 2 frames) ne peut pas y tourner.

```
APOProcess (thread RT, audiodg)
  → push input dans ring buffer SPSC lock-free (préalloué à Initialize())
  → pop output traité (retard fixe ~40-60 ms)
  → si underrun/worker mort : PASSTHROUGH input→output, jamais de silence, jamais de blocage

Worker thread (priorité normale, dans le processus audiodg via la DLL)
  → consomme le ring input, fenêtres 20 ms / hop 10 ms
  → appelle le cdylib Rust (DeepFilterNet3)
  → repousse dans le ring output
```

Le **fail-open est structurel** : worker mort ⇒ passthrough automatique. Un APO
qui crashe audiodg = plus aucun son sur la machine ⇒ la robustesse du shim prime
sur tout.

**Contrôle UI ↔ APO** : shared memory nommée + atomics uniquement (enable/bypass,
agressivité, VU-mètres avant/après). L'UI ne communique jamais directement avec
le thread RT.

Détails et décisions : [`docs/architecture.md`](docs/architecture.md).

## Structure du repo

| Dossier | Rôle |
|---|---|
| [`apo/`](apo/) | C++ : shim COM (`IAudioProcessingObject[RT|Configuration]`, `IAudioSystemEffects2`), ring buffers, worker thread, shmem |
| [`dsp/`](dsp/) | Rust cdylib : wrapper DeepFilterNet3 (crate `df`), ABI C stable — **jalon 2** |
| [`ui/`](ui/) | Tauri v2 : tray, toggle, slider agressivité, VU-mètres — **jalon 4** |
| [`installer/`](installer/) | Scripts d'install/désinstall (registre endpoint FX + backup .reg), puis WiX |
| [`bench/`](bench/) | Latence, CPU, enregistrements A/B, test AGC Chrome/WebRTC — **jalon 3** |
| [`docs/`](docs/) | Décisions d'archi, procédure de test VM |

## Jalons (ordre strict)

1. **APO passthrough stable** — copie input→output, enregistré sur un endpoint
   de capture, survit 48 h : changements de sample rate, débranchement à chaud,
   veille/reprise, plusieurs applis simultanées. Install/uninstall scriptés et
   testés en VM.
2. **Intégration DSP** — ring buffers + worker + cdylib Rust (d'abord un gain
   -6 dB pour valider la plomberie, puis DFN3).
3. **Bench Chrome** — mesurer ce que l'AGC WebRTC fait du bruit résiduel,
   calibrer l'atténuation pour ne pas le déclencher.
4. **UI Tauri + installeur WiX.**

## Build (apo/)

Prérequis : Visual Studio 2019+ (workload C++, Windows 10/11 SDK), CMake ≥ 3.21.

```powershell
cmake -S apo -B apo/build -A x64
cmake --build apo/build --config Release
# → apo/build/Release/KwietApo.dll
```

## Installation — VM UNIQUEMENT

> ⚠️ **Un APO buggé = machine sans son.** Ne jamais installer sur un poste de
> travail. Cible : VM Windows 11 ou machine dédiée, avec checkpoint + export
> .reg avant chaque install. Procédure complète :
> [`docs/procedure-test-vm.md`](docs/procedure-test-vm.md).

```powershell
cd installer
.\install.ps1      # backup .reg automatique + sélection interactive de l'endpoint
.\status.ps1       # vérifie l'état de l'installation
.\uninstall.ps1    # restauration propre
```

Flux shared mode uniquement : les flux exclusive/raw bypassent l'APO (accepté).

## Licence

[Apache-2.0](LICENSE). DeepFilterNet3 est dual MIT/Apache-2.0, compatible.
