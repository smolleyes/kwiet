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

## Ce qui reste à valider

Le pack s'installe proprement (device OK, registre de classe conforme), mais
les endpoints ne le référencent pas encore et la DLL n'est pas chargée.
Prochaine étape : **redémarrage complet**, puis diff de notre
`EffectPackRegistration` contre celle de Voice Clarity.
