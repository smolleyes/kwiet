<div align="center">

<img src="assets/svg/lockup.svg" alt="Kwiet" width="360">

**AI microphone cleanup, in every app you already use.**
No virtual microphone. No device to switch. One toggle.

[![CI](https://github.com/smolleyes/kwiet/actions/workflows/ci.yml/badge.svg)](https://github.com/smolleyes/kwiet/actions/workflows/ci.yml)
[![Licence](https://img.shields.io/badge/licence-Apache--2.0-9FD3C0)](LICENSE)
[![Windows 11](https://img.shields.io/badge/Windows-11-0D1417)](#requirements)

[Français](README.md) · [Architecture](docs/architecture.md) *(French)*

</div>

---

## The problem

Noise-suppression tools install a **virtual microphone**. Which means selecting
it in Teams, in Discord, in Meet, in every app — and doing it again every time
an update resets the default device. When it goes wrong, the app picks up the
wrong microphone and nobody can hear you.

Kwiet creates no device. It attaches to **your** microphone, inside the Windows
audio engine. Apps see nothing unusual: they open the microphone they have
always opened, and what comes out of it is already clean.

## How it works

Kwiet is an **APO** (Audio Processing Object): a user-mode COM DLL loaded by
`audiodg.exe`, the Windows audio engine. No kernel driver.

Denoising is done by [DeepFilterNet3](https://github.com/Rikorose/DeepFilterNet),
a ~2 M parameter network — far too heavy for `audiodg`'s real-time thread, which
must return a block every 10 ms without ever allocating, taking a lock, or
making a system call.

```
APOProcess()  — audiodg's real-time thread ——————————————————————————
   |  pushes input into a lock-free SPSC ring (preallocated)
   |  pops processed output (fixed 30 ms delay)
   +— if the worker is dead or late: PASSTHROUGH immediately
                                     never silence, never a stall
Worker  — normal-priority thread —————————————————————————————————————
   +— drains the ring, calls the Rust cdylib (DeepFilterNet3)
```

**Fail-open is structural**: an APO that crashes `audiodg` leaves the whole
machine without sound. Every design decision follows from that. If the DSP
disappears, audio passes through untouched — that is the worst case.

Details and decisions: [`docs/architecture.md`](docs/architecture.md) *(French)*.

## Measurements

Windows 11 build 26200, USB microphone, 48 kHz stereo:

| | |
|---|---|
| Noise suppression | **≥ 24 dB** on a controlled source |
| Added latency | **30 ms**, fixed |
| CPU | **~2 %** of one core (RTF 0.017–0.020) |
| Dropouts | 0 |

## Installing

> [!IMPORTANT]
> **One step after installing is not ours to take.** Windows 11 requires you to
> choose the effect pack yourself: **Settings → Sound → your microphone →
> Audio enhancements → Kwiet**. Until you do, Kwiet is installed and entirely
> inert. The panel will tell you so, with a button to open the right page.

1. Download the MSI for your language — `Kwiet_x.y.z_x64_en-US.msi` or
   `Kwiet_x.y.z_x64_fr-FR.msi` — from
   [releases](https://github.com/smolleyes/kwiet/releases).
2. Run it. It asks for elevation: the effect pack is a driver package and
   registers with Windows through `pnputil`. Sound cuts out briefly.
3. Choose Kwiet in your microphone's audio enhancements (see above).
4. The tray icon opens the panel.

Uninstalling removes the pack from the driver store along with the app.

### Requirements

- Windows 11 **24H2 or newer** (x64). The effect-pack mechanism used here does
  not exist on earlier versions.
- A microphone that negotiates **48 kHz**. Outside that, Kwiet deliberately
  falls back to passthrough rather than resampling blind.

> [!WARNING]
> **Signing.** Released binaries are signed with a development certificate. That
> is enough to build and test here, where the certificate sits in the machine's
> trust stores, but **not** on anyone else's machine: Windows refuses a driver
> package whose catalogue does not chain to a trusted authority.
>
> What it takes to fix: an ordinary **commercial code-signing certificate**.
> Microsoft's
> [PnP device installation signing requirements](https://learn.microsoft.com/en-us/windows-hardware/drivers/install/pnp-device-installation-signing-requirements--windows-vista-and-later-)
> ask that the catalogue be signed "by WHQL **or** by a third-party release
> certificate". The Dev Portal requirement is scoped to **kernel-mode** drivers,
> and Kwiet contains none — it is a user-mode COM DLL.
>
> Unverified all the same: no install has been attempted with a commercial
> certificate on a clean machine. And Windows in S mode requires WHQL regardless.

## The panel

One idea, repeated everywhere: **celadon is what your apps receive, amber is
what Kwiet took away.** The gap between them is the product.

- **Scope** — the last few seconds, as two stacked envelopes.
- **Meter** — the present instant, as one bar: the bright fill is the signal
  going out, the amber beyond it is the noise removed.
- **Strength** — from gentle to maximum. Higher means total silence between
  words, at the risk of clipping their attacks.
- **Toggle** — immediate bypass, without dropping the stream.

The panel speaks **French or English**, following Windows, and switches by hand
at the bottom right.

## Building from source

Requirements: Visual Studio 2019+ (C++ workload, Windows 10/11 SDK),
CMake ≥ 3.21, stable Rust, Node 20+.

```powershell
# 1. The APO (C++)
cmake -S apo -B apo/build -A x64
cmake --build apo/build --config Release

# 2. The DSP (Rust, embeds the DeepFilterNet3 model)
cd dsp ; cargo build --release ; cd ..

# 3. The effect pack, signed and timestamped
.\installer\effectpack\build-package.ps1 -Version 0.2.2 `
    -CertPath my-certificate.pfx -CertPassword $env:PFX_PW

# 4. The app and the installer
cd ui ; npm ci ; npm run tauri build
# -> ui/src-tauri/target/release/bundle/msi/Kwiet_0.2.2_x64_en-US.msi
#    ui/src-tauri/target/release/bundle/msi/Kwiet_0.2.2_x64_fr-FR.msi
```

The format is **MSI**, not NSIS: NSIS stubs draw antivirus false positives far
more often than a Windows Installer package does. The effect pack is placed by a
deferred custom action, defined in
[`ui/src-tauri/wix/effectpack.wxs`](ui/src-tauri/wix/effectpack.wxs).

> [!CAUTION]
> **Do not install an in-development APO on your working machine.** An APO that
> crashes leaves the machine without sound at boot. Use a VM or a dedicated
> machine: [`docs/procedure-test-vm.md`](docs/procedure-test-vm.md) *(French)*.

Icons and logo are generated: `node assets/build-assets.mjs`.

## Repository layout

| Directory | Contents |
|---|---|
| [`apo/`](apo/) | C++: the COM shim, SPSC rings, worker thread, shared memory |
| [`dsp/`](dsp/) | Rust cdylib: DeepFilterNet3 behind a stable C ABI |
| [`ui/`](ui/) | Tauri v2: panel, tray, NSIS installer |
| [`installer/`](installer/) | Effect pack (INF, catalogue) and install scripts |
| [`assets/`](assets/) | Visual identity, generated by script |
| [`bench/`](bench/) | Measurement tools |
| [`docs/`](docs/) | Architecture decisions, test procedure |

## What is not done

Stated here rather than left to be discovered in use:

- **Release signing** — see the warning above. This is what blocks distribution
  beyond the development machine.
- **Resampling** — outside 48 kHz, Kwiet falls back to passthrough.
- **48 h soak** — sample-rate changes, hot-plug, sleep and resume, several apps
  at once: never held over a long run.
- **Echo cancellation** — deliberately absent. Claiming it would switch off
  Chrome's own canceller, which is far better than anything we would ship.
- **Reported latency** (30 ms) counts the ring only, not DeepFilterNet3's own
  lookahead.

## Licence

[Apache-2.0](LICENSE).

DeepFilterNet3 is dual MIT/Apache-2.0, compatible. The embedded model comes from
the [DeepFilterNet](https://github.com/Rikorose/DeepFilterNet) project by
Hendrik Schröter.
