# Kwiet — consignes projet

- Langue de travail : **français** (docs, messages installeur, échanges).
  Code et commentaires C++/Rust : anglais.
- **Jamais installer/enregistrer l'APO sur le poste de travail.** Compiler ici
  est OK ; `installer/install.ps1` ne se lance qu'en VM (cf.
  `docs/procedure-test-vm.md`). Un APO buggé = machine sans son.
- Jalons stricts (cf. README) : ne pas toucher `dsp/` ni `ui/` tant que le
  jalon 1 (passthrough stable 48 h en VM) n'est pas validé.
- `APOProcess()` est un chemin temps-réel : aucune allocation, lock, syscall,
  logging ou appel COM. Fail-open obligatoire (passthrough, jamais de silence).
- Décisions actées (ne pas rouvrir sans raison) : slot **MFX**
  (`PKEY_FX_ModeEffectClsid`), modes DEFAULT + COMMUNICATIONS, x64 uniquement,
  CRT statique (/MT), licence Apache-2.0. Détail : `docs/architecture.md`.
- Sur les choix registre/COM non couverts par les docs : poser la question
  plutôt qu'improviser.
- Le CLSID `{65D564E6-9709-4F5C-85CF-449D92949CFE}` est défini dans
  `apo/src/KwietGuids.h` et dupliqué dans `installer/lib/common.ps1` — garder
  les deux synchronisés.

## Build

```powershell
cmake -S apo -B apo/build -A x64
cmake --build apo/build --config Release
```
