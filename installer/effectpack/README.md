# installer/effectpack/ — the Kwiet effect pack

This replaces per-endpoint `FxProperties` editing, which **no longer works** on
recent Windows 11 builds: the APO is instantiated and then dropped before
`LockForProcess`. Mechanism and evidence in
[`../../docs/architecture.md`](../../docs/architecture.md) §7bis and §8.

The recipe follows Microsoft's own **Voice Clarity** pack
(`C:\Windows\INF\oem155.inf`) and [Aec3APO](https://github.com/msdx321/Aec3APO).

| File | Role |
|---|---|
| `kwiet_extension.inf` | `Class=Extension` on `COMPUTER\Generic` → `AddComponent` creates the `SWC\VEN_KWIET&AUDIO_EFFECTPACK_KWIET` devnode |
| `kwiet_component.inf` | `Class=AudioProcessingObject`: copies the DLLs into the driver store, writes the CLSID, the APO catalogue and `EffectPackRegistration` (capture/microphone targeting) |
| `new-signing-cert.ps1` | Creates the self-signed signing key. Run once |
| `build-package.ps1` | Assembles `package/`: DLLs, INFs, signed and timestamped catalogue, public certificate |
| `pack.ps1` | Installs or removes the pack. Called by the MSI, usable by hand to repair an installation |
| `sign-install-dev.ps1` | Older development path: signs and installs in one step |
| `uninstall-effectpack.ps1` | `pnputil /delete-driver /uninstall`, devnode removal, `-RemoveCert` |

`package/` and `state/` are generated locally and ignored by git. **`state/`
holds the private signing key — it must never be committed.**

## Usage

```powershell
# Once: create the signing key
.\new-signing-cert.ps1 -Password (Read-Host -AsSecureString)

# Each build
cmake --build ..\..\apo\build --config Release
.\build-package.ps1 -Version 0.2.3 -CertPath state\kwiet-signing.pfx -CertPassword ...

# Install by hand (the MSI does this for you)
.\pack.ps1 -Action install -PackageDir package
Get-PnpDevice -Class AudioProcessingObject
.\pack.ps1 -Action uninstall
```

## Signing

Windows only registers a driver package whose catalogue chains to a trusted root.
Kwiet is signed with its own self-signed key, so `pack.ps1` adds the **public
certificate** to the machine's `Root` and `TrustedPublisher` stores before
calling `pnputil`, and removes it again on uninstall. The private key never ships:
it signs at build time.

The certificate carries the Code Signing usage and nothing else, so it cannot
validate TLS chains. It remains a real grant of trust all the same. A commercial
code-signing certificate — which chains to a root already present everywhere and
needs nothing added to anyone's stores — is the cleaner answer.

Attestation signing through the Partner Center is *not* required here: that
governs the loading of **kernel-mode** binaries, and Kwiet contains none.

## The step no installer can take for you

Installing the pack does not enable it. It becomes an **option** to pick by hand in

`Settings > System > Sound > [the microphone] > Audio enhancements`

where "Kwiet" appears next to "Voice Clarity" and the manufacturer's effects.
Only one pack can be active per microphone (MEP, *Multiple Effect Packs*).
`PKEY_FX_MEP_UserInterfaceClsid` in the INF is what makes the pack selectable —
without that value it installs and never shows up in the list.
