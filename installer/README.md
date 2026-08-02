# installer/ — scripts d'installation de l'APO Kwiet

> ⚠️ **VM ou machine dédiée UNIQUEMENT.** Un APO buggé = machine sans son.
> Procédure complète et checklist de sécurité : [`../docs/procedure-test-vm.md`](../docs/procedure-test-vm.md).

| Script | Rôle |
|---|---|
| `install.ps1` | Backup .reg → copie DLL → enregistrement COM → FxProperties (slot MFX par défaut) → `DisableProtectedAudioDG=1` → état JSON |
| `uninstall.ps1` | Défait exactement ce que l'install a fait (état JSON), ou scanne et nettoie. **No-op propre sur machine vierge** — à tester avant la première install |
| `status.ps1` | État de l'installation, lecture seule |
| `lib/common.ps1` | Constantes (CLSID, PKEY, modes) et fonctions partagées |

Les dossiers `backups/` (exports .reg horodatés) et `state/` (états JSON) sont
créés à la première installation et ignorés par git.

L'installeur WiX (jalon 4) remplacera ces scripts pour la distribution ; ils
restent la référence du contenu registre.

## Usage type (dans la VM)

```powershell
# 0. AVANT toute première install : vérifier que la désinstallation est un no-op propre
.\uninstall.ps1

# 1. Installer (sélection interactive de l'endpoint)
.\install.ps1

# 2. Vérifier
.\status.ps1

# 3. Désinstaller et vérifier le retour à l'état initial
.\uninstall.ps1
```
