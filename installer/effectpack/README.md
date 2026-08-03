# installer/effectpack/ — pack d'effets Kwiet (voie moderne Windows 11)

Remplace l'édition manuelle des `FxProperties` par endpoint (`installer/install.ps1`),
qui **ne fonctionne plus** sur les builds Windows 11 récents : l'APO est bien
instancié puis écarté avant `LockForProcess`. Mécanisme et constats détaillés
dans [`../../docs/architecture.md`](../../docs/architecture.md) §7bis et §8.

Recette calquée sur le pack **Voice Clarity** de Microsoft
(`C:\Windows\INF\oem155.inf`) et sur [Aec3APO](https://github.com/msdx321/Aec3APO).

| Fichier | Rôle |
|---|---|
| `kwiet_extension.inf` | `Class=Extension` sur `COMPUTER\Generic` → `AddComponent` crée le devnode `SWC\VEN_KWIET&AUDIO_EFFECTPACK_KWIET` |
| `kwiet_component.inf` | `Class=AudioProcessingObject` : copie la DLL dans le DriverStore, écrit CLSID + catalogue APO + `EffectPackRegistration` (ciblage capture/micro) |
| `sign-install-dev.ps1` | **DEV** : certificat auto-signé, signature DLL + catalogue, `pnputil /add-driver /install` des deux INF |
| `uninstall-effectpack.ps1` | `pnputil /delete-driver /uninstall`, suppression du devnode, `-RemoveCert` pour retirer le certificat de test |

`package/` et `state/` sont générés localement et ignorés par git.

## Usage (dev)

```powershell
cmake --build ..\..\apo\build --config Release   # produire KwietApo.dll
.\sign-install-dev.ps1                            # admin requis
Get-PnpDevice -Class AudioProcessingObject        # device « Kwiet » attendu
# ... test ...
.\uninstall-effectpack.ps1 -RemoveCert
```

> Le certificat auto-signé est ajouté aux magasins **machine** `Root` et
> `TrustedPublisher` : acceptable en dev, à retirer ensuite
> (`-RemoveCert`). La distribution passera par une signature attestation
> (Partner Center).

## ⚠️ Étape indispensable après l'installation

Installer le pack ne l'active pas : il devient une **option** à choisir
manuellement dans

`Paramètres > Système > Son > [le micro] > Améliorations audio`

où « Kwiet » apparaît à côté de « Voice Clarity » / des effets du fabricant.
Un seul pack peut être actif par micro (« MEP » = *Multiple Effect Packs*).
C'est `PKEY_FX_MEP_UserInterfaceClsid` dans l'INF qui rend le pack
sélectionnable — sans cette valeur, il est installé mais invisible dans la
liste.

## Statut

✅ Validé le 2026-08-03 sur Windows 11 build 26200 : APO chargé dans
`audiodg.exe`, `LockForProcess` OK (2 ch / 48 kHz), 8 cycles Lock/Unlock sans
fuite, audio transmis. Détails : [`../../docs/architecture.md`](../../docs/architecture.md) §8.
