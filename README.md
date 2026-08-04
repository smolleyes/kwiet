<div align="center">

<img src="assets/svg/lockup.svg" alt="Kwiet" width="360">

**Votre micro, nettoyÃ© par IA, dans toutes vos applications.**
Pas de micro virtuel. Pas de pÃ©riphÃ©rique Ã  changer. Un interrupteur.

[![CI](https://github.com/smolleyes/kwiet/actions/workflows/ci.yml/badge.svg)](https://github.com/smolleyes/kwiet/actions/workflows/ci.yml)
[![Licence](https://img.shields.io/badge/licence-Apache--2.0-9FD3C0)](LICENSE)
[![Windows 11](https://img.shields.io/badge/Windows-11-0D1417)](#pr%C3%A9requis)

[English](README.en.md) Â· [Architecture](docs/architecture.md)

</div>

---

## Le problÃ¨me

Les outils de suppression de bruit installent un **micro virtuel**. Il faut donc
aller le sÃ©lectionner dans Teams, dans Discord, dans Meet, dans chaque
application â€” et le refaire Ã  chaque fois qu'une mise Ã  jour remet le
pÃ©riphÃ©rique par dÃ©faut. Quand Ã§a se passe mal, l'application capte le mauvais
micro et personne ne vous entend.

Kwiet ne crÃ©e aucun pÃ©riphÃ©rique. Il se greffe sur **votre** micro, Ã 
l'intÃ©rieur du moteur audio de Windows. Les applications ne voient rien
d'inhabituel : elles ouvrent le micro qu'elles ont toujours ouvert, et le signal
qui en sort est dÃ©jÃ  propre.

## Comment Ã§a marche

Kwiet est un **APO** (*Audio Processing Object*) : une DLL COM en espace
utilisateur, chargÃ©e par `audiodg.exe`, le moteur audio de Windows. Aucun
pilote noyau.

Le dÃ©bruitage est fait par [DeepFilterNet3](https://github.com/Rikorose/DeepFilterNet),
un rÃ©seau de ~2 M de paramÃ¨tres â€” bien trop lourd pour le thread temps rÃ©el
d'`audiodg`, qui doit rendre un bloc toutes les 10 ms sans jamais allouer, ni
prendre un verrou, ni faire un appel systÃ¨me.

```
APOProcess()  â”€ thread temps rÃ©el d'audiodg â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
   â”‚  pousse l'entrÃ©e dans un anneau SPSC sans verrou (prÃ©allouÃ©)
   â”‚  rÃ©cupÃ¨re la sortie traitÃ©e (retard fixe de 30 ms)
   â””â”€ si le worker est mort ou en retard : PASSTHROUGH immÃ©diat
                                           jamais de silence, jamais de blocage
Worker  â”€ thread de prioritÃ© normale â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
   â””â”€ consomme l'anneau, appelle le cdylib Rust (DeepFilterNet3)
```

Le **fail-open est structurel** : un APO qui plante `audiodg` prive la machine
entiÃ¨re de son. Tout le design part de lÃ . Si le DSP disparaÃ®t, le son passe
sans traitement â€” c'est le pire qui puisse arriver.

DÃ©tails et dÃ©cisions : [`docs/architecture.md`](docs/architecture.md).

## Mesures

Sur Windows 11 build 26200, micro USB, 48 kHz stÃ©rÃ©o :

| | |
|---|---|
| Suppression de bruit | **â‰¥ 24 dB** sur source contrÃ´lÃ©e |
| Latence ajoutÃ©e | **30 ms**, fixe |
| Charge CPU | **~2 %** d'un cÅ“ur (RTF 0,017â€“0,020) |
| DÃ©crochages | 0 |

## Installation

> [!IMPORTANT]
> **AprÃ¨s l'installation, il reste une Ã©tape que Kwiet ne peut pas faire Ã  votre
> place.** Windows 11 exige que vous choisissiez vous-mÃªme le pack d'effets :
> **ParamÃ¨tres â†’ Son â†’ votre micro â†’ AmÃ©liorations audio â†’ Kwiet**.
> Tant que ce n'est pas fait, Kwiet est installÃ© et complÃ¨tement inerte. Le
> panneau vous le dira, avec un bouton pour ouvrir la bonne page.

1. TÃ©lÃ©chargez `Kwiet_x.y.z_x64-setup.exe` depuis les
   [releases](https://github.com/smolleyes/kwiet/releases).
2. Lancez-le. Il demande l'Ã©lÃ©vation : le pack d'effets est un package pilote,
   il s'enregistre auprÃ¨s de Windows via `pnputil`. Le son est briÃ¨vement coupÃ©.
3. Choisissez Kwiet dans les amÃ©liorations audio de votre micro (voir ci-dessus).
4. L'icÃ´ne dans la zone de notification ouvre le panneau.

La dÃ©sinstallation retire le pack du magasin de pilotes en mÃªme temps que
l'application.

### PrÃ©requis

- Windows 11 **24H2 ou plus rÃ©cent** (x64). Le mÃ©canisme de pack d'effets
  utilisÃ© ici n'existe pas sur les versions antÃ©rieures.
- Un micro qui nÃ©gocie **48 kHz**. En dehors, Kwiet se met volontairement en
  passthrough plutÃ´t que de rÃ©Ã©chantillonner Ã  l'aveugle.

> [!WARNING]
> **Signature.** Les binaires de release sont signÃ©s avec un certificat de
> dÃ©veloppement, ce qui suffit pour construire et tester chez soi mais **pas**
> pour une installation propre sur une machine tierce : Windows refuse un
> package pilote dont le catalogue ne remonte pas Ã  une autoritÃ© de confiance.
> La distribution publique demande une signature attestation via le Partner
> Center. Ce n'est pas encore en place, et c'est le principal obstacle avant que
> Kwiet soit installable par tout le monde.

## Le panneau

Une seule idÃ©e, rÃ©pÃ©tÃ©e partout : **le cÃ©ladon est ce que vos applications
reÃ§oivent, l'ambre est ce que Kwiet a retirÃ©.** L'Ã©cart entre les deux est le
produit.

- **Oscilloscope** â€” les derniÃ¨res secondes, en deux enveloppes superposÃ©es.
- **VU-mÃ¨tre** â€” l'instant, en une barre : le remplissage vif est le signal
  transmis, l'ambre qui dÃ©passe est le bruit supprimÃ©.
- **IntensitÃ©** â€” de discrÃ¨te Ã  maximale. Plus haut, le silence entre les mots
  devient total, au risque de raboter les attaques.
- **Interrupteur** â€” contournement immÃ©diat, sans couper le flux.

Le panneau parle **franÃ§ais ou anglais**, selon Windows, et se bascule Ã  la main
en bas Ã  droite.

## Construire depuis les sources

PrÃ©requis : Visual Studio 2019+ (charge de travail C++, SDK Windows 10/11),
CMake â‰¥ 3.21, Rust stable, Node 20+.

```powershell
# 1. L'APO (C++)
cmake -S apo -B apo/build -A x64
cmake --build apo/build --config Release

# 2. Le DSP (Rust, embarque le modÃ¨le DeepFilterNet3)
cd dsp ; cargo build --release ; cd ..

# 3. Le pack d'effets, signÃ©
.\installer\effectpack\build-package.ps1 -Version 0.2.1 `
    -CertPath mon-certificat.pfx -CertPassword $env:PFX_PW

# 4. L'application et l'installeur
cd ui ; npm ci ; npm run tauri build
# â†’ ui/src-tauri/target/release/bundle/nsis/Kwiet_0.2.1_x64-setup.exe
```

> [!CAUTION]
> **N'installez pas un APO en cours de dÃ©veloppement sur votre poste de
> travail.** Un APO qui plante prive la machine de son au dÃ©marrage. Utilisez
> une VM ou une machine dÃ©diÃ©e : [`docs/procedure-test-vm.md`](docs/procedure-test-vm.md).

Les icÃ´nes et le logo sont gÃ©nÃ©rÃ©s : `node assets/build-assets.mjs`.

## Structure du dÃ©pÃ´t

| Dossier | RÃ´le |
|---|---|
| [`apo/`](apo/) | C++ : le shim COM, les anneaux SPSC, le worker, la mÃ©moire partagÃ©e |
| [`dsp/`](dsp/) | Rust cdylib : DeepFilterNet3 derriÃ¨re une ABI C stable |
| [`ui/`](ui/) | Tauri v2 : panneau, zone de notification, installeur NSIS |
| [`installer/`](installer/) | Pack d'effets (INF, catalogue) et scripts d'installation |
| [`assets/`](assets/) | IdentitÃ© visuelle, gÃ©nÃ©rÃ©e par script |
| [`bench/`](bench/) | Outils de mesure |
| [`docs/`](docs/) | DÃ©cisions d'architecture, procÃ©dure de test |

## Ce qui n'est pas fait

Par honnÃªtetÃ©, plutÃ´t que de le dÃ©couvrir Ã  l'usage :

- **Signature attestation** â€” voir l'avertissement plus haut. C'est le blocage.
- **RÃ©Ã©chantillonnage** â€” hors 48 kHz, Kwiet passe en passthrough.
- **Soak 48 h** â€” changements de frÃ©quence, dÃ©branchement Ã  chaud, veille et
  reprise, plusieurs applications : jamais tenu sur une durÃ©e longue.
- **Annulation d'Ã©cho** â€” dÃ©libÃ©rÃ©ment absente. La rÃ©clamer ferait dÃ©sactiver Ã 
  Chrome son propre annuleur, qui est bien meilleur que ce qu'on livrerait.
- **La latence annoncÃ©e** (30 ms) ne compte que l'anneau, pas le lookahead
  propre Ã  DeepFilterNet3.

## Licence

[Apache-2.0](LICENSE).

DeepFilterNet3 est sous double licence MIT/Apache-2.0, compatible. Le modÃ¨le
embarquÃ© provient du projet [DeepFilterNet](https://github.com/Rikorose/DeepFilterNet)
de Hendrik SchrÃ¶ter.

