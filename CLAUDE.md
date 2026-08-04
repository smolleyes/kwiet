# Kwiet — consignes projet

- Langue de travail : **français** (docs, messages installeur, échanges).
  Code et commentaires C++/Rust : anglais.
- **Installation sur ce poste : autorisée explicitement par le propriétaire du
  dépôt**, qui accepte de redémarrer si besoin. La consigne d'origine
  (VM uniquement) reste valable pour quiconque d'autre, et
  `docs/procedure-test-vm.md` reste la procédure de référence. Un APO buggé =
  machine sans son.
- `APOProcess()` est un chemin temps-réel : aucune allocation, lock, syscall,
  logging ou appel COM. Fail-open obligatoire (passthrough, jamais de silence).
- Décisions actées (ne pas rouvrir sans raison) : slot **MFX**, installation par
  **pack d'effets** (INF composant + extension), x64 uniquement, CRT statique
  (/MT), licence Apache-2.0, **pas d'annulation d'écho** (la réclamer désactive
  celle de Chrome, meilleure). Détail : `docs/architecture.md`.
- Sur les choix registre/COM non couverts par les docs : poser la question
  plutôt qu'improviser.

## Valeurs dupliquées à garder synchronisées

| Valeur | Emplacements |
|---|---|
| CLSID `{65D564E6-9709-4F5C-85CF-449D92949CFE}` | `apo/src/KwietGuids.h`, `installer/lib/common.ps1`, `ui/src-tauri/src/pack.rs` |
| ExtensionId `{6A05FC42-…}` + nom du composant | `installer/effectpack/kwiet_extension.inf`, `ui/src-tauri/src/pack.rs` |
| Contrat du bloc de contrôle | `apo/src/KwietControl.h` ↔ `ui/src-tauri/src/control.rs` |
| Version | `ui/src-tauri/tauri.conf.json`, `ui/src-tauri/Cargo.toml`, `ui/package.json` |

## Pièges déjà payés

- **PowerShell 5.1 lit un `.ps1` UTF-8 sans BOM comme de l'ANSI** : tout
  caractère accentué casse l'analyse. Les scripts accentués doivent avoir un
  BOM. Ne jamais faire transiter un fichier accentué par
  `Get-Content`/`Set-Content` : ça le corrompt silencieusement.
- **L'installeur NSIS est 32 bits** : WOW64 redirige `System32` vers
  `SysWOW64`, où `pnputil.exe` n'existe pas. Passer par `Sysnative`.
- **`pnputil` compare les paquets sur `DriverVer`, jamais sur le contenu** : sans
  bump, une DLL recompilée n'est pas redéployée et l'installation *signale un
  succès*.
- **Un pack installé n'est pas un pack actif** : l'utilisateur doit le choisir
  dans Paramètres > Son > micro > Améliorations audio. Rien dans la pile audio
  ne le signale ; le panneau le détecte et le dit.
- **« DLL chargée dans audiodg » et « 0 nouvelle ligne de log » ne prouvent
  rien** : les DLL restent mappées, et un flux déjà verrouillé n'écrit rien.
  Le seul instrument fiable est un flux visible dans le panneau.

## Build

```powershell
cmake -S apo -B apo/build -A x64
cmake --build apo/build --config Release
cd dsp ; cargo build --release ; cd ..
.\installer\effectpack\build-package.ps1 -Version x.y.z -CertPath … -CertPassword …
cd ui ; npm run tauri build
```
